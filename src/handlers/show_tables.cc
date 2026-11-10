// src/handlers/show_tables.cc
#include "handlers/show_tables.h"

#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTShowColumnsQuery.h>
#include <Parsers/ASTShowIndexesQuery.h>
#include <Parsers/ASTShowTablesQuery.h>

#include <iostream>
#include <string>

#include "handlers/common.h"
#include "handlers/name_rewrite.h"
#include "handlers/sql_tokens.h"
#include "handlers/storage_integrity.h"
#include "handlers/system_tables.h"
#include "handlers/table_reference.h"

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
  return dynamic && storageIntegritySurfaceActive(*dynamic);
}

// The SHOW COLUMNS / INDEXES family's target, read straight off the AST.
// ClickHouse's parsers resolve both clauses eagerly into plain strings:
// ParserShowColumnsQuery / ParserShowIndexesQuery take a compound identifier
// for the table and split its qualifier into `database`, or read a second
// FROM/IN clause into `database` when the table is unqualified.
//
// `database` is empty exactly when the statement names no database: both
// parsers require an identifier after the second FROM/IN and fail the whole
// parse otherwise, and neither passes allow_query_parameter, so — unlike
// SHOW TABLES / SHOW DICTIONARIES — an explicit-but-unresolvable database
// target (`FROM {db:Identifier}`) is a SyntaxError before any handler runs and
// cannot be observed here. `table` is always set for a statement that parsed.
struct ShowColumnsTarget {
  bool matched = false;
  std::string database;
  std::string table;
};

