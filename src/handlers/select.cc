// src/handlers/select.cc
#include "handlers/select.h"

#include <Core/Field.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSetQuery.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "ASTTransformers.h"
#include "handlers/common.h"

namespace rewriter_handlers {

namespace {

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
      // SELECT-path rewrites live under `select_stmt_args`; `general_args`
      // is only consumed by the non-SELECT handlers. If this option
      // carries no select_stmt_args we skip it silently — the caller may
      // have set only general_args for a write in the same request.
      if (!option.table_name_args().has_select_stmt_args()) break;
      const auto &select_args = option.table_name_args().select_stmt_args();
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

      // If the table name has been rewritten previously, apply it again after the CTE rewrite.
      if (has_table_name_written) {
        doTableNameRewrite(ast, table_name_map_, remote_table_map_, table_with_database_map_,
                           out_table_rewrites);
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

  std::unordered_set<std::string> original_accessed_table_names_set;
  DB::collectTableNamesFromAST(ast, original_accessed_table_names_set);
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
      if (auto it = original_accessed_table_names_set.find(kv.first);
          it != original_accessed_table_names_set.end()) {
        original_accessed_table_names_set.erase(it);
      }
      DB::collectTableNamesFromAST(kv.second, original_accessed_table_names_set);
    }
    preparsed_cte_ast_maps.push_back(std::move(cte_ast_map));
  }

  std::map<std::string, std::string> table_rewrites;
  ast = applyRewriteOptions(request->options(), ast, &preparsed_cte_ast_maps, &table_rewrites);

  for (const auto &name : original_accessed_table_names_set) {
    response->add_original_accessed_table_names(name);
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
