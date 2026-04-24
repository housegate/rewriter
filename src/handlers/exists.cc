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

  const auto *general = findGeneralArgs(request->options());
  if (!general) {
    // No rewrite config → passthrough.
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_EXISTS_TABLE);
    return ExistsDispatchResult::Handled;
  }

  const auto outcome = applyDynamicRewrite(origin_db, origin_table, *general, "EXISTS TABLE");
  if (outcome.status == DynamicRewriteStatus::InvalidRequest) {
    rejectExists(response, rewriter::RewriteCode::InvalidRewriteRequest, outcome.reject_reason);
    return ExistsDispatchResult::Handled;
  }

  // OK: rewrite the AST's db + table slots.
  q->setDatabase(outcome.physical_db);
  q->setTable(outcome.new_table);
  recordTableRewrite(response, origin_db, origin_table,
                     outcome.physical_db, outcome.new_table);

  setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_EXISTS_TABLE);
  return ExistsDispatchResult::Handled;
}

} // namespace rewriter_handlers
