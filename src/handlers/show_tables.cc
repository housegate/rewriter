// src/handlers/show_tables.cc
#include "handlers/show_tables.h"

#include <Parsers/ASTShowTablesQuery.h>

#include <iostream>
#include <string>

#include "handlers/common.h"
#include "handlers/name_rewrite.h"
#include "handlers/storage_integrity.h"

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

std::string showDatabase(const DB::ASTShowTablesQuery &show_query) {
  std::string database = show_query.getFrom();
  if (const auto dot = database.find('.'); dot != std::string::npos)
    database = database.substr(0, dot);
  return database;
}

bool storageIntegrityActive(const rewriter::RewriteTableDynamicArgs *dynamic) {
  return dynamic && dynamic->has_storage_integrity()
    && !dynamic->storage_integrity().tables().empty();
}

} // namespace

bool handleShowTablesQuery(DB::ASTPtr ast,
                           const rewriter::RewriteSQLRequest *request,
                           rewriter::RewriteSQLResponse *response) {
  auto *show_query = ast->as<DB::ASTShowTablesQuery>();
  if (!show_query) return false;

  const auto *dynamic = findDynamicArgs(request->options());
  const bool has_explicit_database = show_query->from != nullptr;
  const std::string explicit_database = showDatabase(*show_query);

  // ASTQueryParameter is a syntactically explicit database target, but
  // ASTShowTablesQuery::getFrom() cannot resolve it to a semantic name. Under
  // active SI it must not fall through to the session context as if no clause
  // were present.
  if (storageIntegrityActive(dynamic) && has_explicit_database
      && explicit_database.empty()) {
    const std::string family = show_query->dictionaries
      ? "SHOW DICTIONARIES" : "SHOW TABLES";
    rejectUnsupported(response,
      "storage-integrity " + family + " database is not statically resolvable");
    return true;
  }

  // SHOW DICTIONARIES is otherwise a modeled passthrough, but its FROM/IN
  // database is still an addressable namespace. Guard that structured target
  // before the passthrough so an SI physical/logical database cannot escape
  // through this early return. Other SHOW families have no database target
  // and retain their legacy behavior.
  if (show_query->dictionaries && dynamic) {
    std::string logical = explicit_database;
    if (!has_explicit_database) {
      // Context values are already semantic names. Do not feed them through
      // SQL identifier decoding. The actual execution database is the first
      // contextual guard, ahead of the logical session name.
      const std::string &physical_context =
        dynamic->upstream_physical_database_in_context();
      if (isStorageIntegrityPhysicalDatabase(physical_context, *dynamic)) {
        recordPhysicalStorageIntegrityAccess(
          response, physical_context, "", physical_context);
        rejectUnsupported(
          response, storageIntegrityPhysicalDatabaseRejectMessage(physical_context));
        return true;
      }
      logical = dynamic->upstream_logical_database_in_context();
    }
    if (isStorageIntegrityPhysicalDatabase(logical, *dynamic)) {
      recordPhysicalStorageIntegrityAccess(response, logical, "", logical);
      rejectUnsupported(response, storageIntegrityPhysicalDatabaseRejectMessage(logical));
      return true;
    }
    if (isStorageIntegrityLogicalDatabase(logical, *dynamic)) {
      std::string physical;
      if (auto resolved = resolvePhysicalDatabase(logical, *dynamic)) physical = *resolved;
      recordStorageIntegrityAccess(response, logical, "", logical, physical);
      std::string authorized_logical;
      if (!authorizeStorageIntegrityLogical(logical, *dynamic, &authorized_logical)) {
        rejectInvalid(response, storageIntegrityUnauthorizedMessage(authorized_logical));
      } else {
        rejectUnsupported(response,
          "storage-integrity logical database " + logical + " is not directly addressable");
      }
      return true;
    }
  }

  // SHOW CLUSTERS / SHOW DICTIONARIES / SHOW SETTINGS / ... — the rewrite
  // policy here only covers SHOW TABLES proper; SHOW DATABASES is handled
  // by handleShowDatabasesQuery, which dispatches before us.
  if (show_query->clusters || show_query->cluster
      || show_query->dictionaries || show_query->m_settings
      || show_query->merges || show_query->caches) {
    // DICTIONARIES is an exact passthrough family. ASTShowTablesQuery's
    // formatter omits the legal FULL prefix, so formatting here would
    // silently change SHOW FULL [TEMPORARY] DICTIONARIES after its namespace
    // guard has succeeded. Other legacy SHOW families keep their normalized
    // AST output.
    const std::string result_sql = show_query->dictionaries
      ? request->sql()
      : formatAst(ast);
    setSuccessResponse(response, result_sql, rewriter::STATEMENT_TYPE_SHOW_TABLES);
    return true;
  }

  if (!dynamic) {
    // No rewrite config → passthrough.
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_SHOW_TABLES);
    return true;
  }

  // "FROM <db>" / "IN <db>" in the original SQL. `getFrom()` returns
  // "database.table" or "database"; keep the leading db part only — the
  // SHOW TABLES semantics treat the FROM clause as the logical db.
  std::string from_logical = explicit_database;

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

  if (isStorageIntegrityPhysicalDatabase(logical, *dynamic)) {
    recordPhysicalStorageIntegrityAccess(response, logical, "", logical);
    rejectUnsupported(response,
      storageIntegrityPhysicalDatabaseRejectMessage(logical));
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
