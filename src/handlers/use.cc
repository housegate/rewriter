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

} // namespace

bool handleUseQuery(DB::ASTPtr ast,
                    const rewriter::RewriteSQLRequest *request,
                    rewriter::RewriteSQLResponse *response) {
  auto *use_query = ast->as<DB::ASTUseQuery>();
  if (!use_query) return false;

  const std::string origin_db = use_query->getDatabase();

  const auto *general = findGeneralArgs(request->options());
  if (!general) {
    // No TableNameRewrite / general_args in the request → passthrough.
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_USE);
    return true;
  }

  auto physical = resolvePhysicalDatabase(origin_db, *general);
  if (!physical) {
    rejectInvalid(response,
      "USE target '" + origin_db
        + "' is not in database_map and not a known physical database; "
          "user does not have this database");
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
