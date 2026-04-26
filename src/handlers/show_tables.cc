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

  // Pick the FROM source for `system.tables`. For a local logical it's the
  // bare identifier; for a remote-mapped logical we route through
  // remote('addr', system, tables, user, password) so the enumeration runs
  // on the upstream cluster — same wrapping the SELECT path uses for
  // remote-mapped tables. The (database, prefix) filter still uses the
  // physical name resolved from database_map, since the prefix scheme is
  // the same on both sides.
  std::string system_tables_source = "system.tables";
  if (auto idx_it = dynamic->logical_database_to_remote_upstream_index().find(logical);
      idx_it != dynamic->logical_database_to_remote_upstream_index().end()) {
    const std::string &upstream_key = idx_it->second;
    auto upstream_it = dynamic->remote_upstreams().find(upstream_key);
    if (upstream_it == dynamic->remote_upstreams().end()) {
      rejectInvalid(response,
        "SHOW TABLES target logical database '" + logical
          + "' is mapped to remote upstream key '" + upstream_key
          + "' but that key is not in remote_upstreams");
      return true;
    }
    const auto &upstream = upstream_it->second;
    system_tables_source =
      "remote('" + escapeSqlLiteral(upstream.addr()) + "', "
      "system, tables, "
      "'" + escapeSqlLiteral(upstream.user()) + "', "
      "'" + escapeSqlLiteral(upstream.password()) + "')";
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
    "FROM (SELECT name FROM " + system_tables_source + " "
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
