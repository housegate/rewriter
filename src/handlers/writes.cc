// src/handlers/writes.cc
#include "handlers/writes.h"

#include <Common/Exception.h>

#include <Parsers/ASTAlterQuery.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTDeleteQuery.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTOptimizeQuery.h>
#include <Parsers/ASTRenameQuery.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTUpdateQuery.h>
#include <Parsers/ASTViewTargets.h>
#include <Parsers/Lexer.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>

#include <Interpreters/StorageID.h>

#include <cctype>
#include <iostream>
#include <string>
#include <string_view>

#include "ASTTransformers.h"
#include "handlers/common.h"
#include "handlers/name_rewrite.h"
#include "handlers/select.h"
#include "handlers/storage_integrity.h"
#include "handlers/table_reference.h"

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

bool tokenEqualsKeyword(const DB::Token &token, std::string_view keyword) {
  if (token.type != DB::TokenType::BareWord || token.size() != keyword.size())
    return false;
  for (size_t i = 0; i < keyword.size(); ++i) {
    const auto actual = static_cast<unsigned char>(token.begin[i]);
    if (std::toupper(actual) != keyword[i]) return false;
  }
  return true;
}

bool beginsNonLiveCreateObject(const DB::Token &token) {
  return tokenEqualsKeyword(token, "OR")
    || tokenEqualsKeyword(token, "TEMPORARY")
    || tokenEqualsKeyword(token, "TABLE")
    || tokenEqualsKeyword(token, "DATABASE")
    || tokenEqualsKeyword(token, "MATERIALIZED")
    || tokenEqualsKeyword(token, "VIEW")
    || tokenEqualsKeyword(token, "DICTIONARY")
    || tokenEqualsKeyword(token, "WINDOW")
    || tokenEqualsKeyword(token, "USER")
    || tokenEqualsKeyword(token, "ROLE")
    || tokenEqualsKeyword(token, "QUOTA")
    || tokenEqualsKeyword(token, "POLICY")
    || tokenEqualsKeyword(token, "ROW")
    || tokenEqualsKeyword(token, "MASKING")
    || tokenEqualsKeyword(token, "SETTINGS")
    || tokenEqualsKeyword(token, "PROFILE")
    || tokenEqualsKeyword(token, "FUNCTION")
    || tokenEqualsKeyword(token, "WORKLOAD")
    || tokenEqualsKeyword(token, "RESOURCE")
    || tokenEqualsKeyword(token, "NAMED")
    || tokenEqualsKeyword(token, "UNIQUE")
    || tokenEqualsKeyword(token, "INDEX");
}