ShowColumnsTarget showColumnsTarget(const DB::ASTPtr &ast) {
  if (const auto *columns = ast->as<DB::ASTShowColumnsQuery>())
    return {true, columns->database, columns->table};
  if (const auto *indexes = ast->as<DB::ASTShowIndexesQuery>())
    return {true, indexes->database, indexes->table};
  return {};
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
    // SHOW CHANGED SETTINGS is a kind the SI surface does not model; the SI
    // catch-all answers it (rewriter-go showKindClass).
    if (show_query->changed && storageIntegrityActive(dynamic)) {
      rejectUnsupported(response, std::string(kStorageIntegrityUnmodelledMessage));
      return true;
    }
    // Spec 2026-09-26 R7: the trailing clauses are forwarded as written.
    if (rejectShowBody(*show_query, request->sql(), dynamic, response)) return true;
    // Spec 2026-09-26 §5 "The system database": a SHOW form that reads a
    // system table outside the allowlist, after its SI namespace check and
    // its body check (rewriter-go systemTableShowRefusal).
    if (dynamic) {
      // R5 outranks it in both SI states (rewriter-go).
      if (rejectQuerySettings(ast, response, &request->sql())) return true;
      const char *system_table = show_query->dictionaries ? "dictionaries"
        : show_query->merges                             ? "merges"
        : show_query->caches                             ? "filesystem_cache_settings"
                                                         : nullptr;
      if (system_table) {
        rejectShowSystemTable(response, system_table);
        return true;
      }
    }
    // DICTIONARIES (static / no-rewrite mode only, from here on) is an exact
    // passthrough family. ASTShowTablesQuery's
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

  // Spec 2026-09-26 R7 holds for every SHOW trailing clause. The synthetic
  // enumeration below does not carry the WHERE / LIMIT, so nothing there
  // reaches ClickHouse today; the clause is refused rather than silently
  // dropped when it could read a table.
  if (rejectShowBody(*show_query, request->sql(), dynamic, response)) return true;

  // The synthetic enumeration models SHOW [FULL] TABLES [{FROM | IN} <db>]
  // [[NOT] {LIKE | ILIKE} '<pattern>'] [LIMIT <n>] (housegate/housegate#319).
  // It would drop a WHERE, FORMAT, INTO OUTFILE or SETTINGS clause, and it
  // cannot answer SHOW TEMPORARY TABLES (session tables, no database filter),
  // so those are refused rather than silently changed, as are an empty
  // pattern and a LIMIT that is not a UInt64 literal (rewriter-go
  // dispatchShowTables, the same order: after the database is resolved). R5
  // outranks it in both SI states (with the surface inactive the preflight
  // already ran it).
  if (rejectQuerySettings(ast, response, &request->sql())) return true;
  const bool has_like = !show_query->like.empty()
    || textHasWord(request->sql(), "like") || textHasWord(request->sql(), "ilike");
  const auto *limit_literal = show_query->limit_length
    ? show_query->limit_length->as<DB::ASTLiteral>()
    : nullptr;
  if (show_query->temporary || (has_like && show_query->like.empty())
      || show_query->where_expression
      || (show_query->limit_length
          && (!limit_literal || limit_literal->value.getType() != DB::Field::Types::UInt64))
      || show_query->hasOutputOptions()) {
    rejectUnsupported(response, std::string(kUnsupportedStatementMessage));
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
  // FULL answers (name, engine), as ClickHouse's own SHOW FULL TABLES does.
  const std::string columns = show_query->full ? "name, engine" : "name";
  const std::string listing =
    "SELECT multiIf("
      "startsWith(name, '" + esc_prefix + "'), "
        "substring(name, length('" + esc_prefix + "') + 1), "
      "name) AS name" + std::string(show_query->full ? ", engine" : "") + " "
    "FROM (SELECT " + columns + " FROM " + system_tables_source + " "
          "WHERE database = '" + esc_physical + "' "
            "AND startsWith(name, '" + esc_prefix + "'))";
  std::string result_sql = listing;
  if (show_query->full || has_like || limit_literal) {
    // The pattern and the limit apply to the logical name, after the prefix
    // is stripped, ordered by name as ClickHouse's own SHOW TABLES is; an
    // outer query keeps the filter off the stripped alias. The pattern is
    // the value ClickHouse decoded (`\_` keeps its backslash), re-escaped.
    result_sql = "SELECT " + columns + " FROM (" + listing + ")";
    if (has_like) {
      std::string op = show_query->case_insensitive_like ? "ILIKE" : "LIKE";
      if (show_query->not_like) op = "NOT " + op;
      result_sql += " WHERE name " + op + " '" + escapeSqlLiteral(show_query->like) + "'";
    }
    result_sql += " ORDER BY name";
    if (limit_literal)
      result_sql += " LIMIT " + std::to_string(limit_literal->value.safeGet<UInt64>());
  }

  setSuccessResponse(response, result_sql, rewriter::STATEMENT_TYPE_SHOW_TABLES);
  // Record the DB rewrite for observability — the per-table prefix is
  // already encoded into the output SQL, so callers only need the logical
  // -> physical mapping out of band.
  recordDatabaseRewrite(response, logical, *physical);
  return true;
}

bool handleShowColumnsQuery(DB::ASTPtr ast,
                            const rewriter::RewriteSQLRequest *request,
                            rewriter::RewriteSQLResponse *response) {
  const auto target = showColumnsTarget(ast);
  if (!target.matched) return false;

  const auto *dynamic = findDynamicArgs(request->options());
  if (storageIntegrityActive(dynamic)) {
    std::string database = target.database;
    if (database.empty()) {
      // A bare SHOW COLUMNS FROM <table> executes in the configured physical
      // context. Guard reserved physical databases before consulting the
      // logical context; an explicit clause remains authoritative over both.
      // Context values are already semantic names and must not be fed through
      // SQL identifier decoding.
      const std::string &physical_context = dynamic->upstream_physical_database_in_context();
      if (isStorageIntegrityPhysicalDatabase(physical_context, *dynamic)) {
        recordPhysicalStorageIntegrityAccess(response, physical_context, "", physical_context);
        rejectUnsupported(
          response, storageIntegrityPhysicalDatabaseRejectMessage(physical_context));
        return true;
      }
      database = dynamic->upstream_logical_database_in_context();
    }
    if (isStorageIntegrityPhysicalDatabase(database, *dynamic)) {
      // Unlike the database-only SHOW families this one also names a table, so
      // reject the pair and record the reserved (database, table) object — the
      // caller's storage-integrity flag path then sees the object rather than
      // only its database.
      if (!target.table.empty()) {
        recordPhysicalStorageIntegrityAccess(response, database, target.table, database);
        rejectUnsupported(
          response, storageIntegrityPhysicalRejectMessage(database + "." + target.table));
        return true;
      }
      recordPhysicalStorageIntegrityAccess(response, database, "", database);
      rejectUnsupported(response, storageIntegrityPhysicalDatabaseRejectMessage(database));
      return true;
    }
    if (isStorageIntegrityLogicalDatabase(database, *dynamic)) {
      std::string physical;
      if (auto resolved = resolvePhysicalDatabase(database, *dynamic)) physical = *resolved;
      recordStorageIntegrityAccess(response, database, "", database, physical);
      std::string authorized_logical;
      if (!authorizeStorageIntegrityLogical(database, *dynamic, &authorized_logical)) {
        rejectInvalid(response, storageIntegrityUnauthorizedMessage(authorized_logical));
      } else {
        rejectUnsupported(response,
          "storage-integrity logical database " + database + " is not directly addressable");
      }
      return true;
    }
  }

  // Spec 2026-09-26 R7: the trailing clauses are forwarded as written.
  if (rejectShowBody(*ast, request->sql(), dynamic, response)) return true;

  // A table clause of three or more parts: ClickHouse reads database = the
  // first part and table = the last, which the AST holds with the part count
  // dropped. The preflight's system-table scan already named a refused
  // system table in either reading; what is left is refused fail-closed
  // (rewriter-go DBLevelInfo.ShowTableMultiPart).
  if (dynamic && showTargetIsMultiPart(request->sql())) {
    response->clear_statement_type();
    rejectUnsupported(response, std::string(kUnsupportedStatementMessage));
    return true;
  }

  // Spec 2026-09-26 R7, amended 2026-11-07: the target resolves like an
  // EXISTS TABLE target. An unqualified one (a bare quoted `db2.x` is a table
  // named db2.x) resolves in the session's logical database; an explicit
  // database (`FROM db.t` or `FROM t FROM db`) is mapped the same way. Echoed
  // verbatim, ClickHouse would read the logical name as a physical database
  // (another tenant's, or `default`). Only ClickHouse's metadata databases are
  // echoed: the preflight's system-table allowlist judged their tables.
  if (dynamic && !isClickHouseMetadataDatabase(target.database)) {
    const auto sel = findActiveTableRewrite(request->options());
    recordAccessedTable(response, target.database, target.table, sel);
    const auto outcome = applyDynamicRewrite(target.database, target.table, *dynamic, "SHOW COLUMNS");
    if (outcome.status == DynamicRewriteStatus::InvalidRequest) {
      rejectInvalid(response, outcome.reject_reason);
      return true;
    }
    if (outcome.status == DynamicRewriteStatus::OkRemote) {
      rejectUnsupported(response,
        "SHOW COLUMNS target's logical resolves to a remote upstream "
        "(via dynamic_args.logical_database_to_remote_upstream_index); "
        "remote() can only appear as a SELECT-side table function");
      return true;
    }
    if (auto *columns = ast->as<DB::ASTShowColumnsQuery>()) {
      columns->database = outcome.physical_db;
      columns->table = outcome.new_table;
    } else if (auto *indexes = ast->as<DB::ASTShowIndexesQuery>()) {
      indexes->database = outcome.physical_db;
      indexes->table = outcome.new_table;
    }
    recordTableRewrite(response, target.database, target.table, outcome.physical_db, outcome.new_table);
    // ClickHouse's formatter spells the database as a second FROM clause.
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_SHOW_TABLES);
    return true;
  }

  // The namespace is ordinary (or no storage-integrity contract is active):
  // echo the original text instead of formatAst(ast). ASTShowColumnsQuery and
  // ASTShowIndexesQuery do not round-trip — the formatter canonicalizes
  // SHOW INDEX / KEYS to SHOW INDEXES and IN to FROM — and rewriter-go echoes
  // this family verbatim, so echoing is what keeps the two engines equal on
  // every spelling rather than only on the canonical ones the corpus pins.
  setSuccessResponse(response, request->sql(), rewriter::STATEMENT_TYPE_SHOW_TABLES);
  return true;
}

} // namespace rewriter_handlers
