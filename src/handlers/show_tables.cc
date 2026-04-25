// src/handlers/show_tables.cc
#include "handlers/show_tables.h"

#include <Parsers/ASTShowTablesQuery.h>

#include <iostream>
#include <string>

#include "handlers/common.h"
#include "handlers/name_rewrite.h"

namespace rewriter_handlers {

namespace {

void rejectInvalid(rewriter::RewriteSQLResponse *response, const std::string &msg) {
  std::cerr << "[show-tables-reject] " << msg << std::endl;
  response->set_message(msg);
  response->set_code(rewriter::RewriteCode::InvalidRewriteRequest);
}

void rejectUnsupported(rewriter::RewriteSQLResponse *response, const std::string &msg) {
  std::cerr << "[show-tables-reject] " << msg << std::endl;
  response->set_message(msg);
  response->set_code(rewriter::RewriteCode::UnsupportedStatement);
}

} // namespace

bool handleShowTablesQuery(DB::ASTPtr ast,
                           const rewriter::RewriteSQLRequest *request,
                           rewriter::RewriteSQLResponse *response) {
  auto *show_query = ast->as<DB::ASTShowTablesQuery>();
  if (!show_query) return false;

  // SHOW CLUSTERS / SHOW DICTIONARIES / SHOW SETTINGS / ... — the rewrite
  // policy here only covers SHOW TABLES proper; SHOW DATABASES is handled
  // by handleShowDatabasesQuery, which dispatches before us.
  if (show_query->clusters || show_query->cluster
      || show_query->dictionaries || show_query->m_settings
      || show_query->merges || show_query->caches) {
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_SHOW_TABLES);
    return true;
  }

  const auto *dynamic = findDynamicArgs(request->options());
  if (!dynamic) {
    // No rewrite config → passthrough.
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_SHOW_TABLES);
    return true;
  }

  // "FROM <db>" / "IN <db>" in the original SQL. `getFrom()` returns
  // "database.table" or "database"; keep the leading db part only — the
  // SHOW TABLES semantics treat the FROM clause as the logical db.
  std::string from_logical = show_query->getFrom();
  if (auto dot = from_logical.find('.'); dot != std::string::npos) {
    from_logical = from_logical.substr(0, dot);
  }

  // SQL qualifier wins over upstream context, same as applyDynamicRewrite.
  const std::string logical = from_logical.empty()
    ? dynamic->upstream_logical_database_in_context()
    : from_logical;

  if (logical.empty()) {
    rejectInvalid(response,
      "SHOW TABLES has no FROM clause and no "
      "upstream_logical_database_in_context is set; caller must send "
      "`USE <db>` or use `SHOW TABLES FROM <db>`");
    return true;
  }

  auto physical = resolvePhysicalDatabase(logical, *dynamic);
  if (!physical) {
    rejectInvalid(response,
      "SHOW TABLES target logical database '" + logical
        + "' is not in database_map and not a known physical database; "
          "user does not have this database");
    return true;
  }
  if (isLogicalRemoteMapped(logical, *dynamic)) {
    // The output SQL queries `system.tables` on the LOCAL physical, which
    // wouldn't reflect the remote cluster's tables. Reject so the proxy /
    // caller can decide how to surface the remote tenant's table list
    // (e.g. by issuing a `remote(...) → system.tables` query of its own).
    rejectUnsupported(response,
      "SHOW TABLES target logical database '" + logical
        + "' is mapped to a remote upstream via "
          "dynamic_args.logical_database_to_remote_upstream_index; "
          "the rewriter cannot enumerate tables on a remote cluster");
    return true;
  }

  const std::string prefix = buildDynamicTablePrefix(logical, *dynamic);
  const std::string esc_prefix   = escapeSqlLiteral(prefix);
  const std::string esc_physical = escapeSqlLiteral(*physical);

  // Outer multiIf strips the prefix from each row; inner subquery filters
  // system.tables by (database = physical AND startsWith(name, prefix)).
  // Subquery avoids alias-shadowing the outer `name` column.
  std::string result_sql =
    "SELECT multiIf("
      "startsWith(name, '" + esc_prefix + "'), "
        "substring(name, length('" + esc_prefix + "') + 1), "
      "name) AS name "
    "FROM (SELECT name FROM system.tables "
          "WHERE database = '" + esc_physical + "' "
            "AND startsWith(name, '" + esc_prefix + "'))";

  setSuccessResponse(response, result_sql, rewriter::STATEMENT_TYPE_SHOW_TABLES);
  // Record the DB rewrite for observability — the per-table prefix is
  // already encoded into the output SQL, so callers only need the logical
  // -> physical mapping out of band.
  recordDatabaseRewrite(response, logical, *physical);
  return true;
}

} // namespace rewriter_handlers
