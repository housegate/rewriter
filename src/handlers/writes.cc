// src/handlers/writes.cc
#include "handlers/writes.h"

#include <Parsers/ASTAlterQuery.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTDeleteQuery.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTUpdateQuery.h>

#include <iostream>
#include <string>

#include "handlers/common.h"

namespace rewriter_handlers {

namespace {

std::string qualifyTable(const std::string &db, const std::string &table) {
  return db.empty() ? table : db + "." + table;
}

void acceptWrite(const DB::ASTPtr &ast,
                 rewriter::RewriteSQLResponse *response,
                 const std::string &stmt_type,
                 const std::string &target) {
  setSuccessResponse(response, formatAst(ast), stmt_type);
  response->mutable_write_stmt_value()->set_target_table(target);
}

void rejectWrite(rewriter::RewriteSQLResponse *response, const std::string &msg) {
  // Stderr so operators can see what kinds of unsupported statements callers
  // are attempting (CREATE VIEW, DROP DATABASE, multi-table DROP, ...) without
  // having to correlate every RewriteError response back to a client.
  std::cerr << "[write-reject] " << msg << std::endl;
  response->set_message(msg);
  response->set_code(rewriter::RewriteCode::RewriteError);
}

} // namespace

WriteDispatchResult handleWriteQuery(DB::ASTPtr ast,
                                     const rewriter::RewriteSQLRequest * /*request*/,
                                     rewriter::RewriteSQLResponse *response) {
  if (auto *create_query = ast->as<DB::ASTCreateQuery>()) {
    if (create_query->is_ordinary_view || create_query->is_materialized_view
        || create_query->is_live_view || create_query->is_window_view) {
      rejectWrite(response, "CREATE VIEW is not supported; only CREATE TABLE is allowed");
      return WriteDispatchResult::Rejected;
    }
    if (create_query->is_dictionary) {
      rejectWrite(response, "CREATE DICTIONARY is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (!create_query->table) {
      rejectWrite(response, "CREATE DATABASE is not supported; only CREATE TABLE is allowed");
      return WriteDispatchResult::Rejected;
    }
    if (create_query->as_table_function) {
      rejectWrite(response, "CREATE TABLE AS table_function(...) is not supported");
      return WriteDispatchResult::Rejected;
    }
    acceptWrite(ast, response, "CREATE_TABLE",
                qualifyTable(create_query->getDatabase(), create_query->getTable()));
    return WriteDispatchResult::Handled;
  }

  if (auto *drop_query = ast->as<DB::ASTDropQuery>()) {
    if (drop_query->kind == DB::ASTDropQuery::Kind::Truncate) {
      rejectWrite(response, "TRUNCATE is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (drop_query->kind == DB::ASTDropQuery::Kind::Detach) {
      rejectWrite(response, "DETACH is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (drop_query->is_view || drop_query->is_dictionary) {
      rejectWrite(response, "DROP VIEW/DICTIONARY is not supported; only DROP TABLE is allowed");
      return WriteDispatchResult::Rejected;
    }
    if (drop_query->database_and_tables) {
      rejectWrite(response, "multi-table DROP is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (!drop_query->table) {
      rejectWrite(response, "DROP DATABASE is not supported; only DROP TABLE is allowed");
      return WriteDispatchResult::Rejected;
    }
    acceptWrite(ast, response, "DROP_TABLE",
                qualifyTable(drop_query->getDatabase(), drop_query->getTable()));
    return WriteDispatchResult::Handled;
  }

  if (auto *alter_query = ast->as<DB::ASTAlterQuery>()) {
    if (alter_query->alter_object != DB::ASTAlterQuery::AlterObjectType::TABLE) {
      rejectWrite(response, "only ALTER TABLE is supported");
      return WriteDispatchResult::Rejected;
    }
    acceptWrite(ast, response, "ALTER_TABLE",
                qualifyTable(alter_query->getDatabase(), alter_query->getTable()));
    return WriteDispatchResult::Handled;
  }

  if (auto *insert_query = ast->as<DB::ASTInsertQuery>()) {
    if (insert_query->table_function) {
      rejectWrite(response, "INSERT INTO FUNCTION(...) is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (!insert_query->table) {
      rejectWrite(response, "INSERT target table is missing");
      return WriteDispatchResult::Rejected;
    }
    acceptWrite(ast, response, "INSERT",
                qualifyTable(insert_query->getDatabase(), insert_query->getTable()));
    return WriteDispatchResult::Handled;
  }

  if (auto *update_query = ast->as<DB::ASTUpdateQuery>()) {
    acceptWrite(ast, response, "UPDATE",
                qualifyTable(update_query->getDatabase(), update_query->getTable()));
    return WriteDispatchResult::Handled;
  }

  if (auto *delete_query = ast->as<DB::ASTDeleteQuery>()) {
    acceptWrite(ast, response, "DELETE",
                qualifyTable(delete_query->getDatabase(), delete_query->getTable()));
    return WriteDispatchResult::Handled;
  }

  // Bare-reject write-ish kinds that would otherwise fall through to SELECT.
  switch (ast->getQueryKind()) {
    case DB::IAST::QueryKind::Rename:
      rejectWrite(response, "RENAME is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::Optimize:
      rejectWrite(response, "OPTIMIZE is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::Undrop:
      rejectWrite(response, "UNDROP is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::Grant:
    case DB::IAST::QueryKind::Revoke:
      rejectWrite(response, "GRANT/REVOKE is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::Move:
      rejectWrite(response, "MOVE is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::Backup:
    case DB::IAST::QueryKind::Restore:
      rejectWrite(response, "BACKUP/RESTORE is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::KillQuery:
      rejectWrite(response, "KILL is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::ExternalDDL:
      rejectWrite(response, "external DDL is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::AsyncInsertFlush:
      rejectWrite(response, "async insert flush is not supported");
      return WriteDispatchResult::Rejected;
    case DB::IAST::QueryKind::Copy:
      rejectWrite(response, "COPY is not supported");
      return WriteDispatchResult::Rejected;
    default:
      break;  // Not a write - caller falls through to SELECT.
  }

  return WriteDispatchResult::NotAWrite;
}

} // namespace rewriter_handlers
