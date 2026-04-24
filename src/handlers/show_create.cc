// src/handlers/show_create.cc
#include "handlers/show_create.h"

#include <Parsers/TablePropertiesQueriesASTs.h>

#include <iostream>

#include "handlers/common.h"
#include "handlers/name_rewrite.h"

namespace rewriter_handlers {

namespace {

void rejectShowCreate(rewriter::RewriteSQLResponse *response,
                      rewriter::RewriteCode code,
                      const std::string &msg) {
  std::cerr << "[show-create-reject] " << msg << std::endl;
  response->set_message(msg);
  response->set_code(code);
}

} // namespace

ShowCreateDispatchResult handleShowCreateQuery(DB::ASTPtr ast,
                                               const rewriter::RewriteSQLRequest *request,
                                               rewriter::RewriteSQLResponse *response) {
  if (ast->as<DB::ASTShowCreateDatabaseQuery>()) {
    rejectShowCreate(response, rewriter::RewriteCode::UnsupportedStatement,
      "SHOW CREATE DATABASE is not supported; only SHOW CREATE TABLE is allowed");
    return ShowCreateDispatchResult::Handled;
  }
  if (ast->as<DB::ASTShowCreateViewQuery>()) {
    rejectShowCreate(response, rewriter::RewriteCode::UnsupportedStatement,
      "SHOW CREATE VIEW is not supported; only SHOW CREATE TABLE is allowed");
    return ShowCreateDispatchResult::Handled;
  }
  if (ast->as<DB::ASTShowCreateDictionaryQuery>()) {
    rejectShowCreate(response, rewriter::RewriteCode::UnsupportedStatement,
      "SHOW CREATE DICTIONARY is not supported; only SHOW CREATE TABLE is allowed");
    return ShowCreateDispatchResult::Handled;
  }

  auto *q = ast->as<DB::ASTShowCreateTableQuery>();
  if (!q) return ShowCreateDispatchResult::NotShowCreate;

  const std::string origin_db = q->getDatabase();
  const std::string origin_table = q->getTable();
  std::string new_db;
  std::string new_table;
  const auto outcome = applySingleTableRewrite(
    origin_db, origin_table, request->options(), &new_db, &new_table, "SHOW CREATE TABLE");

  if (outcome.status == SingleTableRewriteStatus::RejectRemote) {
    rejectShowCreate(response, rewriter::RewriteCode::InvalidRewriteRequest,
                     outcome.reject_reason);
    return ShowCreateDispatchResult::Handled;
  }

  if (outcome.status == SingleTableRewriteStatus::Rewritten) {
    if (!new_db.empty()) q->setDatabase(new_db);
    q->setTable(new_table);
    recordTableRewrite(response, origin_db, origin_table, new_db, new_table);
  }

  setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_SHOW_CREATE_TABLE);
  return ShowCreateDispatchResult::Handled;
}

} // namespace rewriter_handlers
