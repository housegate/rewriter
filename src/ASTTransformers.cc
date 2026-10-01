#include "ASTTransformers.h"
#include "handlers/common.h"

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
        rewriter_handlers::registerFieldOnlyChildren(*sub_ast);
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
  const std::unordered_map<std::string, std::string> &table_name_map_) {
  table_name_map = table_name_map_;
}

void ASTReplaceTransformer::setRemoteTableMap(
  const std::unordered_map<std::string, DB::RemoteTable> &remote_table_map_) {
  remote_table_map = remote_table_map_;
}

void ASTReplaceTransformer::setTableWithDatabaseMap(
  const std::unordered_map<std::string, DB::TableWithDatabase> &table_with_database_map_) {
  table_with_database_map = table_with_database_map_;
}

void ASTReplaceTransformer::format(WriteBuffer &ostr, const FormatSettings &settings) const {
  ostr << "APPLY ";
  if (!table_name_map.empty()) {
    ostr << "table_name_map: ";
    for (const auto &kv : table_name_map) { ostr << kv.first << " -> " << kv.second << ", "; }
  }
}

TableRewritePlan planTableRewrite(
  const String &origin_name,
  const std::unordered_map<String, String> &table_name_map,
  const std::unordered_map<String, RemoteTable> &remote_table_map,
  const std::unordered_map<String, TableWithDatabase> &table_with_database_map) {
  TableRewritePlan plan;
  if (const auto it = table_name_map.find(origin_name); it != table_name_map.end()) {
    plan.kind = TableRewriteKind::Rename;
    plan.table = it->second;
    return plan;
  }
  if (const auto it = remote_table_map.find(origin_name); it != remote_table_map.end()) {
    plan.kind = TableRewriteKind::Remote;
    plan.remote = it->second;
    return plan;
  }
  if (const auto it = table_with_database_map.find(origin_name);
      it != table_with_database_map.end()) {
    plan.kind = TableRewriteKind::WithDatabase;
    plan.db = it->second.database_name;
    plan.table = it->second.table_name;
    return plan;
  }
  return plan; // kind = None
}

namespace {
// "db.table" (or bare "table" when db is empty). Matches the key format
// callers hand off to table_name_map and the value format we write into
// ASTReplaceTransformer::rewrites / RewriteSQLResponse.table_rewrites.
inline std::string qualifyTable(const std::string &db, const std::string &table) {
  return db.empty() ? table : db + "." + table;
}
} // namespace

void ASTReplaceTransformer::transform(ASTPtr &ast) const {
  // replace table identifier with table name from map
  if (auto *table_expression = ast->as<ASTTableExpression>()) {
    if (table_expression->database_and_table_name) {
      auto *identifier = table_expression->database_and_table_name->as<ASTTableIdentifier>();
      auto origin_table_name = identifier->name();
      auto origin_alias = identifier->tryGetAlias();
      const auto plan = planTableRewrite(
        origin_table_name, table_name_map, remote_table_map, table_with_database_map);
      switch (plan.kind) {
      case TableRewriteKind::Rename: {
        identifier->setShortName(plan.table);
        if (origin_alias != "") {
          identifier->setAlias(origin_alias);
        } else {
          identifier->setAlias(origin_table_name);
        }
        rewrites[origin_table_name] = identifier->name();
        break;
      }
      case TableRewriteKind::Remote: {
        // use remote() function to replace table identifier
        const auto &remote_table = plan.remote;
        auto addr = makeRewriterAST<ASTLiteral>(remote_table.addr);
        auto database_name = makeRewriterAST<ASTLiteral>(remote_table.database_name);
        auto table_name = makeRewriterAST<ASTLiteral>(remote_table.table_name);
        auto user_name = makeRewriterAST<ASTLiteral>(remote_table.user_name);
        auto password = makeRewriterAST<ASTLiteral>(remote_table.password);
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
        rewrites[origin_table_name] = qualifyTable(remote_table.database_name, remote_table.table_name);
        break;
      }
      case TableRewriteKind::WithDatabase: {
        if (plan.db.empty()) {
          identifier->setShortName(plan.table);
        } else {
          identifier->resetTable(plan.db, plan.table);
        }
        if (origin_alias != "") {
          identifier->setAlias(origin_alias);
        } else {
          identifier->setAlias(origin_table_name);
        }
        rewrites[origin_table_name] = identifier->name();
        break;
      }
      case TableRewriteKind::None:
        break;
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
  this->cte_ast_map_set = true;
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
  const ASTPtr root = const_cast<DB::ASTSelectQuery *>(select_ast)->ptr();
  dfs(root);

  return used;
}

// Build a `WITH ... AS ...` clause for each SELECT that references the CTE.
ASTPtr ASTRewriteCTETransformer::buildWithList(
  const DB::ASTSelectQuery *select_ast, const std::unordered_set<String> &used_key) const {
  if (!select_ast || used_key.empty()) return nullptr;
  auto list = makeRewriterAST<ASTExpressionList>();
  const std::unordered_map<String, ASTPtr> *cte_ast_map_ptr = &cte_ast_map;
  std::unordered_map<String, ASTPtr> local_cte_ast_map;
  if (!cte_ast_map_set && cte_ast_map_ptr->empty()) {
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

    const auto sub = makeRewriterAST<ASTSubquery>();
    sub->children.clear();
    sub->children.push_back(sub_ast);

    auto elem = makeRewriterAST<ASTWithElement>();
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

} // namespace DB
