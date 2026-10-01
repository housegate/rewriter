// src/handlers/exists.cc
#include "handlers/exists.h"

#include <Parsers/TablePropertiesQueriesASTs.h>

#include <iostream>

#include "handlers/common.h"
#include "handlers/name_rewrite.h"
#include "handlers/storage_integrity.h"
#include "handlers/table_reference.h"

namespace rewriter_handlers {

namespace {

void rejectExists(rewriter::RewriteSQLResponse *response,
                  rewriter::RewriteCode code,
                  const std::string &msg) {
  std::cerr << "[exists-reject] " << msg << std::endl;
  response->set_message(msg);
  response->set_code(code);
}

} // namespace

ExistsDispatchResult handleExistsQuery(DB::ASTPtr ast,
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
    rejectExists(response, rewriter::RewriteCode::UnsupportedStatement,
      storageIntegrityPhysicalRejectMessage(effective + "." + table));
    return true;
  };

  if (auto *database = ast->as<DB::ASTExistsDatabaseQuery>()) {
    if (sel.mode == TableRewriteMode::Dynamic
        && isStorageIntegrityPhysicalDatabase(
          database->getDatabase(), *sel.dynamic_args)) {
      recordPhysicalStorageIntegrityAccess(
        response, database->getDatabase(), "", database->getDatabase());
      rejectExists(response, rewriter::RewriteCode::UnsupportedStatement,
        storageIntegrityPhysicalDatabaseRejectMessage(database->getDatabase()));
      return ExistsDispatchResult::Handled;
    }
    rejectExists(response, rewriter::RewriteCode::UnsupportedStatement,
                 "EXISTS DATABASE is not supported; only EXISTS TABLE is allowed");
    return ExistsDispatchResult::Handled;
  }
  if (auto *view = ast->as<DB::ASTExistsViewQuery>()) {
    if (reject_non_table_physical(view->getDatabase(), view->getTable()))
      return ExistsDispatchResult::Handled;
    rejectExists(response, rewriter::RewriteCode::UnsupportedStatement,
                 "EXISTS VIEW is not supported; only EXISTS TABLE is allowed");
    return ExistsDispatchResult::Handled;
  }
  if (auto *dictionary = ast->as<DB::ASTExistsDictionaryQuery>()) {
    if (reject_non_table_physical(
          dictionary->getDatabase(), dictionary->getTable()))
      return ExistsDispatchResult::Handled;
    rejectExists(response, rewriter::RewriteCode::UnsupportedStatement,
                 "EXISTS DICTIONARY is not supported; only EXISTS TABLE is allowed");
    return ExistsDispatchResult::Handled;
  }

  auto *q = ast->as<DB::ASTExistsTableQuery>();
  if (!q) return ExistsDispatchResult::NotExists;

  const std::string origin_db = q->getDatabase();
  const std::string origin_table = q->getTable();
  // Spec 2026-09-26 T2 / T12: an Identifier parameter target has an empty
  // name. setTable("") would erase it and the formatter would dereference
  // the missing table (the v0.15.0 segfault). Refuse it in every mode.
  if (origin_table.empty() || isIdentifierParameter(q->table.get())
      || isIdentifierParameter(q->database.get())) {
    rejectExists(response, rewriter::RewriteCode::InvalidRewriteRequest,
      std::string(kIdentifierParameterMessage));
    return ExistsDispatchResult::Handled;
  }

  // Record the access regardless of mode/outcome so callers see what target
  // the rewriter processed even on static-mode passthrough or eventual reject.
  recordAccessedTable(response, origin_db, origin_table, sel);
  switch (sel.mode) {
  case TableRewriteMode::None:
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_EXISTS_TABLE);
    return ExistsDispatchResult::Handled;
  case TableRewriteMode::Dynamic: {
    const auto &args = *sel.dynamic_args;
    const std::string effective = origin_db.empty()
      ? storageIntegrityExecutionDatabase(args) : origin_db;
    if (isStorageIntegrityPhysicalDatabase(effective, args)) {
      rejectExists(response, rewriter::RewriteCode::UnsupportedStatement,
        storageIntegrityPhysicalRejectMessage(effective + "." + origin_table));
      return ExistsDispatchResult::Handled;
    }
    if (auto hit = lookupStorageIntegrity(origin_db, origin_table, args)) {
      std::string logical;
      if (!authorizeStorageIntegrityLogical(origin_db, args, &logical)) {
        rejectExists(response, rewriter::RewriteCode::InvalidRewriteRequest,
          storageIntegrityUnauthorizedMessage(logical));
        return ExistsDispatchResult::Handled;
      }
      const auto dot = hit->table->safe_table().find('.');
      const std::string safe_db = dot == std::string::npos
        ? std::string() : hit->table->safe_table().substr(0, dot);
      const std::string safe_table = dot == std::string::npos
        ? hit->table->safe_table() : hit->table->safe_table().substr(dot + 1);
      q->setDatabase(safe_db);
      q->setTable(safe_table);
      recordTableRewrite(response, origin_db, origin_table,
                         safe_db, safe_table);
      break;
    }
    const auto outcome = applyDynamicRewrite(
      origin_db, origin_table, *sel.dynamic_args, "EXISTS TABLE");
    if (outcome.status == DynamicRewriteStatus::InvalidRequest) {
      rejectExists(response, rewriter::RewriteCode::InvalidRewriteRequest,
                   outcome.reject_reason);
      return ExistsDispatchResult::Handled;
    }
    if (outcome.status == DynamicRewriteStatus::OkRemote) {
      rejectExists(response, rewriter::RewriteCode::UnsupportedStatement,
        "EXISTS TABLE target's logical resolves to a remote upstream "
        "(via dynamic_args.logical_database_to_remote_upstream_index); "
        "remote() can only appear as a SELECT-side table function");
      return ExistsDispatchResult::Handled;
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
      // Static-mode passthrough: target wasn't in any map. Emit unchanged.
      break;
    case StaticLookupOutcome::Status::RemoteUnsupported:
      rejectExists(response, rewriter::RewriteCode::UnsupportedStatement,
        "EXISTS TABLE target maps to remote_table_map; remote() can only "
        "appear as a SELECT-side table function");
      return ExistsDispatchResult::Handled;
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

  setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_EXISTS_TABLE);
  return ExistsDispatchResult::Handled;
}

} // namespace rewriter_handlers
