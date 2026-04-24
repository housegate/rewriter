// src/handlers/select.cc
#include "handlers/select.h"

#include <iostream>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "ASTTransformers.h"
#include "handlers/common.h"

namespace rewriter_handlers {

namespace {

void doTableNameRewrite(
  DB::ASTPtr ast,
  const std::map<std::string, std::string> &table_name_map,
  const std::map<std::string, DB::RemoteTable> &remote_table_map,
  const std::map<std::string, DB::TableWithDatabase> &table_with_database_map) {
  DB::ASTReplaceTransformer transformer;
  transformer.setTableNameMap(table_name_map);
  transformer.setRemoteTableMap(remote_table_map);
  transformer.setTableWithDatabaseMap(table_with_database_map);
  transformer.transform(ast);
  std::cout << "after table name transformation " << ast->dumpTree() << std::endl;
}

} // namespace

DB::ASTPtr applyRewriteOptions(
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options,
  DB::ASTPtr ast) {

  std::map<std::string, DB::CommonTableExpr> cte_map_;
  std::map<std::string, std::string> table_name_map_;
  std::map<std::string, DB::RemoteTable> remote_table_map_;
  std::map<std::string, DB::TableWithDatabase> table_with_database_map_;
  bool has_table_name_written = false;

  for (const auto &option : options) {
    switch (option.op()) {
    case rewriter::RewriteOp::LimitRewrite: {
      auto limit = option.limit_args().replace_limit().replace_to();
      auto force_limit = option.limit_args().force_limit();
      auto threshold = option.limit_args().replace_limit().threshold();
      DB::ASTRewriteLimitTransformer limit_transformer = DB::ASTRewriteLimitTransformer();
      if (force_limit) {
        limit_transformer.setLimit(force_limit, true);
      } else {
        limit_transformer.setLimit(limit);
        limit_transformer.setThreshold(threshold);
      }
      limit_transformer.transform(ast);
      std::cout << "after limit transformation " << ast->dumpTree() << std::endl;
      break;
    }
    case rewriter::RewriteOp::TableNameRewrite: {
      // table_name
      {
        auto table_name_map = option.table_name_args().table_map();
        table_name_map_.clear();
        for (auto &kv : table_name_map) { table_name_map_[kv.first] = kv.second; }
      }
      // remote_table
      {
        auto remote_table_map = option.table_name_args().remote_table_map();
        remote_table_map_.clear();
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
        auto table_with_database_map = option.table_name_args().table_with_database_map();
        table_with_database_map_.clear();
        for (auto &kv : table_with_database_map) {
          DB::TableWithDatabase table_with_database;
          table_with_database.database_name = kv.second.database();
          table_with_database.table_name = kv.second.table();
          table_with_database_map_[kv.first] = table_with_database;
        }
      }
      doTableNameRewrite(ast, table_name_map_, remote_table_map_, table_with_database_map_);
      has_table_name_written = true;
      break;
    }
    case rewriter::RewriteOp::OffsetRewrite: {
      auto offset = option.offset_args().offset();
      DB::ASTRewriteOffsetTransformer offset_transformer = DB::ASTRewriteOffsetTransformer();
      offset_transformer.setOffset(offset);
      offset_transformer.transform(ast);
      std::cout << "after offset transformation " << ast->dumpTree() << std::endl;
      break;
    }
    case rewriter::RewriteOp::SettingsRewrite: {
      auto settings = option.settings_args().settings();
      DB::ASTSettingsTransformer settings_transformer = DB::ASTSettingsTransformer();
      std::vector<DB::Setting> settings_;
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
        settings_.push_back(s);
      }
      settings_transformer.setSettings(settings_);
      settings_transformer.transform(ast);
      std::cout << "after settings transformation " << ast->dumpTree() << std::endl;
      break;
    }
    case rewriter::RewriteOp::CommonTableExprRewrite: {
      DB::ASTRewriteCTETransformer cte_transformer = DB::ASTRewriteCTETransformer();
      auto ctes = option.common_table_expr_args().cte_map();
      cte_map_.clear();
      for (auto &kv : ctes) {
        cte_map_[kv.first] = {kv.second.alias(), kv.second.sql()};
      }
      auto cte_ast_map = DB::parseCTEMapToAST(cte_map_);

      cte_transformer.setCTEASTMap(cte_ast_map);
      cte_transformer.setCTEMap(cte_map_);
      cte_transformer.transform(ast);

      // If the table name has been rewritten previously, apply it again after the CTE rewrite.
      if (has_table_name_written) {
        doTableNameRewrite(ast, table_name_map_, remote_table_map_, table_with_database_map_);
      }
      break;
    }
    default: break;
    }
  }

  return std::move(ast);
}

void handleSelectQuery(DB::ASTPtr ast,
                       const rewriter::RewriteSQLRequest *request,
                       rewriter::RewriteSQLResponse *response) {
  std::cout << "before transformation " << ast->dumpTree() << std::endl;

  std::unordered_set<std::string> original_accessed_table_names_set;
  DB::collectTableNamesFromAST(ast, original_accessed_table_names_set);

  // Pre-scan CTE to collect the original table names accessed in the CTE subquery
  // (without altering the AST).
  for (const auto &option : request->options()) {
    if (option.op() != rewriter::RewriteOp::CommonTableExprRewrite) continue;
    std::map<std::string, DB::CommonTableExpr> cte_map;
    for (const auto &kv : option.common_table_expr_args().cte_map()) {
      cte_map[kv.first] = {kv.second.alias(), kv.second.sql()};
    }
    auto cte_ast_map = DB::parseCTEMapToAST(cte_map);
    for (const auto &kv : cte_ast_map) {
      if (auto it = original_accessed_table_names_set.find(kv.first);
          it != original_accessed_table_names_set.end()) {
        original_accessed_table_names_set.erase(it);
      }
      DB::collectTableNamesFromAST(kv.second, original_accessed_table_names_set);
    }
  }

  ast = applyRewriteOptions(request->options(), ast);

  for (const auto &name : original_accessed_table_names_set) {
    response->add_original_accessed_table_names(name);
  }
  setSuccessResponse(response, formatAst(ast), "SELECT");
}

} // namespace rewriter_handlers
