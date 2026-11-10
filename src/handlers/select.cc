// src/handlers/select.cc
#include "handlers/select.h"

#include <Core/Field.h>
#include <Core/Joins.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSetQuery.h>
#include <Parsers/ASTSubquery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ASTWithElement.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ASTTransformers.h"
#include "handlers/common.h"
#include "handlers/cte_scope.h"
#include "handlers/name_rewrite.h"
#include "handlers/storage_integrity.h"
#include "handlers/table_reference.h"

namespace rewriter_handlers {

namespace {

struct AccessedTablePair {
  std::string original_db;     // empty when the SQL didn't qualify the table.
  std::string original_table;
  bool in_operand = false;     // collected from an IN operand (spec T4)
};

// The sources a query reads, ordered by the name as written (spec §5 "What the
// engines report"), then by (database, table) when two spellings collide. Two
// sources with the same spelling but a different identity — a quoted one-part
// `hg_safe.db1__t` and the two-part hg_safe.db1__t — are both kept, so neither
// can hide the other (review C1); ties order by database then table (the empty
// database of a one-part name first), as rewriter-go orders them. CTE-alias
// erasure and table_rewrites keys use the written name.
struct AccessedKey {
  std::string written;
  std::string database;
  std::string table;
  bool operator<(const AccessedKey &other) const {
    return std::tie(written, database, table) < std::tie(other.written, other.database, other.table);
  }
};
using AccessedTables = std::map<AccessedKey, AccessedTablePair>;

void addAccessedTable(AccessedTables &out, const std::string &written, AccessedTablePair pair) {
  AccessedKey key{written, pair.original_db, pair.original_table};
  const auto [it, inserted] = out.emplace(std::move(key), pair);
  // A FROM of the same identity keeps the entry a real table read.
  if (!inserted && !pair.in_operand) it->second.in_operand = false;
}

void eraseAccessedByWrittenName(AccessedTables &out, const std::string &written) {
  std::erase_if(out, [&](const auto &entry) { return entry.first.written == written; });
}

void populateAccessedTables(
  const AccessedTables &originals,
  const TableRewriteSelection &selection,
  rewriter::RewriteSQLResponse *response) {
  for (const auto &[_, pair] : originals) {
    auto *entry = response->add_original_accessed_tables();
    entry->set_original_database(pair.original_db);
    entry->set_original_table(pair.original_table);
    const auto resolution = resolveAccessedTable(
      pair.original_db, pair.original_table, selection);
    entry->set_logical_database(resolution.logical_database);
    entry->set_physical_database(resolution.physical_database);
    entry->set_is_remote(resolution.is_remote);
    entry->set_is_storage_integrity(resolution.is_storage_integrity);
  }
}

bool validateStorageIntegritySelectTargets(
  const AccessedTables &originals,
  const TableRewriteSelection &selection,
  rewriter::RewriteSQLResponse *response,
  rewriter::RewriteCode physical_code = rewriter::RewriteCode::RewriteError) {
  if (selection.mode != TableRewriteMode::Dynamic) return true;
  const auto &args = *selection.dynamic_args;
  for (const auto &[_, pair] : originals) {
    if (auto hit = lookupStorageIntegrity(
          pair.original_db, pair.original_table, args)) {
      std::string logical;
      if (!authorizeStorageIntegrityLogical(pair.original_db, args, &logical)) {
        response->set_code(rewriter::RewriteCode::InvalidRewriteRequest);
        response->set_message(storageIntegrityUnauthorizedMessage(logical));
        return false;
      }
      continue;
    }
    const std::string physical = pair.original_db.empty()
      ? storageIntegrityExecutionDatabase(args) : pair.original_db;
    if (isStorageIntegrityPhysicalDatabase(physical, args)) {
      response->set_code(physical_code);
      response->set_message(storageIntegrityPhysicalRejectMessage(
        pair.original_db.empty()
          ? pair.original_table
          : physical + "." + pair.original_table));
      return false;
    }
  }
  return true;
}

const DB::ASTTableExpression *lastTableExpression(
  const DB::ASTSelectQuery &select) {
  const auto tables = select.tables();
  const auto *list = tables ? tables->as<DB::ASTTablesInSelectQuery>() : nullptr;
  if (!list) return nullptr;
  for (auto it = list->children.rbegin(); it != list->children.rend(); ++it) {
    const auto *element = (*it)->as<DB::ASTTablesInSelectQueryElement>();
    if (!element || !element->table_expression) continue;
    return element->table_expression->as<DB::ASTTableExpression>();
  }
  return nullptr;
}

const DB::ASTTableExpression *firstTableExpression(
  const DB::ASTSelectQuery &select) {
  const auto tables = select.tables();
  const auto *list = tables ? tables->as<DB::ASTTablesInSelectQuery>() : nullptr;
  if (!list) return nullptr;
  for (const auto &child : list->children) {
    const auto *element = child->as<DB::ASTTablesInSelectQueryElement>();
    if (!element || !element->table_expression) continue;
    return element->table_expression->as<DB::ASTTableExpression>();
  }
  return nullptr;
}

bool tableExpressionTouchesStorageIntegrity(
  const DB::ASTTableExpression &table_expression,
  const rewriter::RewriteTableDynamicArgs &args) {
  const auto *identifier = table_expression.database_and_table_name
    ? table_expression.database_and_table_name->as<DB::ASTTableIdentifier>()
    : nullptr;
  if (!identifier) return false;
  const std::string full = identifier->name();
  std::string database;
  std::string table = full;
  if (const auto dot = full.find('.'); dot != std::string::npos) {
    database = full.substr(0, dot);
    table = full.substr(dot + 1);
  }
  return lookupStorageIntegrity(database, table, args).has_value();
}

bool hasUnsupportedStorageIntegrityWrapper(
  const DB::ASTPtr &ast,
  const rewriter::RewriteTableDynamicArgs &args) {
  if (!ast) return false;
  if (const auto *table_expression = ast->as<DB::ASTTableExpression>()) {
    if (tableExpressionTouchesStorageIntegrity(*table_expression, args)
        && (table_expression->final || table_expression->sample_size
            || table_expression->sample_offset)) return true;
  }
  if (const auto *select = ast->as<DB::ASTSelectQuery>()) {
    if (select->prewhere()) {
      const auto *first = firstTableExpression(*select);
      if (first && tableExpressionTouchesStorageIntegrity(*first, args))
        return true;
    }
    if (select->aliases()) {
      const auto *last = lastTableExpression(*select);
      if (last && tableExpressionTouchesStorageIntegrity(*last, args))
        return true;
    }
  }
  for (const auto &child : ast->children)
    if (hasUnsupportedStorageIntegrityWrapper(child, args)) return true;
  return false;
}

bool responseTouchesStorageIntegrity(
  const rewriter::RewriteSQLResponse &response) {
  for (const auto &entry : response.original_accessed_tables())
    if (entry.is_storage_integrity()) return true;
  return false;
}

std::string recordLogicalStorageIntegrityRewrites(
  const AccessedTables &originals,
  const rewriter::RewriteTableDynamicArgs &args,
  rewriter::RewriteSQLResponse *response) {
  std::string first_key;
  for (const auto &[key, pair] : originals) {
    const std::string &origin_name = key.written;
    if (auto hit = lookupStorageIntegrity(
          pair.original_db, pair.original_table, args)) {
      (*response->mutable_table_rewrites())[origin_name] =
        hit->table->safe_table();
      if (first_key.empty()) first_key = hit->logical_key;
    }
  }
  return first_key;
}

void recordStorageIntegrityPairUnique(
  rewriter::RewriteSQLResponse *response,
  const AccessedTablePair &pair,
  const TableRewriteSelection &selection) {
  for (const auto &entry : response->original_accessed_tables()) {
    if (entry.original_database() == pair.original_db
        && entry.original_table() == pair.original_table) return;
  }
  auto *entry = response->add_original_accessed_tables();
  entry->set_original_database(pair.original_db);
  entry->set_original_table(pair.original_table);
  const auto resolved = resolveAccessedTable(
    pair.original_db, pair.original_table, selection);
  entry->set_logical_database(resolved.logical_database);
  entry->set_physical_database(resolved.physical_database);
  entry->set_is_remote(resolved.is_remote);
  entry->set_is_storage_integrity(resolved.is_storage_integrity);
}

// Walk every ASTTableExpression and collect its (db, table) split. Keyed by
// the SQL-shaped name ("db.table" / bare "table") so we can dedup and so
// CTE-alias entries can be erased by alias when their bodies inline. Replaces
// the prior set-of-string collector — same dedup behavior, but carries the
// db/table split forward to the response without the caller having to re-split.
//
// `cte_scope` carries CTE-alias names declared by enclosing WITH clauses;
// bare references to those aliases aren't real tables and must be skipped.
// Qualified `db.alias` references stay collected — those are real tables.
void collectAccessedTablePairsFromAST(
  const DB::ASTPtr &node,
  AccessedTables &out,
  const CTEAliasScope &cte_scope) {
  if (!node) return;

  // Spec 2026-09-26 T4: an IN operand naming a table is a read source like
  // FROM. The parser unwraps its parentheses and drops a unary plus, so the
  // second argument of any IN-family call is the operand at every depth.
  if (const auto *fn = node->as<DB::ASTFunction>();
      fn && isInFamilyFunction(fn->name) && fn->arguments && fn->arguments->children.size() == 2) {
    const auto &operand = fn->arguments->children[1];
    if (const auto *id = operand->as<DB::ASTIdentifier>(); id && !id->isParam()) {
      const auto [db, table] = inOperandTarget(*id);
      const bool is_cte_ref = db.empty() && cte_scope.bindsInOperand(table);
      if (!table.empty() && !is_cte_ref)
        addAccessedTable(out, id->name(), AccessedTablePair{db, table, true});
    }
  }

  if (auto *te = node->as<DB::ASTTableExpression>()) {
    if (te->database_and_table_name) {
      if (auto *id = te->database_and_table_name->as<DB::ASTTableIdentifier>()) {
        const auto full = id->name();  // written name, for order and rewrite keys
        if (!full.empty()) {
          // The identifier's parts decide the identity: one part is a table of
          // the current database whatever dots its quoted name holds
          // (`db2.x` is not tenant db2's x); two or more split at the first.
          const bool is_cte_ref =
            id->name_parts.size() == 1 && cte_scope.bindsTable(full);
          if (!is_cte_ref) {
            AccessedTablePair p;
            std::tie(p.original_db, p.original_table) = inOperandTarget(*id);
            addAccessedTable(out, full, std::move(p));
          }
        }
      }
    }
  }
  visitChildrenWithCTEVisibility(
    *node, cte_scope,
    [&](const DB::ASTPtr &child, const CTEAliasScope &child_scope) {
      collectAccessedTablePairsFromAST(child, out, child_scope);
    });
}

void collectAccessedTablePairsFromAST(
  const DB::ASTPtr &node,
  AccessedTables &out) {
  collectAccessedTablePairsFromAST(node, out, CTEAliasScope{});
}

// Per-transformer AST dumps are only useful when debugging the rewrite
// pipeline; in production they blow up log volume (every SELECT prints the
// full tree 5x). Gate them behind REWRITER_DUMP_AST=1. Read once at
// first use so we don't pay a getenv per request.
bool shouldDumpAst() {
  static const bool enabled = [] {
    const char *v = std::getenv("REWRITER_DUMP_AST");
    return v != nullptr && *v != '\0' && std::string_view(v) != "0";
  }();
  return enabled;
}

bool isHasColumnInTableCall(const DB::IAST &node) {
  const auto *fn = node.as<DB::ASTFunction>();
  if (!fn || !fn->arguments || fn->arguments->children.empty()) return false;
  std::string lower = fn->name;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lower.rfind("hascolumnintable", 0) == 0;
}

void collectHasColumnInTableCalls(const DB::ASTPtr &node, std::vector<DB::ASTFunction *> &out) {
  if (!node) return;
  if (isHasColumnInTableCall(*node)) out.push_back(node->as<DB::ASTFunction>());
  for (const auto &child : node->children) collectHasColumnInTableCalls(child, out);
}

// One hasColumnInTable call's (database, table) literals (its last two
// arguments before the column name; a leading host[, user[, password]]
// shifts them) and their resolution. `resolved` only for a pair of non-empty,
// dot-free string literals naming an ordinary mapped table, as rewriter-go's
// decideStringLookup reads the two names ClickHouse reads.
struct HasColumnInTableTarget {
  bool literal = false;
  std::string database;
  std::string table;
  DynamicRewriteOutcome outcome;
  bool resolved() const { return outcome.status == DynamicRewriteStatus::OK; }
};

HasColumnInTableTarget resolveHasColumnInTable(const DB::ASTFunction &fn,
                                               const rewriter::RewriteTableDynamicArgs &args) {
  HasColumnInTableTarget target;
  const auto &children = fn.arguments->children;
  if (children.size() < 3) return target;
  const auto *db_literal = children[children.size() - 3]->as<DB::ASTLiteral>();
  const auto *table_literal = children[children.size() - 2]->as<DB::ASTLiteral>();
  if (!db_literal || !table_literal || db_literal->value.getType() != DB::Field::Types::String
      || table_literal->value.getType() != DB::Field::Types::String)
    return target;
  target.literal = true;
  target.database = db_literal->value.safeGet<String>();
  target.table = table_literal->value.safeGet<String>();
  if (target.database.empty() || target.table.empty()
      || target.database.find('.') != std::string::npos
      || target.table.find('.') != std::string::npos
      || lookupStorageIntegrity(target.database, target.table, args))
    return target;
  target.outcome = applyDynamicRewrite(target.database, target.table, args, "hasColumnInTable");
  return target;
}

// Spec 2026-09-26 T6: every hasColumnInTable call in a rewritten SELECT body
// whose (database, table) literals resolve to an ordinary mapped table is
// rewritten to the physical pair, recorded in table_rewrites and reported
// once (after the body's own sources, unless one of them is the same table);
// any other call refuses the body with `code` (rewriter-go answers
// UnsupportedStatement for an INSERT … SELECT / CTAS body). Every call is
// decided before any is rewritten (rewriter-go rewriteStringLookups).
// Returns false after the refusal.
bool rewriteHasColumnInTable(const DB::ASTPtr &body,
                             const TableRewriteSelection &selection,
                             rewriter::RewriteSQLResponse *response,
                             rewriter::RewriteCode code = rewriter::RewriteCode::InvalidRewriteRequest) {
  if (!body || selection.mode != TableRewriteMode::Dynamic) return true;
  const auto &args = *selection.dynamic_args;
  std::vector<DB::ASTFunction *> calls;
  collectHasColumnInTableCalls(body, calls);
  std::vector<HasColumnInTableTarget> targets;
  for (const auto *fn : calls) {
    targets.push_back(resolveHasColumnInTable(*fn, args));
    const auto &target = targets.back();
    if (target.resolved()) continue;
    response->set_code(code);
    response->set_message(stringLookupUnresolvedMessage(
      fn->name, target.literal ? target.database + "." + target.table : ""));
    return false;
  }
  for (size_t i = 0; i < calls.size(); ++i) {
    auto &children = calls[i]->arguments->children;
    const auto &target = targets[i];
    children[children.size() - 3] = DB::makeRewriterAST<DB::ASTLiteral>(target.outcome.physical_db);
    children[children.size() - 2] = DB::makeRewriterAST<DB::ASTLiteral>(target.outcome.new_table);
    (*response->mutable_table_rewrites())[target.database + "." + target.table] =
      target.outcome.physical_db + "." + target.outcome.new_table;
    const auto &reported = response->original_accessed_tables();
    const bool already = std::any_of(reported.begin(), reported.end(), [&](const auto &entry) {
      return entry.original_database() == target.database && entry.original_table() == target.table;
    });
    if (!already) recordAccessedTable(response, target.database, target.table, selection);
  }
  return true;
}

void doTableNameRewrite(
  DB::ASTPtr ast,
  const std::unordered_map<std::string, std::string> &table_name_map,
  const std::unordered_map<std::string, DB::RemoteTable> &remote_table_map,
  const std::unordered_map<std::string, DB::TableWithDatabase> &table_with_database_map,
  std::map<std::string, std::string> *out_table_rewrites) {
  DB::ASTReplaceTransformer transformer;
  transformer.setTableNameMap(table_name_map);
  transformer.setRemoteTableMap(remote_table_map);
  transformer.setTableWithDatabaseMap(table_with_database_map);
  transformer.transform(ast);
  if (out_table_rewrites) {
    for (const auto &[k, v] : transformer.rewrites) {
      (*out_table_rewrites)[k] = v;
    }
  }
  if (shouldDumpAst()) {
    std::cout << "after table name transformation " << ast->dumpTree() << std::endl;
  }
}

// `first_unresolved`, when set, receives the first unqualified table name
// (document order) that does not resolve through the logical context: spec
// 2026-09-26 §5 "Unqualified names that do not resolve". The walk itself stays
// lenient; the callers refuse (firstUnresolvedUnqualifiedTable).
void dynamicRewriteWalk(
  DB::ASTPtr ast,
  const rewriter::RewriteTableDynamicArgs &args,
  std::map<std::string, std::string> *out_table_rewrites,
  const CTEAliasScope &cte_scope,
  std::optional<std::string> *first_unresolved = nullptr) {
  if (!ast) return;
  // With `first_unresolved` set the walk only checks: it never mutates the
  // tree, so it can run on the live AST (a clone would share the subtrees a
  // registered tree adopted as children, registerFieldOnlyChildren).
  const bool check_only = first_unresolved != nullptr;
  const auto noteUnresolved = [&](const std::string &db, const std::string &table) {
    if (first_unresolved && !first_unresolved->has_value() && db.empty()) *first_unresolved = table;
  };

  // Spec 2026-09-26 T4: rewrite an IN operand exactly like a FROM reference.
  // An Active SI table becomes the derived safe/unsafe read; a mapped table
  // its physical name; an unmapped logical name stays (lenient, like FROM).
  if (auto *fn = ast->as<DB::ASTFunction>();
      fn && isInFamilyFunction(fn->name) && fn->arguments && fn->arguments->children.size() == 2) {
    auto &operand = fn->arguments->children[1];
    const auto *id = operand->as<DB::ASTIdentifier>();
    if (id && !id->isParam()) {
      const std::string origin_full_name = id->name();
      const auto [origin_db, origin_table] = inOperandTarget(*id);
      const bool is_cte_ref = origin_db.empty() && cte_scope.bindsInOperand(origin_table);
      if (!origin_table.empty() && !is_cte_ref) {
        if (auto hit = lookupStorageIntegrity(origin_db, origin_table, args)) {
          if (!check_only) operand = buildStorageIntegrityDerivedTable(*hit->table, args.storage_integrity());
          if (out_table_rewrites)
            (*out_table_rewrites)[origin_full_name] = hit->table->safe_table();
        } else {
          const auto outcome = applyDynamicRewrite(origin_db, origin_table, args, "IN");
          if (check_only) {
            if (outcome.status == DynamicRewriteStatus::InvalidRequest) noteUnresolved(origin_db, origin_table);
          } else if (outcome.status == DynamicRewriteStatus::OK) {
            operand = DB::makeRewriterAST<DB::ASTIdentifier>(
              std::vector<std::string>{outcome.physical_db, outcome.new_table});
            if (out_table_rewrites)
              (*out_table_rewrites)[origin_full_name] = outcome.physical_db + "." + outcome.new_table;
          } else if (outcome.status == DynamicRewriteStatus::OkRemote) {
            operand = DB::makeASTFunction("remote",
              DB::makeRewriterAST<DB::ASTLiteral>(outcome.remote_addr),
              DB::makeRewriterAST<DB::ASTLiteral>(outcome.physical_db),
              DB::makeRewriterAST<DB::ASTLiteral>(outcome.new_table),
              DB::makeRewriterAST<DB::ASTLiteral>(outcome.remote_user),
              DB::makeRewriterAST<DB::ASTLiteral>(outcome.remote_password));
            if (out_table_rewrites)
              (*out_table_rewrites)[origin_full_name] = outcome.physical_db + "." + outcome.new_table;
          } else {
            noteUnresolved(origin_db, origin_table);
          }
        }
      }
      // The left operand may hold subqueries of its own.
      dynamicRewriteWalk(fn->arguments->children[0], args, out_table_rewrites, cte_scope, first_unresolved);
      return;
    }
  }

  if (auto *table_expression = ast->as<DB::ASTTableExpression>()) {
    if (table_expression->database_and_table_name) {
      auto *identifier = table_expression->database_and_table_name->as<DB::ASTTableIdentifier>();
      if (identifier) {
        const std::string origin_full_name = identifier->name();
        const std::string origin_alias = identifier->tryGetAlias();

        // Bare reference to a CTE alias — leave it alone. Qualified
        // `db.alias` is a real table and falls through to the rewrite path.
        if (identifier->name_parts.size() == 1 && cte_scope.bindsTable(origin_full_name)) {
          return;
        }

        // The identity is the same one the collector reports: one part is a
        // table of the current database (a quoted `db2.x` is one table, never
        // tenant db2's x), two or more split at the first part. table_rewrites
        // keys stay the written name.
        const auto [origin_db, origin_table] = inOperandTarget(*identifier);

        if (auto hit = lookupStorageIntegrity(origin_db, origin_table, args)) {
          if (check_only) return;
          // Spec G: the SI physical mapping wins over database_map and remote
          // routing. Replace only the table node and never descend into the
          // synthesized hg_safe/hg_unsafe surface.
          auto subquery = buildStorageIntegrityDerivedTable(
            *hit->table, args.storage_integrity());
          table_expression->children.clear();
          table_expression->database_and_table_name = nullptr;
          table_expression->table_function = nullptr;
          table_expression->subquery = subquery;
          table_expression->children.push_back(subquery);
          subquery->setAlias(origin_alias.empty()
            ? origin_full_name : origin_alias);
          if (out_table_rewrites)
            (*out_table_rewrites)[origin_full_name] = hit->table->safe_table();
          return;
        }

        const auto outcome = applyDynamicRewrite(
          origin_db, origin_table, args, "SELECT");
        if (check_only) {
          if (outcome.status == DynamicRewriteStatus::InvalidRequest) noteUnresolved(origin_db, origin_table);
          return;
        }
        if (outcome.status == DynamicRewriteStatus::OK) {
          identifier->resetTable(outcome.physical_db, outcome.new_table);
          if (!origin_alias.empty()) {
            identifier->setAlias(origin_alias);
          } else {
            identifier->setAlias(origin_full_name);
          }
          if (out_table_rewrites) {
            (*out_table_rewrites)[origin_full_name] = identifier->name();
          }
        } else if (outcome.status == DynamicRewriteStatus::OkRemote) {
          // Remote upstream — replace the identifier with a remote() table
          // function call. Mirrors ASTReplaceTransformer's static
          // remote_table_map case so the SQL shape is identical regardless
          // of whether the remote routing came from static or dynamic args.
          auto addr  = DB::makeRewriterAST<DB::ASTLiteral>(outcome.remote_addr);
          auto db    = DB::makeRewriterAST<DB::ASTLiteral>(outcome.physical_db);
          auto tbl   = DB::makeRewriterAST<DB::ASTLiteral>(outcome.new_table);
          auto user  = DB::makeRewriterAST<DB::ASTLiteral>(outcome.remote_user);
          auto pass  = DB::makeRewriterAST<DB::ASTLiteral>(outcome.remote_password);
          table_expression->children.clear();
          table_expression->database_and_table_name = nullptr;
          table_expression->table_function =
            DB::makeASTFunction("remote", addr, db, tbl, user, pass);
          table_expression->children.push_back(table_expression->table_function);
          if (!origin_alias.empty()) {
            table_expression->table_function->setAlias(origin_alias);
          } else {
            table_expression->table_function->setAlias(origin_full_name);
          }
          if (out_table_rewrites) {
            // Record the (origin -> "physical.new_table") mapping. addr /
            // user / password are not part of the table-rewrites map; they
            // only live in `sql_after_rewrite` (matching the static
            // remote_table_map convention).
            (*out_table_rewrites)[origin_full_name] =
              outcome.physical_db.empty()
                ? outcome.new_table
                : outcome.physical_db + "." + outcome.new_table;
          }
        }
        // InvalidRequest: a qualified unmapped name is a lenient skip (the
        // caller's permission check owns it); an unqualified one is noted for
        // the callers' refusal.
        else {
          noteUnresolved(origin_db, origin_table);
        }
      }
    } else if (table_expression->subquery || table_expression->table_function) {
      // A subquery, and every SELECT nested in a table function's arguments:
      // view(SELECT …) is a data-only table function whose body is an
      // ordinary read (spec 2026-09-26 §5 table functions); the bodies of the
      // other table functions (viewIfPermitted, viewExplain, executable,
      // remote(…, view(…)), …) are rewritten as well, and T5 decides those
      // functions later. An outer CTE alias is visible only where ClickHouse
      // binds it: visitChildrenWithCTEVisibility drops it inside view(),
      // viewIfPermitted(), viewExplain() and executable() at any depth, so a
      // name there is a table and is rewritten. Elsewhere a CTE-bound name
      // stays a CTE reference.
      visitChildrenWithCTEVisibility(
        *ast, cte_scope,
        [&](const DB::ASTPtr &child, const CTEAliasScope &child_scope) {
          dynamicRewriteWalk(child, args, out_table_rewrites, child_scope, first_unresolved);
        });
    }
    return;
  }

  visitChildrenWithCTEVisibility(
    *ast, cte_scope,
    [&](const DB::ASTPtr &child, const CTEAliasScope &child_scope) {
      dynamicRewriteWalk(child, args, out_table_rewrites, child_scope, first_unresolved);
    });
}

// Spec 2026-09-26 §5 "Unqualified names that do not resolve" (rewriter-go
// #49): the first read-position table name with no database qualifier — the
// one-part dotted quoted `db1.t` included — that the dynamic walk cannot
// resolve through upstream_logical_database_in_context (empty, unmapped, or
// mapped to a missing remote upstream). ClickHouse would resolve it in the
// session's current database, the physical one. Decided by a check-only run
// of the rewrite walk itself (it mutates nothing), so the two cannot disagree
// about which names are tables; the aliases of caller-supplied CTE options are bound (they
// are inlined later) and their bodies are walked too.
std::optional<std::string> firstUnresolvedUnqualifiedTable(
  const DB::ASTPtr &ast,
  const rewriter::RewriteTableDynamicArgs &args,
  const std::vector<ParsedCTEASTMap> &option_ctes) {
  std::optional<std::string> first;
  CTEAliasScope scope;
  std::map<std::string, DB::ASTPtr> bodies;
  for (const auto &cte_map : option_ctes) {
    for (const auto &[alias, body] : cte_map) {
      scope.ctes.insert(alias);
      bodies.emplace(alias, body);
    }
  }
  dynamicRewriteWalk(ast, args, nullptr, scope, &first);
  for (const auto &[alias, body] : bodies) {
    if (first) break;
    if (body) dynamicRewriteWalk(body, args, nullptr, CTEAliasScope{}, &first);
  }
  return first;
}

void dynamicRewriteWalk(
  DB::ASTPtr ast,
  const rewriter::RewriteTableDynamicArgs &args,
  std::map<std::string, std::string> *out_table_rewrites) {
  dynamicRewriteWalk(ast, args, out_table_rewrites, CTEAliasScope{});
}

// SELECT-path fallback when the caller set `dynamic_args` but no
// `static_args`: walk every ASTTableExpression and apply the same
// dynamic construction the non-SELECT handlers use, via applyDynamicRewrite.
//
// Lenient on InvalidRequest — unlike INSERT/CREATE/etc. which target a
// single table, SELECT may legitimately reference system tables, CTE
// aliases, or DBs the caller didn't declare in `database_map` /
// `known_physical_databases`. A qualified unresolvable name is skipped (the
// caller enforces DB-access policy); an unqualified one never reaches this
// walk, because the SELECT paths refuse it first
// (rejectUnresolvedUnqualifiedTable).
void doDynamicSelectTableRewrite(
  DB::ASTPtr ast,
  const rewriter::RewriteTableDynamicArgs &args,
  std::map<std::string, std::string> *out_table_rewrites) {
  dynamicRewriteWalk(ast, args, out_table_rewrites);
  if (shouldDumpAst()) {
    std::cout << "after dynamic table name transformation " << ast->dumpTree() << std::endl;
  }
}

struct LimitAction {
  size_t limit = 0;
  bool force = false;
  size_t threshold = 0;
};

struct PendingSelectTransforms {
  std::vector<LimitAction> limits;
  std::vector<size_t> offsets;
  std::vector<DB::Setting> settings;

