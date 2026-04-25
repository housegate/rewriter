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

// Run applyDynamicRewrite on (origin_db, origin_table) using `general` (may
// be null, meaning "no rewrite requested"). On OK: updates (*cur_db,
// *cur_table) via setDb/setTable callbacks and records the rewrite on
// response.table_rewrites. Returns false on reject — response was populated
// and the caller must return Rejected.
template <typename SetDb, typename SetTable>
bool rewriteOneTarget(
  rewriter::RewriteSQLResponse *response,
  const rewriter::RewriteTableForDynamicArgs *general,
  const std::string &origin_db,
  const std::string &origin_table,
  std::string_view stmt_kind,
  SetDb set_db,
  SetTable set_table) {
  if (!general) return true;  // No rewrite requested; caller echoes the AST.

  const auto outcome = applyDynamicRewrite(origin_db, origin_table, *general, stmt_kind);
  if (outcome.status == DynamicRewriteStatus::InvalidRequest) {
    rejectInvalidRequest(response, outcome.reject_reason);
    return false;
  }
  set_db(outcome.physical_db);
  set_table(outcome.new_table);
  recordTableRewrite(response, origin_db, origin_table,
                     outcome.physical_db, outcome.new_table);
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
// already populated). Mutates side.database / side.table in place to the
// resolved physical name + dynamically-built table name.
bool rewriteRenameSide(
  rewriter::RewriteSQLResponse *response,
  const rewriter::RewriteTableForDynamicArgs *general,
  DB::ASTRenameQuery::Table &side,
  std::string_view stmt_kind) {
  if (!general) return true;  // passthrough
  const std::string origin_db = side.getDatabase();
  const std::string origin_table = side.getTable();

  const auto outcome = applyDynamicRewrite(origin_db, origin_table, *general, stmt_kind);
  if (outcome.status == DynamicRewriteStatus::InvalidRequest) {
    rejectInvalidRequest(response, outcome.reject_reason);
    return false;
  }
  side.database = std::make_shared<DB::ASTIdentifier>(outcome.physical_db);
  side.table    = std::make_shared<DB::ASTIdentifier>(outcome.new_table);
  recordTableRewrite(response, origin_db, origin_table,
                     outcome.physical_db, outcome.new_table);
  return true;
}

} // namespace

WriteDispatchResult handleWriteQuery(DB::ASTPtr ast,
                                     const rewriter::RewriteSQLRequest *request,
                                     rewriter::RewriteSQLResponse *response) {
  const auto *general = findGeneralArgs(request->options());

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
      // Without a TableNameRewrite/general_args option in the request we
      // can't validate anything and don't have the policy context we'd
      // need to construct the debug rewrite — fall back to the previous
      // behavior of UnsupportedStatement so callers can't accidentally
      // ship the DDL to the physical DB.
      if (!general) {
        rejectUnsupported(response,
          "CREATE DATABASE requires a TableNameRewrite/general_args option to validate against");
        return WriteDispatchResult::Rejected;
      }

      const std::string target_db = create_query->getDatabase();

      // Check 1: target logical DB already mapped in database_map → reject
      // (mirrors "database already exists" for the proxy's view of the
      // world). IF NOT EXISTS lets us silently fall through to the debug
      // rewrite, since CH semantics make that case a no-op.
      const auto db_map_it = general->database_map().find(target_db);
      if (db_map_it != general->database_map().end() && !create_query->if_not_exists) {
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
      if (general->has_upstream_physical_database_in_context()) {
        const std::string &phys = general->upstream_physical_database_in_context();
        bool known = false;
        for (const auto &p : general->known_physical_databases()) {
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
    if (!rewriteOneTarget(response, general, origin_db, origin_table, "CREATE TABLE",
        [&](const std::string &s){ create_query->setDatabase(s); },
        [&](const std::string &s){ create_query->setTable(s); })) {
      return WriteDispatchResult::Rejected;
    }

    // CREATE TABLE x AS y — rewrite the AS source the same way. as_database /
    // as_table are plain Strings; assign directly.
    if (general && !create_query->as_table.empty()) {
      const std::string as_origin_db = create_query->as_database;
      const std::string as_origin_table = create_query->as_table;
      const auto outcome = applyDynamicRewrite(
        as_origin_db, as_origin_table, *general, "CREATE TABLE AS source");
      if (outcome.status == DynamicRewriteStatus::InvalidRequest) {
        rejectInvalidRequest(response, outcome.reject_reason);
        return WriteDispatchResult::Rejected;
      }
      create_query->as_database = outcome.physical_db;
      create_query->as_table    = outcome.new_table;
      recordTableRewrite(response, as_origin_db, as_origin_table,
                         outcome.physical_db, outcome.new_table);
    }

    acceptWrite(ast, response, rewriter::STATEMENT_TYPE_CREATE_TABLE);
    return WriteDispatchResult::Handled;
  }

  if (auto *drop_query = ast->as<DB::ASTDropQuery>()) {
    if (drop_query->kind == DB::ASTDropQuery::Kind::Truncate) {
      rejectUnsupported(response, "TRUNCATE is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (drop_query->kind == DB::ASTDropQuery::Kind::Detach) {
      rejectUnsupported(response, "DETACH is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (drop_query->is_view || drop_query->is_dictionary) {
      rejectUnsupported(response, "DROP VIEW/DICTIONARY is not supported; only DROP TABLE is allowed");
      return WriteDispatchResult::Rejected;
    }
    if (drop_query->database_and_tables) {
      rejectUnsupported(response, "multi-table DROP is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (!drop_query->table) {
      rejectUnsupported(response, "DROP DATABASE is not supported; only DROP TABLE is allowed");
      return WriteDispatchResult::Rejected;
    }

    const std::string origin_db = drop_query->getDatabase();
    const std::string origin_table = drop_query->getTable();
    if (!rewriteOneTarget(response, general, origin_db, origin_table, "DROP TABLE",
        [&](const std::string &s){ drop_query->setDatabase(s); },
        [&](const std::string &s){ drop_query->setTable(s); })) {
      return WriteDispatchResult::Rejected;
    }
    acceptWrite(ast, response, rewriter::STATEMENT_TYPE_DROP_TABLE);
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
    if (!rewriteOneTarget(response, general, origin_db, origin_table, "ALTER TABLE",
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
    if (!rewriteOneTarget(response, general, origin_db, origin_table, "INSERT",
        [&](const std::string &s){ insert_query->setDatabase(s); },
        [&](const std::string &s){ insert_query->setTable(s); })) {
      return WriteDispatchResult::Rejected;
    }
    acceptWrite(ast, response, rewriter::STATEMENT_TYPE_INSERT);
    return WriteDispatchResult::Handled;
  }

  if (auto *update_query = ast->as<DB::ASTUpdateQuery>()) {
    const std::string origin_db = update_query->getDatabase();
    const std::string origin_table = update_query->getTable();
    if (!rewriteOneTarget(response, general, origin_db, origin_table, "UPDATE",
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
    if (!rewriteOneTarget(response, general, origin_db, origin_table, "DELETE",
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
      if (!rewriteRenameSide(response, general, elem.from, kind)) {
        return WriteDispatchResult::Rejected;
      }
      if (!rewriteRenameSide(response, general, elem.to, kind)) {
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
