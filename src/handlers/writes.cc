// src/handlers/writes.cc
#include "handlers/writes.h"

#include <Parsers/ASTAlterQuery.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTDeleteQuery.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTRenameQuery.h>
#include <Parsers/ASTUpdateQuery.h>

#include <iostream>
#include <string>

#include "handlers/common.h"
#include "handlers/name_rewrite.h"

namespace rewriter_handlers {

namespace {

std::string qualifyTable(const std::string &db, const std::string &table) {
  return db.empty() ? table : db + "." + table;
}

void acceptWrite(const DB::ASTPtr &ast,
                 rewriter::RewriteSQLResponse *response,
                 rewriter::StatementType stmt_type,
                 const std::string &target) {
  setSuccessResponse(response, formatAst(ast), stmt_type);
  if (!target.empty()) {
    response->mutable_write_stmt_value()->set_target_table(target);
  }
}

void rejectWrite(rewriter::RewriteSQLResponse *response,
                 rewriter::RewriteCode code,
                 const std::string &msg) {
  // Stderr so operators can see what kinds of statements callers are
  // attempting (CREATE VIEW, DROP DATABASE, multi-table DROP, remote-hit
  // on writes, ...) without having to correlate every error response back
  // to a client.
  std::cerr << "[write-reject] " << msg << std::endl;
  response->set_message(msg);
  response->set_code(code);
}

// Statement kind / variant is outside the rewriter's scope. Callers may
// choose to pass the SQL through to ClickHouse unchanged.
inline void rejectUnsupported(rewriter::RewriteSQLResponse *response, const std::string &msg) {
  rejectWrite(response, rewriter::RewriteCode::UnsupportedStatement, msg);
}

// Caller's RewriteOption can't be applied to this statement — their rewrite
// config is incorrect for the SQL. Callers MUST NOT pass through.
inline void rejectInvalidRequest(rewriter::RewriteSQLResponse *response, const std::string &msg) {
  rejectWrite(response, rewriter::RewriteCode::InvalidRewriteRequest, msg);
}

// Apply TableNameRewrite to (*cur_db, *cur_table) using `request->options()`.
// Returns false if the rewrite resolved to Remote — in that case
// `rejectWrite` has already been called and the caller must bail out.
template <typename SetDb, typename SetTable>
bool rewriteOneTarget(
  const rewriter::RewriteSQLRequest *request,
  rewriter::RewriteSQLResponse *response,
  const std::string &origin_db,
  const std::string &origin_table,
  std::string_view stmt_kind,
  SetDb set_db,
  SetTable set_table,
  std::string *out_new_db,
  std::string *out_new_table) {
  std::string new_db;
  std::string new_table;
  const auto outcome = applySingleTableRewrite(
    origin_db, origin_table, request->options(), &new_db, &new_table, stmt_kind);

  if (outcome.status == SingleTableRewriteStatus::RejectRemote) {
    rejectInvalidRequest(response, outcome.reject_reason);
    return false;
  }

  if (outcome.status == SingleTableRewriteStatus::Rewritten) {
    if (!new_db.empty()) set_db(new_db);
    set_table(new_table);
    recordTableRewrite(response, origin_db, origin_table, new_db, new_table);
  }
  *out_new_db = new_db;
  *out_new_table = new_table;
  return true;
}

// ALTER commands that reference a second table (ATTACH/REPLACE/FETCH PARTITION
// FROM, MOVE PARTITION TO TABLE) are not rewritten in v1. Reject if any such
// command appears so callers can't silently ship a half-rewritten statement.
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

// Rewrite one side of a RENAME element (each `from` or `to`). Returns false
// on Remote-reject. On success, mutates the Table's `database`/`table`
// ASTIdentifier children in-place and records any rename into response.
bool rewriteRenameSide(
  const rewriter::RewriteSQLRequest *request,
  rewriter::RewriteSQLResponse *response,
  DB::ASTRenameQuery::Table &side,
  std::string_view stmt_kind) {
  const std::string origin_db = side.getDatabase();
  const std::string origin_table = side.getTable();
  std::string new_db;
  std::string new_table;
  const auto outcome = applySingleTableRewrite(
    origin_db, origin_table, request->options(), &new_db, &new_table, stmt_kind);

  if (outcome.status == SingleTableRewriteStatus::RejectRemote) {
    rejectInvalidRequest(response, outcome.reject_reason);
    return false;
  }

  if (outcome.status == SingleTableRewriteStatus::Rewritten) {
    if (!new_db.empty()) {
      side.database = std::make_shared<DB::ASTIdentifier>(new_db);
    }
    side.table = std::make_shared<DB::ASTIdentifier>(new_table);
    recordTableRewrite(response, origin_db, origin_table, new_db, new_table);
  }
  return true;
}

} // namespace

WriteDispatchResult handleWriteQuery(DB::ASTPtr ast,
                                     const rewriter::RewriteSQLRequest *request,
                                     rewriter::RewriteSQLResponse *response) {
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
      rejectUnsupported(response, "CREATE DATABASE is not supported; only CREATE TABLE is allowed");
      return WriteDispatchResult::Rejected;
    }
    if (create_query->as_table_function) {
      rejectUnsupported(response, "CREATE TABLE AS table_function(...) is not supported");
      return WriteDispatchResult::Rejected;
    }

    const std::string origin_db = create_query->getDatabase();
    const std::string origin_table = create_query->getTable();
    std::string new_db;
    std::string new_table;
    if (!rewriteOneTarget(request, response, origin_db, origin_table, "CREATE TABLE",
        [&](const std::string &s){ create_query->setDatabase(s); },
        [&](const std::string &s){ create_query->setTable(s); },
        &new_db, &new_table)) {
      return WriteDispatchResult::Rejected;
    }

