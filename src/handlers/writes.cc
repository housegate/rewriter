// src/handlers/writes.cc
#include "handlers/writes.h"

#include <Parsers/ASTAlterQuery.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTDeleteQuery.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTRenameQuery.h>
#include <Parsers/ASTUpdateQuery.h>

#include <iostream>
#include <string>

#include "handlers/common.h"
#include "handlers/name_rewrite.h"

namespace rewriter_handlers {

namespace {

void rejectWrite(rewriter::RewriteSQLResponse *response,
                 rewriter::RewriteCode code,
                 const std::string &msg) {
  // Stderr so operators can see what kinds of statements callers are
  // attempting (CREATE VIEW, DROP DATABASE, unqualified writes without
  // upstream context, ...) without having to correlate every error
  // response back to a client.
  std::cerr << "[write-reject] " << msg << std::endl;
  response->set_message(msg);
  response->set_code(code);
}

inline void rejectUnsupported(rewriter::RewriteSQLResponse *response, const std::string &msg) {
  rejectWrite(response, rewriter::RewriteCode::UnsupportedStatement, msg);
}

inline void rejectInvalidRequest(rewriter::RewriteSQLResponse *response, const std::string &msg) {
  rejectWrite(response, rewriter::RewriteCode::InvalidRewriteRequest, msg);
}

void acceptWrite(const DB::ASTPtr &ast,
                 rewriter::RewriteSQLResponse *response,
                 rewriter::StatementType stmt_type) {
  setSuccessResponse(response, formatAst(ast), stmt_type);
}

// Static-mode lookup result helper: dispatches the three possible static
// outcomes, populating `response` for the rejection cases. On Matched,
// invokes setDb only when the lookup gave a non-empty new_db (matching the
// SELECT path's behavior of preserving the original db when the map entry
// only renames the table). Returns the effective new_db (origin_db when
// the matched entry didn't supply one) so the caller can record the
// rewrite. `applied` tells the caller whether the AST was actually
// mutated (vs. NotMatched passthrough).
template <typename SetDb, typename SetTable>
bool applyStaticLookup(
  rewriter::RewriteSQLResponse *response,
  const StaticLookupOutcome &lookup,
  const std::string &origin_db,
  const std::string &origin_table,
  std::string_view stmt_kind,
  SetDb set_db,
  SetTable set_table) {
  switch (lookup.status) {
  case StaticLookupOutcome::Status::NotMatched:
    // Statement-level rule: once static-mode is selected, unmatched
    // targets pass through (no per-target fallback to dynamic).
    return true;
  case StaticLookupOutcome::Status::RemoteUnsupported:
    rejectUnsupported(response,
      std::string(stmt_kind)
        + " target maps to remote_table_map; remote() can only appear as a "
          "SELECT-side table function");
    return false;
  case StaticLookupOutcome::Status::Matched: {
    const std::string effective_new_db =
      lookup.new_db.empty() ? origin_db : lookup.new_db;
    if (!lookup.new_db.empty()) set_db(lookup.new_db);
    set_table(lookup.new_table);
    recordTableRewrite(response, origin_db, origin_table,
                       effective_new_db, lookup.new_table);
    return true;
  }
  }
  return true;
}

// Apply the unified TableNameRewrite policy to one (origin_db, origin_table)
// target, dispatching on the active mode. Returns false (and populates
// `response`) on rejection — the caller must short-circuit Rejected.
//
// Mode::None       -> no-op (caller echoes the original AST).
// Mode::Static     -> three-map lookup; see applyStaticLookup.
// Mode::Dynamic    -> existing applyDynamicRewrite (rejects InvalidRequest).
template <typename SetDb, typename SetTable>
bool rewriteOneTarget(
  rewriter::RewriteSQLResponse *response,
  const TableRewriteSelection &sel,
  const std::string &origin_db,
  const std::string &origin_table,
  std::string_view stmt_kind,
  SetDb set_db,
  SetTable set_table) {
  // Record the access before any reject path so original_accessed_tables
  // captures every (db, table) the handler intended to rewrite — including
  // static-mode passthroughs and dynamic-mode misses. RENAME's two sides
  // and CREATE TABLE AS's source flow through here too, so each gets its
  // own entry. Called only from non-SELECT single-target handlers; SELECT
  // populates the field via its own pre-rewrite AST walk.
  recordAccessedTable(response, origin_db, origin_table, sel);
  switch (sel.mode) {
  case TableRewriteMode::None:
    return true;
  case TableRewriteMode::Dynamic: {
    const auto outcome =
      applyDynamicRewrite(origin_db, origin_table, *sel.dynamic_args, stmt_kind);
    if (outcome.status == DynamicRewriteStatus::InvalidRequest) {
      rejectInvalidRequest(response, outcome.reject_reason);
      return false;
    }
    if (outcome.status == DynamicRewriteStatus::OkRemote) {
      // Dynamic-mode remote routing only makes sense for SELECT (where we
      // can wrap the identifier in remote()). For a write/EXISTS/SHOW
      // CREATE/RENAME target there's no equivalent — INSERT INTO FUNCTION
      // remote() is currently rejected by the rewriter anyway.
      rejectUnsupported(response,
        std::string(stmt_kind)
          + " target's logical resolves to a remote upstream "
            "(via dynamic_args.logical_database_to_remote_upstream_index); "
            "remote() can only appear as a SELECT-side table function");
      return false;
    }
    set_db(outcome.physical_db);
    set_table(outcome.new_table);
    recordTableRewrite(response, origin_db, origin_table,
                       outcome.physical_db, outcome.new_table);
    return true;
  }
  case TableRewriteMode::Static: {
    const auto lookup =
      lookupStaticTableRewrite(origin_db, origin_table, *sel.static_args);
    return applyStaticLookup(response, lookup, origin_db, origin_table,
                             stmt_kind, set_db, set_table);
  }
  }
  return true;
}

// ALTER commands that reference a second table (ATTACH/REPLACE/FETCH
// PARTITION FROM, MOVE PARTITION TO TABLE) are not rewritten in v1. Reject
// so callers can't silently ship a half-rewritten statement.
bool alterHasCrossTableRef(const DB::ASTAlterQuery &alter_query) {
  if (!alter_query.command_list) return false;
  for (const auto &cmd_ast : alter_query.command_list->children) {
    const auto *cmd = cmd_ast->as<DB::ASTAlterCommand>();
    if (!cmd) continue;
    if (!cmd->from_database.empty() || !cmd->from_table.empty()) return true;
    if (!cmd->to_database.empty() || !cmd->to_table.empty()) return true;
  }
  return false;
}

// Rewrite one side of a RENAME element. Returns false on reject (response
// already populated). Mutates side.database / side.table in place per the
// active TableNameRewrite mode.
bool rewriteRenameSide(
  rewriter::RewriteSQLResponse *response,
  const TableRewriteSelection &sel,
  DB::ASTRenameQuery::Table &side,
  std::string_view stmt_kind) {
  const std::string origin_db = side.getDatabase();
  const std::string origin_table = side.getTable();

  return rewriteOneTarget(
    response, sel, origin_db, origin_table, stmt_kind,
    [&](const std::string &s){ side.database = std::make_shared<DB::ASTIdentifier>(s); },
    [&](const std::string &s){ side.table    = std::make_shared<DB::ASTIdentifier>(s); });
}

} // namespace

WriteDispatchResult handleWriteQuery(DB::ASTPtr ast,
                                     const rewriter::RewriteSQLRequest *request,
                                     rewriter::RewriteSQLResponse *response) {
  // Table-targeting handlers (CREATE/DROP/ALTER/INSERT/UPDATE/DELETE/RENAME)
  // pick mode via the unified static→dynamic precedence.
  const auto sel = findActiveTableRewrite(request->options());
  // CREATE DATABASE is database-level — static_args has no semantic meaning
  // there, so it goes through findDynamicArgs directly. Multiple table-level
  // requests in the same call wouldn't expect static_args to suppress the
  // db-level dynamic_args validation.
  const auto *dynamic = findDynamicArgs(request->options());

  if (auto *create_query = ast->as<DB::ASTCreateQuery>()) {
    if (create_query->is_ordinary_view || create_query->is_materialized_view
        || create_query->is_live_view || create_query->is_window_view) {
      rejectUnsupported(response, "CREATE VIEW is not supported; only CREATE TABLE is allowed");
      return WriteDispatchResult::Rejected;
    }
    if (create_query->is_dictionary) {
      rejectUnsupported(response, "CREATE DICTIONARY is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (!create_query->table) {
      // CREATE DATABASE — special handling (see proto comment on
      // STATEMENT_TYPE_CREATE_DATABASE). We never actually emit the DDL
      // against the physical ClickHouse; the proxy is expected to do its
      // own database-creation bookkeeping. We just validate the request
      // and rewrite to a debug SELECT so the user's intent lands in
      // ClickHouse query_log.
      //
      // Without a TableNameRewrite/dynamic_args option in the request we
      // can't validate anything and don't have the policy context we'd
      // need to construct the debug rewrite — fall back to the previous
      // behavior of UnsupportedStatement so callers can't accidentally
      // ship the DDL to the physical DB.
      if (!dynamic) {
        rejectUnsupported(response,
          "CREATE DATABASE requires a TableNameRewrite/dynamic_args option to validate against");
        return WriteDispatchResult::Rejected;
      }

      const std::string target_db = create_query->getDatabase();
      // Record the db-level access before validation so callers see the
      // target on the response even when the request is rejected. For
      // CREATE DATABASE the logical is by precondition NOT in database_map
      // (otherwise we reject below), so physical_database stays empty.
      recordAccessedDatabase(response, target_db, dynamic);

      // Check 1: target logical DB already mapped in database_map → reject
      // (mirrors "database already exists" for the proxy's view of the
      // world). IF NOT EXISTS lets us silently fall through to the debug
      // rewrite, since CH semantics make that case a no-op.
      const auto db_map_it = dynamic->database_map().find(target_db);
      if (db_map_it != dynamic->database_map().end() && !create_query->if_not_exists) {
        rejectInvalidRequest(response,
          "CREATE DATABASE target '" + target_db
            + "' already exists (mapped to physical '" + db_map_it->second
            + "'); use IF NOT EXISTS to suppress this error");
        return WriteDispatchResult::Rejected;
      }

      // Check 2: caller's claimed physical context must be a real physical
      // DB. An unset `upstream_physical_database_in_context` means the
      // caller hasn't picked a target physical yet — that's fine for the
      // debug rewrite (we don't actually run any DDL). When set, it must
      // be in the known list, otherwise the proxy is sending garbage.
      if (dynamic->has_upstream_physical_database_in_context()) {
        const std::string &phys = dynamic->upstream_physical_database_in_context();
        bool known = false;
        for (const auto &p : dynamic->known_physical_databases()) {
          if (p == phys) { known = true; break; }
        }
        if (!phys.empty() && !known) {
          rejectInvalidRequest(response,
            "CREATE DATABASE: upstream_physical_database_in_context '" + phys
              + "' is not in known_physical_databases");
          return WriteDispatchResult::Rejected;
        }
      }

      // Debug rewrite: SELECT '<original SQL>' AS cdstmt
      //
      // We use the AST formatter (not request->sql() verbatim) so the
      // logged string is canonicalized — same shape every time, no
      // trailing whitespace / case quirks / comments leaking through.
      const std::string original_sql = formatAst(ast);
      const std::string debug_sql =
        "SELECT '" + escapeSqlLiteral(original_sql) + "' AS cdstmt";
      setSuccessResponse(response, debug_sql, rewriter::STATEMENT_TYPE_CREATE_DATABASE);
      return WriteDispatchResult::Handled;
    }
    if (create_query->as_table_function) {
      rejectUnsupported(response, "CREATE TABLE AS table_function(...) is not supported");
      return WriteDispatchResult::Rejected;
    }

    const std::string origin_db = create_query->getDatabase();
    const std::string origin_table = create_query->getTable();
    if (!rewriteOneTarget(response, sel, origin_db, origin_table, "CREATE TABLE",
        [&](const std::string &s){ create_query->setDatabase(s); },
        [&](const std::string &s){ create_query->setTable(s); })) {
      return WriteDispatchResult::Rejected;
    }

    // CREATE TABLE x AS y — rewrite the AS source the same way. as_database /
    // as_table are plain Strings; the lambdas assign through to them.
    if (sel.mode != TableRewriteMode::None && !create_query->as_table.empty()) {
      const std::string as_origin_db = create_query->as_database;
      const std::string as_origin_table = create_query->as_table;
      if (!rewriteOneTarget(response, sel, as_origin_db, as_origin_table,
          "CREATE TABLE AS source",
          [&](const std::string &s){ create_query->as_database = s; },
          [&](const std::string &s){ create_query->as_table    = s; })) {
        return WriteDispatchResult::Rejected;
      }
    }

    acceptWrite(ast, response, rewriter::STATEMENT_TYPE_CREATE_TABLE);
    return WriteDispatchResult::Handled;
  }

  if (auto *drop_query = ast->as<DB::ASTDropQuery>()) {
    const bool is_truncate = drop_query->kind == DB::ASTDropQuery::Kind::Truncate;
    const std::string_view stmt_kind_str = is_truncate ? "TRUNCATE TABLE" : "DROP TABLE";

    if (drop_query->kind == DB::ASTDropQuery::Kind::Detach) {
      rejectUnsupported(response, "DETACH is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (drop_query->is_view || drop_query->is_dictionary) {
      rejectUnsupported(response, "DROP/TRUNCATE on VIEW/DICTIONARY is not supported; only DROP TABLE / TRUNCATE TABLE are allowed");
      return WriteDispatchResult::Rejected;
    }
    if (drop_query->database_and_tables) {
      rejectUnsupported(response, "multi-table DROP/TRUNCATE is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (!drop_query->table) {
      // TRUNCATE without a table is "TRUNCATE DATABASE" / "TRUNCATE ALL
      // TABLES FROM <db>". A logical DB shares its physical DB with
      // siblings via prefix-sharing, so a single CH-level TRUNCATE
      // DATABASE would clear unrelated tenants. We could add a
      // soft-flag path symmetric to DROP DATABASE, but TRUNCATE's
      // semantics ("empty now") don't fit "deferred GC" the way DROP
      // does — leaving it unsupported until there's a concrete need.
      if (is_truncate) {
        rejectUnsupported(response, "TRUNCATE DATABASE / TRUNCATE ALL TABLES FROM is not supported; only TRUNCATE TABLE is allowed");
        return WriteDispatchResult::Rejected;
      }
      // DROP DATABASE — symmetric to the CREATE DATABASE path above. We
      // never emit the DDL against the physical CH (a logical DB shares a
      // physical DB with siblings via prefix-sharing; a single CH-level
      // DROP DATABASE would nuke unrelated tenants). Instead, validate
      // the request and rewrite to `SELECT '<original SQL>' AS ddstmt`
      // so the user's intent lands in query_log. An external GC binary
      // is expected to read query_log and reclaim the physical tables
      // asynchronously.
      if (!dynamic) {
        rejectUnsupported(response,
          "DROP DATABASE requires a TableNameRewrite/dynamic_args option to validate against");
        return WriteDispatchResult::Rejected;
      }

      const std::string target_db = drop_query->getDatabase();
      // Record the db-level access before validation. On a database_map hit,
      // physical_database is filled with the resolved physical; on an
      // IF EXISTS miss we still record the access (with empty physical) so
      // downstream sees the attempted target.
      recordAccessedDatabase(response, target_db, dynamic);

      const auto db_map_it = dynamic->database_map().find(target_db);
      if (db_map_it == dynamic->database_map().end()) {
        // Not a managed logical DB. IF EXISTS lets us silently fall
        // through to the debug rewrite (CH semantics: no-op when the DB
        // doesn't exist). Without IF EXISTS, reject — the proxy is
        // asking us to drop something we don't manage. We deliberately
        // do NOT consult known_physical_databases here: rewriter-managed
        // DROP is a logical-level concept; physical-DB lifecycle is out
        // of scope.
        if (!drop_query->if_exists) {
          rejectInvalidRequest(response,
            "DROP DATABASE target '" + target_db
              + "' is not in database_map; use IF EXISTS to suppress this error");
          return WriteDispatchResult::Rejected;
        }
      } else {
        // Hit: record logical -> physical so the GC binary knows where
        // to scan for the prefixed tables. IF EXISTS misses skip this
        // (no logical -> physical to record).
        recordDatabaseRewrite(response, target_db, db_map_it->second);
      }

      const std::string original_sql = formatAst(ast);
      const std::string debug_sql =
        "SELECT '" + escapeSqlLiteral(original_sql) + "' AS ddstmt";
      setSuccessResponse(response, debug_sql, rewriter::STATEMENT_TYPE_DROP_DATABASE);
      return WriteDispatchResult::Handled;
    }

    const std::string origin_db = drop_query->getDatabase();
    const std::string origin_table = drop_query->getTable();
    if (!rewriteOneTarget(response, sel, origin_db, origin_table, stmt_kind_str,
        [&](const std::string &s){ drop_query->setDatabase(s); },
        [&](const std::string &s){ drop_query->setTable(s); })) {
      return WriteDispatchResult::Rejected;
    }
    acceptWrite(ast, response,
                is_truncate ? rewriter::STATEMENT_TYPE_TRUNCATE_TABLE
                            : rewriter::STATEMENT_TYPE_DROP_TABLE);
    return WriteDispatchResult::Handled;
  }

  if (auto *alter_query = ast->as<DB::ASTAlterQuery>()) {
    if (alter_query->alter_object != DB::ASTAlterQuery::AlterObjectType::TABLE) {
      rejectUnsupported(response, "only ALTER TABLE is supported");
      return WriteDispatchResult::Rejected;
    }
    if (alterHasCrossTableRef(*alter_query)) {
      rejectUnsupported(response,
        "ALTER TABLE with cross-table reference (ATTACH/REPLACE/FETCH PARTITION FROM, "
        "MOVE PARTITION TO TABLE) is not supported");
      return WriteDispatchResult::Rejected;
    }

    const std::string origin_db = alter_query->getDatabase();
    const std::string origin_table = alter_query->getTable();
    if (!rewriteOneTarget(response, sel, origin_db, origin_table, "ALTER TABLE",
        [&](const std::string &s){ alter_query->setDatabase(s); },
        [&](const std::string &s){ alter_query->setTable(s); })) {
      return WriteDispatchResult::Rejected;
    }
    acceptWrite(ast, response, rewriter::STATEMENT_TYPE_ALTER_TABLE);
    return WriteDispatchResult::Handled;
  }

  if (auto *insert_query = ast->as<DB::ASTInsertQuery>()) {
    if (insert_query->table_function) {
      rejectUnsupported(response, "INSERT INTO FUNCTION(...) is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (!insert_query->table) {
      rejectUnsupported(response, "INSERT target table is missing");
      return WriteDispatchResult::Rejected;
    }

    const std::string origin_db = insert_query->getDatabase();
    const std::string origin_table = insert_query->getTable();
    if (!rewriteOneTarget(response, sel, origin_db, origin_table, "INSERT",
        [&](const std::string &s){ insert_query->setDatabase(s); },
        [&](const std::string &s){ insert_query->setTable(s); })) {
      return WriteDispatchResult::Rejected;
    }
    // ASTInsertQuery::formatImpl only emits the prelude (`INSERT INTO ...
    // [columns] [SETTINGS ...] [VALUES|FORMAT <fmt>]`); the inline payload
    // for `VALUES (...)` / `FORMAT <fmt> <bytes>` lives off-AST as raw
    // pointers into the original SQL buffer. ClickHouse's executor consumes
    // those bytes via query->data/query->end at insert time, so the
    // formatter intentionally drops them. We're shipping the rewritten SQL
    // back as a single string for the proxy to forward — append the raw
    // bytes verbatim so the downstream sees `... VALUES (1)` instead of a
    // bare `... FORMAT Values` (which would put the upstream into "wait for
    // wire-format data" mode and hang the client).
    std::string sql = formatAst(ast);
    if (insert_query->data && insert_query->end > insert_query->data) {
      sql.append(insert_query->data,
                 static_cast<size_t>(insert_query->end - insert_query->data));
    }
    setSuccessResponse(response, sql, rewriter::STATEMENT_TYPE_INSERT);
    return WriteDispatchResult::Handled;
  }

  if (auto *update_query = ast->as<DB::ASTUpdateQuery>()) {
    const std::string origin_db = update_query->getDatabase();
    const std::string origin_table = update_query->getTable();
    if (!rewriteOneTarget(response, sel, origin_db, origin_table, "UPDATE",
        [&](const std::string &s){ update_query->setDatabase(s); },
        [&](const std::string &s){ update_query->setTable(s); })) {
      return WriteDispatchResult::Rejected;
    }
    acceptWrite(ast, response, rewriter::STATEMENT_TYPE_UPDATE);
    return WriteDispatchResult::Handled;
  }

  if (auto *delete_query = ast->as<DB::ASTDeleteQuery>()) {
    const std::string origin_db = delete_query->getDatabase();
    const std::string origin_table = delete_query->getTable();
    if (!rewriteOneTarget(response, sel, origin_db, origin_table, "DELETE",
        [&](const std::string &s){ delete_query->setDatabase(s); },
        [&](const std::string &s){ delete_query->setTable(s); })) {
      return WriteDispatchResult::Rejected;
    }
    acceptWrite(ast, response, rewriter::STATEMENT_TYPE_DELETE);
    return WriteDispatchResult::Handled;
  }

  if (auto *rename_query = ast->as<DB::ASTRenameQuery>()) {
    if (rename_query->database) {
      rejectUnsupported(response,
        "RENAME DATABASE is not supported; only RENAME TABLE is allowed");
      return WriteDispatchResult::Rejected;
    }
    if (rename_query->dictionary) {
      rejectUnsupported(response, "RENAME DICTIONARY is not supported");
      return WriteDispatchResult::Rejected;
    }
    const std::string_view kind = rename_query->exchange ? "EXCHANGE TABLES" : "RENAME TABLE";
    // ASTRenameQuery::getElements() returns `const Elements &`; we
    // uniquely own the AST here, so const_cast is safe.
    auto &elements = const_cast<DB::ASTRenameQuery::Elements &>(rename_query->getElements());
    for (auto &elem : elements) {
      if (!rewriteRenameSide(response, sel, elem.from, kind)) {
        return WriteDispatchResult::Rejected;
      }
      if (!rewriteRenameSide(response, sel, elem.to, kind)) {
        return WriteDispatchResult::Rejected;
      }
    }
    acceptWrite(ast, response, rewriter::STATEMENT_TYPE_RENAME_TABLE);
    return WriteDispatchResult::Handled;
  }

  // Bare-reject write-ish kinds that would otherwise fall through to SELECT.
  switch (ast->getQueryKind()) {
    case DB::IAST::QueryKind::Optimize:
      rejectUnsupported(response, "OPTIMIZE is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::Undrop:
      rejectUnsupported(response, "UNDROP is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::Grant:
    case DB::IAST::QueryKind::Revoke:
      rejectUnsupported(response, "GRANT/REVOKE is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::Move:
      rejectUnsupported(response, "MOVE is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::Backup:
    case DB::IAST::QueryKind::Restore:
      rejectUnsupported(response, "BACKUP/RESTORE is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::KillQuery:
      rejectUnsupported(response, "KILL is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::ExternalDDL:
      rejectUnsupported(response, "external DDL is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::AsyncInsertFlush:
      rejectUnsupported(response, "async insert flush is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::Copy:
      rejectUnsupported(response, "COPY is not supported");
      return WriteDispatchResult::Rejected;
    default:
      break;  // Not a write - caller falls through to SELECT.
  }

  return WriteDispatchResult::NotAWrite;
}

} // namespace rewriter_handlers