  bool empty() const {
    return limits.empty() && offsets.empty() && settings.empty();
  }

  void clear() {
    limits.clear();
    offsets.clear();
    settings.clear();
  }
};

void setSetting(DB::ASTSetQuery *settings, const DB::Setting &setting) {
  switch (setting.value_type) {
  case rewriter::SettingType::String:
    settings->changes.setSetting(std::string_view(setting.name), DB::Field(setting.string_value));
    break;
  case rewriter::SettingType::Int:
    settings->changes.setSetting(std::string_view(setting.name), DB::Field(setting.int_value));
    break;
  case rewriter::SettingType::Bool:
    settings->changes.setSetting(std::string_view(setting.name), DB::Field(setting.bool_value));
    break;
  case rewriter::SettingType::Uint64:
    settings->changes.setSetting(std::string_view(setting.name), DB::Field(setting.uint64_value));
    break;
  default:
    break;
  }
}

void applyPendingToSelect(DB::ASTSelectQuery *select, const PendingSelectTransforms &pending) {
  for (const auto &action : pending.limits) {
    if (action.limit == 0) continue;
    if (action.force) {
      select->setExpression(
        DB::ASTSelectQuery::Expression::LIMIT_LENGTH, DB::makeRewriterAST<DB::ASTLiteral>(action.limit));
      continue;
    }

    auto limit_expr = select->limitLength();
    if (limit_expr) {
      auto *limit_literal = limit_expr->as<DB::ASTLiteral>();
      if (limit_literal) {
        // UInt64 is a global typedef (clickHouse/base/base/types.h), not a
        // DB-namespace alias. `safeGet<DB::UInt64>()` fails to resolve from
        // namespace rewriter_handlers — the old call site in ASTTransformers.cc
        // worked only because it's inside `namespace DB`.
        size_t original_limit = limit_literal->value.safeGet<UInt64>();
        if (original_limit == 0 || original_limit > action.threshold) {
          select->setExpression(
            DB::ASTSelectQuery::Expression::LIMIT_LENGTH,
            DB::makeRewriterAST<DB::ASTLiteral>(action.limit));
        }
      }
    } else {
      select->setExpression(
        DB::ASTSelectQuery::Expression::LIMIT_LENGTH, DB::makeRewriterAST<DB::ASTLiteral>(action.limit));
    }
  }

  for (size_t offset : pending.offsets) {
    if (offset > 0) {
      select->setExpression(
        DB::ASTSelectQuery::Expression::LIMIT_OFFSET, DB::makeRewriterAST<DB::ASTLiteral>(offset));
    }
  }

  if (!pending.settings.empty()) {
    auto settings_expr = select->settings();
    DB::ASTSetQuery *settings = nullptr;
    if (settings_expr) {
      settings = settings_expr->as<DB::ASTSetQuery>();
    } else {
      auto new_settings = DB::makeRewriterAST<DB::ASTSetQuery>();
      new_settings->is_standalone = false;
      settings = new_settings.get();
      select->setExpression(DB::ASTSelectQuery::Expression::SETTINGS, new_settings);
    }
    if (settings) {
      for (const auto &setting : pending.settings) {
        setSetting(settings, setting);
      }
    }
  }
}

void applyPendingSelectTransforms(DB::ASTPtr ast, PendingSelectTransforms &pending) {
  if (pending.empty() || !ast) return;
  if (auto *select = ast->as<DB::ASTSelectQuery>()) {
    applyPendingToSelect(select, pending);
    return;
  }
  for (auto &child : ast->children) {
    applyPendingSelectTransforms(child, pending);
  }
}

// ---- GLOBAL JOIN / GLOBAL IN rewrite for cross-shard queries ------------
//
// ClickHouse ships an outer query plan to a remote shard whenever its FROM
// references a remote() / cluster() table function (or anything we expanded
// into one via `remote_table_map` / `logical_database_to_remote_upstream_index`).
// Anything *else* the query references — the other side of a JOIN, the
// right operand of an IN — must then be resolvable on that shard. Plain
// table identifiers aren't, since they only exist on the initiator. The
// canonical fix is GLOBAL JOIN / GLOBAL IN, which evaluates the local side
// once on the initiator and broadcasts the result as a temp table.
//
// Symmetric remote-vs-remote joins are left alone (per request) — the
// initiator's plan still works without a broadcast in that case, and
// forcing GLOBAL would cost an unnecessary temp-table round-trip.
//
// Runs *after* table-name rewrite, since `remote_table_map` /
// `logical_database_to_remote_upstream_index` rewrites can introduce new
// remote() calls the user didn't write themselves.

constexpr std::string_view kRemoteFunctionNames[] = {
  "remote", "remoteSecure", "cluster", "clusterAllReplicas",
};

bool isRemoteFunctionName(const std::string &name) {
  for (auto candidate : kRemoteFunctionNames) {
    if (candidate == name) return true;
  }
  return false;
}

// Any descendant ASTFunction with a remote-style name. We don't restrict to
// table-position ASTFunctions because `WHERE x IN remote(...)` parses as a
// free-standing function call on the IN operand, not wrapped in an
// ASTTableExpression — and we want that to count as a remote source too.
// Collisions with user-defined functions named exactly `remote` / `cluster`
// are theoretically possible but vanishingly rare in practice.
bool subtreeHasRemoteSource(const DB::IAST *node) {
  if (!node) return false;
  if (const auto *fn = node->as<DB::ASTFunction>()) {
    if (isRemoteFunctionName(fn->name)) return true;
  }
  for (const auto &ch : node->children) {
    if (subtreeHasRemoteSource(ch.get())) return true;
  }
  return false;
}

// Any descendant ASTTableExpression that names a real persistent table —
// i.e., a database_and_table_name that isn't a bare CTE-alias reference.
// Bare table-function calls like `numbers(10)` and `generateRandom()` are
// node-agnostic (available on any shard) and intentionally don't count.
bool subtreeHasLocalSource(const DB::IAST *node,
                           const CTEAliasScope &cte_scope) {
  if (!node) return false;
  if (const auto *te = node->as<DB::ASTTableExpression>()) {
    if (te->database_and_table_name) {
      if (const auto *id = te->database_and_table_name->as<DB::ASTTableIdentifier>()) {
        const auto full = id->name();
        const bool is_cte_ref =
          full.find('.') == std::string::npos && cte_scope.bindsTable(full);
        if (!is_cte_ref) return true;
      }
    }
  }
  bool found = false;
  visitChildrenWithCTEVisibility(
    *node, cte_scope,
    [&](const DB::ASTPtr &child, const CTEAliasScope &child_scope) {
      if (!found) found = subtreeHasLocalSource(child.get(), child_scope);
    });
  return found;
}

// Walk this select's body looking for IN-family functions whose right
// operand contains a local source; rename them to their `globalIn` /
// `globalNotIn` / etc. counterparts so the right side is broadcast from
// the initiator instead of resolved on each remote shard.
//
// Stops at ASTSubquery boundaries — nested SELECT bodies are processed by
// the outer bottom-up recursion at their own scope.
void forceGlobalInWalk(DB::IAST *node,
                       const CTEAliasScope &cte_scope) {
  if (!node) return;
  if (auto *fn = node->as<DB::ASTFunction>()) {
    static const std::unordered_map<std::string, std::string> kInToGlobal = {
      {"in",        "globalIn"},
      {"notIn",     "globalNotIn"},
      {"nullIn",    "globalNullIn"},
      {"notNullIn", "globalNotNullIn"},
    };
    auto it = kInToGlobal.find(fn->name);
    if (it != kInToGlobal.end() && fn->arguments &&
        fn->arguments->children.size() == 2) {
      const auto &right = fn->arguments->children[1];
      if (subtreeHasLocalSource(right.get(), cte_scope)) {
        fn->name = it->second;
      }
    }
  }
  for (auto &ch : node->children) {
    if (ch->as<DB::ASTSubquery>()) continue;
    forceGlobalInWalk(ch.get(), cte_scope);
  }
}

// Bottom-up walker. For each ASTSelectQuery encountered:
//   1. Scan its tables() left-to-right. If a JOIN element introduces a
//      remote/local mismatch with what's accumulated to its left and the
//      user didn't already pin locality (Unspecified), set Global.
//   2. If the select's FROM ships out (any remote source), rewrite IN-family
//      functions in the rest of its body that have a local right operand.
void forceGlobalForRemoteAsymmetryImpl(
  DB::IAST *node,
  const CTEAliasScope &cte_scope) {
  if (!node) return;

  const auto scope = visitChildrenWithCTEVisibility(
    *node, cte_scope,
    [&](const DB::ASTPtr &child, const CTEAliasScope &child_scope) {
      forceGlobalForRemoteAsymmetryImpl(child.get(), child_scope);
    });

  auto *select = node->as<DB::ASTSelectQuery>();
  if (!select) return;

  if (auto tables = select->tables()) {
    bool acc_remote = false;
    bool acc_local  = false;
    for (auto &el_ast : tables->children) {
      auto *el = el_ast->as<DB::ASTTablesInSelectQueryElement>();
      if (!el || !el->table_expression) continue;
      const bool this_remote =
        subtreeHasRemoteSource(el->table_expression.get());
      const bool this_local =
        subtreeHasLocalSource(el->table_expression.get(), scope);
      if (el->table_join) {
        if (auto *tj = el->table_join->as<DB::ASTTableJoin>()) {
          if (tj->locality == DB::JoinLocality::Unspecified) {
            const bool mixed =
              (acc_remote && this_local) || (acc_local && this_remote);
            if (mixed) tj->locality = DB::JoinLocality::Global;
          }
        }
      }
      acc_remote = acc_remote || this_remote;
      acc_local  = acc_local  || this_local;
    }
  }

  bool from_has_remote = false;
  if (auto tables = select->tables()) {
    from_has_remote = subtreeHasRemoteSource(tables.get());
  }
  if (from_has_remote) {
    auto tables_ptr = select->tables();
    auto with_ptr   = select->with();
    for (auto &child : select->children) {
      if (child.get() == tables_ptr.get()) continue;
      if (child.get() == with_ptr.get())   continue;
      forceGlobalInWalk(child.get(), scope);
    }
  }
}

void forceGlobalForRemoteAsymmetry(DB::ASTPtr ast) {
  forceGlobalForRemoteAsymmetryImpl(ast.get(), CTEAliasScope{});
}

// Refuses a dynamic-mode SELECT body naming an unresolved unqualified table
// (firstUnresolvedUnqualifiedTable) with `code`. The refusal is the
// statement's final answer, so it carries an empty table_rewrites map.
bool rejectUnresolvedUnqualifiedTable(
  const DB::ASTPtr &ast,
  const TableRewriteSelection &selection,
  const std::vector<ParsedCTEASTMap> &option_ctes,
  rewriter::RewriteSQLResponse *response,
  rewriter::RewriteCode code) {
  if (!ast || selection.mode != TableRewriteMode::Dynamic) return false;
  const auto name = firstUnresolvedUnqualifiedTable(ast, *selection.dynamic_args, option_ctes);
  if (!name) return false;
  response->clear_table_rewrites();
  response->set_code(code);
  response->set_message(unresolvedUnqualifiedTableMessage(*name));
  return true;
}

} // namespace

DB::ASTPtr applyRewriteOptions(
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options,
  DB::ASTPtr ast,
  const std::vector<ParsedCTEASTMap> *preparsed_cte_ast_maps,
  std::map<std::string, std::string> *out_table_rewrites) {

  std::map<std::string, DB::CommonTableExpr> cte_map_;
  std::unordered_map<std::string, std::string> table_name_map_;
  std::unordered_map<std::string, DB::RemoteTable> remote_table_map_;
  std::unordered_map<std::string, DB::TableWithDatabase> table_with_database_map_;
  bool has_table_name_written = false;
  // Last-wins stash for the SELECT-path `dynamic_args` fallback: when a
  // TableNameRewrite option carries `dynamic_args` but no `static_args`,
  // we apply the dynamic rewrite directly and remember the args so the
  // post-CTE re-apply (which mirrors the three-map path's re-apply) covers
  // any tables introduced by CTE inlining. Mutually exclusive with the
  // three-map state above — set the one that matches the latest option.
  std::optional<rewriter::RewriteTableDynamicArgs> last_dynamic_args;
  size_t cte_option_index = 0;
  PendingSelectTransforms pending_select;
  auto flushPendingSelect = [&]() {
    if (pending_select.empty()) return;
    applyPendingSelectTransforms(ast, pending_select);
    if (shouldDumpAst()) {
      std::cout << "after select clause transformations " << ast->dumpTree() << std::endl;
    }
    pending_select.clear();
  };

  for (const auto &option : options) {
    switch (option.op()) {
    case rewriter::RewriteOp::LimitRewrite: {
      auto limit = option.limit_args().replace_limit().replace_to();
      auto force_limit = option.limit_args().force_limit();
      auto threshold = option.limit_args().replace_limit().threshold();
      if (force_limit) {
        pending_select.limits.push_back(
          LimitAction{static_cast<size_t>(force_limit), true, 0});
      } else {
        pending_select.limits.push_back(
          LimitAction{static_cast<size_t>(limit), false, static_cast<size_t>(threshold)});
      }
      break;
    }
    case rewriter::RewriteOp::TableNameRewrite: {
      // Unified static→dynamic precedence (matches the non-SELECT handlers):
      //   1. Use `static_args` whenever it's set (even with every map
      //      empty — an explicit "static-only, no rewrite" intent).
      //   2. Else fall back to `dynamic_args` (lenient walk).
      //   3. Else skip silently.
      const auto &args = option.table_name_args();
      if (args.has_static_args()) {
        const auto &select_args = args.static_args();
        // table_name
        {
          const auto &table_name_map = select_args.table_map();
          table_name_map_.clear();
          table_name_map_.reserve(table_name_map.size());
          for (auto &kv : table_name_map) { table_name_map_[kv.first] = kv.second; }
        }
        // remote_table
        {
          const auto &remote_table_map = select_args.remote_table_map();
          remote_table_map_.clear();
          remote_table_map_.reserve(remote_table_map.size());
          for (auto &kv : remote_table_map) {
            DB::RemoteTable remote_table;
            remote_table.addr = kv.second.addr();
            remote_table.database_name = kv.second.database();
            remote_table.table_name = kv.second.table();
            remote_table.user_name = kv.second.user();
            remote_table.password = kv.second.password();
            remote_table_map_[kv.first] = remote_table;
          }
        }
        // table_with_database
        {
          const auto &table_with_database_map = select_args.table_with_database_map();
          table_with_database_map_.clear();
          table_with_database_map_.reserve(table_with_database_map.size());
          for (auto &kv : table_with_database_map) {
            DB::TableWithDatabase table_with_database;
            table_with_database.database_name = kv.second.database();
            table_with_database.table_name = kv.second.table();
            table_with_database_map_[kv.first] = table_with_database;
          }
        }
        doTableNameRewrite(ast, table_name_map_, remote_table_map_, table_with_database_map_,
                           out_table_rewrites);
        has_table_name_written = true;
        // The two paths are mutually exclusive — drop any stashed dynamic
        // args from a prior option so the post-CTE re-apply stays on the
        // three-map branch.
        last_dynamic_args.reset();
      } else if (args.has_dynamic_args()) {
        const auto &dyn_args = args.dynamic_args();
        doDynamicSelectTableRewrite(ast, dyn_args, out_table_rewrites);
        last_dynamic_args = dyn_args;
        // Symmetric reset: clear three-map state so the post-CTE re-apply
        // doesn't double-apply a stale set of mappings.
        table_name_map_.clear();
        remote_table_map_.clear();
        table_with_database_map_.clear();
        has_table_name_written = false;
      }
      break;
    }
    case rewriter::RewriteOp::OffsetRewrite: {
      auto offset = option.offset_args().offset();
      pending_select.offsets.push_back(static_cast<size_t>(offset));
      break;
    }
    case rewriter::RewriteOp::SettingsRewrite: {
      const auto &settings = option.settings_args().settings();
      for (const auto &setting : settings) {
        DB::Setting s;
        s.name = setting.key();
        s.value_type = setting.type();
        switch (setting.type()) {
        case rewriter::SettingType::String: {
          s.string_value = setting.string_value();
          break;
        }
        case rewriter::SettingType::Int: {
          s.int_value = setting.int_value();
          break;
        }
        case rewriter::SettingType::Bool: {
          s.bool_value = setting.bool_value();
          break;
        }
        case rewriter::SettingType::Uint64: {
          s.uint64_value = setting.uint64_value();
          break;
        }
        default: break;
        }
        pending_select.settings.push_back(s);
      }
      break;
    }
    case rewriter::RewriteOp::CommonTableExprRewrite: {
      flushPendingSelect();
      DB::ASTRewriteCTETransformer cte_transformer = DB::ASTRewriteCTETransformer();
      const auto &ctes = option.common_table_expr_args().cte_map();
      cte_map_.clear();
      for (auto &kv : ctes) {
        cte_map_[kv.first] = {kv.second.alias(), kv.second.sql()};
      }
      const bool has_preparsed =
        preparsed_cte_ast_maps && cte_option_index < preparsed_cte_ast_maps->size();
      auto cte_ast_map = has_preparsed
        ? (*preparsed_cte_ast_maps)[cte_option_index]
        : DB::parseCTEMapToAST(cte_map_);
      ++cte_option_index;

      cte_transformer.setCTEASTMap(cte_ast_map);
      cte_transformer.setCTEMap(cte_map_);
      cte_transformer.transform(ast);

      // If a TableNameRewrite was applied earlier in this option chain,
      // re-apply it now — CTE inlining can introduce new SELECT subqueries
      // that reference tables which weren't visible to the first pass. The
      // two TableNameRewrite branches are mutually exclusive (see the
      // TableNameRewrite case above), so at most one of these fires.
      if (has_table_name_written) {
        doTableNameRewrite(ast, table_name_map_, remote_table_map_, table_with_database_map_,
                           out_table_rewrites);
      } else if (last_dynamic_args.has_value()) {
        doDynamicSelectTableRewrite(ast, *last_dynamic_args, out_table_rewrites);
      }
      break;
    }
    default: break;
    }
  }

  flushPendingSelect();

  return std::move(ast);
}

void handleSelectQuery(DB::ASTPtr ast,
                       const rewriter::RewriteSQLRequest *request,
                       rewriter::RewriteSQLResponse *response) {
  if (shouldDumpAst()) {
    std::cout << "before transformation " << ast->dumpTree() << std::endl;
  }
  // Spec 2026-09-26 T7: a session SET is the one modelled pass-through while
  // the SI surface is inactive (the preflight refused it otherwise). It names
  // no table and is not a SELECT.
  if (ast->as<DB::ASTSetQuery>()
      && findActiveTableRewrite(request->options()).mode == TableRewriteMode::Dynamic) {
    if (rejectQuerySettings(ast, response, &request->sql())) return;
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    return;
  }

  AccessedTables originals;
  collectAccessedTablePairsFromAST(ast, originals);
  std::vector<ParsedCTEASTMap> preparsed_cte_ast_maps;

  // Pre-scan CTE to collect the original table names accessed in the CTE subquery
  // (without altering the AST). Also captures CTE sub-queries that fail to parse
  // so we can surface them to the caller via response.failed_cte_aliases.
  for (const auto &option : request->options()) {
    if (option.op() != rewriter::RewriteOp::CommonTableExprRewrite) continue;
    std::map<std::string, DB::CommonTableExpr> cte_map;
    for (const auto &kv : option.common_table_expr_args().cte_map()) {
      cte_map[kv.first] = {kv.second.alias(), kv.second.sql()};
    }
    std::vector<std::string> failed_aliases;
    auto cte_ast_map = DB::parseCTEMapToAST(cte_map, &failed_aliases);
    for (const auto &a : failed_aliases) response->add_failed_cte_aliases(a);
    for (const auto &kv : cte_ast_map) {
      // CTE alias was parsed as a bare-table reference in the outer AST;
      // drop it so the CTE body's actual tables stand in for it.
      eraseAccessedByWrittenName(originals, kv.first);
      collectAccessedTablePairsFromAST(kv.second, originals);
    }
    preparsed_cte_ast_maps.push_back(std::move(cte_ast_map));
  }

  // Resolve each originally-accessed table against the active TableNameRewrite
  // selection so callers get logical/physical/is_remote without having to
  // replay the resolution logic themselves. Done after the option-pre-scan but
  // before applyRewriteOptions mutates the AST — the selection itself is
  // option-config, not AST state, so there's no ordering hazard.
  const auto selection = findActiveTableRewrite(request->options());

  // Classification is established before any reject or AST mutation. This is
  // the downstream fail-closed proof for unauthorized logical SI targets and
  // direct protocol-owned physical namespace attempts.
  // A one-part IN operand read through an SI physical execution context is
  // named by the namespace policy below (its ref is kept), which reports the
  // physical table as rewriter-go does; drop its source entry so the target
  // check does not answer first with a differently spelled message.
  if (selection.mode == TableRewriteMode::Dynamic
      && isStorageIntegrityPhysicalDatabase(
        storageIntegrityExecutionDatabase(*selection.dynamic_args), *selection.dynamic_args)) {
    std::erase_if(originals, [](const auto &entry) {
      return entry.second.in_operand && entry.second.original_db.empty();
    });
  }
  populateAccessedTables(originals, selection, response);
  if (!validateStorageIntegritySelectTargets(originals, selection, response))
    return;
  if (selection.mode == TableRewriteMode::Dynamic) {
    const auto &args = *selection.dynamic_args;
    // Qualified IN operands are read sources in `originals` now (T4), keyed by
    // (database, table) identity so no spelling twin can hide one; the SI
    // target check above classified them. A one-part operand keeps its ref: it
    // reads through the current database, which must be resolvable and not an
    // SI physical one.
    auto refs = collectStorageIntegrityNamespaceRefs(ast, InOperandReading::SelectRewrite);
    // Caller-supplied CTE bodies are collected and rewritten as part of this
    // query too, so their namespace refs are governed with it.
    for (const auto &cte_map : preparsed_cte_ast_maps) {
      for (const auto &[alias, body] : cte_map) {
        auto body_refs = collectStorageIntegrityNamespaceRefs(body, InOperandReading::SelectRewrite);
        refs.insert(refs.end(), std::make_move_iterator(body_refs.begin()),
                    std::make_move_iterator(body_refs.end()));
      }
    }
    refs = withoutQualifiedInOperandRefs(std::move(refs));
    if (rejectStorageIntegrityNamespaces(
          response, refs, args, rewriter::RewriteCode::RewriteError)) return;
    // T5 and R5 while the SI surface is active run after the SI namespace
    // policy.
    if (storageIntegritySurfaceActive(args)
        && (rejectDisallowedCarriers(ast, response) || rejectQuerySettings(ast, response, &request->sql()))) return;
    if (hasUnsupportedStorageIntegrityWrapper(ast, args)) {
      response->set_code(rewriter::RewriteCode::RewriteError);
      response->set_message(std::string(kStorageIntegrityWrapperMessage));
      return;
    }
    if (responseTouchesStorageIntegrity(*response)
        && astReferencesIdentifierInScope(ast, reservedRowIdColumn(args), args)) {
      const std::string rid = reservedRowIdColumn(args);
      response->set_code(rewriter::RewriteCode::RewriteError);
      response->set_message("reserved column " + rid + " is not addressable");
      return;
    }
  }

  // T6 after the SI checks (rewriter-go rewriteSelectCore order), before the
  // table rewrite.
  if (!rewriteHasColumnInTable(ast, selection, response)) return;
  // An unresolved unqualified name, after every other check of the walk.
  if (rejectUnresolvedUnqualifiedTable(ast, selection, preparsed_cte_ast_maps, response,
                                       rewriter::RewriteCode::InvalidRewriteRequest)) return;

  std::map<std::string, std::string> table_rewrites;
  ast = applyRewriteOptions(request->options(), ast, &preparsed_cte_ast_maps, &table_rewrites);

  // Cross-shard correctness pass: any JOIN/IN that mixes a remote() source
  // with a local-table reference would otherwise fail on the remote shard
  // with UNKNOWN_TABLE. Done after option application so static- and
  // dynamic-mode rewrites that *introduce* remote() calls are visible.
  forceGlobalForRemoteAsymmetry(ast);
  if (shouldDumpAst()) {
    std::cout << "after forceGlobalForRemoteAsymmetry " << ast->dumpTree() << std::endl;
  }

  auto *resp_rewrites = response->mutable_table_rewrites();
  for (const auto &[k, v] : table_rewrites) {
    // Only record entries where the rewrite actually changed the name —
    // per-handler contract documented on RewriteSQLResponse.table_rewrites.
    if (k != v) {
      (*resp_rewrites)[k] = v;
    }
  }
  std::string sql = formatAst(ast);
  if (selection.mode == TableRewriteMode::Dynamic
      && responseTouchesStorageIntegrity(*response)) {
    sql = restoreStorageIntegrityProjectionSyntax(
      std::move(sql), reservedRowIdColumn(*selection.dynamic_args));
  }
  setSuccessResponse(response, sql, rewriter::STATEMENT_TYPE_SELECT);
}

bool rejectEmbeddedStorageIntegrityRead(
  DB::ASTPtr body,
  const TableRewriteSelection &selection,
  rewriter::RewriteSQLResponse *response,
  rewriter::RewriteCode code,
  bool allow_logical_reads,
  bool *rejected_table_source,
  bool r2_position,
  bool rewritten_body) {
  if (rejected_table_source) *rejected_table_source = false;
  if (!body || selection.mode != TableRewriteMode::Dynamic
      || !storageIntegritySurfaceActive(*selection.dynamic_args)) {
    return false;
  }
  const auto &args = *selection.dynamic_args;
  // An R2 position (a column / constraint expression, a mutation, an ALTER
  // action, a VALUES row) binds no WITH name (cte_scope.h). A rewritten body
  // reads a one-part name whole, as the SELECT handler does.
  for (const auto &event : collectStorageIntegrityReadRefs(
         body, r2_position ? CTEBinding::Unbound : CTEBinding::Bound,
         rewritten_body && !r2_position ? InOperandReading::SplitAndWhole : InOperandReading::Split)) {
    if (event.kind == StorageIntegrityReadRef::Kind::Table) {
      // A rewritten body reads a one-part dotted name (`db1.t` quoted) as one
      // table of the current database, never the SI table db1.t.
      const AccessedTablePair pair = rewritten_body && event.split_one_part
        ? AccessedTablePair{"", event.database + "." + event.table}
        : AccessedTablePair{event.database, event.table};
      if (auto hit = lookupStorageIntegrity(
            pair.original_db, pair.original_table, args)) {
        std::string logical;
        const bool authorized = authorizeStorageIntegrityLogical(pair.original_db, args, &logical);
        // The derived read. A split one-part name (`db1.t`) is one table of
        // the current database, never the derived read, and a table inside a
        // table function other than view() is not admitted before T5 decides
        // that function; both stay refused.
        if (allow_logical_reads && authorized && !event.split_one_part
            && !event.inside_non_view_table_function) continue;
        if (rejected_table_source) *rejected_table_source = true;
        recordStorageIntegrityPairUnique(response, pair, selection);
        if (!authorized) {
          response->set_code(rewriter::RewriteCode::InvalidRewriteRequest);
          response->set_message(storageIntegrityUnauthorizedMessage(logical));
        } else {
          response->set_code(code);
          response->set_message(storageIntegrityReferenceRejectMessage(hit->logical_key));
        }
        return true;
      }
      const std::string physical = pair.original_db.empty()
        ? storageIntegrityExecutionDatabase(args) : pair.original_db;
      if (isStorageIntegrityPhysicalDatabase(physical, args)) {
        if (rejected_table_source) *rejected_table_source = true;
        recordStorageIntegrityPairUnique(response, pair, selection);
        response->set_code(code);
        response->set_message(storageIntegrityPhysicalRejectMessage(
          physical + "." + pair.original_table));
        return true;
      }
      continue;
    }
    auto ref = event.namespace_ref;
    // The derived read of an Active IN operand is admitted only where a
    // logical Active read is (an INSERT into an SI target keeps the refusal).
    if (!allow_logical_reads) ref.rewritten_read = false;
    if (rejectStorageIntegrityNamespaces(response, {ref}, args, code)) return true;
  }
  return false;
}

void rewriteEmbeddedViewBody(
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options,
  DB::ASTPtr body,
  rewriter::RewriteSQLResponse *response) {
  rewriteEmbeddedBody(options, body, response, EmbeddedBodyKind::View);
}

void rewriteEmbeddedBody(
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options,
  DB::ASTPtr body,
  rewriter::RewriteSQLResponse *response,
  EmbeddedBodyKind kind) {
  if (!body) return;
  // A CREATE VIEW / MATERIALIZED VIEW body is a stored view body for every
  // walk below, which reaches it without the CREATE node (cte_scope.h). An
  // INSERT … SELECT / CTAS body follows the plain-SELECT rule.
  const StoredViewBodyGuard stored_view(kind == EmbeddedBodyKind::View ? body.get() : nullptr);

  // Collect the body's accessed tables from the pre-rewrite AST.
  AccessedTables originals;
  collectAccessedTablePairsFromAST(body, originals);

  // Pre-scan CTE options so failed_cte_aliases and CTE-body table
  // collection work for the view body too (mirrors handleSelectQuery).
  std::vector<ParsedCTEASTMap> preparsed_cte_ast_maps;
  for (const auto &option : options) {
    if (option.op() != rewriter::RewriteOp::CommonTableExprRewrite) continue;
    std::map<std::string, DB::CommonTableExpr> cte_map;
    for (const auto &kv : option.common_table_expr_args().cte_map()) {
      cte_map[kv.first] = {kv.second.alias(), kv.second.sql()};
    }
    std::vector<std::string> failed_aliases;
    auto cte_ast_map = DB::parseCTEMapToAST(cte_map, &failed_aliases);
    for (const auto &a : failed_aliases) response->add_failed_cte_aliases(a);
    for (const auto &kv : cte_ast_map) {
      eraseAccessedByWrittenName(originals, kv.first);
      collectAccessedTablePairsFromAST(kv.second, originals);
    }
    preparsed_cte_ast_maps.push_back(std::move(cte_ast_map));
  }

  const auto selection = findActiveTableRewrite(options);

  populateAccessedTables(originals, selection, response);
  if (!validateStorageIntegritySelectTargets(
        originals, selection, response,
        rewriter::RewriteCode::UnsupportedStatement)) return;
  if (selection.mode == TableRewriteMode::Dynamic) {
    const auto &args = *selection.dynamic_args;
    // Every body keeps the IN-operand namespace policy (a one-part operand
    // read whole, as the SELECT handler reads it); a view body may not
    // capture an SI read either, while an INSERT … SELECT / CTAS body reads
    // an Active FROM / JOIN table or IN operand through the derived read
    // (spec 2026-09-26 T4).
    const bool view = kind == EmbeddedBodyKind::View;
    if (rejectStorageIntegrityNamespaces(
          response, collectStorageIntegrityNamespaceRefs(body, InOperandReading::SplitAndWhole), args,
          rewriter::RewriteCode::UnsupportedStatement)) return;
    if (!view) {
      recordLogicalStorageIntegrityRewrites(originals, args, response);
    } else if (const std::string key = recordLogicalStorageIntegrityRewrites(
          originals, args, response);
        !key.empty()) {
      response->set_code(rewriter::RewriteCode::UnsupportedStatement);
      response->set_message(storageIntegrityReferenceRejectMessage(key));
      return;
    }
    if (hasUnsupportedStorageIntegrityWrapper(body, args)) {
      response->set_code(rewriter::RewriteCode::UnsupportedStatement);
      response->set_message(std::string(kStorageIntegrityWrapperMessage));
      return;
    }
    // A body that reads an Active table through the derived read may not
    // address the reserved row-id column, exactly as the SELECT handler
    // refuses it. Keyed on the body's own sources, so an ordinary source
    // under an Active INSERT target is unaffected.
    const bool body_reads_si = std::any_of(originals.begin(), originals.end(), [&](const auto &entry) {
      return lookupStorageIntegrity(entry.second.original_db, entry.second.original_table, args).has_value();
    });
    if (!view && body_reads_si && astReferencesIdentifierInScope(body, reservedRowIdColumn(args), args)) {
      response->set_code(rewriter::RewriteCode::UnsupportedStatement);
      response->set_message("reserved column " + reservedRowIdColumn(args) + " is not addressable");
      return;
    }
  }

  const auto body_code = kind == EmbeddedBodyKind::View ? rewriter::RewriteCode::InvalidRewriteRequest
                                                        : rewriter::RewriteCode::UnsupportedStatement;
  if (!rewriteHasColumnInTable(body, selection, response, body_code)) return;
  // The write statement's own targets were resolved first; this refusal is
  // the statement's final answer, so it drops their table_rewrites too.
  if (rejectUnresolvedUnqualifiedTable(body, selection, preparsed_cte_ast_maps, response, body_code)) return;

  std::map<std::string, std::string> table_rewrites;
  body = applyRewriteOptions(options, body, &preparsed_cte_ast_maps,
                             &table_rewrites);

  // Cross-shard correctness pass — a view body that joins a remote() source
  // with a local table would otherwise fail on the remote shard.
  forceGlobalForRemoteAsymmetry(body);

  auto *resp_rewrites = response->mutable_table_rewrites();
  for (const auto &[k, v] : table_rewrites) {
    if (k != v) (*resp_rewrites)[k] = v;
  }
}

} // namespace rewriter_handlers
