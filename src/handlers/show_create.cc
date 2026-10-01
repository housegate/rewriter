// src/handlers/show_create.cc
#include "handlers/show_create.h"

#include <Parsers/TablePropertiesQueriesASTs.h>

#include <iostream>

#include "handlers/common.h"
#include "handlers/name_rewrite.h"
#include "handlers/storage_integrity.h"
#include "handlers/table_reference.h"

namespace rewriter_handlers {

namespace {

void rejectShowCreate(rewriter::RewriteSQLResponse *response,
                      rewriter::RewriteCode code,
                      const std::string &msg) {
  std::cerr << "[show-create-reject] " << msg << std::endl;
  response->set_message(msg);
  response->set_code(code);
}

} // namespace

ShowCreateDispatchResult handleShowCreateQuery(DB::ASTPtr ast,
                                               const rewriter::RewriteSQLRequest *request,
                                               rewriter::RewriteSQLResponse *response) {
  const auto sel = findActiveTableRewrite(request->options());
  auto reject_non_table_physical = [&](const std::string &database,
                                       const std::string &table) {
    if (sel.mode != TableRewriteMode::Dynamic) return false;
    const auto &args = *sel.dynamic_args;
    const std::string effective = database.empty()
      ? storageIntegrityExecutionDatabase(args) : database;
    if (!isStorageIntegrityPhysicalDatabase(effective, args)) return false;
    recordAccessedTable(response, database, table, sel);
    rejectShowCreate(response, rewriter::RewriteCode::UnsupportedStatement,
      storageIntegrityPhysicalRejectMessage(effective + "." + table));
    return true;
  };
  if (auto *database = ast->as<DB::ASTShowCreateDatabaseQuery>()) {
    if (sel.mode == TableRewriteMode::Dynamic
        && isStorageIntegrityPhysicalDatabase(
          database->getDatabase(), *sel.dynamic_args)) {
      recordPhysicalStorageIntegrityAccess(
        response, database->getDatabase(), "", database->getDatabase());
      rejectShowCreate(response, rewriter::RewriteCode::UnsupportedStatement,
        storageIntegrityPhysicalDatabaseRejectMessage(database->getDatabase()));
      return ShowCreateDispatchResult::Handled;
    }
    rejectShowCreate(response, rewriter::RewriteCode::UnsupportedStatement,
      "SHOW CREATE DATABASE is not supported; only SHOW CREATE TABLE is allowed");
    return ShowCreateDispatchResult::Handled;
  }
  if (auto *view = ast->as<DB::ASTShowCreateViewQuery>()) {
    if (reject_non_table_physical(view->getDatabase(), view->getTable()))
      return ShowCreateDispatchResult::Handled;
    rejectShowCreate(response, rewriter::RewriteCode::UnsupportedStatement,
      "SHOW CREATE VIEW is not supported; only SHOW CREATE TABLE is allowed");
    return ShowCreateDispatchResult::Handled;
  }
  if (auto *dictionary = ast->as<DB::ASTShowCreateDictionaryQuery>()) {
    if (reject_non_table_physical(
          dictionary->getDatabase(), dictionary->getTable()))
      return ShowCreateDispatchResult::Handled;
    rejectShowCreate(response, rewriter::RewriteCode::UnsupportedStatement,
      "SHOW CREATE DICTIONARY is not supported; only SHOW CREATE TABLE is allowed");
    return ShowCreateDispatchResult::Handled;
  }

  auto *q = ast->as<DB::ASTShowCreateTableQuery>();
  if (!q) return ShowCreateDispatchResult::NotShowCreate;

  const std::string origin_db = q->getDatabase();
  const std::string origin_table = q->getTable();
  // Spec 2026-09-26 T2 / T12: an Identifier parameter target has an empty
  // name. setTable("") would erase it and the formatter would dereference
  // the missing table (the v0.15.0 segfault). Refuse it in every mode.
  if (origin_table.empty() || isIdentifierParameter(q->table.get())
      || isIdentifierParameter(q->database.get())) {
    rejectShowCreate(response, rewriter::RewriteCode::InvalidRewriteRequest,
      std::string(kIdentifierParameterMessage));
    return ShowCreateDispatchResult::Handled;
  }

  // Record the access regardless of mode/outcome so callers see what target
  // the rewriter processed even on static-mode passthrough or eventual reject.
  recordAccessedTable(response, origin_db, origin_table, sel);
  switch (sel.mode) {
  case TableRewriteMode::None:
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_SHOW_CREATE_TABLE);
    return ShowCreateDispatchResult::Handled;
  case TableRewriteMode::Dynamic: {
    const auto &args = *sel.dynamic_args;
    const std::string effective = origin_db.empty()
      ? storageIntegrityExecutionDatabase(args) : origin_db;
    if (isStorageIntegrityPhysicalDatabase(effective, args)) {
      rejectShowCreate(response, rewriter::RewriteCode::UnsupportedStatement,
        storageIntegrityPhysicalRejectMessage(effective + "." + origin_table));
      return ShowCreateDispatchResult::Handled;
    }
    if (auto hit = lookupStorageIntegrity(origin_db, origin_table, args)) {
      std::string logical;
      if (!authorizeStorageIntegrityLogical(origin_db, args, &logical)) {
        rejectShowCreate(response,
          rewriter::RewriteCode::InvalidRewriteRequest,
          storageIntegrityUnauthorizedMessage(logical));
      } else {
        rejectShowCreate(response, rewriter::RewriteCode::UnsupportedStatement,
          "SHOW CREATE TABLE on storage-integrity table "
            + hit->logical_key + " is not supported");
      }
      return ShowCreateDispatchResult::Handled;
    }
    const auto outcome = applyDynamicRewrite(
      origin_db, origin_table, *sel.dynamic_args, "SHOW CREATE TABLE");
    if (outcome.status == DynamicRewriteStatus::InvalidRequest) {
      rejectShowCreate(response, rewriter::RewriteCode::InvalidRewriteRequest,
                       outcome.reject_reason);
      return ShowCreateDispatchResult::Handled;
    }
    if (outcome.status == DynamicRewriteStatus::OkRemote) {
      rejectShowCreate(response, rewriter::RewriteCode::UnsupportedStatement,
        "SHOW CREATE TABLE target's logical resolves to a remote upstream "
        "(via dynamic_args.logical_database_to_remote_upstream_index); "
        "remote() can only appear as a SELECT-side table function");
      return ShowCreateDispatchResult::Handled;
    }
    q->setDatabase(outcome.physical_db);
    q->setTable(outcome.new_table);
    recordTableRewrite(response, origin_db, origin_table,
                       outcome.physical_db, outcome.new_table);
    break;
  }
  case TableRewriteMode::Static: {
    const auto lookup = lookupStaticTableRewrite(
      origin_db, origin_table, *sel.static_args);
    switch (lookup.status) {
    case StaticLookupOutcome::Status::NotMatched:
      break;  // Static-mode passthrough.
    case StaticLookupOutcome::Status::RemoteUnsupported:
      rejectShowCreate(response, rewriter::RewriteCode::UnsupportedStatement,
        "SHOW CREATE TABLE target maps to remote_table_map; remote() can only "
        "appear as a SELECT-side table function");
      return ShowCreateDispatchResult::Handled;
    case StaticLookupOutcome::Status::Matched: {
      const std::string effective_new_db =
        lookup.new_db.empty() ? origin_db : lookup.new_db;
      if (!lookup.new_db.empty()) q->setDatabase(lookup.new_db);
      q->setTable(lookup.new_table);
      recordTableRewrite(response, origin_db, origin_table,
                         effective_new_db, lookup.new_table);
      break;
    }
    }
    break;
  }
  }

  setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_SHOW_CREATE_TABLE);
  return ShowCreateDispatchResult::Handled;
}

} // namespace rewriter_handlers