// The formatted write. An INSERT … SELECT / CTAS body that reads an Active
// table carries the derived read, whose projection spelling is restored
// exactly as the SELECT path restores it.
std::string formatWrite(const DB::ASTPtr &ast, const rewriter::RewriteSQLResponse &response,
                        const TableRewriteSelection &sel) {
  std::string sql = formatAst(ast);
  if (sel.mode != TableRewriteMode::Dynamic) return sql;
  for (const auto &entry : response.original_accessed_tables()) {
    if (entry.is_storage_integrity())
      return restoreStorageIntegrityProjectionSyntax(std::move(sql), reservedRowIdColumn(*sel.dynamic_args));
  }
  return sql;
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

bool hasActiveStorageIntegrity(const TableRewriteSelection &sel) {
  return sel.mode == TableRewriteMode::Dynamic
    && storageIntegritySurfaceActive(*sel.dynamic_args);
}

// A query parameter (`{p:Identifier}`) binds a name only at execution time.
// ClickHouse parses it into an identifier whose name() is empty (or ends in
// an empty part), so a write target's getTable() is "" and the rewrite's
// setTable("") erases the target: `DROP TABLE db1.{p:Identifier}` would be
// formatted as `DROP DATABASE phys` (TRUNCATE as `TRUNCATE DATABASE`, CREATE
// TABLE as `CREATE DATABASE`). Every write target must be a literal name; the
// refusal carries the spec 2026-09-26 T2 text (kIdentifierParameterMessage).

bool isParameterOrEmptyName(const DB::ASTPtr &node) {
  if (!node) return false;
  const auto *identifier = dynamic_cast<const DB::ASTIdentifier *>(node.get());
  return identifier && (identifier->isParam() || identifier->name().empty());
}

bool hasParameterWriteTarget(const DB::ASTPtr &ast) {
  const DB::ASTQueryWithTableAndOutput *target = nullptr;
  if (const auto *drop = ast->as<DB::ASTDropQuery>()) {
    if (drop->database_and_tables) {
      for (const auto &child : drop->database_and_tables->children)
        if (isParameterOrEmptyName(child)) return true;
    }
    target = drop;
  } else if (const auto *alter = ast->as<DB::ASTAlterQuery>()) {
    target = alter;
  } else if (const auto *create = ast->as<DB::ASTCreateQuery>()) {
    target = create;
  } else if (const auto *update = ast->as<DB::ASTUpdateQuery>()) {
    target = update;
  } else if (const auto *del = ast->as<DB::ASTDeleteQuery>()) {
    target = del;
  }
  if (target) {
    return isParameterOrEmptyName(target->database)
      || isParameterOrEmptyName(target->table);
  }
  if (const auto *insert = ast->as<DB::ASTInsertQuery>()) {
    return isParameterOrEmptyName(insert->database)
      || isParameterOrEmptyName(insert->table);
  }
  if (const auto *rename = ast->as<DB::ASTRenameQuery>()) {
    for (const auto &elem : rename->getElements()) {
      if (isParameterOrEmptyName(elem.from.database)
          || isParameterOrEmptyName(elem.from.table)
          || isParameterOrEmptyName(elem.to.database)
          || isParameterOrEmptyName(elem.to.table)) {
        return true;
      }
    }
  }
  return false;
}

// Contract V2 accepts exactly `DROP TABLE [IF EXISTS] name[, name...] [SYNC]`
// for logical SI targets (rewriter-go engine.PlainDropTable). ClickHouse
// parses NO DELAY into the same `sync` flag, so it is accepted too; every
// other DROP modifier keeps the V1 rejection.
bool storageIntegrityPlainDrop(const DB::ASTDropQuery &drop,
                               const TableRewriteSelection &sel) {
  return sel.mode == TableRewriteMode::Dynamic
    && storageIntegrityDropContract(*sel.dynamic_args)
    && drop.kind == DB::ASTDropQuery::Kind::Drop
    && !drop.is_view && !drop.is_dictionary && !drop.isTemporary()
    && !drop.if_empty && !drop.has_all && !drop.has_tables && drop.like.empty()
    && drop.cluster.empty() && !drop.permanently
    && !drop.out_file && !drop.format_ast && !drop.settings_ast
    && (drop.table || drop.database_and_tables);
}

// allow_authorized_logical admits an authorized logical SI target: the
// signed-ingress INSERT target, or a V2 plain DROP TABLE target, which drops
// only the ordinary physical table.
bool rejectStorageIntegrityWriteTarget(
  rewriter::RewriteSQLResponse *response,
  const TableRewriteSelection &sel,
  const std::string &database,
  const std::string &table,
  bool allow_authorized_logical) {
  if (!hasActiveStorageIntegrity(sel) || table.empty()) return false;
  const auto &args = *sel.dynamic_args;
  if (auto hit = lookupStorageIntegrity(database, table, args)) {
    recordAccessedTable(response, database, table, sel);
    std::string logical;
    if (!authorizeStorageIntegrityLogical(database, args, &logical)) {
      rejectInvalidRequest(response, storageIntegrityUnauthorizedMessage(logical));
      return true;
    }
    if (allow_authorized_logical) {
      response->mutable_original_accessed_tables()->RemoveLast();
      return false;
    }
    rejectUnsupported(response,
      storageIntegrityAppendOnlyMessage(hit->logical_key, args));
    return true;
  }
  const std::string physical = database.empty()
    ? storageIntegrityExecutionDatabase(args) : database;
  if (!isStorageIntegrityPhysicalDatabase(physical, args)) return false;
  recordAccessedTable(response, database, table, sel);
  rejectUnsupported(response, storageIntegrityPhysicalRejectMessage(
    physical + "." + table));
  return true;
}

bool rejectStorageIntegrityWriteDatabase(
  rewriter::RewriteSQLResponse *response,
  const TableRewriteSelection &sel,
  const std::string &database) {
  if (!hasActiveStorageIntegrity(sel)
      || !isStorageIntegrityPhysicalDatabase(database, *sel.dynamic_args)) {
    return false;
  }
  recordPhysicalStorageIntegrityAccess(response, database, "", database);
  rejectUnsupported(response,
    storageIntegrityPhysicalDatabaseRejectMessage(database));
  return true;
}

// Record a write target ahead of every entry already present, with its
// table rewrite: a rejection that fired on a table source still reports the
// target first, as rewriter-go does (spec 2026-09-26 §5 "What the engines
// report").
void prependAccessedTable(rewriter::RewriteSQLResponse *response,
                          const TableRewriteSelection &sel,
                          const std::string &database,
                          const std::string &table) {
  recordAccessedTable(response, database, table, sel);
  auto *list = response->mutable_original_accessed_tables();
  for (int i = list->size() - 1; i > 0; --i) list->SwapElements(i, i - 1);
  if (sel.mode == TableRewriteMode::Dynamic) {
    const auto outcome = applyDynamicRewrite(database, table, *sel.dynamic_args, "write target");
    if (outcome.status == DynamicRewriteStatus::OK)
      recordTableRewrite(response, database, table, outcome.physical_db, outcome.new_table);
  }
}

DB::ASTPtr ownedChild(const DB::IAST &parent, const DB::IAST *child) {
  if (!child) return nullptr;
  for (const auto &candidate : parent.children)
    if (candidate.get() == child) return candidate;
  return nullptr;
}

// Run before every statement-specific generic reject. A non-Success response
// without SI metadata can be forwarded by legacy fail-open consumers, so the
// protocol-owned target must win over multi-target/cross-table/bare rejects.
bool preflightStorageIntegrityWrite(
  const DB::ASTPtr &ast,
  const TableRewriteSelection &sel,
  rewriter::RewriteSQLResponse *response) {
  if (!hasActiveStorageIntegrity(sel)) return false;
  const auto &args = *sel.dynamic_args;

  // CREATE and INSERT only: their SELECT body is rewritten (spec 2026-09-26
  // T4), so a dotted one-part IN operand is also read as one table of the
  // current database; the split reading and its SI refusal stay.
  auto reject_namespaces = [&] {
    return rejectStorageIntegrityNamespaces(
      response, collectStorageIntegrityNamespaceRefs(ast, InOperandReading::SplitAndWhole), args,
      rewriter::RewriteCode::UnsupportedStatement);
  };

  if (auto *create = ast->as<DB::ASTCreateQuery>()) {
    if (reject_namespaces()) return true;
    const bool ctas = !create->is_ordinary_view && !create->is_materialized_view && create->select;
    if (!create->table) {
      return rejectStorageIntegrityWriteDatabase(
        response, sel, create->getDatabase());
    }
    if (rejectStorageIntegrityWriteTarget(
          response, sel, create->getDatabase(), create->getTable(), false)) {
      return true;
    }
    if (!create->as_table.empty()
        && rejectStorageIntegrityWriteTarget(
          response, sel, create->as_database, create->as_table, false)) {
      return true;
    }
    // Spec 2026-09-26 T12: a materialized view writes into its TO / APPEND TO
    // target, so an Active target is refused like any other write.
    if (create->targets && create->hasTargetTableID(DB::ViewTarget::To)) {
      const auto to = create->getTargetTableID(DB::ViewTarget::To);
      if (rejectStorageIntegrityWriteTarget(response, sel, to.database_name, to.table_name, false))
        return true;
    }
    // Column declarations and table constraints are expression-bearing AST
    // carriers too: DEFAULT/MATERIALIZED/ALIAS/EPHEMERAL and CHECK may contain
    // subqueries. Walk the owned definitions as one ordered subtree so the
    // first embedded logical/physical SI read is rejected before CREATE can
    // reach ClickHouse.
    if (auto definitions = ownedChild(*create, create->columns_list);
        definitions && rejectEmbeddedStorageIntegrityRead(
          definitions, sel, response, rewriter::RewriteCode::UnsupportedStatement,
          /*allow_logical_reads=*/false, nullptr, /*r2_position=*/true)) {
      return true;
    }
    if (ctas) {
      bool table_source = false;
      if (auto body = ownedChild(*create, create->select);
          body && rejectEmbeddedStorageIntegrityRead(
            body, sel, response, rewriter::RewriteCode::UnsupportedStatement,
            /*allow_logical_reads=*/true, &table_source, /*r2_position=*/false, /*rewritten_body=*/true)) {
        if (table_source) prependAccessedTable(response, sel, create->getDatabase(), create->getTable());
        return true;
      }
    }
    return false;
  }

  if (auto *drop = ast->as<DB::ASTDropQuery>()) {
    const bool plain_si_drop = storageIntegrityPlainDrop(*drop, sel);
    if (drop->database_and_tables) {
      for (const auto &single : drop->getRewrittenASTsOfSingleTable(ast)) {
        const auto *one = single->as<DB::ASTDropQuery>();
        if (one && rejectStorageIntegrityWriteTarget(
              response, sel, one->getDatabase(), one->getTable(), plain_si_drop)) {
          return true;
        }
      }
      return false;
    }
    if (!drop->table) {
      return rejectStorageIntegrityWriteDatabase(
        response, sel, drop->getDatabase());
    }
    return rejectStorageIntegrityWriteTarget(
      response, sel, drop->getDatabase(), drop->getTable(), plain_si_drop);
  }

  if (auto *alter = ast->as<DB::ASTAlterQuery>()) {
    if (rejectStorageIntegrityWriteTarget(
          response, sel, alter->getDatabase(), alter->getTable(), false)) {
      return true;
    }
    if (alter->command_list) {
      for (const auto &raw : alter->command_list->children) {
        const auto *command = raw->as<DB::ASTAlterCommand>();
        if (!command) continue;
        if (rejectStorageIntegrityWriteTarget(
              response, sel, command->from_database,
              command->from_table, false)
            || rejectStorageIntegrityWriteTarget(
              response, sel, command->to_database,
              command->to_table, false)) {
          return true;
        }
        // Mutations may read tables from their assignment/predicate
        // subqueries even though the outer target is ordinary. Preserve the
        // grammar order shared with Go: UPDATE assignments before predicate;
        // DELETE predicate only (Spec I Task 2a).
        if (command->type == DB::ASTAlterCommand::UPDATE) {
          if (auto assignments = ownedChild(*command, command->update_assignments);
              assignments && rejectEmbeddedStorageIntegrityRead(
                assignments, sel, response, rewriter::RewriteCode::UnsupportedStatement,
                /*allow_logical_reads=*/false, nullptr, /*r2_position=*/true)) {
            return true;
          }
          if (auto predicate = ownedChild(*command, command->predicate);
              predicate && rejectEmbeddedStorageIntegrityRead(
                predicate, sel, response, rewriter::RewriteCode::UnsupportedStatement,
                /*allow_logical_reads=*/false, nullptr, /*r2_position=*/true)) {
            return true;
          }
        } else if (command->type == DB::ASTAlterCommand::DELETE) {
          if (auto predicate = ownedChild(*command, command->predicate);
              predicate && rejectEmbeddedStorageIntegrityRead(
                predicate, sel, response, rewriter::RewriteCode::UnsupportedStatement,
                /*allow_logical_reads=*/false, nullptr, /*r2_position=*/true)) {
            return true;
          }
        } else if (command->type == DB::ASTAlterCommand::MODIFY_QUERY) {
          if (auto body = ownedChild(*command, command->select);
              body && rejectEmbeddedStorageIntegrityRead(
                body, sel, response, rewriter::RewriteCode::UnsupportedStatement,
                /*allow_logical_reads=*/false, nullptr, /*r2_position=*/true)) {
            return true;
          }
        } else if (rejectEmbeddedStorageIntegrityRead(
                     raw, sel, response, rewriter::RewriteCode::UnsupportedStatement,
                     /*allow_logical_reads=*/false, nullptr, /*r2_position=*/true)) {
          // ALTER has several expression-bearing command carriers besides
          // mutations and MODIFY QUERY. In particular, compound column
          // declarations (DEFAULT/MATERIALIZED/ALIAS/EPHEMERAL), constraints,
          // indexes, projections, ORDER BY, SAMPLE BY and TTL expressions may
          // contain subqueries. Walk the complete command subtree in source
          // order so new or less-common carriers cannot bypass SI preflight.
          return true;
        }
      }
    }
    return false;
  }

  if (auto *insert = ast->as<DB::ASTInsertQuery>()) {
    if (reject_namespaces()) return true;
    if (insert->table && rejectStorageIntegrityWriteTarget(
          response, sel, insert->getDatabase(), insert->getTable(), true)) {
      return true;
    }
    if (insert->select) {
      // An ordinary target reads an Active source through the derived read;
      // an SI target keeps the refusal (spec 2026-09-26 §7 "Sources"), and so
      // does INSERT INTO FUNCTION, whose body is never rewritten.
      const bool ordinary_target = insert->table
        && !lookupStorageIntegrity(insert->getDatabase(), insert->getTable(), args).has_value();
      bool table_source = false;
      if (rejectEmbeddedStorageIntegrityRead(
            insert->select, sel, response, rewriter::RewriteCode::UnsupportedStatement,
            /*allow_logical_reads=*/ordinary_target, &table_source, /*r2_position=*/false,
            /*rewritten_body=*/insert->table != nullptr)) {
        if (table_source && insert->table)
          prependAccessedTable(response, sel, insert->getDatabase(), insert->getTable());
        return true;
      }
    }
    // A VALUES row is an expression no rewrite reaches. rewriter-go's SI
    // namespace policy names an SI physical table, an IN operand or a table
    // function there; a logical Active table in a FROM position is left to
    // R2, which refuses every table read in a row afterwards.
    for (const auto &row : insertValuesRows(ast)) {
      if (rejectEmbeddedStorageIntegrityRead(row, sel, response, rewriter::RewriteCode::UnsupportedStatement,
                                             /*allow_logical_reads=*/true, nullptr, /*r2_position=*/true))
        return true;
    }
    return false;
  }

  if (auto *update = ast->as<DB::ASTUpdateQuery>()) {
    if (rejectStorageIntegrityWriteTarget(
          response, sel, update->getDatabase(), update->getTable(), false)) {
      return true;
    }
    if (update->assignments && rejectEmbeddedStorageIntegrityRead(
          update->assignments, sel, response, rewriter::RewriteCode::UnsupportedStatement,
          /*allow_logical_reads=*/false, nullptr, /*r2_position=*/true)) {
      return true;
    }
    if (update->predicate && rejectEmbeddedStorageIntegrityRead(
          update->predicate, sel, response, rewriter::RewriteCode::UnsupportedStatement,
          /*allow_logical_reads=*/false, nullptr, /*r2_position=*/true)) {
      return true;
    }
    return false;
  }

  if (auto *delete_query = ast->as<DB::ASTDeleteQuery>()) {
    if (rejectStorageIntegrityWriteTarget(
          response, sel, delete_query->getDatabase(), delete_query->getTable(), false)) {
      return true;
    }
    if (delete_query->predicate && rejectEmbeddedStorageIntegrityRead(
          delete_query->predicate, sel, response, rewriter::RewriteCode::UnsupportedStatement,
          /*allow_logical_reads=*/false, nullptr, /*r2_position=*/true)) {
      return true;
    }
    return false;
  }

  if (auto *rename = ast->as<DB::ASTRenameQuery>()) {
    for (const auto &element : rename->getElements()) {
      if (rename->database) {
        if (rejectStorageIntegrityWriteDatabase(
              response, sel, element.from.getDatabase())
            || rejectStorageIntegrityWriteDatabase(
              response, sel, element.to.getDatabase())) return true;
      } else if (rejectStorageIntegrityWriteTarget(
                   response, sel, element.from.getDatabase(),
                   element.from.getTable(), false)
                 || rejectStorageIntegrityWriteTarget(
                   response, sel, element.to.getDatabase(),
                   element.to.getTable(), false)) {
        return true;
      }
    }
    return false;
  }

  if (auto *optimize = ast->as<DB::ASTOptimizeQuery>()) {
    return rejectStorageIntegrityWriteTarget(
      response, sel, optimize->getDatabase(), optimize->getTable(), false);
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
    [&](const std::string &s){ side.database = DB::makeRewriterAST<DB::ASTIdentifier>(s); },
    [&](const std::string &s){ side.table    = DB::makeRewriterAST<DB::ASTIdentifier>(s); });
}

// CREATE VIEW / CREATE MATERIALIZED VIEW. Rewrites three things via the
// unified static->dynamic precedence already resolved into `sel`:
//   1. the view name itself (single target, like CREATE TABLE);
//   2. a materialized view's TO target, when present;
//   3. the view body SELECT (full option pipeline, via select.cc).
// LIVE / WINDOW views are rejected by the caller before reaching here.
WriteDispatchResult handleViewCreate(
  const DB::ASTPtr &ast,
  DB::ASTCreateQuery *create_query,
  const rewriter::RewriteSQLRequest *request,
  rewriter::RewriteSQLResponse *response,
  const TableRewriteSelection &sel) {
  const bool is_mv = create_query->is_materialized_view;
  const std::string_view stmt_kind =
    is_mv ? "CREATE MATERIALIZED VIEW" : "CREATE VIEW";

  // 1. View name.
  if (!rewriteOneTarget(
        response, sel, create_query->getDatabase(), create_query->getTable(),
        stmt_kind,
        [&](const std::string &s){ create_query->setDatabase(s); },
        [&](const std::string &s){ create_query->setTable(s); })) {
    return WriteDispatchResult::Rejected;
  }

  // 2. Materialized view TO target. A TO-less materialized view has an
  //    implicit inner table whose name CH derives — nothing to rewrite.
  if (is_mv && create_query->hasTargetTableID(DB::ViewTarget::To)) {
    const DB::StorageID to_id =
      create_query->getTargetTableID(DB::ViewTarget::To);
    std::string to_db = to_id.database_name;
    std::string to_table = to_id.table_name;
    if (!rewriteOneTarget(
          response, sel, to_id.database_name, to_id.table_name,
          "CREATE MATERIALIZED VIEW TO target",
          [&](const std::string &s){ to_db = s; },
          [&](const std::string &s){ to_table = s; })) {
      return WriteDispatchResult::Rejected;
    }
    create_query->targets->setTableID(
      DB::ViewTarget::To, DB::StorageID{to_db, to_table});
  }

  // 3. View body SELECT — full option pipeline + cross-shard GLOBAL pass.
  if (create_query->select) {
    DB::ASTPtr body;
    for (auto &child : create_query->children) {
      if (child.get() == static_cast<DB::IAST *>(create_query->select)) {
        body = child;
        break;
      }
    }
    if (body) {
      rewriteEmbeddedViewBody(request->options(), body, response);
      if (response->code() != rewriter::RewriteCode::Success)
        return WriteDispatchResult::Rejected;
    }
  }

  acceptWrite(ast, response,
              is_mv ? rewriter::STATEMENT_TYPE_CREATE_MATERIALIZED_VIEW
                    : rewriter::STATEMENT_TYPE_CREATE_VIEW);
  return WriteDispatchResult::Handled;
}

} // namespace

