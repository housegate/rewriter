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

  const auto *general = findGeneralArgs(request->options());
  if (!general) {
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_SHOW_CREATE_TABLE);
    return ShowCreateDispatchResult::Handled;
  }

  const auto outcome = applyDynamicRewrite(
    origin_db, origin_table, *general, "SHOW CREATE TABLE");
  if (outcome.status == DynamicRewriteStatus::InvalidRequest) {
    rejectShowCreate(response, rewriter::RewriteCode::InvalidRewriteRequest,
                     outcome.reject_reason);
    return ShowCreateDispatchResult::Handled;
  }

  q->setDatabase(outcome.physical_db);
  q->setTable(outcome.new_table);
  recordTableRewrite(response, origin_db, origin_table,
                     outcome.physical_db, outcome.new_table);

  setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_SHOW_CREATE_TABLE);
  return ShowCreateDispatchResult::Handled;
}

} // namespace rewriter_handlers