    // CREATE TABLE x AS y — rewrite `y` with the same map. `as_database` and
    // `as_table` are plain Strings (no AST node to mutate), so we just
    // reassign them.
    if (!create_query->as_table.empty()) {
      const std::string as_origin_db = create_query->as_database;
      const std::string as_origin_table = create_query->as_table;
      std::string as_new_db = as_origin_db;
      std::string as_new_table = as_origin_table;
      const auto outcome = applySingleTableRewrite(
        as_origin_db, as_origin_table, request->options(),
        &as_new_db, &as_new_table, "CREATE TABLE AS source");
      if (outcome.status == SingleTableRewriteStatus::RejectRemote) {
        rejectInvalidRequest(response, outcome.reject_reason);
        return WriteDispatchResult::Rejected;
      }
      if (outcome.status == SingleTableRewriteStatus::Rewritten) {
        create_query->as_database = as_new_db;
        create_query->as_table = as_new_table;
        recordTableRewrite(response, as_origin_db, as_origin_table, as_new_db, as_new_table);
      }
    }

    const std::string final_db = new_db.empty() ? origin_db : new_db;
    const std::string final_table = new_table.empty() ? origin_table : new_table;
    acceptWrite(ast, response, rewriter::STATEMENT_TYPE_CREATE_TABLE,
                qualifyTable(final_db, final_table));
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
    std::string new_db;
    std::string new_table;
    if (!rewriteOneTarget(request, response, origin_db, origin_table, "DROP TABLE",
        [&](const std::string &s){ drop_query->setDatabase(s); },
        [&](const std::string &s){ drop_query->setTable(s); },
        &new_db, &new_table)) {
      return WriteDispatchResult::Rejected;
    }
    const std::string final_db = new_db.empty() ? origin_db : new_db;
    const std::string final_table = new_table.empty() ? origin_table : new_table;
    acceptWrite(ast, response, rewriter::STATEMENT_TYPE_DROP_TABLE,
                qualifyTable(final_db, final_table));
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
    std::string new_db;
    std::string new_table;
    if (!rewriteOneTarget(request, response, origin_db, origin_table, "ALTER TABLE",
        [&](const std::string &s){ alter_query->setDatabase(s); },
        [&](const std::string &s){ alter_query->setTable(s); },
        &new_db, &new_table)) {
      return WriteDispatchResult::Rejected;
    }
    const std::string final_db = new_db.empty() ? origin_db : new_db;
    const std::string final_table = new_table.empty() ? origin_table : new_table;
    acceptWrite(ast, response, rewriter::STATEMENT_TYPE_ALTER_TABLE,
                qualifyTable(final_db, final_table));
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
    std::string new_db;
    std::string new_table;
    if (!rewriteOneTarget(request, response, origin_db, origin_table, "INSERT",
        [&](const std::string &s){ insert_query->setDatabase(s); },
        [&](const std::string &s){ insert_query->setTable(s); },
        &new_db, &new_table)) {
      return WriteDispatchResult::Rejected;
    }
    const std::string final_db = new_db.empty() ? origin_db : new_db;
    const std::string final_table = new_table.empty() ? origin_table : new_table;
    acceptWrite(ast, response, rewriter::STATEMENT_TYPE_INSERT,
                qualifyTable(final_db, final_table));
    return WriteDispatchResult::Handled;
  }

  if (auto *update_query = ast->as<DB::ASTUpdateQuery>()) {
    const std::string origin_db = update_query->getDatabase();
    const std::string origin_table = update_query->getTable();
    std::string new_db;
    std::string new_table;
    if (!rewriteOneTarget(request, response, origin_db, origin_table, "UPDATE",
        [&](const std::string &s){ update_query->setDatabase(s); },
        [&](const std::string &s){ update_query->setTable(s); },
        &new_db, &new_table)) {
      return WriteDispatchResult::Rejected;
    }
    const std::string final_db = new_db.empty() ? origin_db : new_db;
    const std::string final_table = new_table.empty() ? origin_table : new_table;
    acceptWrite(ast, response, rewriter::STATEMENT_TYPE_UPDATE,
                qualifyTable(final_db, final_table));
    return WriteDispatchResult::Handled;
  }

  if (auto *delete_query = ast->as<DB::ASTDeleteQuery>()) {
    const std::string origin_db = delete_query->getDatabase();
    const std::string origin_table = delete_query->getTable();
    std::string new_db;
    std::string new_table;
    if (!rewriteOneTarget(request, response, origin_db, origin_table, "DELETE",
        [&](const std::string &s){ delete_query->setDatabase(s); },
        [&](const std::string &s){ delete_query->setTable(s); },
        &new_db, &new_table)) {
      return WriteDispatchResult::Rejected;
    }
    const std::string final_db = new_db.empty() ? origin_db : new_db;
    const std::string final_table = new_table.empty() ? origin_table : new_table;
    acceptWrite(ast, response, rewriter::STATEMENT_TYPE_DELETE,
                qualifyTable(final_db, final_table));
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
    // ASTRenameQuery exposes elements as `const Elements &` — we need to
    // mutate, so cast away const (the AST is uniquely owned at this point).
    auto &elements = const_cast<DB::ASTRenameQuery::Elements &>(rename_query->getElements());
    for (auto &elem : elements) {
      if (!rewriteRenameSide(request, response, elem.from, kind)) {
        return WriteDispatchResult::Rejected;
      }
      if (!rewriteRenameSide(request, response, elem.to, kind)) {
        return WriteDispatchResult::Rejected;
      }
    }
    // No single target_table for RENAME — callers must consult table_rewrites.
    acceptWrite(ast, response, rewriter::STATEMENT_TYPE_RENAME_TABLE, /*target=*/"");
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
