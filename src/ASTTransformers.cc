#include "ASTTransformers.h"

#include <Common/SettingsChanges.h>
#include <Common/SipHash.h>
#include <Common/quoteString.h>
#include <Common/re2.h>
#include <Core/Field.h>
#include <IO/Operators.h>
#include <IO/WriteHelpers.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTExplainQuery.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTSetQuery.h>
#include <Parsers/ASTSubquery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>

#include <iostream>
#include <map>
#include <stack>
#include <vector>

#include "Parsers/ASTWithElement.h"

namespace DB {
namespace ErrorCodes {
extern const int ILLEGAL_TYPE_OF_ARGUMENT;
extern const int NO_SUCH_COLUMN_IN_TABLE;
extern const int CANNOT_COMPILE_REGEXP;
} // namespace ErrorCodes

void collectTableNamesFromAST(const DB::ASTPtr &node, std::unordered_set<std::string> &out) {
  if (!node) return;

  if (auto *te = node->as<DB::ASTTableExpression>()) {
    if (te->database_and_table_name) {
      if (auto *id = te->database_and_table_name->as<ASTTableIdentifier>()) {
        const auto n = id->name(); // "db.table"
        if (!n.empty()) out.insert(n);
      }
    }
  }

  for (auto &ch : node->children) { collectTableNamesFromAST(ch, out); }
}

std::unordered_map<String, ASTPtr> parseCTEMapToAST(
  const std::map<String, CommonTableExpr> &cte_map,
  std::vector<String> *failed_aliases) {
  std::unordered_map<String, ASTPtr> out;
  for (const auto &kv : cte_map) {
    const String &alias = kv.first;
    const CommonTableExpr &cte = kv.second;
    const String &sub_sql = cte.sql;
    if (sub_sql.empty()) continue;
    try {
      const char *sub_pos = sub_sql.data();
      const char *sub_end = sub_pos + sub_sql.size();
      ParserQuery sub_parser(sub_end, false);
      ASTPtr sub_ast = parseQuery(sub_parser, sub_pos, sub_end, 0, 0, 0);
      if (sub_ast) {
        out.emplace(alias, std::move(sub_ast));
      } else {
        // parseQuery returned nullptr without throwing — treat as failure so
        // the caller doesn't silently drop this CTE.
        std::cerr << "[cte] skipped alias=\"" << alias
                  << "\" reason=\"empty AST from parseQuery\"" << std::endl;
        if (failed_aliases) failed_aliases->push_back(alias);
      }
    } catch (const Exception &e) {
      // Keep the rest of the rewrite going, but log + surface the failing
      // alias so debugging doesn't require reading server logs line-by-line.
      std::cerr << "[cte] skipped alias=\"" << alias
                << "\" reason=\"" << e.displayText() << "\"" << std::endl;
      if (failed_aliases) failed_aliases->push_back(alias);
    }
  }
  return out;
}

void collectTableNamesFromCTEMap(
  const std::map<String, CommonTableExpr> &cte_map, std::unordered_set<std::string> &out) {
  const auto ast_map = parseCTEMapToAST(cte_map);
  for (const auto &p : ast_map) { collectTableNamesFromAST(p.second, out); }
}

void ASTTransformerList::format(WriteBuffer &ostr, const FormatSettings &settings) const {
  for (const auto &child : children) {
    ostr << ' ';
    child->format(ostr, settings);
  }
}

void IASTTransformer::transform(const ASTPtr &transformer, ASTPtr &ast) {
  if (const auto *apply = transformer->as<ASTReplaceTransformer>()) { apply->transform(ast); }
}

void ASTReplaceTransformer::setTableNameMap(
  const std::map<std::string, std::string> &table_name_map_) {
  table_name_map = table_name_map_;
}

void ASTReplaceTransformer::setRemoteTableMap(
  const std::map<std::string, DB::RemoteTable> &remote_table_map_) {
  remote_table_map = remote_table_map_;
}

void ASTReplaceTransformer::setTableWithDatabaseMap(
  const std::map<std::string, DB::TableWithDatabase> &table_with_database_map_) {
  table_with_database_map = table_with_database_map_;
}

void ASTReplaceTransformer::format(WriteBuffer &ostr, const FormatSettings &settings) const {
  ostr << "APPLY ";
  if (!table_name_map.empty()) {
    ostr << "table_name_map: ";
    for (const auto &kv : table_name_map) { ostr << kv.first << " -> " << kv.second << ", "; }
  }
}

void ASTReplaceTransformer::transform(ASTPtr &ast) const {
  // replace table identifier with table name from map
  if (auto *table_expression = ast->as<ASTTableExpression>()) {
    if (table_expression->database_and_table_name) {
      auto *identifier = table_expression->database_and_table_name->as<ASTTableIdentifier>();
      auto origin_table_name = identifier->name();
      auto origin_alias = identifier->tryGetAlias();
      if (table_name_map.find(origin_table_name) != table_name_map.end()) {
        identifier->setShortName(table_name_map.at(identifier->name()));
        if (origin_alias != "") {
          identifier->setAlias(origin_alias);
        } else {
          identifier->setAlias(origin_table_name);
        }
      } else if (remote_table_map.find(origin_table_name) != remote_table_map.end()) {
        // use remote() function to replace table identifier
        auto remote_table = remote_table_map.at(origin_table_name);
        auto remote_function = std::make_shared<ASTFunction>();
        auto arguments = std::make_shared<ASTExpressionList>();
        auto addr = std::make_shared<ASTLiteral>(remote_table.addr);
        auto database_name = std::make_shared<ASTLiteral>(remote_table.database_name);
        auto table_name = std::make_shared<ASTLiteral>(remote_table.table_name);
        auto user_name = std::make_shared<ASTLiteral>(remote_table.user_name);
        auto password = std::make_shared<ASTLiteral>(remote_table.password);
        table_expression->children.clear();
        table_expression->database_and_table_name = nullptr;
        table_expression->table_function =
          DB::makeASTFunction("remote", addr, database_name, table_name, user_name, password);
        table_expression->children.push_back(table_expression->table_function);
        if (origin_alias != "") {
          table_expression->table_function->setAlias(origin_alias);
        } else {
          table_expression->table_function->setAlias(origin_table_name);
        }
      } else if (table_with_database_map.find(origin_table_name) != table_with_database_map.end()) {
        auto table_with_database = table_with_database_map.at(origin_table_name);
        if (table_with_database.database_name.empty()) {
          identifier->setShortName(table_with_database.table_name);
        } else {
          identifier->resetTable(table_with_database.database_name, table_with_database.table_name);
        }
        if (origin_alias != "") {
          identifier->setAlias(origin_alias);
        } else {
          identifier->setAlias(origin_table_name);
        }
      }
    } else if (table_expression->subquery) {
      for (auto &child : ast->children) { transform(child); }
    }
  } else {
    for (auto &child : ast->children) { transform(child); }
  }
}

/*============================CTE Rewriting Start============================*/

void ASTRewriteCTETransformer::setCTEASTMap(const std::unordered_map<String, ASTPtr> &cte_ast_map) {
  this->cte_ast_map = cte_ast_map;
}

void ASTRewriteCTETransformer::format(WriteBuffer &ostr, const FormatSettings &settings) const {
  ostr << "APPLY ";
  if (!cte_map.empty()) {
    ostr << "cte_map: ";
    for (const auto &kv : cte_map) { ostr << kv.first << " -> " << kv.second.sql << ", "; }
  }
}

void ASTRewriteCTETransformer::transform(ASTPtr &ast) const {
  if (cte_map.empty() || !ast) return;
  std::unordered_map<String, bool> used_cte_key_map;
  injectOnce(ast, used_cte_key_map);
}

void ASTRewriteCTETransformer::injectOnce(
  const ASTPtr &ast, std::unordered_map<String, bool> &used_cte_key_map) const {
  if (!ast) return;

  if (auto *select = ast->as<DB::ASTSelectQuery>()) {
    const auto used_key = collectCTEKeysForSelect(select, used_cte_key_map);
    ASTPtr with_list = buildWithList(select, used_key);
    if (!with_list || !with_list->as<ASTExpressionList>() || with_list->children.empty()) {
      return;
    }
    mergeWithIntoSelect(select /*ASTSelectQuery**/, with_list /*ASTPtr&*/);
  }
  for (auto &child : ast->children) { injectOnce(child, used_cte_key_map); }
}

std::unordered_set<String> ASTRewriteCTETransformer::collectCTEKeysForSelect(
  const DB::ASTSelectQuery *select_ast, std::unordered_map<String, bool> &used_cte_key_map) const {
  std::unordered_set<String> used;
  if (!select_ast) return used;

  auto has_key = [&](const String &k) { return cte_map.contains(k); };

  std::function<void(const ASTPtr &)> dfs = [&](const ASTPtr &n) {
    if (!n) return;

    if (const auto *tid = n->as<DB::ASTTableIdentifier>()) {
      if (const String key = tid->shortName(); has_key(key) && !used_cte_key_map[key]) {
        used.insert(key);
        used_cte_key_map[key] = true;
      }
    } else if (const auto *id = dynamic_cast<DB::ASTIdentifier *>(n.get())) {
      if (const String key = id->shortName(); has_key(key) && !used_cte_key_map[key]) {
        used.insert(key);
        used_cte_key_map[key] = true;
      }
    }

    for (auto &ch : n->children) dfs(ch);
  };
  const ASTPtr root = const_cast<DB::ASTSelectQuery *>(select_ast)->shared_from_this();
  dfs(root);

  return used;
}

// Build a `WITH ... AS ...` clause for each SELECT that references the CTE.
ASTPtr ASTRewriteCTETransformer::buildWithList(
  const DB::ASTSelectQuery *select_ast, const std::unordered_set<String> &used_key) const {
  std::cout << "========cout used_key========" << std::endl;
  for (auto key : used_key) { std::cout << key << ", "; }
  std::cout << "========cout used_key========" << std::endl;

  if (!select_ast || used_key.empty()) return nullptr;
  auto list = std::make_shared<ASTExpressionList>();
  const std::unordered_map<String, ASTPtr> *cte_ast_map_ptr = &cte_ast_map;
  std::unordered_map<String, ASTPtr> local_cte_ast_map;
  if (cte_ast_map_ptr->empty()) {
    local_cte_ast_map = parseCTEMapToAST(cte_map);
    cte_ast_map_ptr = &local_cte_ast_map;
  }
  // const auto cte_ast_map = parseCTEMapToAST(cte_map);
  for (const auto &[alias, cte] : cte_map) {
    if (!used_key.contains(alias) || cte.sql.empty()) continue;

    auto it = cte_ast_map_ptr->find(alias);
    if (it == cte_ast_map_ptr->end()) continue;
    ASTPtr sub_ast = it->second;
    if (!sub_ast) continue;

    const auto sub = std::make_shared<ASTSubquery>();
    sub->children.clear();
    sub->children.push_back(sub_ast);

    auto elem = std::make_shared<ASTWithElement>();
    elem->name = alias;
    elem->subquery = sub;
    elem->children.clear();
    elem->children.push_back(elem->subquery);

    list->children.push_back(elem);
  }
  return list;
}

void ASTRewriteCTETransformer::mergeWithIntoSelect(ASTSelectQuery *sel, ASTPtr &new_list) const {
  if (!sel || !new_list) return;

  ASTPtr with_expr = sel->with();
  if (!with_expr) {
    sel->setExpression(ASTSelectQuery::Expression::WITH, std::move(new_list));
    return;
  }

  auto *exist = with_expr->as<ASTExpressionList>();
  auto *add = new_list->as<ASTExpressionList>();
  if (!exist || !add) {
    sel->setExpression(ASTSelectQuery::Expression::WITH, std::move(new_list));
    return;
  }

  std::unordered_set<String> existing;
  for (auto &ch : exist->children) {
    if (auto *we = ch->as<ASTWithElement>()) {
      if (!we->name.empty()) existing.insert(we->name);
    } else {
      String alias = ch->tryGetAlias();
      if (!alias.empty()) existing.insert(alias);
    }
  }

  // Prepend new CTEs (so injected ones come before existing ones).
  for (auto &n : add->children) {
    String nm;
    if (auto *we = n->as<ASTWithElement>())
      nm = we->name;
    else
      nm = n->tryGetAlias();

    if (!nm.empty() && !existing.count(nm)) {
      exist->children.insert(exist->children.begin(), n);
      existing.insert(nm);
    }
  }
}

void ASTRewriteCTETransformer::setCTEMap(const std::map<String, DB::CommonTableExpr> &cte_map_) {
  cte_map = cte_map_;
}

/*============================CTE Rewriting End============================*/

void ASTRewriteLimitTransformer::format(WriteBuffer &ostr, const FormatSettings &settings) const {
  ostr << "APPLY ";
  ostr << "limit: " << limit;
}

void ASTRewriteLimitTransformer::setLimit(size_t limit_, bool force_limit_) {
  limit = limit_;
  force_limit = force_limit_;
}

void ASTRewriteLimitTransformer::setThreshold(size_t threshold_) {
  threshold = threshold_;
}

void ASTRewriteLimitTransformer::transform(ASTPtr &ast) const {
  // replace table identifier with table name from map
  if (auto *identifier = ast->as<ASTSelectQuery>()) {
    if (limit > 0) {
      if (force_limit) {
        identifier->setExpression(
          ASTSelectQuery::Expression::LIMIT_LENGTH, std::make_shared<ASTLiteral>(limit));
      } else {
        // Get the original limit value
        auto limit_expr = identifier->limitLength();
        if (limit_expr) {
          auto *limit_literal = limit_expr->as<ASTLiteral>();
          if (limit_literal) {
            size_t original_limit = limit_literal->value.safeGet<UInt64>();
            if (original_limit == 0 || original_limit > threshold) {
              identifier->setExpression(
                ASTSelectQuery::Expression::LIMIT_LENGTH, std::make_shared<ASTLiteral>(limit));
            }
          }
        } else {
          identifier->setExpression(
            ASTSelectQuery::Expression::LIMIT_LENGTH, std::make_shared<ASTLiteral>(limit));
        }
      }
    }
  } else {
    for (auto &child : ast->children) { transform(child); }
  }
}

void ASTRewriteOffsetTransformer::format(WriteBuffer &ostr, const FormatSettings &settings) const {
  ostr << "APPLY ";
  ostr << "offset: " << offset;
}

void ASTRewriteOffsetTransformer::setOffset(size_t offset_) {
  offset = offset_;
}

void ASTRewriteOffsetTransformer::transform(ASTPtr &ast) const {
  // replace table identifier with table name from map
  if (auto *identifier = ast->as<ASTSelectQuery>()) {
    if (offset > 0) {
      identifier->setExpression(
        ASTSelectQuery::Expression::LIMIT_OFFSET, std::make_shared<ASTLiteral>(offset));
    }
  } else {
    for (auto &child : ast->children) { transform(child); }
  }
}

void ASTSettingsTransformer::format(WriteBuffer &ostr, const FormatSettings &settings) const {
  ostr << "APPLY ";
  if (!settings_.empty()) { ostr << "settings: " << settings_.size(); }
}

void ASTSettingsTransformer::setSettings(const std::vector<Setting> &settings) {
  settings_ = settings;
}

void ASTSettingsTransformer::transform(ASTPtr &ast) const {
  if (auto *identifier = ast->as<ASTSelectQuery>()) {
    if (settings_.size() <= 0) { return; }
    auto settings_expr = identifier->settings();
    if (settings_expr) {
      auto *settings = settings_expr->as<ASTSetQuery>();
      if (settings) {
        for (const auto &setting : settings_) {
          switch (setting.value_type) {
          case rewriter::SettingType::String: {
            settings->changes.setSetting(
              std::string_view(setting.name), Field(setting.string_value));
            break;
          }
          case rewriter::SettingType::Int: {
            settings->changes.setSetting(std::string_view(setting.name), Field(setting.int_value));
            break;
          }
          case rewriter::SettingType::Bool: {
            settings->changes.setSetting(std::string_view(setting.name), Field(setting.bool_value));
            break;
          }
          case rewriter::SettingType::Uint64: {
            settings->changes.setSetting(
              std::string_view(setting.name), Field(setting.uint64_value));
            break;
          }
          default: break;
          }
        }
      }
    } else {
      auto new_settings = std::make_shared<ASTSetQuery>();
      new_settings->is_standalone = false;
      for (const auto &setting : settings_) {
        switch (setting.value_type) {
        case rewriter::SettingType::String: {
          new_settings->changes.setSetting(
            std::string_view(setting.name), Field(setting.string_value));
          break;
        }
        case rewriter::SettingType::Int: {
          new_settings->changes.setSetting(
            std::string_view(setting.name), Field(setting.int_value));
          break;
        }
        case rewriter::SettingType::Bool: {
          new_settings->changes.setSetting(
            std::string_view(setting.name), Field(setting.bool_value));
          break;
        }
        case rewriter::SettingType::Uint64: {
          new_settings->changes.setSetting(
            std::string_view(setting.name), Field(setting.uint64_value));
          break;
        }
        default: break;
        }
      }
      identifier->setExpression(ASTSelectQuery::Expression::SETTINGS, new_settings);
    }
  } else {
    for (auto &child : ast->children) { transform(child); }
  }
}
} // namespace DB