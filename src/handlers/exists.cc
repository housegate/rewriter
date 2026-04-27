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

  const auto sel = findActiveTableRewrite(request->options());
  // Record the access regardless of mode/outcome so callers see what target
  // the rewriter processed even on static-mode passthrough or eventual reject.
  recordAccessedTable(response, origin_db, origin_table, sel);
  switch (sel.mode) {
  case TableRewriteMode::None:
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_EXISTS_TABLE);
    return ExistsDispatchResult::Handled;
  case TableRewriteMode::Dynamic: {
    const auto outcome = applyDynamicRewrite(
      origin_db, origin_table, *sel.dynamic_args, "EXISTS TABLE");
    if (outcome.status == DynamicRewriteStatus::InvalidRequest) {
      rejectExists(response, rewriter::RewriteCode::InvalidRewriteRequest,
                   outcome.reject_reason);
      return ExistsDispatchResult::Handled;
    }
    if (outcome.status == DynamicRewriteStatus::OkRemote) {
      rejectExists(response, rewriter::RewriteCode::UnsupportedStatement,
        "EXISTS TABLE target's logical resolves to a remote upstream "
        "(via dynamic_args.logical_database_to_remote_upstream_index); "
        "remote() can only appear as a SELECT-side table function");
      return ExistsDispatchResult::Handled;
    }
    q->setDatabase(outcome.physical_db);
    q->setTable(outcome.new_table);
    recordTableRewrite(response, origin_db, origin_table,
                       outcome.physical_db, outcome.new_table);
    break;
  }
  case TableRewriteMode::Static: {
    const auto lookup = lookupStaticTableRewrite(
      origin_db, origin_table, *sel.static_args);
    switch (lookup.status) {
    case StaticLookupOutcome::Status::NotMatched:
      // Static-mode passthrough: target wasn't in any map. Emit unchanged.
      break;
    case StaticLookupOutcome::Status::RemoteUnsupported:
      rejectExists(response, rewriter::RewriteCode::UnsupportedStatement,
        "EXISTS TABLE target maps to remote_table_map; remote() can only "
        "appear as a SELECT-side table function");
      return ExistsDispatchResult::Handled;
    case StaticLookupOutcome::Status::Matched: {
      const std::string effective_new_db =
        lookup.new_db.empty() ? origin_db : lookup.new_db;
      if (!lookup.new_db.empty()) q->setDatabase(lookup.new_db);
      q->setTable(lookup.new_table);
      recordTableRewrite(response, origin_db, origin_table,
                         effective_new_db, lookup.new_table);
      break;
    }
    }
    break;
  }
  }

  setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_EXISTS_TABLE);
  return ExistsDispatchResult::Handled;
}

} // namespace rewriter_handlers
