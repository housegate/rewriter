// src/handlers/use.cc
#include "handlers/use.h"

#include <Parsers/ASTUseQuery.h>

#include <iostream>
#include <string>

#include "handlers/common.h"
#include "handlers/name_rewrite.h"

namespace rewriter_handlers {

namespace {

void rejectInvalid(rewriter::RewriteSQLResponse *response, const std::string &msg) {
  std::cerr << "[use-reject] " << msg << std::endl;
  response->set_message(msg);
  response->set_code(rewriter::RewriteCode::InvalidRewriteRequest);
}

void rejectUnsupported(rewriter::RewriteSQLResponse *response, const std::string &msg) {
  std::cerr << "[use-reject] " << msg << std::endl;
  response->set_message(msg);
  response->set_code(rewriter::RewriteCode::UnsupportedStatement);
}

} // namespace

bool handleUseQuery(DB::ASTPtr ast,
                    const rewriter::RewriteSQLRequest *request,
                    rewriter::RewriteSQLResponse *response) {
  auto *use_query = ast->as<DB::ASTUseQuery>();
  if (!use_query) return false;

  const std::string origin_db = use_query->getDatabase();

  const auto *dynamic = findDynamicArgs(request->options());
  if (!dynamic) {
    // No TableNameRewrite / dynamic_args in the request → passthrough.
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_USE);
    return true;
  }

  auto physical = resolvePhysicalDatabase(origin_db, *dynamic);
  if (!physical) {
    rejectInvalid(response,
      "USE target '" + origin_db
        + "' is not in database_map and not a known physical database; "
          "user does not have this database");
    return true;
  }
  if (isLogicalRemoteMapped(origin_db, *dynamic)) {
    // USE has no remote analog — rewriting to `USE <local physical>` would
    // silently misroute every subsequent unqualified query to the local
    // physical instead of the remote cluster. Reject so the proxy /
    // caller can decide how to expose this state.
    rejectUnsupported(response,
      "USE target '" + origin_db
        + "' is mapped to a remote upstream via "
          "dynamic_args.logical_database_to_remote_upstream_index; "
          "USE has no remote analog");
    return true;
  }

  // ASTUseQuery has no public setDatabase(); build the SQL by hand.
  // Matches the old handler's approach — USE basically only has a db name,
  // so re-formatting the AST wouldn't add anything.
  if (*physical != origin_db) {
    setSuccessResponse(response, "USE " + *physical, rewriter::STATEMENT_TYPE_USE);
    // USE is purely a DB-level rewrite — record under database_rewrites,
    // not table_rewrites.
    recordDatabaseRewrite(response, origin_db, *physical);
  } else {
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_USE);
  }
  return true;
}

} // namespace rewriter_handlers