bool storageIntegrityWritePreflight(const DB::ASTPtr &ast,
                                    const TableRewriteSelection &sel,
                                    rewriter::RewriteSQLResponse *response) {
  return preflightStorageIntegrityWrite(ast, sel, response);
}

bool handleRemovedLiveViewCompatibility(
  const std::string &query,
  const rewriter::RewriteSQLRequest *request,
  rewriter::RewriteSQLResponse *response) {
  DB::Lexer lexer(query.data(), query.data() + query.size());
  auto nextSignificantToken = [&lexer] {
    DB::Token token;
    do {
      token = lexer.nextToken();
    } while (!token.isSignificant());
    return token;
  };

  const DB::Token command = nextSignificantToken();
  if (!tokenEqualsKeyword(command, "CREATE")
      && !tokenEqualsKeyword(command, "ATTACH")) {
    return false;
  }

  // LIVE VIEW used to allow SQL SECURITY / DEFINER clauses between the
  // command and object kind. Search only the statement prefix (before AS),
  // requiring adjacent significant LIVE + VIEW tokens; the full parser below
  // remains the authority on whether the resulting legacy statement is valid.
  DB::Token previous;
  bool has_previous = false;
  size_t live_offset = 0;
  size_t live_size = 0;
  while (true) {
    const DB::Token token = nextSignificantToken();
    if (token.isEnd() || token.isError()
        || token.type == DB::TokenType::Semicolon
        || tokenEqualsKeyword(token, "AS")) {
      return false;
    }
    if (has_previous
        && tokenEqualsKeyword(previous, "LIVE")
        && tokenEqualsKeyword(token, "VIEW")) {
      live_offset = static_cast<size_t>(previous.begin - query.data());
      live_size = previous.size();
      break;
    }
    if (beginsNonLiveCreateObject(token)) return false;
    previous = token;
    has_previous = true;
  }

  // Validate the complete statement with the current ClickHouse parser. A
  // token-only match must not turn malformed legacy SQL into a structured
  // UnsupportedStatement response.
  std::string candidate = query;
  candidate.replace(live_offset, live_size, "MATERIALIZED");

  DB::ASTPtr ast;
  try {
    const char *begin = candidate.data();
    const char *end = begin + candidate.size();
    DB::ParserQuery parser(end, false);
    ast = parseBounded(parser, begin, end);
  } catch (const DB::Exception &) {
    return false;
  }

  auto *create = ast->as<DB::ASTCreateQuery>();
  if (!create || !create->is_materialized_view) return false;

  if (create->if_not_exists) {
    response->set_existence_clause(
      rewriter::EXISTENCE_CLAUSE_IF_NOT_EXISTS);
  }

  // The removed LIVE VIEW AST was neither an ordinary nor a materialized
  // view. Restore those flags before preflight so an embedded SELECT is
  // checked with the same fail-closed storage-integrity policy as before.
  create->is_materialized_view = false;
  create->is_ordinary_view = false;
  const auto sel = findActiveTableRewrite(request->options());
  if (preflightStorageIntegrityWrite(ast, sel, response)) return true;
  // A live view is a view: its body may not read an Active table, which the
  // CREATE preflight (now a CTAS-shaped create) no longer refuses (T4).
  if (auto body = ownedChild(*create, create->select);
      body && rejectEmbeddedStorageIntegrityRead(body, sel, response)) return true;

  rejectUnsupported(
    response, "CREATE LIVE VIEW / WINDOW VIEW is not supported");
  return true;
}

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

  // Before storage-integrity preflight, which skips an empty table name, and
  // before any rewrite. In dynamic mode the table-reference preflight has
  // refused these already; this keeps Mode None and static requests safe.
  if (hasParameterWriteTarget(ast)) {
    rejectInvalidRequest(response, std::string(kIdentifierParameterMessage));
    return WriteDispatchResult::Rejected;
  }

  if (preflightStorageIntegrityWrite(ast, sel, response))
    return WriteDispatchResult::Rejected;
  // The table-reference checks that the preflight runs while the SI surface
  // is inactive run here, after the SI write preflight, while it is active,
  // so an SI-owned message wins (spec 2026-09-26 §5 precedence).
  if (hasActiveStorageIntegrity(sel)
      && (ast->as<DB::ASTCreateQuery>() || ast->as<DB::ASTInsertQuery>() || ast->as<DB::ASTAlterQuery>()
          || ast->as<DB::ASTUpdateQuery>() || ast->as<DB::ASTDeleteQuery>())
      && (rejectDisallowedCarriers(ast, response) || rejectQuerySettings(ast, response, &request->sql())
          || rejectUngovernedReads(ast, request->sql(), response, &sel)))
    return WriteDispatchResult::Rejected;

  if (auto *create_query = ast->as<DB::ASTCreateQuery>()) {
    // LIVE VIEW is handled by the pre-parse compatibility path because
    // ClickHouse 26.3 removed it from ParserQuery and ASTCreateQuery. WINDOW
    // VIEW is still parsed and rejected here.
    if (create_query->is_window_view) {
      rejectUnsupported(response, "CREATE LIVE VIEW / WINDOW VIEW is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (create_query->is_dictionary) {
      rejectUnsupported(response, "CREATE DICTIONARY is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (create_query->is_ordinary_view || create_query->is_materialized_view) {
      return handleViewCreate(ast, create_query, request, response, sel);
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

    // Spec 2026-09-26 T4: a CREATE TABLE … AS SELECT body (EMPTY and OR
    // REPLACE included) is rewritten and reported like a view body; its
    // sources follow the target in original_accessed_tables.
    if (create_query->select) {
      if (auto body = ownedChild(*create_query, create_query->select)) {
        rewriteEmbeddedBody(request->options(), body, response, EmbeddedBodyKind::CreateSelect);
        if (response->code() != rewriter::RewriteCode::Success)
          return WriteDispatchResult::Rejected;
      }
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

    setSuccessResponse(response, formatWrite(ast, *response, sel), rewriter::STATEMENT_TYPE_CREATE_TABLE);
    return WriteDispatchResult::Handled;
  }

  if (auto *drop_query = ast->as<DB::ASTDropQuery>()) {
    const bool is_truncate = drop_query->kind == DB::ASTDropQuery::Kind::Truncate;
    const std::string_view stmt_kind_str =
      drop_query->is_view ? "DROP VIEW"
      : is_truncate       ? "TRUNCATE TABLE"
                          : "DROP TABLE";

    if (drop_query->kind == DB::ASTDropQuery::Kind::Detach) {
      rejectUnsupported(response, "DETACH is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (drop_query->is_dictionary) {
      rejectUnsupported(response, "DROP/TRUNCATE on DICTIONARY is not supported");
      return WriteDispatchResult::Rejected;
    }
    if (drop_query->is_view && is_truncate) {
      // TRUNCATE has no meaning for a view. (CH's parser normally rejects
      // `TRUNCATE VIEW` outright; this guard is belt-and-suspenders.)
      rejectUnsupported(response, "TRUNCATE on VIEW is not supported");
      return WriteDispatchResult::Rejected;
    }
    // DROP VIEW (kind == Drop, is_view == true) falls through to the
    // single-target rewriteOneTarget path below, same as DROP TABLE.
    if (drop_query->database_and_tables) {
      // Contract V2 accepts a plain multi-table DROP TABLE whose targets may
      // mix storage-integrity and ordinary tables; everything else stays
      // rejected. Each target takes the ordinary single-target policy.
      if (!storageIntegrityPlainDrop(*drop_query, sel)) {
        rejectUnsupported(response, "multi-table DROP/TRUNCATE is not supported");
        return WriteDispatchResult::Rejected;
      }
      auto &list = drop_query->database_and_tables->as<DB::ASTExpressionList &>();
      for (auto &child : list.children) {
        auto *identifier = child->as<DB::ASTTableIdentifier>();
        // hasParameterWriteTarget already rejected query-parameter names.
        // Re-check defensively: resetTable swaps only name_parts and keeps
        // the parameter child, which the formatter prints in place of an
        // empty part, so `db.{p:Identifier}` would survive the rewrite as
        // `phys.{p:Identifier}` and bind any physical table at execution.
        if (!identifier || identifier->isParam()) {
          rejectUnsupported(response, "multi-table DROP/TRUNCATE is not supported");
          return WriteDispatchResult::Rejected;
        }
        const auto table_id = identifier->getTableId();
        const std::string origin_db = table_id.database_name;
        const std::string origin_table = table_id.table_name;
        std::string new_db = origin_db;
        std::string new_table = origin_table;
        if (!rewriteOneTarget(response, sel, origin_db, origin_table, stmt_kind_str,
            [&](const std::string &s){ new_db = s; },
            [&](const std::string &s){ new_table = s; })) {
          return WriteDispatchResult::Rejected;
        }
        if (new_db != origin_db || new_table != origin_table)
          identifier->resetTable(new_db, new_table);
      }
      acceptWrite(ast, response, rewriter::STATEMENT_TYPE_DROP_TABLE);
      return WriteDispatchResult::Handled;
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
    const rewriter::StatementType drop_stmt_type =
      drop_query->is_view ? rewriter::STATEMENT_TYPE_DROP_VIEW
      : is_truncate       ? rewriter::STATEMENT_TYPE_TRUNCATE_TABLE
                          : rewriter::STATEMENT_TYPE_DROP_TABLE;
    acceptWrite(ast, response, drop_stmt_type);
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
    if (insert_query->select) {
      if (auto body = ownedChild(*insert_query, insert_query->select.get())) {
        rewriteEmbeddedBody(request->options(), body, response, EmbeddedBodyKind::InsertSelect);
        if (response->code() != rewriter::RewriteCode::Success)
          return WriteDispatchResult::Rejected;
      }
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
    std::string sql = formatWrite(ast, *response, sel);
    if (insert_query->data && insert_query->end > insert_query->data) {
      if (insert_query->format == "Values") {
        const std::string suffix = " FORMAT Values";
        if (sql.size() >= suffix.size()
            && sql.compare(sql.size() - suffix.size(), suffix.size(), suffix) == 0) {
          sql.replace(sql.size() - suffix.size(), suffix.size(), " VALUES");
        }
      }
      if (!sql.empty()
          && !std::isspace(static_cast<unsigned char>(sql.back()))) sql += ' ';
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
    // GRANT / REVOKE are handled by handlers/grant.cc, which dispatches
    // before handleWriteQuery in rewriter-server.cc. They never fall through
    // to this switch.
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
