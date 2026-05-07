// src/handlers/select.cc
#include "handlers/select.h"

#include <Core/Field.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSetQuery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ASTWithElement.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "ASTTransformers.h"
#include "handlers/common.h"
#include "handlers/name_rewrite.h"

namespace rewriter_handlers {

namespace {

struct AccessedTablePair {
  std::string original_db;     // empty when the SQL didn't qualify the table.
  std::string original_table;
};

// Collect bare CTE aliases declared by the WITH clause of a SELECT into
// `out_aliases`. Returns true when at least one alias was added — the caller
// uses this to decide whether to fork a new scope set on the recursion.
bool collectInlineCTEAliases(
  const DB::ASTSelectQuery *select,
  std::unordered_set<std::string> &out_aliases) {
  if (!select) return false;
  const auto with_expr = const_cast<DB::ASTSelectQuery *>(select)->with();
  if (!with_expr) return false;
  bool added = false;
  for (const auto &ch : with_expr->children) {
    if (const auto *we = ch->as<DB::ASTWithElement>()) {
      if (!we->name.empty()) {
        out_aliases.insert(we->name);
        added = true;
      }
    }
  }
  return added;
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
  std::map<std::string, AccessedTablePair> &out,
  const std::unordered_set<std::string> &cte_scope) {
  if (!node) return;

  std::unordered_set<std::string> extended;
  const std::unordered_set<std::string> *scope = &cte_scope;
  if (const auto *select = node->as<DB::ASTSelectQuery>()) {
    extended = cte_scope;
    if (collectInlineCTEAliases(select, extended)) {
      scope = &extended;
    }
  }

  if (auto *te = node->as<DB::ASTTableExpression>()) {
    if (te->database_and_table_name) {
      if (auto *id = te->database_and_table_name->as<DB::ASTTableIdentifier>()) {
        const auto full = id->name();  // "db.table" or bare "table"
        if (!full.empty()) {
          const bool is_cte_ref =
            full.find('.') == std::string::npos && scope->count(full) > 0;
          if (!is_cte_ref) {
            AccessedTablePair p;
            // Match the split that dynamicRewriteWalk and the table_rewrites
            // map keys use — first-dot split, with everything before as db.
            if (const auto dot = full.find('.'); dot != std::string::npos) {
              p.original_db = full.substr(0, dot);
              p.original_table = full.substr(dot + 1);
            } else {
              p.original_table = full;
            }
            out.emplace(full, std::move(p));
          }
        }
      }
    }
  }
  for (auto &ch : node->children) { collectAccessedTablePairsFromAST(ch, out, *scope); }
}

void collectAccessedTablePairsFromAST(
  const DB::ASTPtr &node,
  std::map<std::string, AccessedTablePair> &out) {
  static const std::unordered_set<std::string> kEmpty;
  collectAccessedTablePairsFromAST(node, out, kEmpty);
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

void dynamicRewriteWalk(
  DB::ASTPtr ast,
  const rewriter::RewriteTableDynamicArgs &args,
  std::map<std::string, std::string> *out_table_rewrites,
  const std::unordered_set<std::string> &cte_scope) {
  if (!ast) return;

  // Extend the CTE scope when entering a SELECT with a WITH clause so that
  // bare references to those aliases inside the SELECT body aren't rewritten
  // as if they were real tables. We fork a local copy (rather than mutate the
  // caller's set) so siblings of this SELECT don't see aliases that only
  // belong to this subtree.
  std::unordered_set<std::string> extended;
  const std::unordered_set<std::string> *scope = &cte_scope;
  if (const auto *select = ast->as<DB::ASTSelectQuery>()) {
    extended = cte_scope;
    if (collectInlineCTEAliases(select, extended)) {
      scope = &extended;
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
        if (origin_full_name.find('.') == std::string::npos &&
            scope->count(origin_full_name) > 0) {
          return;
        }

        // Split "db.table" → (db, table); bare "table" → ("", table). Matches
        // the key shape that recordTableRewrite / table_rewrites use.
        std::string origin_db;
        std::string origin_table;
        if (const auto dot = origin_full_name.find('.'); dot != std::string::npos) {
          origin_db = origin_full_name.substr(0, dot);
          origin_table = origin_full_name.substr(dot + 1);
        } else {
          origin_table = origin_full_name;
        }

        const auto outcome = applyDynamicRewrite(
          origin_db, origin_table, args, "SELECT");
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
          auto addr  = std::make_shared<DB::ASTLiteral>(outcome.remote_addr);
          auto db    = std::make_shared<DB::ASTLiteral>(outcome.physical_db);
          auto tbl   = std::make_shared<DB::ASTLiteral>(outcome.new_table);
          auto user  = std::make_shared<DB::ASTLiteral>(outcome.remote_user);
          auto pass  = std::make_shared<DB::ASTLiteral>(outcome.remote_password);
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
        // InvalidRequest: lenient skip — see doDynamicSelectTableRewrite
        // doc-comment for why SELECT swallows unresolvable identifiers.
      }
    } else if (table_expression->subquery) {
      for (auto &child : ast->children) {
        dynamicRewriteWalk(child, args, out_table_rewrites, *scope);
      }
    }
    return;
  }

  for (auto &child : ast->children) {
    dynamicRewriteWalk(child, args, out_table_rewrites, *scope);
  }
}

void dynamicRewriteWalk(
  DB::ASTPtr ast,
  const rewriter::RewriteTableDynamicArgs &args,
  std::map<std::string, std::string> *out_table_rewrites) {
  static const std::unordered_set<std::string> kEmpty;
  dynamicRewriteWalk(ast, args, out_table_rewrites, kEmpty);
}

// SELECT-path fallback when the caller set `dynamic_args` but no
// `static_args`: walk every ASTTableExpression and apply the same
// dynamic construction the non-SELECT handlers use, via applyDynamicRewrite.
//
// Lenient on InvalidRequest — unlike INSERT/CREATE/etc. which target a
// single table, SELECT may legitimately reference system tables, CTE
// aliases, or DBs the caller didn't declare in `database_map` /
// `known_physical_databases`. Silently skipping unresolvable identifiers
// matches the three-map path's behavior of leaving unmatched names alone;
// the caller is expected to enforce DB-access policy elsewhere.
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
        DB::ASTSelectQuery::Expression::LIMIT_LENGTH, std::make_shared<DB::ASTLiteral>(action.limit));
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
            std::make_shared<DB::ASTLiteral>(action.limit));
        }
      }
    } else {
      select->setExpression(
        DB::ASTSelectQuery::Expression::LIMIT_LENGTH, std::make_shared<DB::ASTLiteral>(action.limit));
    }
  }

  for (size_t offset : pending.offsets) {
    if (offset > 0) {
      select->setExpression(
        DB::ASTSelectQuery::Expression::LIMIT_OFFSET, std::make_shared<DB::ASTLiteral>(offset));
    }
  }

  if (!pending.settings.empty()) {
    auto settings_expr = select->settings();
    DB::ASTSetQuery *settings = nullptr;
    if (settings_expr) {
      settings = settings_expr->as<DB::ASTSetQuery>();
    } else {
      auto new_settings = std::make_shared<DB::ASTSetQuery>();
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

  std::map<std::string, AccessedTablePair> originals;
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
      originals.erase(kv.first);
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

  std::map<std::string, std::string> table_rewrites;
  ast = applyRewriteOptions(request->options(), ast, &preparsed_cte_ast_maps, &table_rewrites);

  for (const auto &[_, pair] : originals) {
    auto *entry = response->add_original_accessed_tables();
    entry->set_original_database(pair.original_db);
    entry->set_original_table(pair.original_table);
    const auto resolution = resolveAccessedTable(
      pair.original_db, pair.original_table, selection);
    entry->set_logical_database(resolution.logical_database);
    entry->set_physical_database(resolution.physical_database);
    entry->set_is_remote(resolution.is_remote);
  }
  auto *resp_rewrites = response->mutable_table_rewrites();
  for (const auto &[k, v] : table_rewrites) {
    // Only record entries where the rewrite actually changed the name —
    // per-handler contract documented on RewriteSQLResponse.table_rewrites.
    if (k != v) {
      (*resp_rewrites)[k] = v;
    }
  }
  setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_SELECT);
}

} // namespace rewriter_handlers
