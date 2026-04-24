// src/handlers/exists.cc
#include "handlers/exists.h"

#include <Parsers/TablePropertiesQueriesASTs.h>

#include <iostream>

#include "handlers/common.h"
#include "handlers/name_rewrite.h"

namespace rewriter_handlers {

namespace {

void rejectExists(rewriter::RewriteSQLResponse *response,
                  rewriter::RewriteCode code,
                  const std::string &msg) {
  std::cerr << "[exists-reject] " << msg << std::endl;
  response->set_message(msg);
  response->set_code(code);
}

} // namespace

ExistsDispatchResult handleExistsQuery(DB::ASTPtr ast,
                                       const rewriter::RewriteSQLRequest *request,
                                       rewriter::RewriteSQLResponse *response) {
  if (ast->as<DB::ASTExistsDatabaseQuery>()) {
    rejectExists(response, rewriter::RewriteCode::UnsupportedStatement,
                 "EXISTS DATABASE is not supported; only EXISTS TABLE is allowed");
    return ExistsDispatchResult::Handled;
  }
  if (ast->as<DB::ASTExistsViewQuery>()) {
    rejectExists(response, rewriter::RewriteCode::UnsupportedStatement,
                 "EXISTS VIEW is not supported; only EXISTS TABLE is allowed");
    return ExistsDispatchResult::Handled;
  }
  if (ast->as<DB::ASTExistsDictionaryQuery>()) {
    rejectExists(response, rewriter::RewriteCode::UnsupportedStatement,
                 "EXISTS DICTIONARY is not supported; only EXISTS TABLE is allowed");
    return ExistsDispatchResult::Handled;
  }

  auto *q = ast->as<DB::ASTExistsTableQuery>();
  if (!q) return ExistsDispatchResult::NotExists;

  const std::string origin_db = q->getDatabase();
  const std::string origin_table = q->getTable();
  std::string new_db;
  std::string new_table;
  const auto outcome = applySingleTableRewrite(
    origin_db, origin_table, request->options(), &new_db, &new_table, "EXISTS TABLE");

  if (outcome.status == SingleTableRewriteStatus::RejectRemote) {
    rejectExists(response, rewriter::RewriteCode::InvalidRewriteRequest, outcome.reject_reason);
    return ExistsDispatchResult::Handled;
  }

  if (outcome.status == SingleTableRewriteStatus::Rewritten) {
    if (!new_db.empty()) q->setDatabase(new_db);
    q->setTable(new_table);
    recordTableRewrite(response, origin_db, origin_table, new_db, new_table);
  }

  setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_EXISTS_TABLE);
  return ExistsDispatchResult::Handled;
}

} // namespace rewriter_handlers
