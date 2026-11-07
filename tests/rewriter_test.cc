#include "rewriter.grpc.pb.h"
#include "rewriter-server.h"
#include "handlers/common.h"
#include "handlers/storage_integrity.h"
#include "handlers/table_reference.h"
#include "si_corpus.h"
#include "si_normalize.h"
#include <Poco/JSON/Object.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>
#include <DataTypes/DataTypesDecimal.h>
#include <google/protobuf/util/json_util.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <tuple>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// ------------------------ Debug dump helpers ------------------------
static std::string DumpMap(const std::map<std::string, DB::CommonTableExpr> &m) {
  std::string s;
  s += "{\n";
  for (const auto &kv : m) { s += "  \"" + kv.first + "\": \"" + kv.second.sql + "\",\n"; }
  s += "}";
  return s;
}

static std::string DumpMap(const std::map<std::string, std::string> &m) {
  std::string s;
  s += "{\n";
  for (const auto &kv : m) { s += "  \"" + kv.first + "\": \"" + kv.second + "\",\n"; }
  s += "}";
  return s;
}

static std::string DumpContext(const std::string &origin_sql,
  const std::string &rewritten_sql,
  const std::map<std::string, DB::CommonTableExpr> &cte_map,
  const std::map<std::string, std::string> &table_map) {
  std::string s;
  s += "=== Rewrite Context ===\n";
  s += "Origin SQL:\n" + origin_sql + "\n\n";
  s += "Rewritten SQL:\n" + rewritten_sql + "\n\n";
  if (!cte_map.empty()) s += "CTE Map:\n" + DumpMap(cte_map) + "\n\n";
  if (!table_map.empty()) s += "Table Map:\n" + DumpMap(table_map) + "\n";
  s += "=======================\n";
  return s;
}

// thread-local buffer to hold last debug info so tests can SCOPED_TRACE it
static thread_local std::string g_last_debug;

static const std::string &LastDebug() {
  return g_last_debug;
}

#define TRACE_LAST_REWRITE() SCOPED_TRACE(std::string("\n") + LastDebug())

static size_t CountSubstr(const std::string &haystack, const std::string &needle) {
  if (needle.empty()) return 0;
  size_t count = 0;
  size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

// Normalize whitespace: collapse all runs of whitespace to single spaces, trim leading/trailing
// spaces.
static std::string NormalizeWS(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  bool in_space = false;
  for (unsigned char ch : s) {
    if (ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == ' ') {
      if (!in_space) {
        out.push_back(' ');
        in_space = true;
      }
    } else {
      out.push_back(static_cast<char>(ch));
      in_space = false;
    }
  }
  // trim leading/trailing spaces
  if (!out.empty() && out.front() == ' ') out.erase(out.begin());
  if (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// ------------------------ Test data provider ------------------------
namespace TestData {
struct CTECase {
  const char *name;
  const char *origin_sql;
  std::vector<std::pair<const char *, const char *>> cte_kv; // {name, sql}
  // Optional expectations (lightweight; per-case tests may add more)
  int expect_with_count = -1; // -1 means ignore
  std::vector<const char *> expect_contains;
};

inline const std::vector<CTECase> &Cases() {
  static const std::vector<CTECase> kCases = {
    {"simple_select",
      "SELECT * FROM c1",
      {{"c1", "SELECT 1"}},
      /*expect_with_count*/ 1,
      {"WITH c1 AS", "SELECT 1"}},
    {"union_inject_once",
      "SELECT * FROM cte1 UNION ALL SELECT * FROM t2",
      {{"cte1", "SELECT * FROM t1"}},
      /*expect_with_count*/ 1,
      {"WITH cte1 AS", "t2"}},
    {"existing_with_merge",
      "WITH a AS (SELECT 0) SELECT * FROM a",
      {{"a", "SELECT 1"}, {"b", "SELECT 2"}},
      /*expect_with_count*/ -1,
      {" a AS ", " b AS "}},
    {"parse_failure_skips_bad",
      "SELECT * FROM good_cte",
      {{"bad", "SELECT * FROM"}, {"good_cte", "SELECT 1"}},
      /*expect_with_count*/ -1,
      {"WITH good_cte AS"}},
    {
      "join_and_alias",
      "SELECT t.id FROM c1 AS t JOIN c2 USING (id)",
      {{"c1", "SELECT id FROM s1"}, {"c2", "SELECT id FROM s2"}},
      /*expect_with_count*/ 1,
      {"WITH c1 AS", "WITH c2 AS"}
      // we will not assert both prefixes literally; see specific test below
    },
    {"subquery_scope",
      "SELECT 1 FROM (SELECT * FROM c1) s",
      {{"c1", "SELECT 1"}},
      /*expect_with_count*/ 1,
      {"WITH c1 AS"}},
    {"none_used_still_inject",
      "SELECT 1",
      {{"a", "SELECT 1"}},
      /*expect_with_count*/ 1,
      {"WITH a AS"}},
    {"db_qualified",
      "SELECT * FROM db.c1",
      {{"c1", "SELECT 42"}},
      /*expect_with_count*/ 1,
      {"WITH c1 AS"}},
    {"multiple_refs_once",
      "SELECT * FROM c1 UNION ALL SELECT * FROM c1",
      {{"c1", "SELECT 1"}},
      /*expect_with_count*/ 1,
      {"WITH c1 AS"}},
    {"cte_value_with_union",
      "SELECT * FROM c1",
      {{"c1", "SELECT 1 UNION ALL SELECT 2"}},
      /*expect_with_count*/ 1,
      {"WITH c1 AS"}},
    {"nested_union_tree",
      "(SELECT * FROM c1) UNION ALL (SELECT * FROM c2)",
      {{"c1", "SELECT 1"}, {"c2", "SELECT 2"}},
      /*expect_with_count*/ 1,
      {"WITH c1 AS", "WITH c2 AS"}},
    {"huge_cte_map_unused_many",
      "SELECT 1",
      {{"a", "SELECT 1"},
        {"b", "SELECT 2"},
        {"c", "SELECT 3"},
        {"d", "SELECT 4"},
        {"e", "SELECT 5"}},
      /*expect_with_count*/ 1,
      {"WITH a AS", "WITH b AS", "WITH c AS"}},
  };
  return kCases;
}

inline const CTECase *Find(const std::string &name) {
  for (const auto &c : Cases())
    if (name == c.name) return &c;
  return nullptr;
}
} // namespace TestData

// ------------------------ Helpers: run CTE rewrite ------------------------
// Core helper: run ONLY CTE rewrite and return rewritten SQL
static std::string RunCTERewrite(
  const std::string &origin_sql, const std::map<std::string, DB::CommonTableExpr> &cte_map) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(origin_sql);

  if (!cte_map.empty()) {
    auto *cte_option = req.add_options();
    cte_option->set_op(rewriter::RewriteOp::CommonTableExprRewrite);
    auto *cte_args = cte_option->mutable_common_table_expr_args();
    for (const auto &kv : cte_map) {
      auto &msg = (*cte_args->mutable_cte_map())[kv.first];
      if (!kv.second.alias.empty())
        msg.set_alias(kv.second.alias);
      else
        msg.set_alias(kv.first);
      msg.set_sql(kv.second.sql);
    }
  }

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  if (!status.ok() || resp.code() != rewriter::RewriteCode::Success) {
    return ""; // or throw
  }
  auto out = resp.sql_after_rewrite();
  g_last_debug = DumpContext(origin_sql, out, cte_map, /*table_map*/ {});
  std::cout << "\n******************** REWRITE DEBUG START (CTE) ********************\n";
  std::cout << g_last_debug;
  std::cout << "******************** REWRITE DEBUG END (CTE) **********************\n" << std::endl;
  return out;
}

// Convenience helper: read params from provider by case name
static std::string RunCTERewriteByName(const std::string &case_name) {
  const auto *c = TestData::Find(case_name);
  if (!c) return "";
  std::map<std::string, DB::CommonTableExpr> m;
  for (const auto &kv : c->cte_kv) {
    m.emplace(kv.first, DB::CommonTableExpr{kv.first, kv.second});
  }
  return RunCTERewrite(c->origin_sql, m);
}

// Run CTE rewrite and also return the collected original table names
static std::pair<std::string, std::vector<std::string>> RunCTERewriteWithNames(
  const std::string &origin_sql, const std::map<std::string, DB::CommonTableExpr> &cte_map) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(origin_sql);

  if (!cte_map.empty()) {
    auto *cte_option = req.add_options();
    cte_option->set_op(rewriter::RewriteOp::CommonTableExprRewrite);
    auto *cte_args = cte_option->mutable_common_table_expr_args();
    for (const auto &kv : cte_map) {
      auto &msg = (*cte_args->mutable_cte_map())[kv.first];
      msg.set_alias(kv.second.alias.empty() ? kv.first : kv.second.alias);
      msg.set_sql(kv.second.sql);
    }
  }

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  if (!status.ok() || resp.code() != rewriter::RewriteCode::Success) {
    return {"", {}}; // or throw
  }
  std::vector<std::string> names;
  names.reserve(resp.original_accessed_tables_size());
  for (const auto &t : resp.original_accessed_tables()) {
    names.push_back(t.original_database().empty()
      ? t.original_table()
      : t.original_database() + "." + t.original_table());
  }
  auto out = resp.sql_after_rewrite();
  g_last_debug = DumpContext(origin_sql, out, cte_map, /*table_map*/ {});
  std::cout << "\n******************** REWRITE DEBUG START (CTE+NAMES) ********************\n";
  std::cout << g_last_debug;
  std::cout << "******************** REWRITE DEBUG END (CTE+NAMES) **********************\n"
            << std::endl;
  return {out, std::move(names)};
}

// Run CTE + TableName rewrite and also return the collected original table names
static std::pair<std::string, std::vector<std::string>> RunCTEAndTableRewriteWithNames(
  const std::string &origin_sql,
  const std::map<std::string, DB::CommonTableExpr> &cte_map,
  const std::map<std::string, std::string> &table_map) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(origin_sql);

  if (!cte_map.empty()) {
    auto *cte_option = req.add_options();
    cte_option->set_op(rewriter::RewriteOp::CommonTableExprRewrite);
    auto *cte_args = cte_option->mutable_common_table_expr_args();
    for (const auto &kv : cte_map) {
      auto &msg = (*cte_args->mutable_cte_map())[kv.first];
      msg.set_alias(kv.second.alias.empty() ? kv.first : kv.second.alias);
      msg.set_sql(kv.second.sql);
    }
  }
  if (!table_map.empty()) {
    auto *tbl_option = req.add_options();
    tbl_option->set_op(rewriter::RewriteOp::TableNameRewrite);
    auto *tbl_args = tbl_option->mutable_table_name_args()->mutable_static_args();
    for (const auto &kv : table_map) { (*tbl_args->mutable_table_map())[kv.first] = kv.second; }
  }

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  if (!status.ok() || resp.code() != rewriter::RewriteCode::Success) {
    return {"", {}}; // or throw
  }
  std::vector<std::string> names;
  names.reserve(resp.original_accessed_tables_size());
  for (const auto &t : resp.original_accessed_tables()) {
    names.push_back(t.original_database().empty()
      ? t.original_table()
      : t.original_database() + "." + t.original_table());
  }
  auto out = resp.sql_after_rewrite();
  g_last_debug = DumpContext(origin_sql, out, cte_map, table_map);
  std::cout << "\n****************** REWRITE DEBUG START (CTE+TABLE+NAMES) ******************\n";
  std::cout << g_last_debug;
  std::cout << "****************** REWRITE DEBUG END (CTE+TABLE+NAMES) ********************\n"
            << std::endl;
  return {out, std::move(names)};
}

// ------------------------ Helpers: run CTE + TableName rewrite ------------------------
static std::string RunCTEAndTableRewrite(const std::string &origin_sql,
  const std::map<std::string, DB::CommonTableExpr> &cte_map,
  const std::map<std::string, std::string> &table_map) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(origin_sql);

  if (!cte_map.empty()) {
    auto *cte_option = req.add_options();
    cte_option->set_op(rewriter::RewriteOp::CommonTableExprRewrite);
    auto *cte_args = cte_option->mutable_common_table_expr_args();
    for (const auto &kv : cte_map) {
      auto &msg = (*cte_args->mutable_cte_map())[kv.first];
      if (!kv.second.alias.empty())
        msg.set_alias(kv.second.alias);
      else
        msg.set_alias(kv.first);
      msg.set_sql(kv.second.sql);
    }
  }
  if (!table_map.empty()) {
    auto *tbl_option = req.add_options();
    tbl_option->set_op(rewriter::RewriteOp::TableNameRewrite);
    auto *tbl_args = tbl_option->mutable_table_name_args()->mutable_static_args();
    for (const auto &kv : table_map) { (*tbl_args->mutable_table_map())[kv.first] = kv.second; }
  }

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  if (!status.ok() || resp.code() != rewriter::RewriteCode::Success) { return ""; }
  auto out = resp.sql_after_rewrite();
  g_last_debug = DumpContext(origin_sql, out, cte_map, table_map);
  std::cout << "\n****************** REWRITE DEBUG START (CTE+TABLE) ******************\n";
  std::cout << g_last_debug;
  std::cout << "****************** REWRITE DEBUG END (CTE+TABLE) ********************\n"
            << std::endl;
  return out;
}

// ------------------------ CTE-only tests using helpers ------------------------

TEST(CTEOnly, RunByName_Simple) {
  auto out = RunCTERewriteByName("simple_select");
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());
  EXPECT_NE(norm.find("WITH c1 AS"), std::string::npos);
  EXPECT_EQ(CountSubstr(norm, "WITH "), 1u);
}

TEST(CTEOnly, RunByName_ExistingWithMerge) {
  auto out = RunCTERewriteByName("existing_with_merge");
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());
  // Only one WITH clause should be present, merging existing CTEs without duplication
  EXPECT_EQ(CountSubstr(norm, "WITH "), 1u);
  // Only 'a' is injected; 'b' is unused and should not be injected
  EXPECT_EQ(CountSubstr(norm, " a AS "), 1u);
  // 'b' is unused and should not be injected
  EXPECT_EQ(CountSubstr(norm, " b AS "), 0u);
}

TEST(CTEOnly, RunByName_UnionOnce) {
  auto out = RunCTERewriteByName("union_inject_once");
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());
  EXPECT_EQ(CountSubstr(norm, "WITH "), 1u);
}

TEST(CTEOnly, RunByName_ParseFailureSkipsBad) {
  auto out = RunCTERewriteByName("parse_failure_skips_bad");
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());
  EXPECT_NE(norm.find("WITH good_cte AS"), std::string::npos);
  EXPECT_EQ(norm.find("WITH bad AS"), std::string::npos);
}

TEST(CTEOnly, RunByName_JoinAndAlias) {
  auto out = RunCTERewriteByName("join_and_alias");
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());
  EXPECT_EQ(CountSubstr(norm, "WITH "), 1u);
  // both CTE names should appear in WITH definitions exactly once each
  EXPECT_EQ(CountSubstr(norm, "WITH c1 AS ("), 1u);
  // Only the first CTE in a WITH list is prefixed by "WITH".
  // Subsequent CTEs are separated by commas, e.g. "WITH c1 AS (...), c2 AS (...)".
  EXPECT_EQ(CountSubstr(norm, "c2 AS ("), 1u);
  // and the JOIN remains
  EXPECT_NE(norm.find("JOIN"), std::string::npos);
}

TEST(CTEOnly, RunByName_SubqueryScope) {
  auto out = RunCTERewriteByName("subquery_scope");
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());
  EXPECT_EQ(CountSubstr(norm, "WITH "), 1u);
  EXPECT_NE(norm.find("WITH c1 AS"), std::string::npos);
}

TEST(CTEOnly, RunByName_NoneUsedNoInject) {
  auto out = RunCTERewriteByName("none_used_still_inject");
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());
  // No CTE should be injected when none of the CTE keys are referenced
  EXPECT_EQ(CountSubstr(norm, "WITH "), 0u);
  EXPECT_EQ(norm.find("WITH a AS"), std::string::npos);
}

TEST(CTEOnly, RunByName_DbQualified) {
  auto out = RunCTERewriteByName("db_qualified");
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());
  EXPECT_EQ(CountSubstr(norm, "WITH "), 1u);
  EXPECT_NE(norm.find("WITH c1 AS"), std::string::npos);
}

TEST(CTEOnly, RunByName_MultipleRefsOnce) {
  auto out = RunCTERewriteByName("multiple_refs_once");
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());
  EXPECT_EQ(CountSubstr(norm, "WITH "), 1u);
  EXPECT_EQ(CountSubstr(norm, "c1 AS "), 1u);
}

TEST(CTEOnly, RunByName_CTEValueWithUnion) {
  auto out = RunCTERewriteByName("cte_value_with_union");
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());
  EXPECT_EQ(CountSubstr(norm, "WITH "), 1u);
  // ensure the union parts are present in the output SQL
  EXPECT_NE(norm.find("UNION ALL"), std::string::npos);
}

TEST(CTEOnly, RunByName_NestedUnionTree) {
  auto out = RunCTERewriteByName("nested_union_tree");
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());
  // Two WITH clauses are injected, one per union branch
  EXPECT_EQ(CountSubstr(norm, "WITH "), 2u);
  EXPECT_NE(norm.find(" c1 AS "), std::string::npos);
  EXPECT_NE(norm.find(" c2 AS "), std::string::npos);
}

TEST(CTEOnly, RunByName_HugeCTEMapUnusedMany) {
  auto out = RunCTERewriteByName("huge_cte_map_unused_many");
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());
  // No CTE should be injected when none of the entries are referenced
  EXPECT_EQ(CountSubstr(norm, "WITH "), 0u);
  EXPECT_EQ(norm.find(" a AS "), std::string::npos);
  EXPECT_EQ(norm.find(" b AS "), std::string::npos);
  EXPECT_EQ(norm.find(" c AS "), std::string::npos);
}

TEST(CTE_Table, Combine_CTE_and_TableName) {
  // origin: select * from `sqlquery-123abc` limit 1 union select * from Withdraw
  // we quote the hyphenated identifier in origin SQL to make it parseable.
  const std::string origin_sql =
    "SELECT * FROM `sqlquery-123abc` LIMIT 1 UNION SELECT * FROM Withdraw";

  std::map<std::string, DB::CommonTableExpr> cte_map{
    {"sqlquery-123abc", {"sqlquery-123abc", "SELECT * FROM Transfer"}},
    {"sqlquery-456def", {"sqlquery-456def", "SELECT * FROM Withdraw"}},
  };
  std::map<std::string, std::string> table_map{
    {"Transfer", "autogen_123"},
    {"Withdraw", "autogen_456"},
  };

  auto out = RunCTEAndTableRewrite(origin_sql, cte_map, table_map);
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());

  // 1) WITH appears once
  EXPECT_EQ(CountSubstr(norm, "WITH "), 1u);

  // 2) CTE name preserved on outer SELECT, and CTE body table renamed
  // allow either backticks or unquoted printing; we check for raw tokens
  EXPECT_NE(norm.find("sqlquery-123abc"), std::string::npos);
  EXPECT_NE(norm.find("autogen_123"), std::string::npos);

  // 3) Second UNION branch table renamed
  EXPECT_NE(norm.find("autogen_456"), std::string::npos);

  // 4) No raw source table names remain as table sources (FROM/JOIN), but may remain as aliases.
  // Old names should not appear as table sources (FROM/JOIN), but may remain as aliases.
  EXPECT_EQ(norm.find(" FROM Transfer"), std::string::npos);
  EXPECT_EQ(norm.find(" JOIN Transfer"), std::string::npos);
  EXPECT_EQ(norm.find(" FROM Withdraw"), std::string::npos);
  EXPECT_EQ(norm.find(" JOIN Withdraw"), std::string::npos);
  // Alias retention is expected by our transformer:
  EXPECT_NE(norm.find(" AS Transfer"), std::string::npos);
  EXPECT_NE(norm.find(" AS Withdraw"), std::string::npos);
}

TEST(TableOnly, TableRewrite_NoCTE) {
  // origin: select * from `sqlquery-123abc` limit 1 union select * from Withdraw
  // We only provide table mappings; no CTE mapping supplied.
  const std::string origin_sql =
    "SELECT * FROM `sqlquery-123abc` LIMIT 1 UNION SELECT * FROM Withdraw";

  std::map<std::string, DB::CommonTableExpr> cte_map{}; // empty
  std::map<std::string, std::string> table_map{
    {"Transfer", "autogen_123"},
    {"Withdraw", "autogen_456"},
  };

  auto out = RunCTEAndTableRewrite(origin_sql, cte_map, table_map);
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());

  // 1) No WITH should be injected (since cte_map is empty)
  EXPECT_EQ(CountSubstr(norm, "WITH "), 0u);

  // 2) Left branch keeps the original identifier (hyphenated name), right branch renamed
  EXPECT_NE(norm.find("sqlquery-123abc"), std::string::npos);
  EXPECT_NE(norm.find("autogen_456"), std::string::npos);

  // 3) Ensure source table name on right branch is gone as a table source; it may remain as an
  // alias. Old name should not be used as a table source; it may remain as an alias.
  EXPECT_EQ(norm.find(" FROM Withdraw"), std::string::npos);
  EXPECT_EQ(norm.find(" JOIN Withdraw"), std::string::npos);
  EXPECT_NE(norm.find(" AS Withdraw"), std::string::npos);
}

// Only inject keys actually used (take a subset of the large map).
TEST(CTEOnly, LargeMap_OnlyUsedInjected_Subset) {
  const std::string origin_sql = "SELECT * FROM c3 JOIN e5 USING (id)";

  std::map<std::string, DB::CommonTableExpr> cte_map{
    {"a1", {"a1", "SELECT 1 AS id"}},
    {"a2", {"a2", "SELECT 2 AS id"}},
    {"b1", {"b1", "SELECT 3 AS id"}},
    {"b2", {"b2", "SELECT 4 AS id"}},
    {"c3", {"c3", "SELECT 5 AS id"}},
    {"c4", {"c4", "SELECT 6 AS id"}},
    {"d1", {"d1", "SELECT 7 AS id"}},
    {"d2", {"d2", "SELECT 8 AS id"}},
    {"e5", {"e5", "SELECT 9 AS id"}},
    {"f6", {"f6", "SELECT 10 AS id"}},
  };

  auto out = RunCTERewrite(origin_sql, cte_map);
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());

  // Exactly one WITH.
  EXPECT_EQ(CountSubstr(norm, "WITH "), 1u);

  // Only c3/e5 were injected.
  EXPECT_EQ(CountSubstr(norm, " c3 AS "), 1u);
  EXPECT_EQ(CountSubstr(norm, " e5 AS "), 1u);

  // Unused keys must not appear.
  EXPECT_EQ(norm.find(" a1 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" a2 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" b1 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" b2 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" c4 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" d1 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" d2 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" f6 AS "), std::string::npos);

  // JOIN is still present.
  EXPECT_NE(norm.find("JOIN"), std::string::npos);
}

// fixme:
// Origin SQL:
// SELECT * FROM c1 UNION ALL SELECT * FROM c1 UNION ALL SELECT * FROM d2
// Rewritten SQL:
// WITH c1 AS (SELECT 1) SELECT * FROM c1 UNION ALL SELECT * FROM c1 UNION ALL SELECT * FROM d2
TEST(CTEOnly, LargeMap_MultiRefs_DedupAndOnlyUsed) {
  const std::string origin_sql =
    "SELECT * FROM c1 UNION ALL SELECT * FROM c1 UNION ALL SELECT * FROM d2";

  std::map<std::string, DB::CommonTableExpr> cte_map{
    {"a0", {"a0", "SELECT 0"}},
    {"c1", {"c1", "SELECT 1"}},
    {"c2", {"c2", "SELECT 2"}},
    {"d2", {"d2", "SELECT 3"}},
    {"x9", {"x9", "SELECT 9"}},
  };

  auto out = RunCTERewrite(origin_sql, cte_map);
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());

  EXPECT_EQ(CountSubstr(norm, "WITH "), 2u);
  EXPECT_EQ(CountSubstr(norm, " c1 AS "), 1u);
  EXPECT_EQ(CountSubstr(norm, " d2 AS "), 1u);

  // Unused CTEs must not appear.
  EXPECT_EQ(norm.find(" a0 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" c2 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" x9 AS "), std::string::npos);

  // UNION structure is preserved.
  EXPECT_NE(norm.find("UNION ALL"), std::string::npos);
}

TEST(CTEOnly, LargeMap_NoneUsed_NoInjection) {
  const std::string origin_sql = "SELECT 1";
  std::map<std::string, DB::CommonTableExpr> cte_map{
    {"a", {"a", "SELECT 1"}},
    {"b", {"b", "SELECT 2"}},
    {"c", {"c", "SELECT 3"}},
    {"d", {"d", "SELECT 4"}},
    {"e", {"e", "SELECT 5"}},
  };

  auto out = RunCTERewrite(origin_sql, cte_map);
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());

  EXPECT_EQ(CountSubstr(norm, "WITH "), 0u);
  EXPECT_EQ(norm.find(" a AS "), std::string::npos);
  EXPECT_EQ(norm.find(" b AS "), std::string::npos);
  EXPECT_EQ(norm.find(" c AS "), std::string::npos);
  EXPECT_EQ(norm.find(" d AS "), std::string::npos);
  EXPECT_EQ(norm.find(" e AS "), std::string::npos);
}

// Original-accessed table names track base source tables, not CTE aliases used by the outer query.
// So we only assert base table names, not CTE names.
TEST(OriginalNames, CTEOnly_MainAndCTEBody) {
  const std::string origin_sql = "SELECT * FROM c1";
  std::map<std::string, DB::CommonTableExpr> cte_map{{"c1", {"c1", "SELECT id FROM s1"}}};

  auto [out, names] = RunCTERewriteWithNames(origin_sql, cte_map);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());

  std::unordered_set<std::string> s(names.begin(), names.end());
  // Only base table inside CTE body should be present.
  EXPECT_TRUE(s.count("s1")) << "cte body base table should be present";
}

TEST(OriginalNames, TableOnly_KeepOriginalNames) {
  const std::string origin_sql = "SELECT * FROM Withdraw";
  std::map<std::string, DB::CommonTableExpr> cte_map{};
  std::map<std::string, std::string> table_map{{"Withdraw", "autogen_456"}};

  auto [out, names] = RunCTEAndTableRewriteWithNames(origin_sql, cte_map, table_map);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());

  std::unordered_set<std::string> s(names.begin(), names.end());
  EXPECT_TRUE(s.count("Withdraw"));
  EXPECT_FALSE(s.count("autogen_456")) << "rewritten name should not appear in original names";
}

// Original-accessed table names track base source tables, not CTE aliases used by the outer query.
// So we only assert base table names, not CTE names.
TEST(OriginalNames, CTEAndTableRewrite_Combined) {
  const std::string origin_sql =
    "SELECT * FROM `sqlquery-123abc` LIMIT 1 UNION SELECT * FROM Withdraw";

  std::map<std::string, DB::CommonTableExpr> cte_map{
    {"sqlquery-123abc", {"sqlquery-123abc", "SELECT * FROM Transfer"}},
  };
  std::map<std::string, std::string> table_map{
    {"Transfer", "autogen_123"}, {"Withdraw", "autogen_456"}};

  auto [out, names] = RunCTEAndTableRewriteWithNames(origin_sql, cte_map, table_map);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());

  std::unordered_set<std::string> s(names.begin(), names.end());
  // Only base tables should be present in the set, not CTE names.
  // EXPECT_TRUE(s.count("Transfer"));
  EXPECT_TRUE(s.count("Withdraw"));
  // Rewritten names must not appear in original list.
  EXPECT_FALSE(s.count("autogen_123"));
  EXPECT_FALSE(s.count("autogen_456"));
}

TEST(OriginalNames, NoTables_NoCTE) {
  const std::string origin_sql = "SELECT 1";
  std::map<std::string, DB::CommonTableExpr> cte_map{};

  auto [out, names] = RunCTERewriteWithNames(origin_sql, cte_map);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());
  EXPECT_TRUE(names.empty());
}

// ----- AccessedTable resolution: structured original_accessed_tables field -----
//
// These tests exercise the logical_database / physical_database / is_remote
// fields populated for every entry in response.original_accessed_tables. They
// run the full RewriterServiceImpl path so the same code that handles real
// requests is what's under test.

namespace {

// Look up the resolution for `qualify(db, table)` (or bare `table` when db is
// empty) in resp.original_accessed_tables. ASSERTs the entry exists so test
// failures point at the missing access rather than a silent miss.
const rewriter::AccessedTable *FindAccessed(
  const rewriter::RewriteSQLResponse &resp,
  const std::string &origin_db,
  const std::string &origin_table) {
  for (const auto &t : resp.original_accessed_tables()) {
    if (t.original_database() == origin_db && t.original_table() == origin_table) {
      return &t;
    }
  }
  return nullptr;
}

rewriter::RewriteSQLResponse RunSelectWithStaticArgs(
  const std::string &sql,
  const std::map<std::string, std::string> &table_map,
  const std::map<std::string, std::pair<std::string, std::string>>
    &remote_table_map = {},
  const std::map<std::string, std::pair<std::string, std::string>>
    &table_with_db_map = {}) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *args = opt->mutable_table_name_args()->mutable_static_args();
  for (const auto &kv : table_map) (*args->mutable_table_map())[kv.first] = kv.second;
  for (const auto &kv : remote_table_map) {
    auto &rt = (*args->mutable_remote_table_map())[kv.first];
    rt.set_addr("ch:9000");
    rt.set_database(kv.second.first);
    rt.set_table(kv.second.second);
    rt.set_user("u");
    rt.set_password("p");
  }
  for (const auto &kv : table_with_db_map) {
    auto &twd = (*args->mutable_table_with_database_map())[kv.first];
    twd.set_database(kv.second.first);
    twd.set_table(kv.second.second);
  }
  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  service.Rewrite(&ctx, &req, &resp);
  return resp;
}

rewriter::RewriteSQLResponse RunSelectWithDynamicArgs(
  const std::string &sql,
  const std::map<std::string, std::string> &database_map,
  const std::vector<std::string> &known_physical = {},
  const std::string &upstream_logical_in_context = "",
  const std::map<std::string, std::string> &remote_index = {},
  const std::map<std::string, std::tuple<std::string, std::string, std::string>>
    &remote_upstreams = {}) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
  for (const auto &kv : database_map) (*dyn->mutable_database_map())[kv.first] = kv.second;
  for (const auto &p : known_physical) dyn->add_known_physical_databases(p);
  dyn->set_upstream_logical_database_in_context(upstream_logical_in_context);
  for (const auto &kv : remote_index) {
    (*dyn->mutable_logical_database_to_remote_upstream_index())[kv.first] = kv.second;
  }
  for (const auto &kv : remote_upstreams) {
    auto &up = (*dyn->mutable_remote_upstreams())[kv.first];
    up.set_addr(std::get<0>(kv.second));
    up.set_user(std::get<1>(kv.second));
    up.set_password(std::get<2>(kv.second));
  }
  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  service.Rewrite(&ctx, &req, &resp);
  return resp;
}

} // namespace

TEST(AccessedTable, Static_TableMapHit_KeepsOriginDB) {
  // Qualified target — `db.t` rename leaves db unchanged.
  auto resp = RunSelectWithStaticArgs(
    "SELECT * FROM mydb.t", {{"mydb.t", "t_renamed"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto *e = FindAccessed(resp, "mydb", "t");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->logical_database(), "");           // static mode: no logical concept.
  EXPECT_EQ(e->physical_database(), "mydb");       // table_map only renames.
  EXPECT_FALSE(e->is_remote());
}

TEST(AccessedTable, Static_TableMapHit_BareTable_PhysicalUnknown) {
  // Bare table — rewriter doesn't track session-default db, so physical is empty.
  auto resp = RunSelectWithStaticArgs("SELECT * FROM t", {{"t", "t_renamed"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto *e = FindAccessed(resp, "", "t");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->logical_database(), "");
  EXPECT_EQ(e->physical_database(), "");
  EXPECT_FALSE(e->is_remote());
}

TEST(AccessedTable, Static_RemoteTableMapHit_FlagsRemote) {
  auto resp = RunSelectWithStaticArgs(
    "SELECT * FROM t",
    /*table_map*/ {},
    /*remote_table_map*/ {{"t", {"remote_db", "remote_t"}}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto *e = FindAccessed(resp, "", "t");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->physical_database(), "remote_db");
  EXPECT_TRUE(e->is_remote());
}

TEST(AccessedTable, Static_TableWithDatabaseHit_UsesEntryDB) {
  auto resp = RunSelectWithStaticArgs(
    "SELECT * FROM t",
    /*table_map*/ {},
    /*remote_table_map*/ {},
    /*table_with_db_map*/ {{"t", {"explicit_db", "t_renamed"}}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto *e = FindAccessed(resp, "", "t");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->physical_database(), "explicit_db");
  EXPECT_FALSE(e->is_remote());
}

TEST(AccessedTable, Static_Unmatched_PhysicalEmpty) {
  // Static-mode passthrough — rewriter doesn't know the physical db.
  auto resp = RunSelectWithStaticArgs(
    "SELECT * FROM untouched", {{"other", "still_other"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto *e = FindAccessed(resp, "", "untouched");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->logical_database(), "");
  EXPECT_EQ(e->physical_database(), "");
  EXPECT_FALSE(e->is_remote());
}

TEST(AccessedTable, Dynamic_QualifiedLogical_ResolvesPhysical) {
  auto resp = RunSelectWithDynamicArgs(
    "SELECT * FROM tenant1.events",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto *e = FindAccessed(resp, "tenant1", "events");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->logical_database(), "tenant1");
  EXPECT_EQ(e->physical_database(), "physical_1");
  EXPECT_FALSE(e->is_remote());
}

TEST(AccessedTable, Dynamic_BareTable_UsesUpstreamContext) {
  auto resp = RunSelectWithDynamicArgs(
    "SELECT * FROM events",
    /*database_map*/ {{"tenant1", "physical_1"}},
    /*known_physical*/ {},
    /*upstream_logical_in_context*/ "tenant1");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto *e = FindAccessed(resp, "", "events");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->logical_database(), "tenant1");
  EXPECT_EQ(e->physical_database(), "physical_1");
  EXPECT_FALSE(e->is_remote());
}

TEST(AccessedTable, Dynamic_KnownPhysicalPassthrough) {
  // System-table style passthrough: logical resolves to itself as physical.
  auto resp = RunSelectWithDynamicArgs(
    "SELECT * FROM system.numbers",
    /*database_map*/ {},
    /*known_physical*/ {"system"});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto *e = FindAccessed(resp, "system", "numbers");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->logical_database(), "system");
  EXPECT_EQ(e->physical_database(), "system");
  EXPECT_FALSE(e->is_remote());
}

TEST(AccessedTable, Dynamic_Unresolvable_PhysicalEmpty_LogicalKept) {
  // SELECT is lenient — unresolvable identifiers stay in the response with
  // logical_database reflecting what we'd have used (here: SQL qualifier).
  auto resp = RunSelectWithDynamicArgs(
    "SELECT * FROM ghost.t",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto *e = FindAccessed(resp, "ghost", "t");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->logical_database(), "ghost");
  EXPECT_EQ(e->physical_database(), "");
  EXPECT_FALSE(e->is_remote());
}

TEST(AccessedTable, Dynamic_RemoteMappedLogical_FlagsRemote) {
  auto resp = RunSelectWithDynamicArgs(
    "SELECT * FROM tenant_remote.events",
    /*database_map*/ {{"tenant_remote", "physical_remote"}},
    /*known_physical*/ {},
    /*upstream_logical_in_context*/ "",
    /*remote_index*/ {{"tenant_remote", "us_east"}},
    /*remote_upstreams*/ {{"us_east", {"ch-remote:9000", "ro", "secret"}}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto *e = FindAccessed(resp, "tenant_remote", "events");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->logical_database(), "tenant_remote");
  EXPECT_EQ(e->physical_database(), "physical_remote");
  EXPECT_TRUE(e->is_remote());
}

TEST(SelectDynamic, InlineCTE_AliasIsNotRewritten) {
  // `WITH main AS (SELECT * FROM xxx) SELECT * FROM main` — the bare `main`
  // in the outer FROM is a CTE alias, not a real table, and must pass through
  // the rewrite untouched even though it would otherwise resolve via the
  // upstream-logical-in-context. The CTE body's real table (`xxx`) still
  // gets rewritten.
  auto resp = RunSelectWithDynamicArgs(
    "WITH main AS (SELECT * FROM xxx) SELECT * FROM main",
    /*database_map*/ {{"testnet", "physical_testnet"}},
    /*known_physical*/ {},
    /*upstream_logical_in_context*/ "testnet");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const std::string &out = resp.sql_after_rewrite();
  EXPECT_EQ(out.find("testnet.main"), std::string::npos)
    << "CTE alias `main` was rewritten as if it were a table: " << out;
  EXPECT_EQ(out.find("physical_testnet.main"), std::string::npos)
    << "CTE alias `main` was rewritten as if it were a table: " << out;
  // The CTE body's real table reference still gets rewritten.
  EXPECT_NE(out.find("`testnet.xxx`"), std::string::npos)
    << "CTE body table `xxx` should have been rewritten: " << out;

  // CTE alias should not appear in original_accessed_tables either.
  EXPECT_EQ(FindAccessed(resp, "", "main"), nullptr)
    << "CTE alias `main` leaked into original_accessed_tables";
  const auto *xxx = FindAccessed(resp, "", "xxx");
  ASSERT_NE(xxx, nullptr);
  EXPECT_EQ(xxx->logical_database(), "testnet");
  EXPECT_EQ(xxx->physical_database(), "physical_testnet");

  // table_rewrites must not contain the CTE alias as a key.
  EXPECT_EQ(resp.table_rewrites().count("main"), 0u);
}

TEST(SelectDynamic, InlineCTE_QualifiedAliasNameIsStillRewritten) {
  // Qualifying with a db turns it back into a real table reference — the
  // CTE-skip should only apply to bare references.
  auto resp = RunSelectWithDynamicArgs(
    "WITH main AS (SELECT 1) SELECT * FROM testnet.main",
    /*database_map*/ {{"testnet", "physical_testnet"}},
    /*known_physical*/ {},
    /*upstream_logical_in_context*/ "");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_NE(resp.sql_after_rewrite().find("`testnet.main`"), std::string::npos)
    << "qualified `testnet.main` should be rewritten: "
    << resp.sql_after_rewrite();
}

TEST(SelectDynamic, InlineCTE_NestedSelectAliasShadowsScope) {
  // CTE aliases declared inside a subquery must NOT leak to outer scope —
  // the outer `FROM x` should still be rewritten as a real table.
  auto resp = RunSelectWithDynamicArgs(
    "SELECT * FROM x WHERE id IN (WITH x AS (SELECT 1) SELECT * FROM x)",
    /*database_map*/ {{"testnet", "physical_testnet"}},
    /*known_physical*/ {},
    /*upstream_logical_in_context*/ "testnet");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  // Outer `FROM x` should be rewritten (it's a real table in scope), but the
  // inner subquery's `FROM x` references the local CTE and must not be.
  EXPECT_NE(resp.sql_after_rewrite().find("`testnet.x`"), std::string::npos)
    << "outer `x` should have been rewritten: " << resp.sql_after_rewrite();
}

TEST(AccessedTable, NoTableNameRewriteOption_AllResolvedFieldsEmpty) {
  // No TableNameRewrite option at all — original_database/original_table are
  // still populated, but the resolved fields stay empty.
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql("SELECT * FROM mydb.t");
  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  service.Rewrite(&ctx, &req, &resp);
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto *e = FindAccessed(resp, "mydb", "t");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->logical_database(), "");
  EXPECT_EQ(e->physical_database(), "");
  EXPECT_FALSE(e->is_remote());
}

// ----- AccessedTable: non-SELECT handlers -----
//
// Mirrors the SELECT-side coverage above for every non-SELECT handler that
// goes through rewriteOneTarget (writes.cc) plus exists.cc / show_create.cc.
// Confirms each records exactly one AccessedTable per (db, table) target it
// processed, with logical/physical/is_remote populated against the active
// TableNameRewrite mode — including static-mode passthrough and the
// no-option case.

namespace {

rewriter::RewriteSQLResponse RunWithDynamicArgs(
  const std::string &sql,
  const std::map<std::string, std::string> &database_map,
  const std::vector<std::string> &known_physical = {},
  const std::string &upstream_logical = "") {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
  for (const auto &kv : database_map) (*dyn->mutable_database_map())[kv.first] = kv.second;
  for (const auto &p : known_physical) dyn->add_known_physical_databases(p);
  dyn->set_upstream_logical_database_in_context(upstream_logical);
  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  service.Rewrite(&ctx, &req, &resp);
  return resp;
}

rewriter::RewriteSQLResponse RunWithStaticTableWithDb(
  const std::string &sql,
  const std::string &origin_key,
  const std::string &physical_db,
  const std::string &new_table) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *args = opt->mutable_table_name_args()->mutable_static_args();
  auto &twd = (*args->mutable_table_with_database_map())[origin_key];
  twd.set_database(physical_db);
  twd.set_table(new_table);
  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  service.Rewrite(&ctx, &req, &resp);
  return resp;
}

rewriter::RewriteSQLResponse RunNoOptions(const std::string &sql) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);
  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  service.Rewrite(&ctx, &req, &resp);
  return resp;
}

} // namespace

TEST(AccessedTable, Insert_Dynamic_RecordsTargetWithPhysical) {
  auto resp = RunWithDynamicArgs(
    "INSERT INTO tenant1.events VALUES (1)",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  ASSERT_EQ(resp.original_accessed_tables_size(), 1);
  const auto *e = FindAccessed(resp, "tenant1", "events");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->logical_database(), "tenant1");
  EXPECT_EQ(e->physical_database(), "physical_1");
  EXPECT_FALSE(e->is_remote());
}

TEST(AccessedTable, CreateTable_StaticTableWithDb_RecordsPhysical) {
  auto resp = RunWithStaticTableWithDb(
    "CREATE TABLE t (a Int32) ENGINE=Memory",
    /*origin_key*/ "t",
    /*physical_db*/ "explicit_db",
    /*new_table*/ "t_renamed");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  ASSERT_EQ(resp.original_accessed_tables_size(), 1);
  const auto *e = FindAccessed(resp, "", "t");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->physical_database(), "explicit_db");
}

TEST(ExistenceClause, CreateTable_IfNotExists) {
  auto resp = RunNoOptions("CREATE TABLE IF NOT EXISTS t (a Int32) ENGINE=Memory");
  EXPECT_EQ(resp.existence_clause(), rewriter::EXISTENCE_CLAUSE_IF_NOT_EXISTS);
}

TEST(ExistenceClause, CreateTable_Plain_Unspecified) {
  auto resp = RunNoOptions("CREATE TABLE t (a Int32) ENGINE=Memory");
  EXPECT_EQ(resp.existence_clause(), rewriter::EXISTENCE_CLAUSE_UNSPECIFIED);
}

TEST(ExistenceClause, CreateDatabase_IfNotExists) {
  // CREATE DATABASE with no dynamic_args is rejected, but existence_clause is
  // a pure syntactic property set before dispatch — so it's still accurate.
  auto resp = RunNoOptions("CREATE DATABASE IF NOT EXISTS d");
  EXPECT_EQ(resp.existence_clause(), rewriter::EXISTENCE_CLAUSE_IF_NOT_EXISTS);
}

TEST(ExistenceClause, DropTable_IfExists) {
  auto resp = RunNoOptions("DROP TABLE IF EXISTS mydb.t");
  EXPECT_EQ(resp.existence_clause(), rewriter::EXISTENCE_CLAUSE_IF_EXISTS);
}

TEST(ExistenceClause, DropDatabase_IfExists) {
  auto resp = RunNoOptions("DROP DATABASE IF EXISTS d");
  EXPECT_EQ(resp.existence_clause(), rewriter::EXISTENCE_CLAUSE_IF_EXISTS);
}

TEST(ExistenceClause, TruncateTable_IfExists) {
  auto resp = RunNoOptions("TRUNCATE TABLE IF EXISTS mydb.t");
  EXPECT_EQ(resp.existence_clause(), rewriter::EXISTENCE_CLAUSE_IF_EXISTS);
}

TEST(ExistenceClause, Select_Unspecified) {
  auto resp = RunNoOptions("SELECT 1");
  EXPECT_EQ(resp.existence_clause(), rewriter::EXISTENCE_CLAUSE_UNSPECIFIED);
}

TEST(AccessedTable, DropTable_NoOptions_StillRecordsOriginal) {
  // Even with no TableNameRewrite option, the rewriter records what was
  // accessed so callers always get a uniform view of the request shape.
  auto resp = RunNoOptions("DROP TABLE mydb.t");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  ASSERT_EQ(resp.original_accessed_tables_size(), 1);
  const auto *e = FindAccessed(resp, "mydb", "t");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->logical_database(), "");    // No mode → no logical resolved.
  EXPECT_EQ(e->physical_database(), "");
  EXPECT_FALSE(e->is_remote());
}

TEST(AccessedTable, TruncateTable_Dynamic_RecordsTarget) {
  auto resp = RunWithDynamicArgs(
    "TRUNCATE TABLE tenant1.events",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  ASSERT_EQ(resp.original_accessed_tables_size(), 1);
  const auto *e = FindAccessed(resp, "tenant1", "events");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->physical_database(), "physical_1");
}

TEST(AccessedTable, AlterTable_Dynamic_RecordsTarget) {
  auto resp = RunWithDynamicArgs(
    "ALTER TABLE tenant1.events ADD COLUMN c Int32",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  ASSERT_EQ(resp.original_accessed_tables_size(), 1);
  const auto *e = FindAccessed(resp, "tenant1", "events");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->physical_database(), "physical_1");
}

TEST(AccessedTable, Update_Dynamic_RecordsTarget) {
  auto resp = RunWithDynamicArgs(
    "UPDATE tenant1.events SET a = 1 WHERE id = 0",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto *e = FindAccessed(resp, "tenant1", "events");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->physical_database(), "physical_1");
}

TEST(AccessedTable, Delete_Dynamic_RecordsTarget) {
  auto resp = RunWithDynamicArgs(
    "DELETE FROM tenant1.events WHERE id = 0",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto *e = FindAccessed(resp, "tenant1", "events");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->physical_database(), "physical_1");
}

TEST(AccessedTable, Rename_RecordsBothSides) {
  auto resp = RunWithDynamicArgs(
    "RENAME TABLE tenant1.a TO tenant1.b",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  ASSERT_EQ(resp.original_accessed_tables_size(), 2);
  const auto *from = FindAccessed(resp, "tenant1", "a");
  const auto *to = FindAccessed(resp, "tenant1", "b");
  ASSERT_NE(from, nullptr);
  ASSERT_NE(to, nullptr);
  EXPECT_EQ(from->physical_database(), "physical_1");
  EXPECT_EQ(to->physical_database(), "physical_1");
}

TEST(AccessedTable, CreateTableAsTable_RecordsTargetAndSource) {
  // CREATE TABLE x AS source — both target and source flow through
  // rewriteOneTarget, so both get recorded. Distinct from the AS SELECT
  // form which doesn't record the inner SELECT's tables (writes.cc doesn't
  // walk into the subquery).
  auto resp = RunWithDynamicArgs(
    "CREATE TABLE tenant1.t AS tenant1.src",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  ASSERT_EQ(resp.original_accessed_tables_size(), 2);
  EXPECT_NE(FindAccessed(resp, "tenant1", "t"), nullptr);
  EXPECT_NE(FindAccessed(resp, "tenant1", "src"), nullptr);
}

TEST(AccessedTable, ExistsTable_Dynamic_RecordsTarget) {
  auto resp = RunWithDynamicArgs(
    "EXISTS TABLE tenant1.events",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  ASSERT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_EXISTS_TABLE);
  ASSERT_EQ(resp.original_accessed_tables_size(), 1);
  const auto *e = FindAccessed(resp, "tenant1", "events");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->physical_database(), "physical_1");
}

TEST(AccessedTable, ShowCreateTable_Dynamic_RecordsTarget) {
  auto resp = RunWithDynamicArgs(
    "SHOW CREATE TABLE tenant1.events",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  ASSERT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_SHOW_CREATE_TABLE);
  ASSERT_EQ(resp.original_accessed_tables_size(), 1);
  const auto *e = FindAccessed(resp, "tenant1", "events");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->physical_database(), "physical_1");
}

TEST(AccessedTable, CreateDatabase_RecordsLogicalEmptyTable) {
  // CREATE DATABASE records a db-level AccessedTable: original_table empty,
  // logical_database set to the target. physical_database stays empty
  // because the new logical isn't in `database_map` yet (precondition for
  // the request to succeed).
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql("CREATE DATABASE newdb");
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  // dynamic_args present so CREATE DATABASE is accepted (debug-rewrite path).
  opt->mutable_table_name_args()->mutable_dynamic_args();
  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  service.Rewrite(&ctx, &req, &resp);
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  ASSERT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_CREATE_DATABASE);
  ASSERT_EQ(resp.original_accessed_tables_size(), 1);
  const auto &e = resp.original_accessed_tables(0);
  EXPECT_EQ(e.original_database(), "newdb");
  EXPECT_TRUE(e.original_table().empty());
  EXPECT_EQ(e.logical_database(), "newdb");
  EXPECT_TRUE(e.physical_database().empty());
  EXPECT_FALSE(e.is_remote());
}

TEST(AccessedTable, DropDatabase_RecordsLogicalAndPhysical) {
  // DROP DATABASE on a `database_map` hit fills physical_database with the
  // resolved physical so the GC binary can correlate the access with its
  // physical-side cleanup target.
  auto resp = RunWithDynamicArgs(
    "DROP DATABASE tenant1",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  ASSERT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_DROP_DATABASE);
  ASSERT_EQ(resp.original_accessed_tables_size(), 1);
  const auto &e = resp.original_accessed_tables(0);
  EXPECT_EQ(e.original_database(), "tenant1");
  EXPECT_TRUE(e.original_table().empty());
  EXPECT_EQ(e.logical_database(), "tenant1");
  EXPECT_EQ(e.physical_database(), "physical_1");
  EXPECT_FALSE(e.is_remote());
}

TEST(AccessedTable, DropDatabase_IfExists_Miss_RecordsAccessOnly) {
  // IF EXISTS on an unmanaged db: the rewrite still succeeds (silently
  // falls through), and the access is still recorded — but with empty
  // physical since the logical isn't in database_map.
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql("DROP DATABASE IF EXISTS unknown");
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
  (*dyn->mutable_database_map())["tenant1"] = "physical_1";
  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  service.Rewrite(&ctx, &req, &resp);
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  ASSERT_EQ(resp.original_accessed_tables_size(), 1);
  const auto &e = resp.original_accessed_tables(0);
  EXPECT_EQ(e.original_database(), "unknown");
  EXPECT_TRUE(e.original_table().empty());
  EXPECT_EQ(e.logical_database(), "unknown");
  EXPECT_TRUE(e.physical_database().empty());
}

TEST(ERROR_MESSAGE_REWRITE, DynamicArgs_PhysicalNameInvertedToLogical) {
  // CH error with the dynamic-rewriter's `<physical_db>.`<logical>.<table>``
  // form (table half is dot-quoted because it contains a dot).
  const std::string kErr =
    "Code: 60. DB::Exception: Table physical_db_1.`tenant1.events` does not exist. "
    "(UNKNOWN_TABLE) (version 25.6.4.12 (official build))";
  // User's original SQL — bare logical table.
  const std::string kSQL = "SELECT count(*) FROM tenant1.events";

  ServerContext server_context;
  RewriteErrorMessageRequest request;
  RewriteErrorMessageResponse response;
  request.set_sql(kSQL);
  request.set_error_message(kErr);
  {
    auto *opt = request.add_options();
    opt->set_op(rewriter::RewriteOp::TableNameRewrite);
    auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
    (*dyn->mutable_database_map())["tenant1"] = "physical_db_1";
  }

  RewriterServiceImpl service;
  auto status = service.RewriteErrorMessage(&server_context, &request, &response);
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(response.code(), rewriter::RewriteCode::Success);
  // Every physical mention of the table should be back to its logical form,
  // and no physical_db.`tenant1.events` artefact should remain.
  EXPECT_NE(response.error_after_rewrite().find("tenant1.events"), std::string::npos);
  EXPECT_EQ(response.error_after_rewrite().find("physical_db_1"), std::string::npos);
  EXPECT_EQ(response.error_after_rewrite().find("`tenant1.events`"), std::string::npos);
}

TEST(ERROR_MESSAGE_REWRITE, PositionRemappedAfterSQLSwap) {
  // Verify CH-style "failed at position N" gets remapped to the equivalent
  // position in the user's original SQL. The rewriter inserts a long prefix
  // ("physical_db_1.`tenant1.") in front of the table identifier; without
  // the alignment table, "position N" in the rewritten coords would point
  // somewhere meaningless after the substring swap.
  const std::string kSQL = "SELECT * FROM events";

  // First do a Rewrite to learn the exact rewritten SQL the rewriter
  // produces (formatter quirks: hilite, casing, whitespace) so the test
  // doesn't depend on the formatter implementation.
  ServerContext server_context;
  RewriteSQLRequest rw_req;
  rw_req.set_sql(kSQL);
  {
    auto *opt = rw_req.add_options();
    opt->set_op(rewriter::RewriteOp::TableNameRewrite);
    auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
    (*dyn->mutable_database_map())["tenant1"] = "physical_db_1";
    dyn->set_upstream_logical_database_in_context("tenant1");
  }
  RewriteSQLResponse rw_resp;
  RewriterServiceImpl service;
  ASSERT_TRUE(service.Rewrite(&server_context, &rw_req, &rw_resp).ok());
  ASSERT_EQ(rw_resp.code(), rewriter::RewriteCode::Success);
  const std::string &rewritten = rw_resp.sql_after_rewrite();

  // Find a recognizable token in rewritten — "events" appears inside the
  // backticked dynamic-rewrite form `physical_db_1.\`tenant1.events\`` and
  // also as the alias the SELECT path appends (AS events). Pick the FIRST
  // occurrence: it's the actual rewritten table reference and Myers will
  // align it to the original "events". The alias is rewriter-added and has
  // no original counterpart — Myers correctly maps it to end-of-original.
  const auto rewritten_off = rewritten.find("events");
  ASSERT_NE(rewritten_off, std::string::npos);
  const auto original_off = kSQL.find("events");
  ASSERT_NE(original_off, std::string::npos);
  const size_t rewritten_pos1 = rewritten_off + 1;  // CH is 1-based.
  const size_t expected_pos1 = original_off + 1;

  // CH's actual error format: see clickHouse/src/Parsers/parseQuery.cpp.
  std::ostringstream err_oss;
  err_oss << "Code: 62. DB::Exception: Syntax error: failed at position "
          << rewritten_pos1 << " (events) (something something).";
  const std::string kErr = err_oss.str();

  RewriteErrorMessageRequest req;
  RewriteErrorMessageResponse resp;
  req.set_sql(kSQL);
  req.set_error_message(kErr);
  *req.mutable_options() = rw_req.options();

  ASSERT_TRUE(service.RewriteErrorMessage(&server_context, &req, &resp).ok());
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  // Stale rewritten-coord position must not survive (would mislead a user
  // reading the message against their original SQL).
  std::ostringstream stale_oss;
  stale_oss << "position " << rewritten_pos1;
  EXPECT_EQ(resp.error_after_rewrite().find(stale_oss.str()), std::string::npos)
      << "stale '" << stale_oss.str() << "' should be gone: "
      << resp.error_after_rewrite();
  // Remapped position must land somewhere inside the original 'events'
  // token. Myers can pick any alignment with the same LCS length, so we
  // accept any byte offset within [original_off, original_off+len("events")]
  // (1-based) — both 15 (start) and 20 (end) are valid alignments.
  std::regex pos_re(R"(position\s+(\d+))");
  std::smatch m;
  ASSERT_TRUE(std::regex_search(resp.error_after_rewrite(), m, pos_re))
      << "no position remained in: " << resp.error_after_rewrite();
  const size_t got_pos1 = std::stoull(m[1].str());
  EXPECT_GE(got_pos1, original_off + 1);
  EXPECT_LE(got_pos1, original_off + std::string("events").size() + 1);
}

TEST(ERROR_MESSAGE_REWRITE, ManualCase_CannotParseDateTime) {
  // ---------- Full original error (as returned by ClickHouse) ----------
  const std::string kErr =
    R"MSG(Code: 41. DB::Exception: Cannot parse datetime: Cannot parse DateTime from String: In scope WITH base AS (SELECT * FROM prod_subgraph.`8CsFI8hZ_latestView_PoolSnapshot` AS PoolSnapshot) SELECT formatDateTime(toDateTime(base.timestamp), '%Y-%m-%d %H:%i') AS timestamp, formatDateTime(dateTrunc('day', toDateTime(base.timestamp)), '%Y-%m-%d %H:%i') AS block_date, base.* EXCEPT (timestamp, __genBlockChain__) FROM base WHERE toDateTime(base.timestamp, 'UTC') > toDateTime('{{timestamp}}') ORDER BY base.timestamp DESC LIMIT 100 SETTINGS log_comment = '{""processor_id"":""8CsFI8hZ"",""processor_version"":63,""project_id"":""8CsFI8hZ"",""project_name"":""paeter3/club246test"",""method"":""ExecuteInternalSQL"",""user_id"":""UtjecdnRnjRB"",""async_query_id"":""sqlquery-KnJ4npcm"",""async_execution_id"":""sqlexec-LaXFqjlq""}', max_execution_time = 600. (CANNOT_PARSE_DATETIME) (version 25.6.4.12 (official build))MSG";

  // ---------- Manually inferred ORIGINAL SQL (pre-rewrite) ----------
  // Only the source table is swapped for its logical name; the rest of the SQL stays identical
  // so the user can read the post-substitution result without confusion.
  const std::string kOriginalSQL = R"(WITH base AS (
    SELECT * FROM latestView_PoolSnapshot AS PoolSnapshot
  )
  SELECT
    formatDateTime(toDateTime(base.timestamp), '%Y-%m-%d %H:%i') AS timestamp,
    formatDateTime(dateTrunc('day', toDateTime(base.timestamp)), '%Y-%m-%d %H:%i') AS block_date,
    base.* EXCEPT (timestamp, __genBlockChain__)
  FROM base
  WHERE toDateTime(base.timestamp, 'UTC') > toDateTime('{{timestamp}}')
  ORDER BY base.timestamp DESC
  LIMIT 100)";

  // ---------- Table rename map used by the rewriter for finer replacements ----------
  // "prod_subgraph.`8CsFI8hZ_latestView_PoolSnapshot`"  ->  "latestView_PoolSnapshot"
  ServerContext server_context;
  RewriteErrorMessageRequest request;
  RewriteErrorMessageResponse response;
  request.set_error_message(kErr);
  request.set_sql(kOriginalSQL);
  {
    // Build options according to proto3: we pass ORIGINAL sql + a TableNameRewrite mapping
    // ORIGINAL->REWRITTEN latestView_PoolSnapshot  ->
    // prod_subgraph.`8CsFI8hZ_latestView_PoolSnapshot`
    auto *opt = request.add_options();
    opt->set_op(rewriter::RewriteOp::TableNameRewrite);
    auto *args = opt->mutable_table_name_args()->mutable_static_args();
    // "prod_subgraph.`8CsFI8hZ_latestView_PoolSnapshot`";;

    auto &item = (*args->mutable_table_with_database_map())["latestView_PoolSnapshot"];
    item.set_database("prod_subgraph");
    item.set_table("8CsFI8hZ_latestView_PoolSnapshot");
  }

  RewriterServiceImpl service;
  auto status = service.RewriteErrorMessage(&server_context, &request, &response);
  ASSERT_TRUE(status.ok());
  // Print the whole response so a human can eyeball the final substitution.
  std::cout << "\n=== Rewriter Response (Case: CANNOT_PARSE_DATETIME) ===\n"
            << response.DebugString() << std::endl;
}


TEST(ERROR_MESSAGE_REWRITE, ManualCase_UnknownFunctionArgMin) {
  // ---------- Full original error ----------
  const std::string kErr =
    R"MSG(Code: 46. DB::Exception: Function with name `argmin` does not exist. In scope SELECT market_id, toUnixTimestamp64Milli(argmin(ts, ts)) AS unix_timestamp_ms, argMin(fields, ts) AS fields, argMin(event_seq, ts) AS event_seq FROM (SELECT timestamp AS ts, fields, event_seq, CAST(JSON_VALUE(fields, '$.event.market_id'), 'INT') AS market_id FROM remote('clickhouse-chain-part-b.clickhouse:9000', 'chain_db', 'sui-mainnet.events', 'sentio_viewer', '[HIDDEN]') AS `sui.events` WHERE type = '0xd631cd66138909636fc3f73ed75820d0c5b76332d1644608ed1c85ea2b8219b4::events::Event<0xd631cd66138909636fc3f73ed75820d0c5b76332d1644608ed1c85ea2b8219b4::market::MarketDataUpdateEvent>') GROUP BY market_id ORDER BY first_unix_timestamp_ms ASC LIMIT 1 SETTINGS log_comment = '{""processor_id"":""LSbQpGYc"",""processor_version"":11,""project_id"":""LSbQpGYc"",""project_name"":""dave/test-project"",""method"":""ExecuteInternalSQL"",""user_id"":""QkRW9cS7bnYN"",""async_query_id"":""sqlquery-9Aq6g7nJ"",""async_execution_id"":""sqlexec-wldDqRiV""}', max_execution_time = 600. Maybe you meant: ['argMin','argMax']. (UNKNOWN_FUNCTION) (version 25.6.4.12 (official build))MSG";

  // ---------- Manually extracted REWRITTEN SQL ----------
  const std::string kRewrittenSQL =
    R"(SELECT market_id, toUnixTimestamp64Milli(argmin(ts, ts)) AS unix_timestamp_ms, argMin(fields, ts) AS fields, argMin(event_seq, ts) AS event_seq FROM (SELECT timestamp AS ts, fields, event_seq, CAST(JSON_VALUE(fields, '$.event.market_id'), 'INT') AS market_id FROM remote('clickhouse-chain-part-b.clickhouse:9000', 'chain_db', 'sui-mainnet.events', 'sentio_viewer', '[HIDDEN]') AS `sui.events` WHERE type = '0xd631cd66138909636fc3f73ed75820d0c5b76332d1644608ed1c85ea2b8219b4::events::Event<0xd631cd66138909636fc3f73ed75820d0c5b76332d1644608ed1c85ea2b8219b4::market::MarketDataUpdateEvent>') GROUP BY market_id ORDER BY first_unix_timestamp_ms ASC LIMIT 1 SETTINGS log_comment = '{""processor_id"":""LSbQpGYc"",""processor_version"":11,""project_id"":""LSbQpGYc"",""project_name"":""dave/test-project"",""method"":""ExecuteInternalSQL"",""user_id"":""QkRW9cS7bnYN"",""async_query_id"":""sqlquery-9Aq6g7nJ"",""async_execution_id"":""sqlexec-wldDqRiV""}', max_execution_time = 600)";

  // ---------- Manually inferred ORIGINAL SQL ----------
  const std::string kOriginalSQL = R"(SELECT
    market_id,
    toUnixTimestamp64Milli(argmin(ts, ts)) AS unix_timestamp_ms,
    argMin(fields, ts) AS fields,
    argMin(event_seq, ts) AS event_seq
  FROM (
    SELECT
      timestamp AS ts,
      fields,
      event_seq,
      CAST(JSON_VALUE(fields, '$.event.market_id'), 'INT') AS market_id
    FROM `sui.events`
    WHERE type = '0xd631cd66138909636fc3f73ed75820d0c5b76332d1644608ed1c85ea2b8219b4::events::Event<0xd631cd66138909636fc3f73ed75820d0c5b76332d1644608ed1c85ea2b8219b4::market::MarketDataUpdateEvent>'
  )
  GROUP BY market_id
  ORDER BY first_unix_timestamp_ms ASC
  LIMIT 1
  SETTINGS log_comment = '{""processor_id"":""LSbQpGYc"",""processor_version"":11,""project_id"":""LSbQpGYc"",""project_name"":""dave/test-project"",""method"":""ExecuteInternalSQL"",""user_id"":""QkRW9cS7bnYN"",""async_query_id"":""sqlquery-9Aq6g7nJ"",""async_execution_id"":""sqlexec-wldDqRiV""}', max_execution_time = 600
  )";

  // ---------- Table rename map ----------
  // "remote(... 'sui-mainnet.events' ...)"  ->  "`sui.events`"
  ServerContext server_context;
  RewriteErrorMessageRequest request;
  RewriteErrorMessageResponse response;
  request.set_error_message(kErr);
  request.set_sql(kOriginalSQL);
  {
    // ORIGINAL logical table -> remote() physical source
    auto *opt = request.add_options();
    opt->set_op(rewriter::RewriteOp::TableNameRewrite);
    auto *args = opt->mutable_table_name_args()->mutable_static_args();
    // Use remote_table_map to specify a structured remote source.
    auto &remote_map = *args->mutable_remote_table_map();
    auto *remote = &remote_map["sui.events"]; // key: original logical table (db.table)
    remote->set_addr("clickhouse-chain-part-b.clickhouse:9000");
    remote->set_database("chain_db");
    remote->set_table("sui-mainnet.events");
    remote->set_user("sentio_viewer");
    remote->set_password("[HIDDEN]");
  }

  RewriterServiceImpl service;
  auto status = service.RewriteErrorMessage(&server_context, &request, &response);
  ASSERT_TRUE(status.ok());
  std::cout << "\n=== Rewriter Response (Case: UNKNOWN_FUNCTION argmin) ===\n"
            << response.DebugString() << std::endl;
}

// ============================================================================
// Shared helper for non-SELECT tests driven by dynamic_args.
// ============================================================================

struct RemoteUpstreamCfg {
  std::string addr;
  std::string user;
  std::string password;
};

struct DynamicArgs {
  std::map<std::string, std::string> database_map;
  std::vector<std::string> known_physical_databases;
  std::string upstream_logical_database_in_context;
  std::string delim;  // "" → server defaults to "_"
  std::vector<std::string> extra_arguments;
  // std::nullopt → leave the optional field unset (proxy hasn't picked
  // a physical context yet). Empty string → set to "" (rare but valid).
  std::optional<std::string> upstream_physical_database_in_context;
  // logical DB -> upstream key, looked up in `remote_upstreams` below.
  std::map<std::string, std::string> logical_database_to_remote_upstream_index;
  std::map<std::string, RemoteUpstreamCfg> remote_upstreams;
};

static void applyDynamicArgs(rewriter::RewriteTableDynamicArgs *args,
                             const DynamicArgs &g) {
  for (const auto &kv : g.database_map) {
    (*args->mutable_database_map())[kv.first] = kv.second;
  }
  for (const auto &p : g.known_physical_databases) {
    args->add_known_physical_databases(p);
  }
  if (!g.upstream_logical_database_in_context.empty()) {
    args->set_upstream_logical_database_in_context(g.upstream_logical_database_in_context);
  }
  if (!g.delim.empty()) args->set_delim(g.delim);
  for (const auto &e : g.extra_arguments) args->add_extra_arguments(e);
  if (g.upstream_physical_database_in_context) {
    args->set_upstream_physical_database_in_context(*g.upstream_physical_database_in_context);
  }
  for (const auto &kv : g.logical_database_to_remote_upstream_index) {
    (*args->mutable_logical_database_to_remote_upstream_index())[kv.first] = kv.second;
  }
  for (const auto &kv : g.remote_upstreams) {
    auto &out = (*args->mutable_remote_upstreams())[kv.first];
    out.set_addr(kv.second.addr);
    out.set_user(kv.second.user);
    out.set_password(kv.second.password);
  }
}

struct DynamicProbe {
  rewriter::RewriteCode code;
  rewriter::StatementType statement_type;
  std::string sql_after_rewrite;
  std::string message;
  std::map<std::string, std::string> table_rewrites;
  std::map<std::string, std::string> database_rewrites;
};

static DynamicProbe RunDynamic(const std::string &sql, const DynamicArgs &g) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);

  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  applyDynamicArgs(opt->mutable_table_name_args()->mutable_dynamic_args(), g);

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  EXPECT_TRUE(status.ok()) << "gRPC status not OK for: " << sql;

  DynamicProbe p;
  p.code = resp.code();
  p.statement_type = resp.statement_type();
  p.sql_after_rewrite = resp.sql_after_rewrite();
  p.message = resp.message();
  for (const auto &kv : resp.table_rewrites()) p.table_rewrites[kv.first] = kv.second;
  for (const auto &kv : resp.database_rewrites()) p.database_rewrites[kv.first] = kv.second;
  return p;
}

// ============================================================================
// USE Statement Tests
// ============================================================================

TEST(UseStatement, Passthrough_NoOptions) {
  // No TableNameRewrite option at all -> echo the original SQL.
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql("USE mydb");
  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  service.Rewrite(&ctx, &req, &resp);
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_USE);
  EXPECT_NE(resp.sql_after_rewrite().find("mydb"), std::string::npos);
}

TEST(UseStatement, DatabaseMap_Hit) {
  auto r = RunDynamic("USE coinbase",
                      {.database_map = {{"coinbase", "sentio_coinbase"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_USE);
  EXPECT_NE(r.sql_after_rewrite.find("sentio_coinbase"), std::string::npos);
  // USE produces a DB-level rewrite. It MUST land in database_rewrites,
  // never in table_rewrites — the latter is reserved for entries with a
  // table component.
  EXPECT_EQ(r.database_rewrites["coinbase"], "sentio_coinbase");
  EXPECT_TRUE(r.table_rewrites.empty());
}

TEST(UseStatement, KnownPhysical_Passthrough) {
  // Target is already a known physical DB -> leave SQL unchanged, no
  // rewrite entry on either map.
  auto r = RunDynamic("USE sentio_coinbase",
                      {.known_physical_databases = {"sentio_coinbase"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_USE);
  EXPECT_NE(r.sql_after_rewrite.find("sentio_coinbase"), std::string::npos);
  EXPECT_TRUE(r.table_rewrites.empty());
  EXPECT_TRUE(r.database_rewrites.empty());
}

TEST(UseStatement, UnknownDatabase_Rejected) {
  auto r = RunDynamic("USE mydb",
                      {.database_map = {{"other", "sentio_other"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
}

TEST(UseStatement, UseWithBackticks) {
  auto r = RunDynamic("USE `my-db`",
                      {.database_map = {{"my-db", "sentio_mydb"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("sentio_mydb"), std::string::npos);
}

// ============================================================================
// SHOW TABLES Tests
// ============================================================================

TEST(ShowTables, Passthrough_NoOptions) {
  // No TableNameRewrite option -> echo back.
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql("SHOW TABLES");
  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  service.Rewrite(&ctx, &req, &resp);
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_SHOW_TABLES);
}

TEST(ShowTables, FromUpstreamContext_BuildsPrefix) {
  // No FROM clause; logical comes from upstream context. prefix = "coinbase.".
  auto r = RunDynamic("SHOW TABLES",
                      {.database_map = {{"coinbase", "sentio_coinbase"}},
                       .upstream_logical_database_in_context = "coinbase"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_SHOW_TABLES);
  // Filter targets the physical DB.
  EXPECT_NE(r.sql_after_rewrite.find("database = 'sentio_coinbase'"), std::string::npos);
  // Prefix = logical + "." (no extra_args; final separator is always a literal dot).
  EXPECT_NE(r.sql_after_rewrite.find("startsWith(name, 'coinbase.')"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("multiIf"), std::string::npos);
  // DB rewrite goes to database_rewrites, NOT table_rewrites.
  EXPECT_EQ(r.database_rewrites["coinbase"], "sentio_coinbase");
  EXPECT_TRUE(r.table_rewrites.empty());
}

TEST(ShowTables, FromClauseOverridesUpstream) {
  // `SHOW TABLES FROM mydb` -> logical = "mydb", not the upstream context.
  auto r = RunDynamic("SHOW TABLES FROM mydb",
                      {.database_map = {{"mydb", "physdb"}, {"coinbase", "sentio_coinbase"}},
                       .upstream_logical_database_in_context = "coinbase"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("database = 'physdb'"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("startsWith(name, 'mydb.')"), std::string::npos);
}

TEST(ShowTables, WithExtraArgs_PrefixIncludesThem) {
  // prefix = logical + delim + extras.join(delim) + "."
  // (the trailing separator before the original table name is always a
  //  literal dot — not the configurable delim).
  auto r = RunDynamic("SHOW TABLES",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1",
                       .extra_arguments = {"v2", "shard0"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("startsWith(name, 'tenant1_v2_shard0.')"),
            std::string::npos);
}

TEST(ShowTables, CustomDelim) {
  // The configurable delim joins logical + extras; the final separator
  // before the table name is still a literal "." regardless of delim.
  auto r = RunDynamic("SHOW TABLES",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1",
                       .delim = "-",
                       .extra_arguments = {"v2"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("startsWith(name, 'tenant1-v2.')"),
            std::string::npos);
}

TEST(ShowTables, NoContext_Rejected) {
  // No FROM clause and no upstream context -> can't pick a logical db.
  auto r = RunDynamic("SHOW TABLES",
                      {.database_map = {{"coinbase", "sentio_coinbase"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
}

TEST(ShowTables, UnknownLogical_Rejected) {
  auto r = RunDynamic("SHOW TABLES FROM mydb",
                      {.database_map = {{"other", "sentio_other"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
}

TEST(ShowTables, RemoteUpstream_RewritesToRemoteSystemTables) {
  // Logical 'transfer' is mapped to physical 'testnet1' AND routed to the
  // remote upstream "3". The handler should enumerate `system.tables` on
  // the upstream cluster via remote(...), filtered by (database, prefix).
  auto r = RunDynamic("SHOW TABLES",
                      {.database_map = {{"transfer", "testnet1"},
                                        {"tenant2", "testnet2"}},
                       .upstream_logical_database_in_context = "transfer",
                       .logical_database_to_remote_upstream_index = {{"transfer", "3"}},
                       .remote_upstreams = {{"3", {.addr = "localhost:3333",
                                                    .user = "username",
                                                    .password = "123"}}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_SHOW_TABLES);
  // Goes through remote() instead of bare system.tables.
  EXPECT_NE(r.sql_after_rewrite.find(
              "remote('localhost:3333', system, tables, 'username', '123')"),
            std::string::npos)
    << r.sql_after_rewrite;
  EXPECT_EQ(r.sql_after_rewrite.find("FROM system.tables"), std::string::npos)
    << "should not enumerate against the local system.tables: " << r.sql_after_rewrite;
  // Filter still uses the resolved physical name + prefix from the local
  // database_map (the upstream uses the same naming scheme).
  EXPECT_NE(r.sql_after_rewrite.find("database = 'testnet1'"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("startsWith(name, 'transfer.')"), std::string::npos);
  EXPECT_EQ(r.database_rewrites["transfer"], "testnet1");
}

TEST(ShowTables, RemoteUpstream_MissingKey_Rejected) {
  // Logical points at upstream key "ghost" but remote_upstreams has no
  // such entry — config bug, must reject InvalidRewriteRequest.
  auto r = RunDynamic("SHOW TABLES",
                      {.database_map = {{"transfer", "testnet1"}},
                       .upstream_logical_database_in_context = "transfer",
                       .logical_database_to_remote_upstream_index = {{"transfer", "ghost"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
  EXPECT_NE(r.message.find("ghost"), std::string::npos);
}

// ============================================================================
// SHOW DATABASES Tests
// ============================================================================

TEST(ShowDatabases, Passthrough_NoOptions) {
  // No TableNameRewrite option at all -> echo the original SQL with the
  // new STATEMENT_TYPE_SHOW_DATABASES classification.
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql("SHOW DATABASES");
  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  service.Rewrite(&ctx, &req, &resp);
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_SHOW_DATABASES);
  EXPECT_NE(resp.sql_after_rewrite().find("DATABASES"), std::string::npos);
  // No SELECT-from-system was synthesized.
  EXPECT_EQ(resp.sql_after_rewrite().find("system.tables"), std::string::npos);
  EXPECT_EQ(resp.sql_after_rewrite().find("system.databases"), std::string::npos);
}

TEST(ShowDatabases, DatabaseMap_AlphabeticalUnion) {
  // database_map -> one bare `SELECT '<logical>' AS name` subquery per
  // entry whose physical is in known_physical_databases, alphabetical by
  // logical name, joined with UNION ALL, wrapped with ORDER BY name. We
  // don't existence-check tables anymore: a declared+trusted logical DB
  // is always returned (an empty logical DB is still a valid DB).
  auto r = RunDynamic(
    "SHOW DATABASES",
    {.database_map = {{"tenant2", "testnet2"}, {"tenant1", "testnet1"}},
     .known_physical_databases = {"testnet1", "testnet2"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_SHOW_DATABASES);

  const auto &sql = r.sql_after_rewrite;
  // Per-tenant subquery shape: bare SELECT, no count()/startsWith/system.tables.
  EXPECT_NE(sql.find("SELECT 'tenant1' AS name"), std::string::npos);
  EXPECT_NE(sql.find("SELECT 'tenant2' AS name"), std::string::npos);
  EXPECT_EQ(sql.find("count()"),        std::string::npos);
  EXPECT_EQ(sql.find("startsWith"),     std::string::npos);
  EXPECT_EQ(sql.find("system.tables"),  std::string::npos);
  EXPECT_EQ(sql.find("system.databases"), std::string::npos);
  // No physical names leak.
  EXPECT_EQ(sql.find("testnet1"), std::string::npos);
  EXPECT_EQ(sql.find("testnet2"), std::string::npos);
  // Alphabetical: tenant1 appears before tenant2.
  EXPECT_LT(sql.find("'tenant1'"), sql.find("'tenant2'"));
  // Outer ORDER BY.
  EXPECT_NE(sql.find("ORDER BY name"), std::string::npos);
  // UNION ALL joins the two per-tenant subqueries.
  EXPECT_NE(sql.find("UNION ALL"), std::string::npos);

  // database_rewrites contains all map entries (logical != physical).
  EXPECT_EQ(r.database_rewrites["tenant1"], "testnet1");
  EXPECT_EQ(r.database_rewrites["tenant2"], "testnet2");
  EXPECT_TRUE(r.table_rewrites.empty());
  EXPECT_EQ(r.database_rewrites.size(), 2u);
}

TEST(ShowDatabases, DatabaseMap_SkipsEntryWhosePhysicalIsNotKnown) {
  // database_map entry whose physical is not in known_physical_databases
  // is dropped — no subquery, no database_rewrites entry. The other entry
  // is unaffected.
  auto r = RunDynamic(
    "SHOW DATABASES",
    {.database_map = {{"tenant_drift", "ghost_phys"},
                      {"tenant_ok",    "testnet_ok"}},
     .known_physical_databases = {"testnet_ok"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);

  const auto &sql = r.sql_after_rewrite;
  EXPECT_EQ(sql.find("'tenant_drift'"), std::string::npos);
  EXPECT_EQ(sql.find("ghost_phys"),     std::string::npos);
  EXPECT_NE(sql.find("'tenant_ok' AS name"), std::string::npos);

  EXPECT_EQ(r.database_rewrites.count("tenant_drift"), 0u);
  EXPECT_EQ(r.database_rewrites["tenant_ok"], "testnet_ok");
}

TEST(ShowDatabases, DatabaseMap_RemoteAndLocal_BothBare) {
  // Remote and local logicals are now indistinguishable in the output —
  // both emit a bare `SELECT '<logical>' AS name`. We always return a
  // declared+trusted logical DB; existence is not checked.
  auto r = RunDynamic(
    "SHOW DATABASES",
    {.database_map = {{"tenant_remote", "testnet_remote"},
                      {"tenant_local",  "testnet_local"}},
     .known_physical_databases = {"testnet_remote", "testnet_local"},
     .logical_database_to_remote_upstream_index = {{"tenant_remote", "us_east"}},
     .remote_upstreams = {{"us_east", {"ch-remote:9000", "ro", "secret"}}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);

  const auto &sql = r.sql_after_rewrite;
  EXPECT_NE(sql.find("SELECT 'tenant_remote' AS name"), std::string::npos);
  EXPECT_NE(sql.find("SELECT 'tenant_local' AS name"),  std::string::npos);
  // Neither shape carries the existence check.
  EXPECT_EQ(sql.find("count()"),         std::string::npos);
  EXPECT_EQ(sql.find("startsWith"),      std::string::npos);
  EXPECT_EQ(sql.find("system.tables"),   std::string::npos);
  // Physical names never leak.
  EXPECT_EQ(sql.find("testnet_remote"),  std::string::npos);
  EXPECT_EQ(sql.find("testnet_local"),   std::string::npos);
  // Remote upstream connection details belong to SHOW TABLES, not here.
  EXPECT_EQ(sql.find("ch-remote:9000"),  std::string::npos);

  EXPECT_EQ(r.database_rewrites["tenant_remote"], "testnet_remote");
  EXPECT_EQ(r.database_rewrites["tenant_local"],  "testnet_local");
}

TEST(ShowDatabases, DatabaseMap_ExtraArgsAndDelim_NoEffect) {
  // Prefix-related fields (`extra_arguments`, `delim`) are inputs to the
  // dynamic *table-prefix* construction used by SHOW TABLES — they have
  // nothing to do with SHOW DATABASES, since we no longer build a prefix
  // here. Verify the output is identical with and without them.
  auto plain = RunDynamic(
    "SHOW DATABASES",
    {.database_map = {{"tenant1", "testnet"}},
     .known_physical_databases = {"testnet"}});
  auto with_extras = RunDynamic(
    "SHOW DATABASES",
    {.database_map = {{"tenant1", "testnet"}},
     .known_physical_databases = {"testnet"},
     .delim = "-",
     .extra_arguments = {"v2", "shard0"}});
  EXPECT_EQ(plain.code,        rewriter::RewriteCode::Success);
  EXPECT_EQ(with_extras.code,  rewriter::RewriteCode::Success);
  EXPECT_EQ(plain.sql_after_rewrite, with_extras.sql_after_rewrite);
  EXPECT_EQ(with_extras.sql_after_rewrite.find("v2"),     std::string::npos);
  EXPECT_EQ(with_extras.sql_after_rewrite.find("shard0"), std::string::npos);
}

TEST(ShowDatabases, KnownPhysicalOnly_NeverSurfacesPhysicals) {
  // No database_map; only known_physical_databases. Physical DB names are
  // a trust anchor, not user-visible — the rewriter must NOT leak them
  // through SHOW DATABASES. Output is the empty-body sentinel.
  auto r = RunDynamic(
    "SHOW DATABASES",
    {.known_physical_databases = {"realdb2", "realdb1"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_SHOW_DATABASES);

  const auto &sql = r.sql_after_rewrite;
  // No system.databases / no IN-list; no physical names in the SQL.
  EXPECT_EQ(sql.find("system.databases"), std::string::npos);
  EXPECT_EQ(sql.find("realdb1"), std::string::npos);
  EXPECT_EQ(sql.find("realdb2"), std::string::npos);
  EXPECT_EQ(sql.find("UNION ALL"), std::string::npos);
  // Empty body sentinel + outer ORDER BY.
  EXPECT_NE(sql.find("SELECT '' AS name WHERE 0"), std::string::npos);
  EXPECT_NE(sql.find("ORDER BY name"), std::string::npos);

  EXPECT_TRUE(r.database_rewrites.empty());
  EXPECT_TRUE(r.table_rewrites.empty());
}

TEST(ShowDatabases, Mixed_DatabaseMapAndKnownPhysical) {
  // testnet1 is both the value of a database_map entry AND a member of
  // known_physical_databases (the trust anchor). realdb1 is only directly
  // exposed as a physical — and must NOT show up in the result, since
  // physical DB names are never surfaced. testnet1 also must not leak as
  // a string literal (it's only ever in `database_rewrites`).
  auto r = RunDynamic(
    "SHOW DATABASES",
    {.database_map = {{"tenant1", "testnet1"}},
     .known_physical_databases = {"testnet1", "realdb1"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  const auto &sql = r.sql_after_rewrite;
  // Only the logical name is in the SQL.
  EXPECT_NE(sql.find("SELECT 'tenant1' AS name"), std::string::npos);
  // No system.* references; no physical names leaked.
  EXPECT_EQ(sql.find("system.databases"), std::string::npos);
  EXPECT_EQ(sql.find("system.tables"),    std::string::npos);
  EXPECT_EQ(sql.find("realdb1"),  std::string::npos);
  EXPECT_EQ(sql.find("testnet1"), std::string::npos);
  // Single per-tenant subquery: no UNION ALL.
  EXPECT_EQ(sql.find("UNION ALL"), std::string::npos);
  // Only the database_map entry is recorded.
  EXPECT_EQ(r.database_rewrites["tenant1"], "testnet1");
  EXPECT_EQ(r.database_rewrites.size(), 1u);
}

TEST(ShowDatabases, Like_Pattern) {
  auto r = RunDynamic(
    "SHOW DATABASES LIKE 'tenant%'",
    {.database_map = {{"tenant1", "testnet1"}, {"sysdb", "physsys"}},
     .known_physical_databases = {"testnet1", "physsys"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("WHERE name LIKE 'tenant%'"),
            std::string::npos);
}

TEST(ShowDatabases, NotLike_Pattern) {
  auto r = RunDynamic(
    "SHOW DATABASES NOT LIKE 'sys%'",
    {.database_map = {{"tenant1", "testnet1"}},
     .known_physical_databases = {"testnet1"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("WHERE name NOT LIKE 'sys%'"),
            std::string::npos);
}

TEST(ShowDatabases, Ilike_Pattern) {
  auto r = RunDynamic(
    "SHOW DATABASES ILIKE 'TENANT%'",
    {.database_map = {{"tenant1", "testnet1"}},
     .known_physical_databases = {"testnet1"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("WHERE name ILIKE 'TENANT%'"),
            std::string::npos);
}

TEST(ShowDatabases, EmptyDynamicArgs_NoEntries) {
  // dynamic_args is set but both maps are empty -> empty result, no rewrites.
  auto r = RunDynamic("SHOW DATABASES", {});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_SHOW_DATABASES);
  // The well-typed empty body sentinel.
  EXPECT_NE(r.sql_after_rewrite.find("SELECT '' AS name WHERE 0"),
            std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("ORDER BY name"), std::string::npos);
  EXPECT_TRUE(r.database_rewrites.empty());
  EXPECT_TRUE(r.table_rewrites.empty());
}

TEST(ShowDatabases, ShowDictionaries_StillPassthroughAsShowTables) {
  // Regression: SHOW DICTIONARIES should not be claimed by the new
  // SHOW DATABASES handler. In dynamic mode it reads system.dictionaries,
  // outside the system-table allowlist (spec 2026-09-26 §5), so the SHOW
  // TABLES family handler refuses it; it is never synthesized.
  auto r = RunDynamic("SHOW DICTIONARIES",
                      {.database_map = {{"tenant1", "testnet1"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_EQ(r.message, "system table system.dictionaries is not accessible");
  EXPECT_EQ(r.sql_after_rewrite, "SHOW DICTIONARIES");
}

// Verify SELECT statements still get statement_type = "SELECT"
TEST(StatementType, SelectHasCorrectType) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql("SELECT 1");

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_SELECT);
}

// ============================================================================
// Single-table write statement tests
// ============================================================================

struct WriteOpResult {
  rewriter::RewriteCode code;
  rewriter::StatementType statement_type;
  std::string sql_after_rewrite;
  std::string message;
  int original_accessed_count;
};

// Run a write statement with NO rewrite options — verifies the classification
// / rejection path. When the caller wants to exercise the dynamic-rewrite
// path, use RunDynamic() instead.
static WriteOpResult RunWriteOp(const std::string &sql) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  EXPECT_TRUE(status.ok()) << "gRPC status not OK for: " << sql;

  return {
    resp.code(),
    resp.statement_type(),
    resp.sql_after_rewrite(),
    resp.message(),
    resp.original_accessed_tables_size(),
  };
}

// ============================================================================
// ClickHouse 26.3 dependency coverage — Decimal512 syntax.
//
// Parser-only SQL tests cannot prove that Decimal512 is implemented: ClickHouse
// accepts unknown type and function names syntactically. The first test therefore
// references the fork-only C++ type directly; compiling this test is the actual
// dependency-pin guard. The SQL cases protect the rewriter-facing syntax paths.
// ============================================================================

TEST(ClickHouseDependency, Decimal512TypeIsAvailable) {
  DB::DataTypeDecimal512 type(/*precision=*/154, /*scale=*/18);

  EXPECT_EQ(type.getTypeId(), DB::TypeIndex::Decimal512);
  EXPECT_EQ(type.getPrecision(), 154u);
  EXPECT_EQ(type.getScale(), 18u);
  EXPECT_EQ(DB::DataTypeDecimal512::maxPrecision(), 154u);
}

TEST(ClickHouseDependency, Decimal512SelectParsesSuccessfully) {
  auto resp = RunNoOptions(
    "SELECT toDecimal512('123456789012345678901234567890.123456789012345678', 18)");

  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_SELECT);
}

TEST(ClickHouseDependency, CreateTableDecimal512ParsesSuccessfully) {
  auto r = RunWriteOp(
    "CREATE TABLE chain_events (amount Decimal512(18)) ENGINE=Memory");

  EXPECT_EQ(r.code, rewriter::RewriteCode::Success) << r.message;
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_TABLE);
  EXPECT_EQ(r.original_accessed_count, 1);
}

TEST(ClickHouseDependency, InsertSelectDecimal512ParsesSuccessfully) {
  auto r = RunWriteOp(
    "INSERT INTO chain_events "
    "SELECT toDecimal512('1000000.000000000000000001', 18)");

  EXPECT_EQ(r.code, rewriter::RewriteCode::Success) << r.message;
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_INSERT);
  EXPECT_EQ(r.original_accessed_count, 1);
}

// --- CREATE TABLE ---

TEST(WriteOp, CreateTable_Simple) {
  auto r = RunWriteOp("CREATE TABLE t (a Int32) ENGINE=Memory");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_TABLE);
  // Writes record their single rewriteOneTarget target as one access.
  EXPECT_EQ(r.original_accessed_count, 1);
}

TEST(WriteOp, CreateTable_WithDatabase) {
  auto r = RunWriteOp("CREATE TABLE db.t (a Int32) ENGINE=Memory");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_TABLE);
}

TEST(WriteOp, CreateTable_AsSelect_ReturnsTargetAndSource) {
  auto r = RunWriteOp("CREATE TABLE t ENGINE=Memory AS SELECT * FROM src");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_TABLE);
  // Spec 2026-09-26 T4: the AS SELECT body is a read source, reported after
  // the target in every mode.
  EXPECT_EQ(r.original_accessed_count, 2);
}

TEST(WriteOp, CreateView_NoOptions_Passthrough) {
  // CREATE VIEW is now supported: with no rewrite options it passes through.
  auto r = RunWriteOp("CREATE VIEW v AS SELECT 1");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_VIEW);
}

TEST(WriteOp, CreateMaterializedView_NoOptions_Passthrough) {
  // CREATE MATERIALIZED VIEW is now supported: with no rewrite options it passes through.
  auto r = RunWriteOp("CREATE MATERIALIZED VIEW mv ENGINE=Memory AS SELECT 1 AS a");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_MATERIALIZED_VIEW);
}

TEST(WriteOp, CreateDatabase_NoOptions_Rejected) {
  // Without a TableNameRewrite/dynamic_args option we can't validate the
  // request, so we fail closed rather than ship the DDL through.
  auto r = RunWriteOp("CREATE DATABASE d");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

// --- DROP TABLE ---

TEST(WriteOp, DropTable_Simple) {
  auto r = RunWriteOp("DROP TABLE t");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DROP_TABLE);
  EXPECT_EQ(r.original_accessed_count, 1);
}

TEST(WriteOp, DropTable_WithDatabase) {
  auto r = RunWriteOp("DROP TABLE db.t");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DROP_TABLE);
}

TEST(WriteOp, DropTable_IfExists) {
  auto r = RunWriteOp("DROP TABLE IF EXISTS t");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DROP_TABLE);
}

TEST(WriteOp, DropTable_MultiTable_Rejected) {
  auto r = RunWriteOp("DROP TABLE t1, t2");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

TEST(WriteOp, DropDatabase_NoOptions_Rejected) {
  // Without a TableNameRewrite/dynamic_args option we can't validate the
  // request (and have no logical -> physical mapping to record for the
  // GC binary), so we fail closed rather than ship the DDL through. The
  // dynamic-args-supplied path is exercised by TableRewrite tests below.
  auto r = RunWriteOp("DROP DATABASE d");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

TEST(WriteOp, DropView_NoOptions_Passthrough) {
  // DROP VIEW is now supported; without options it passes through unchanged.
  auto r = RunWriteOp("DROP VIEW v");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DROP_VIEW);
}

TEST(WriteOp, Detach_Rejected) {
  auto r = RunWriteOp("DETACH TABLE t");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

// --- TRUNCATE TABLE ---
// TRUNCATE TABLE flows through the same single-target path as DROP TABLE;
// only TRUNCATE DATABASE / TRUNCATE ALL TABLES FROM remain unsupported
// (would need to enumerate prefix-shared tables; same problem as DROP
// DATABASE pre-soft-delete, but TRUNCATE doesn't fit "deferred GC").

TEST(WriteOp, TruncateTable_Simple) {
  auto r = RunWriteOp("TRUNCATE TABLE t");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_TRUNCATE_TABLE);
}

TEST(WriteOp, TruncateTable_IfExists) {
  auto r = RunWriteOp("TRUNCATE TABLE IF EXISTS t");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_TRUNCATE_TABLE);
}

TEST(WriteOp, TruncateDatabase_Rejected) {
  auto r = RunWriteOp("TRUNCATE DATABASE d");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

// --- ALTER TABLE ---

TEST(WriteOp, AlterTable_AddColumn) {
  auto r = RunWriteOp("ALTER TABLE t ADD COLUMN c Int32");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_ALTER_TABLE);
}

TEST(WriteOp, AlterTable_Update) {
  // ALTER TABLE ... UPDATE is a mutation, not a SQL-standard UPDATE.
  auto r = RunWriteOp("ALTER TABLE t UPDATE a = 1 WHERE id = 0");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_ALTER_TABLE);
}

TEST(WriteOp, AlterTable_ReplacePartitionFromOtherTable_Rejected) {
  // V1 rejects ALTER TABLE commands that reference a second table
  // (ATTACH/REPLACE/FETCH PARTITION FROM, MOVE PARTITION TO TABLE): the
  // rewriter has no clear policy yet for whether the secondary table should
  // be rewritten with the same map, a different map, or passed through, so
  // we bail rather than ship a half-rewritten statement.
  auto r = RunWriteOp("ALTER TABLE t REPLACE PARTITION 'p' FROM src.other");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

TEST(WriteOp, AlterDatabase_Rejected) {
  auto r = RunWriteOp("ALTER DATABASE d MODIFY SETTING x = 1");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

// --- INSERT ---

TEST(WriteOp, Insert_Values) {
  auto r = RunWriteOp("INSERT INTO t VALUES (1)");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_INSERT);
  EXPECT_EQ(r.original_accessed_count, 1);
}

// ASTInsertQuery::formatImpl drops the inline `VALUES (...)` payload (it
// lives off-AST as raw pointers into the request buffer); the writes
// handler appends those bytes back so downstream sees the data inline
// instead of a bare `... FORMAT Values` that would put the upstream into
// "wait for wire-format data" mode.
TEST(WriteOp, Insert_Values_PayloadRoundTrips) {
  auto r = RunWriteOp("INSERT INTO t VALUES (1)");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("(1)"), std::string::npos)
    << r.sql_after_rewrite;
}

TEST(WriteOp, Insert_Values_MultiRow_PayloadRoundTrips) {
  auto r = RunWriteOp("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("(1, 'a')"), std::string::npos)
    << r.sql_after_rewrite;
  EXPECT_NE(r.sql_after_rewrite.find("(3, 'c')"), std::string::npos)
    << r.sql_after_rewrite;
}

TEST(TableRewrite, Insert_Values_PayloadRoundTrips_AfterRewrite) {
  // The original bug: with a TableNameRewrite option in play, the AST
  // round-trip was producing `INSERT INTO <new>.<new> FORMAT Values` —
  // the `(1)` payload got dropped and the upstream hung waiting for a
  // wire-format data block. Pin the payload back into the rewritten SQL.
  auto r = RunDynamic("INSERT INTO tenant1.t VALUES (1)",
                      {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  // New construction puts a literal "." before the original table, so the
  // physical table name is `tenant1.t` (a CH identifier containing a dot,
  // which the formatter backtick-quotes via WhenNecessary).
  EXPECT_NE(r.sql_after_rewrite.find("`tenant1.t`"), std::string::npos)
    << r.sql_after_rewrite;
  EXPECT_NE(r.sql_after_rewrite.find("(1)"), std::string::npos)
    << r.sql_after_rewrite;
}

TEST(WriteOp, Insert_Select_ReturnsTargetAndSource) {
  auto r = RunWriteOp("INSERT INTO db.t SELECT * FROM src");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_INSERT);
  // Spec 2026-09-26 T4: the SELECT body is a read source, reported after the
  // target (db.t) in every mode.
  EXPECT_EQ(r.original_accessed_count, 2);
}

TEST(WriteOp, InsertFunction_Rejected) {
  auto r = RunWriteOp("INSERT INTO FUNCTION remote('host:9000', 'db', 't') VALUES (1)");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

// --- UPDATE / DELETE ---

TEST(WriteOp, Update_Simple) {
  auto r = RunWriteOp("UPDATE t SET a = 1 WHERE id = 0");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_UPDATE);
}

TEST(WriteOp, Update_WithDatabase) {
  auto r = RunWriteOp("UPDATE db.t SET a = 1 WHERE id = 0");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_UPDATE);
}

TEST(WriteOp, Delete_Simple) {
  auto r = RunWriteOp("DELETE FROM t WHERE id = 0");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DELETE);
}

TEST(WriteOp, Delete_WithDatabase) {
  auto r = RunWriteOp("DELETE FROM db.t WHERE id = 0");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DELETE);
}

// --- Other write-ish statements rejected by getQueryKind() catch-all ---

TEST(WriteOp, Rename_Accepted) {
  auto r = RunWriteOp("RENAME TABLE a TO b");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_RENAME_TABLE);
  // No single target_table for RENAME — callers consult table_rewrites.
}

TEST(WriteOp, RenameDatabase_Rejected) {
  auto r = RunWriteOp("RENAME DATABASE a TO b");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

TEST(WriteOp, Optimize_Rejected) {
  auto r = RunWriteOp("OPTIMIZE TABLE t FINAL");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

// ============================================================================
// TableNameRewrite — dynamic construction (non-SELECT) coverage
// ============================================================================
//
// The DynamicArgs / DynamicProbe / RunDynamic helpers live above with the
// USE / SHOW TABLES tests.

// --- INSERT ---

TEST(TableRewrite, Insert_QualifiedDB_DynamicConstruction) {
  // `INSERT INTO tenant1.t` — SQL qualifier gives logical, database_map
  // provides physical. No extra_args -> final name = `tenant1.t`.
  auto r = RunDynamic("INSERT INTO tenant1.t VALUES (1)",
                      {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_INSERT);
  EXPECT_NE(r.sql_after_rewrite.find("testnet"), std::string::npos);
  // Backticks because `tenant1.t` contains a dot.
  EXPECT_NE(r.sql_after_rewrite.find("`tenant1.t`"), std::string::npos);
  EXPECT_EQ(r.table_rewrites["tenant1.t"], "testnet.tenant1.t");
}

TEST(TableRewrite, Insert_UnqualifiedDB_FromUpstreamContext) {
  // `INSERT INTO t` — no SQL qualifier, upstream context fills in.
  auto r = RunDynamic("INSERT INTO t VALUES (1)",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("testnet"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("`tenant1.t`"), std::string::npos);
  EXPECT_EQ(r.table_rewrites["t"], "testnet.tenant1.t");
}

TEST(TableRewrite, Insert_WithExtraArgs) {
  // extra_args = ["v2", "shard1"] -> name = `tenant1_v2_shard1.t`.
  // Extras are joined with the configurable delim ("_" by default); the
  // final separator before `t` is always a literal ".".
  auto r = RunDynamic("INSERT INTO t VALUES (1)",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1",
                       .extra_arguments = {"v2", "shard1"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("`tenant1_v2_shard1.t`"), std::string::npos);
}

TEST(TableRewrite, Insert_CustomDelim) {
  // Custom delim joins logical+extras only; the final separator is still ".".
  auto r = RunDynamic("INSERT INTO t VALUES (1)",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1",
                       .delim = "-",
                       .extra_arguments = {"v2"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("`tenant1-v2.t`"), std::string::npos);
}

TEST(TableRewrite, Insert_EmptyExtraArgs_NoStrayDelim) {
  // With extras=[], the prefix must NOT include a leading delim before the
  // dot — i.e. the name is `tenant1.t`, NOT `tenant1_.t`. This pins the
  // "extras block is optional" rule in buildDynamicTableName.
  auto r = RunDynamic("INSERT INTO t VALUES (1)",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.sql_after_rewrite.find("tenant1_."), std::string::npos)
    << r.sql_after_rewrite;
  EXPECT_NE(r.sql_after_rewrite.find("`tenant1.t`"), std::string::npos);
}

TEST(TableRewrite, Insert_KnownPhysical_Passthrough) {
  // Logical db is already a known physical -> logical == physical; table
  // name is still prefixed.
  auto r = RunDynamic("INSERT INTO testnet.t VALUES (1)",
                      {.known_physical_databases = {"testnet"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.`testnet.t`"), std::string::npos)
    << r.sql_after_rewrite;
  EXPECT_EQ(r.table_rewrites["testnet.t"], "testnet.testnet.t");
}

TEST(TableRewrite, Insert_UnknownLogical_Rejected) {
  auto r = RunDynamic("INSERT INTO mystery.t VALUES (1)",
                      {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
  EXPECT_NE(r.message.find("mystery"), std::string::npos);
}

TEST(TableRewrite, Insert_NoContext_Rejected) {
  // Unqualified target and no upstream context -> rejected.
  auto r = RunDynamic("INSERT INTO t VALUES (1)",
                      {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
}

TEST(TableRewrite, Insert_Passthrough_WhenNoOption) {
  // No TableNameRewrite option at all -> writes echo back unchanged.
  auto r = RunWriteOp("INSERT INTO t VALUES (1)");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_INSERT);
  EXPECT_NE(r.sql_after_rewrite.find("INSERT INTO t"), std::string::npos);
}

// --- INSERT shapes used by clickhouse-go's PrepareBatch (native protocol) ---
//
// `clickhouse-go`'s batch insert sends just the SQL header in the native
// protocol's Query packet, then streams data blocks separately. The rewriter
// only sees the SQL header — these tests pin the two header forms the
// native driver actually emits.
//
// Note: a fully bare `INSERT INTO t` (no FORMAT, no VALUES, no SELECT)
// FAILS ClickHouse's `ParserQuery` with SyntaxError. That's not a regression
// — clickhouse-go always tacks on `FORMAT Native` (or `FORMAT RowBinary` /
// similar) before the data stream, so the truncated form never reaches us
// from a real driver. If a caller does send bare `INSERT INTO t`, they get
// SyntaxError back and can decide what to do.

TEST(TableRewrite, Insert_FormatNative) {
  // `INSERT INTO t FORMAT Native` — what clickhouse-go's batch typically
  // sends when columns aren't pre-declared.
  auto r = RunDynamic("INSERT INTO t FORMAT Native",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_INSERT);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.`tenant1.t`"), std::string::npos);
  // FORMAT clause must round-trip — the server uses it to pick the decoder.
  EXPECT_NE(r.sql_after_rewrite.find("FORMAT Native"), std::string::npos);
  EXPECT_EQ(r.table_rewrites["t"], "testnet.tenant1.t");
}

TEST(TableRewrite, Insert_WithColumnList_FormatNative) {
  // `INSERT INTO t (a, b, c) FORMAT Native` — clickhouse-go uses this when
  // the caller pre-declares columns. The column list must round-trip
  // unchanged: the data blocks that follow in the protocol stream are
  // ordered by this list, so reordering or dropping columns would break
  // the binary decode.
  auto r = RunDynamic("INSERT INTO t (a, b, c) FORMAT Native",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_INSERT);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.`tenant1.t`"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("FORMAT Native"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("a"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("b"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("c"), std::string::npos);
  EXPECT_EQ(r.table_rewrites["t"], "testnet.tenant1.t");
}

// --- CREATE TABLE ---

TEST(TableRewrite, CreateTable_Basic) {
  auto r = RunDynamic("CREATE TABLE t (a Int32) ENGINE=Memory",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_TABLE);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.`tenant1.t`"), std::string::npos);
  EXPECT_EQ(r.table_rewrites["t"], "testnet.tenant1.t");
}

TEST(TableRewrite, CreateTable_AsSource_AlsoRewritten) {
  // CREATE TABLE dst AS src — both sides rewrite using the same context.
  auto r = RunDynamic("CREATE TABLE dst ENGINE=Memory AS src",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.table_rewrites["dst"], "testnet.tenant1.dst");
  EXPECT_EQ(r.table_rewrites["src"], "testnet.tenant1.src");
}

// --- CREATE DATABASE ---
//
// CREATE DATABASE never actually runs the DDL on the physical CH. The
// rewriter validates the request and rewrites to a debug SELECT
// (`SELECT '<original SQL>' AS cdstmt`) so the user's intent lands in
// query_log. The proxy is responsible for the actual database creation.

TEST(TableRewrite, CreateDatabase_NewLogical_Rewritten) {
  // Logical DB not in database_map and no upstream_physical conflict ->
  // accept and emit the debug SELECT.
  auto r = RunDynamic("CREATE DATABASE my_new_db",
                      {.known_physical_databases = {"testnet1"},
                       .upstream_physical_database_in_context = "testnet1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_DATABASE);
  // SQL is wrapped in SELECT '...' AS cdstmt.
  EXPECT_NE(r.sql_after_rewrite.find("SELECT '"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("AS cdstmt"), std::string::npos);
  // Original CREATE DATABASE text is embedded inside the literal.
  EXPECT_NE(r.sql_after_rewrite.find("CREATE DATABASE"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("my_new_db"), std::string::npos);
}

TEST(TableRewrite, CreateDatabase_AlreadyMapped_Rejected) {
  // Target logical DB is already in database_map -> "already exists".
  auto r = RunDynamic("CREATE DATABASE tenant1",
                      {.database_map = {{"tenant1", "testnet1"}},
                       .known_physical_databases = {"testnet1"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
  EXPECT_NE(r.message.find("already exists"), std::string::npos);
}

TEST(TableRewrite, CreateDatabase_AlreadyMapped_IfNotExists_Accepted) {
  // IF NOT EXISTS suppresses the "already exists" error and lets us emit
  // the debug SELECT — matches CH semantics where the IF NOT EXISTS form
  // silently no-ops when the DB exists.
  auto r = RunDynamic("CREATE DATABASE IF NOT EXISTS tenant1",
                      {.database_map = {{"tenant1", "testnet1"}},
                       .known_physical_databases = {"testnet1"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_DATABASE);
  EXPECT_NE(r.sql_after_rewrite.find("AS cdstmt"), std::string::npos);
}

TEST(TableRewrite, CreateDatabase_UnknownPhysicalContext_Rejected) {
  // upstream_physical_database_in_context set to a value that's NOT in
  // known_physical_databases -> the proxy is sending garbage; reject.
  auto r = RunDynamic("CREATE DATABASE my_new_db",
                      {.known_physical_databases = {"testnet1"},
                       .upstream_physical_database_in_context = "ghost_physical"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
  EXPECT_NE(r.message.find("ghost_physical"), std::string::npos);
}

TEST(TableRewrite, CreateDatabase_NoUpstreamPhysical_Accepted) {
  // upstream_physical_database_in_context unset is fine — we don't run
  // any DDL, the proxy just hasn't routed yet.
  auto r = RunDynamic("CREATE DATABASE my_new_db", {});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_DATABASE);
}

TEST(TableRewrite, CreateDatabase_LiteralEscaping) {
  // A SQL with single quotes round-trips into the debug SELECT with the
  // quotes doubled (CH single-quoted-literal escape).
  auto r = RunDynamic("CREATE DATABASE `weird'name`", {});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  // The single quote inside the original identifier should appear as ''
  // inside the SELECT literal; bare ' would mean we forgot to escape.
  EXPECT_NE(r.sql_after_rewrite.find("''"), std::string::npos)
    << "Expected doubled-quote escape; got: " << r.sql_after_rewrite;
}

// --- DROP DATABASE ---
//
// DROP DATABASE is symmetric to CREATE DATABASE: never emits the DDL
// against the physical CH; instead validates and rewrites to
// `SELECT '<original SQL>' AS ddstmt` so an external GC binary can
// reclaim the prefixed tables async.

TEST(TableRewrite, DropDatabase_Basic) {
  // Logical DB present in database_map -> accept and emit the debug
  // SELECT, recording the logical -> physical mapping in
  // database_rewrites for the GC binary to pick up.
  auto r = RunDynamic("DROP DATABASE tenant1",
                      {.database_map = {{"tenant1", "testnet1"}},
                       .known_physical_databases = {"testnet1"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DROP_DATABASE);
  EXPECT_NE(r.sql_after_rewrite.find("SELECT '"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("AS ddstmt"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("DROP DATABASE"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("tenant1"), std::string::npos);
  EXPECT_EQ(r.database_rewrites["tenant1"], "testnet1");
}

TEST(TableRewrite, DropDatabase_NotMapped_Rejected) {
  // Target logical DB is NOT in database_map and the SQL doesn't say
  // IF EXISTS -> reject (the proxy is asking us to drop something we
  // don't manage).
  auto r = RunDynamic("DROP DATABASE ghost",
                      {.database_map = {{"tenant1", "testnet1"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
  EXPECT_NE(r.message.find("ghost"), std::string::npos);
  EXPECT_NE(r.message.find("not in database_map"), std::string::npos);
}

TEST(TableRewrite, DropDatabase_NotMapped_IfExists_Accepted) {
  // IF EXISTS suppresses the "not mapped" error and lets us emit the
  // debug SELECT — matches CH semantics where the IF EXISTS form
  // silently no-ops when the DB doesn't exist. No database_rewrites
  // entry since there's no logical -> physical to record.
  auto r = RunDynamic("DROP DATABASE IF EXISTS ghost",
                      {.database_map = {{"tenant1", "testnet1"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DROP_DATABASE);
  EXPECT_NE(r.sql_after_rewrite.find("AS ddstmt"), std::string::npos);
  EXPECT_TRUE(r.database_rewrites.empty());
}

TEST(TableRewrite, DropDatabase_KnownPhysicalOnly_Rejected) {
  // Target appears in known_physical_databases but NOT in database_map.
  // rewriter-managed DROP is a logical-level concept; bare physical-DB
  // drops are out of scope. Reject (without IF EXISTS).
  auto r = RunDynamic("DROP DATABASE testnet1",
                      {.database_map = {{"tenant1", "testnet1"}},
                       .known_physical_databases = {"testnet1"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
  EXPECT_NE(r.message.find("testnet1"), std::string::npos);
}

TEST(TableRewrite, DropDatabase_LiteralEscaping) {
  // Single quote in the target identifier round-trips into the debug
  // SELECT with the quote doubled (CH single-quoted-literal escape).
  auto r = RunDynamic("DROP DATABASE IF EXISTS `weird'name`", {});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("''"), std::string::npos)
    << "Expected doubled-quote escape; got: " << r.sql_after_rewrite;
}

// --- DROP / ALTER / UPDATE / DELETE ---

TEST(TableRewrite, DropTable_Basic) {
  auto r = RunDynamic("DROP TABLE t",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DROP_TABLE);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.`tenant1.t`"), std::string::npos);
  EXPECT_EQ(r.table_rewrites["t"], "testnet.tenant1.t");
}

TEST(TableRewrite, TruncateTable_Basic) {
  // TRUNCATE TABLE shares the unified single-target path with DROP TABLE;
  // statement_type is the only thing that distinguishes them on the wire.
  auto r = RunDynamic("TRUNCATE TABLE t",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_TRUNCATE_TABLE);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.`tenant1.t`"), std::string::npos);
  EXPECT_EQ(r.table_rewrites["t"], "testnet.tenant1.t");
}

TEST(TableRewrite, AlterTable_Basic) {
  auto r = RunDynamic("ALTER TABLE t ADD COLUMN c Int32",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_ALTER_TABLE);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.`tenant1.t`"), std::string::npos);
}

TEST(TableRewrite, Update_Basic) {
  auto r = RunDynamic("UPDATE t SET a = 1 WHERE id = 0",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_UPDATE);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.`tenant1.t`"), std::string::npos);
}

TEST(TableRewrite, Delete_Basic) {
  auto r = RunDynamic("DELETE FROM t WHERE id = 0",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DELETE);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.`tenant1.t`"), std::string::npos);
}

// --- RENAME ---

TEST(TableRewrite, Rename_BothSidesRewritten) {
  auto r = RunDynamic("RENAME TABLE a TO b",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_RENAME_TABLE);
  EXPECT_EQ(r.table_rewrites["a"], "testnet.tenant1.a");
  EXPECT_EQ(r.table_rewrites["b"], "testnet.tenant1.b");
}

TEST(TableRewrite, Rename_MultiplePairs) {
  auto r = RunDynamic("RENAME TABLE a TO b, c TO d",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  ASSERT_EQ(r.table_rewrites.size(), 4u);
  EXPECT_EQ(r.table_rewrites["a"], "testnet.tenant1.a");
  EXPECT_EQ(r.table_rewrites["b"], "testnet.tenant1.b");
  EXPECT_EQ(r.table_rewrites["c"], "testnet.tenant1.c");
  EXPECT_EQ(r.table_rewrites["d"], "testnet.tenant1.d");
}

TEST(TableRewrite, Rename_QualifiedSidesUseTheirOwnLogical) {
  // Each side can carry its own qualified logical DB.
  auto r = RunDynamic("RENAME TABLE t1.a TO t2.b",
                      {.database_map = {{"t1", "phys1"}, {"t2", "phys2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.table_rewrites["t1.a"], "phys1.t1.a");
  EXPECT_EQ(r.table_rewrites["t2.b"], "phys2.t2.b");
}

// --- EXISTS TABLE ---

TEST(TableRewrite, ExistsTable_Basic) {
  auto r = RunDynamic("EXISTS TABLE t",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_EXISTS_TABLE);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.`tenant1.t`"), std::string::npos);
  EXPECT_EQ(r.table_rewrites["t"], "testnet.tenant1.t");
}

TEST(TableRewrite, ExistsTable_UnknownLogical_Rejected) {
  auto r = RunDynamic("EXISTS TABLE mystery.t",
                      {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
}

TEST(TableRewrite, ExistsDatabase_Unsupported) {
  auto r = RunDynamic("EXISTS DATABASE d", {});
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

// --- SHOW CREATE TABLE ---

TEST(TableRewrite, ShowCreateTable_Basic) {
  auto r = RunDynamic("SHOW CREATE TABLE t",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_SHOW_CREATE_TABLE);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.`tenant1.t`"), std::string::npos);
}

TEST(TableRewrite, ShowCreateDatabase_Unsupported) {
  auto r = RunDynamic("SHOW CREATE DATABASE d", {});
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

// ============================================================================
// SELECT path still uses static_args maps (table_map / remote_table_map
// / table_with_database_map). Cover the basics to prove the split works.
// ============================================================================

namespace {

struct SelectMaps {
  std::map<std::string, std::string> table_map;
  std::map<std::string, std::pair<std::string, std::string>> remote_map;   // key -> {addr, db.table}
  std::map<std::string, std::pair<std::string, std::string>> with_db_map;  // key -> {db, table}
};

DynamicProbe RunSelect(const std::string &sql, const SelectMaps &maps) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);

  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *sel = opt->mutable_table_name_args()->mutable_static_args();
  for (const auto &kv : maps.table_map) {
    (*sel->mutable_table_map())[kv.first] = kv.second;
  }
  for (const auto &kv : maps.remote_map) {
    auto &r = (*sel->mutable_remote_table_map())[kv.first];
    r.set_addr(kv.second.first);
    const auto dot = kv.second.second.find('.');
    if (dot == std::string::npos) {
      r.set_table(kv.second.second);
    } else {
      r.set_database(kv.second.second.substr(0, dot));
      r.set_table(kv.second.second.substr(dot + 1));
    }
  }
  for (const auto &kv : maps.with_db_map) {
    auto &t = (*sel->mutable_table_with_database_map())[kv.first];
    t.set_database(kv.second.first);
    t.set_table(kv.second.second);
  }

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  EXPECT_TRUE(status.ok()) << "gRPC status not OK for: " << sql;

  DynamicProbe p;
  p.code = resp.code();
  p.statement_type = resp.statement_type();
  p.sql_after_rewrite = resp.sql_after_rewrite();
  p.message = resp.message();
  for (const auto &kv : resp.table_rewrites()) p.table_rewrites[kv.first] = kv.second;
  return p;
}

} // namespace

TEST(TableRewrite, Select_TableMap_Hit) {
  auto r = RunSelect("SELECT * FROM t", {.table_map = {{"t", "t_v2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_SELECT);
  EXPECT_EQ(r.table_rewrites["t"], "t_v2");
}

TEST(TableRewrite, Select_WithDatabaseMap_Hit) {
  auto r = RunSelect("SELECT * FROM t",
                     {.with_db_map = {{"t", {"physdb", "t_v2"}}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.table_rewrites["t"], "physdb.t_v2");
}

TEST(TableRewrite, Select_RemoteMap_Hit) {
  auto r = RunSelect("SELECT * FROM t",
                     {.remote_map = {{"t", {"host:9000", "db.t_v2"}}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  // Output SQL uses remote(...) function.
  EXPECT_NE(r.sql_after_rewrite.find("remote"), std::string::npos);
  // table_rewrites records the db.table form.
  EXPECT_EQ(r.table_rewrites["t"], "db.t_v2");
}

TEST(TableRewrite, Select_Passthrough_WhenNoOption) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql("SELECT * FROM t");
  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  service.Rewrite(&ctx, &req, &resp);
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_SELECT);
  EXPECT_EQ(resp.table_rewrites_size(), 0);
}

// ============================================================================
// Cross-shard correctness: forceGlobalForRemoteAsymmetry
//
// When a SELECT mixes a remote() / cluster() table function with a plain
// local table identifier inside a JOIN or an IN subquery, ClickHouse will
// ship the outer plan to the remote shard and fail to resolve the local
// reference there (UNKNOWN_TABLE). The rewriter's post-pass detects this
// asymmetry and bumps the JOIN to GLOBAL JOIN / IN to globalIn so the
// initiator broadcasts the local side as a temp table.
//
// All-remote and all-local stay untouched by design — GLOBAL costs an extra
// broadcast and the symmetric cases work without it.
// ============================================================================

namespace {

rewriter::RewriteSQLResponse RunSelectNoOpts(const std::string &sql) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);
  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  service.Rewrite(&ctx, &req, &resp);
  return resp;
}

} // namespace

TEST(GlobalForRemote, Join_RemoteLeftLocalRight_BecomesGlobal) {
  // The user's repro: outer FROM is remote(), JOIN with a plain local table.
  // Plain LEFT JOIN -> LEFT GLOBAL JOIN.
  const auto resp = RunSelectNoOpts(
    "SELECT * FROM remote('h:9000','db','t','u','p') AS r "
    "LEFT JOIN swap AS l ON r.value = l.value");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto sql = NormalizeWS(resp.sql_after_rewrite());
  EXPECT_NE(sql.find("GLOBAL"), std::string::npos) << sql;
  EXPECT_EQ(sql.find(" GLOBAL "), sql.rfind(" GLOBAL ")) << "exactly one GLOBAL: " << sql;
}

TEST(GlobalForRemote, Join_LocalLeftRemoteRight_BecomesGlobal) {
  const auto resp = RunSelectNoOpts(
    "SELECT * FROM swap AS l "
    "LEFT JOIN remote('h:9000','db','t','u','p') AS r ON r.value = l.value");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_NE(NormalizeWS(resp.sql_after_rewrite()).find("GLOBAL"), std::string::npos);
}

TEST(GlobalForRemote, Join_BothRemote_StaysPlain) {
  // Per design — symmetric remote-vs-remote works without GLOBAL.
  const auto resp = RunSelectNoOpts(
    "SELECT * FROM remote('h1:9000','d','t1','u','p') AS a "
    "JOIN remote('h2:9000','d','t2','u','p') AS b ON a.x = b.x");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto sql = NormalizeWS(resp.sql_after_rewrite());
  EXPECT_EQ(sql.find(" GLOBAL "), std::string::npos) << sql;
}

TEST(GlobalForRemote, Join_BothLocal_StaysPlain) {
  const auto resp = RunSelectNoOpts(
    "SELECT * FROM a JOIN b ON a.x = b.x");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto sql = NormalizeWS(resp.sql_after_rewrite());
  EXPECT_EQ(sql.find(" GLOBAL "), std::string::npos) << sql;
}

TEST(GlobalForRemote, Join_ExplicitGlobal_Preserved) {
  const auto resp = RunSelectNoOpts(
    "SELECT * FROM remote('h:9000','db','t','u','p') AS r "
    "GLOBAL LEFT JOIN swap AS l ON r.value = l.value");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto sql = NormalizeWS(resp.sql_after_rewrite());
  // Still has exactly one GLOBAL — no double-promotion.
  EXPECT_NE(sql.find("GLOBAL"), std::string::npos);
  EXPECT_EQ(sql.find(" GLOBAL "), sql.rfind(" GLOBAL ")) << sql;
}

TEST(GlobalForRemote, Join_ExplicitLocal_Preserved) {
  // User explicitly pinned LOCAL — we don't override their intent.
  const auto resp = RunSelectNoOpts(
    "SELECT * FROM remote('h:9000','db','t','u','p') AS r "
    "LOCAL LEFT JOIN swap AS l ON r.value = l.value");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  const auto sql = NormalizeWS(resp.sql_after_rewrite());
  EXPECT_EQ(sql.find(" GLOBAL "), std::string::npos) << sql;
}

TEST(GlobalForRemote, Join_RemoteMapMakesAsymmetric_BecomesGlobal) {
  // Static-mode rewrite turns `swap` into a remote() call AFTER which the
  // JOIN becomes remote-vs-local relative to `local_t`.
  auto r = RunSelect(
    "SELECT * FROM swap JOIN local_t ON swap.x = local_t.x",
    {.remote_map = {{"swap", {"h:9000", "db.swap_v2"}}}});
  ASSERT_EQ(r.code, rewriter::RewriteCode::Success);
  const auto sql = NormalizeWS(r.sql_after_rewrite);
  EXPECT_NE(sql.find("remote("), std::string::npos);
  EXPECT_NE(sql.find("GLOBAL"), std::string::npos) << sql;
}

TEST(GlobalForRemote, Join_RemoteMapBothSides_StaysPlain) {
  // After rewrite both sides are remote() — symmetric, no GLOBAL.
  auto r = RunSelect(
    "SELECT * FROM t1 JOIN t2 ON t1.x = t2.x",
    {.remote_map = {{"t1", {"h:9000", "db.t1_v2"}},
                    {"t2", {"h:9000", "db.t2_v2"}}}});
  ASSERT_EQ(r.code, rewriter::RewriteCode::Success);
  const auto sql = NormalizeWS(r.sql_after_rewrite);
  EXPECT_EQ(sql.find(" GLOBAL "), std::string::npos) << sql;
}

TEST(GlobalForRemote, Join_SubqueryRightWithLocalInside_BecomesGlobal) {
  const auto resp = RunSelectNoOpts(
    "SELECT * FROM remote('h:9000','db','t','u','p') AS r "
    "JOIN (SELECT * FROM swap WHERE x > 0) AS s ON r.x = s.x");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_NE(NormalizeWS(resp.sql_after_rewrite()).find("GLOBAL"), std::string::npos);
}

TEST(GlobalForRemote, In_RemoteOuter_LocalSubquery_BecomesGlobalIn) {
  const auto resp = RunSelectNoOpts(
    "SELECT * FROM remote('h:9000','db','t','u','p') AS r "
    "WHERE r.value IN (SELECT y FROM swap)");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_NE(resp.sql_after_rewrite().find("GLOBAL IN"), std::string::npos)
    << resp.sql_after_rewrite();
}

TEST(GlobalForRemote, NotIn_RemoteOuter_LocalSubquery_BecomesGlobalNotIn) {
  const auto resp = RunSelectNoOpts(
    "SELECT * FROM remote('h:9000','db','t','u','p') AS r "
    "WHERE r.value NOT IN (SELECT y FROM swap)");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_NE(resp.sql_after_rewrite().find("GLOBAL NOT IN"), std::string::npos)
    << resp.sql_after_rewrite();
}

TEST(GlobalForRemote, In_LiteralTuple_NoRewrite) {
  const auto resp = RunSelectNoOpts(
    "SELECT * FROM remote('h:9000','db','t','u','p') AS r "
    "WHERE r.value IN (1, 2, 3)");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.sql_after_rewrite().find("GLOBAL IN"), std::string::npos);
}

TEST(GlobalForRemote, In_LocalOuter_NoRewrite) {
  // Outer is local — IN's right side referencing remote works without GLOBAL,
  // and IN with a local subquery is fine too. Just verify no spurious rewrite.
  const auto resp = RunSelectNoOpts(
    "SELECT * FROM swap WHERE value IN (SELECT y FROM other_local)");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.sql_after_rewrite().find("globalIn"), std::string::npos);
  EXPECT_EQ(resp.sql_after_rewrite().find("GLOBAL IN"), std::string::npos);
}

TEST(GlobalForRemote, In_RemoteOuter_RemoteSubquery_NoRewrite) {
  // Symmetric remote — subquery dispatches to its own remote, no broadcast needed.
  const auto resp = RunSelectNoOpts(
    "SELECT * FROM remote('h1:9000','db','t','u','p') AS r "
    "WHERE r.value IN (SELECT y FROM remote('h2:9000','db','t2','u','p'))");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.sql_after_rewrite().find("GLOBAL IN"), std::string::npos)
    << resp.sql_after_rewrite();
}

TEST(GlobalForRemote, NestedSubquery_InnerHandledIndependently) {
  // Outer is local, but the FROM-subquery is itself a remote-shipped
  // SELECT joining a local table. Inner JOIN must become GLOBAL; outer is
  // unaffected.
  const auto resp = RunSelectNoOpts(
    "SELECT * FROM (SELECT * FROM remote('h:9000','db','t','u','p') AS r "
    "JOIN swap AS l ON r.x = l.x) AS sub");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_NE(NormalizeWS(resp.sql_after_rewrite()).find("GLOBAL"), std::string::npos);
}

// ============================================================================
// GRANT / REVOKE tests
// ============================================================================
//
// GRANT/REVOKE is fire-and-forget: the rewriter emits structured PrivilegeDelta
// entries into `response.privileges_deltas`, drops any ON CLUSTER, and rewrites
// the SQL to `SELECT '<original SQL minus ON CLUSTER>' AS gstmt` (GRANT) or
// `... AS rstmt` (REVOKE) so the intent lands in CH query_log.

namespace {

struct GrantProbe {
  rewriter::RewriteCode code;
  rewriter::StatementType statement_type;
  std::string sql_after_rewrite;
  std::string message;
  std::vector<rewriter::PrivilegeDelta> privileges_deltas;
};

static GrantProbe RunGrant(const std::string &sql, const DynamicArgs &g) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);

  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  applyDynamicArgs(opt->mutable_table_name_args()->mutable_dynamic_args(), g);

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  EXPECT_TRUE(status.ok()) << "gRPC status not OK for: " << sql;

  GrantProbe p;
  p.code = resp.code();
  p.statement_type = resp.statement_type();
  p.sql_after_rewrite = resp.sql_after_rewrite();
  p.message = resp.message();
  for (const auto &d : resp.privileges_deltas()) {
    p.privileges_deltas.push_back(d);
  }
  return p;
}

// Run a GRANT/REVOKE without ANY rewrite options. Used for the "no
// dynamic_args -> UnsupportedStatement" reject path.
static GrantProbe RunGrantNoOpts(const std::string &sql) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  EXPECT_TRUE(status.ok()) << "gRPC status not OK for: " << sql;

  GrantProbe p;
  p.code = resp.code();
  p.statement_type = resp.statement_type();
  p.sql_after_rewrite = resp.sql_after_rewrite();
  p.message = resp.message();
  return p;
}

} // namespace

// --- GRANT happy paths ---

TEST(Grant, TableScope_Qualified) {
  // GRANT SELECT ON tenant1.t TO u with database_map -> structured delta
  // with logical/physical resolved + physical_table = "tenant1.t".
  auto r = RunGrant("GRANT SELECT ON tenant1.t TO u",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_GRANT);
  ASSERT_EQ(r.privileges_deltas.size(), 1u);

  const auto &d = r.privileges_deltas[0];
  EXPECT_EQ(d.action(), rewriter::PrivilegeDelta::ACTION_GRANT);
  EXPECT_EQ(d.scope(), rewriter::PrivilegeDelta::SCOPE_TABLE);
  EXPECT_EQ(d.original_database(), "tenant1");
  EXPECT_EQ(d.logical_database(), "tenant1");
  EXPECT_EQ(d.physical_database(), "testnet");
  EXPECT_EQ(d.original_table(), "t");
  EXPECT_EQ(d.physical_table(), "tenant1.t");
  ASSERT_EQ(d.privileges_size(), 1);
  EXPECT_EQ(d.privileges(0), "SELECT");
  ASSERT_EQ(d.grantees_size(), 1);
  EXPECT_EQ(d.grantees(0).name(), "u");
  EXPECT_FALSE(d.grantees(0).is_current_user());
  EXPECT_FALSE(d.grant_option());

  // SQL is wrapped in SELECT '...' AS gstmt with GRANT verbatim.
  EXPECT_NE(r.sql_after_rewrite.find("SELECT '"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("AS gstmt"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("GRANT"), std::string::npos);
}

TEST(Grant, TableScope_BareTable_UsesUpstreamContext) {
  // GRANT SELECT ON t TO u with upstream_logical_database_in_context.
  // original_database stays empty (bare in SQL); logical resolves from context.
  auto r = RunGrant("GRANT SELECT ON t TO u",
                    {.database_map = {{"tenant1", "testnet"}},
                     .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  ASSERT_EQ(r.privileges_deltas.size(), 1u);
  const auto &d = r.privileges_deltas[0];
  EXPECT_EQ(d.original_database(), "");
  EXPECT_EQ(d.logical_database(), "tenant1");
  EXPECT_EQ(d.physical_database(), "testnet");
  EXPECT_EQ(d.original_table(), "t");
  EXPECT_EQ(d.physical_table(), "tenant1.t");
}

TEST(Grant, DatabaseScope) {
  // GRANT SELECT ON tenant1.* TO u -> SCOPE_DATABASE; no table fields.
  auto r = RunGrant("GRANT SELECT ON tenant1.* TO u",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  ASSERT_EQ(r.privileges_deltas.size(), 1u);
  const auto &d = r.privileges_deltas[0];
  EXPECT_EQ(d.scope(), rewriter::PrivilegeDelta::SCOPE_DATABASE);
  EXPECT_EQ(d.logical_database(), "tenant1");
  EXPECT_EQ(d.physical_database(), "testnet");
  EXPECT_TRUE(d.original_table().empty());
  EXPECT_TRUE(d.physical_table().empty());
}

TEST(Grant, MultiplePrivileges_FanOutPerElement) {
  // ClickHouse's parser splits `GRANT SELECT, INSERT ON db.t TO u` into
  // TWO AccessRightsElement entries (one per privilege). We faithfully
  // emit one PrivilegeDelta per element — same target, single-privilege
  // each. Grantees fan out across both.
  auto r = RunGrant("GRANT SELECT, INSERT ON tenant1.t TO u",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  ASSERT_EQ(r.privileges_deltas.size(), 2u);

  // Collect the per-delta single-privilege names; order isn't guaranteed.
  std::vector<std::string> got;
  for (const auto &d : r.privileges_deltas) {
    ASSERT_EQ(d.privileges_size(), 1);
    EXPECT_EQ(d.original_table(), "t");
    EXPECT_EQ(d.physical_table(), "tenant1.t");
    EXPECT_EQ(d.scope(), rewriter::PrivilegeDelta::SCOPE_TABLE);
    got.push_back(d.privileges(0));
  }
  std::sort(got.begin(), got.end());
  std::vector<std::string> want = {"INSERT", "SELECT"};
  EXPECT_EQ(got, want);
}

TEST(Grant, MultipleGrantees_OneDelta) {
  // Multiple grantees stay aggregated within a single delta.
  auto r = RunGrant("GRANT SELECT ON tenant1.t TO u1, u2",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  ASSERT_EQ(r.privileges_deltas.size(), 1u);
  ASSERT_EQ(r.privileges_deltas[0].grantees_size(), 2);
  EXPECT_EQ(r.privileges_deltas[0].grantees(0).name(), "u1");
  EXPECT_EQ(r.privileges_deltas[0].grantees(1).name(), "u2");
}

// (No multi-target test: ClickHouse's GRANT parser does not accept
// comma-separated ON targets like `ON db.t1, db.t2`. Multiple targets
// require separate GRANT statements, each handled independently.)

TEST(Grant, WithGrantOption_FlagSet) {
  auto r = RunGrant("GRANT SELECT ON tenant1.t TO u WITH GRANT OPTION",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  ASSERT_EQ(r.privileges_deltas.size(), 1u);
  EXPECT_TRUE(r.privileges_deltas[0].grant_option());
}

TEST(Grant, CurrentUser_IsTagged) {
  // CURRENT_USER doesn't get expanded — `is_current_user` is set instead.
  auto r = RunGrant("GRANT SELECT ON tenant1.t TO CURRENT_USER",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  ASSERT_EQ(r.privileges_deltas.size(), 1u);
  ASSERT_EQ(r.privileges_deltas[0].grantees_size(), 1);
  EXPECT_TRUE(r.privileges_deltas[0].grantees(0).is_current_user());
  EXPECT_TRUE(r.privileges_deltas[0].grantees(0).name().empty());
}

TEST(Grant, KnownPhysicalPassthrough) {
  // logical name that's also a known physical (e.g. "system") resolves to
  // itself and produces a delta. database_map empty, known_physical_databases
  // covers it.
  auto r = RunGrant("GRANT SELECT ON system.tables TO u",
                    {.known_physical_databases = {"system"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  ASSERT_EQ(r.privileges_deltas.size(), 1u);
  EXPECT_EQ(r.privileges_deltas[0].logical_database(), "system");
  EXPECT_EQ(r.privileges_deltas[0].physical_database(), "system");
  EXPECT_EQ(r.privileges_deltas[0].physical_table(), "system.tables");
}

// --- REVOKE happy paths ---

TEST(Revoke, Basic) {
  auto r = RunGrant("REVOKE SELECT ON tenant1.t FROM u",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_REVOKE);
  ASSERT_EQ(r.privileges_deltas.size(), 1u);
  EXPECT_EQ(r.privileges_deltas[0].action(),
            rewriter::PrivilegeDelta::ACTION_REVOKE);
  // Marker SELECT uses rstmt, not gstmt.
  EXPECT_NE(r.sql_after_rewrite.find("AS rstmt"), std::string::npos);
  EXPECT_EQ(r.sql_after_rewrite.find("AS gstmt"), std::string::npos);
}

TEST(Revoke, GrantOptionFor_FlagSet) {
  // REVOKE GRANT OPTION FOR ... — the parser routes this through either
  // grant_option or is_partial_revoke; the handler ORs them, so the flag
  // surfaces in either case.
  auto r = RunGrant("REVOKE GRANT OPTION FOR SELECT ON tenant1.t FROM u",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  ASSERT_EQ(r.privileges_deltas.size(), 1u);
  EXPECT_TRUE(r.privileges_deltas[0].grant_option());
}

// --- ON CLUSTER drop ---

TEST(Grant, OnCluster_StrippedFromMarker) {
  // ON CLUSTER fragment is dropped from the AST before formatting; the
  // marker SELECT contains a clean GRANT without the ON CLUSTER clause.
  auto r = RunGrant("GRANT ON CLUSTER my_cluster SELECT ON tenant1.t TO u",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.sql_after_rewrite.find("ON CLUSTER"), std::string::npos)
    << "marker SELECT still mentions ON CLUSTER: " << r.sql_after_rewrite;
  EXPECT_EQ(r.sql_after_rewrite.find("my_cluster"), std::string::npos)
    << "marker SELECT still mentions cluster name: " << r.sql_after_rewrite;
  // Deltas still emitted unchanged — cluster doesn't affect routing.
  ASSERT_EQ(r.privileges_deltas.size(), 1u);
  EXPECT_EQ(r.privileges_deltas[0].physical_table(), "tenant1.t");
}

TEST(Grant, MarkerEscapesQuotes) {
  // A single quote in the original SQL round-trips into the marker SELECT
  // with the quote doubled (CH single-quoted-literal escape).
  auto r = RunGrant("GRANT SELECT ON tenant1.`weird'name` TO u",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("''"), std::string::npos)
    << "Expected doubled-quote escape; got: " << r.sql_after_rewrite;
}

// --- Reject: unsupported variants ---

TEST(Grant, GlobalScope_Rejected) {
  auto r = RunGrant("GRANT SELECT ON *.* TO u",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_NE(r.message.find("global"), std::string::npos);
}

TEST(Grant, RoleMembership_Rejected) {
  // GRANT some_role TO some_user — distinct from privilege grants. The
  // parser populates ASTGrantQuery::roles, which the handler short-circuits.
  // To trigger this path the role must exist at parse time syntactically;
  // ClickHouse's parser handles it without lookups.
  auto r = RunGrant("GRANT some_role TO some_user",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_NE(r.message.find("role-membership"), std::string::npos);
}

TEST(Grant, ReplaceOption_Rejected) {
  auto r = RunGrant("GRANT SELECT ON tenant1.t TO u WITH REPLACE OPTION",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_NE(r.message.find("REPLACE"), std::string::npos);
}

TEST(Revoke, FromAll_Rejected) {
  auto r = RunGrant("REVOKE SELECT ON tenant1.t FROM ALL",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_NE(r.message.find("ALL"), std::string::npos);
}

TEST(Revoke, FromAllExcept_Rejected) {
  auto r = RunGrant("REVOKE SELECT ON tenant1.t FROM ALL EXCEPT u1",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

TEST(Grant, CurrentGrants_Rejected) {
  auto r = RunGrant("GRANT CURRENT GRANTS ON tenant1.t TO u",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_NE(r.message.find("CURRENT GRANTS"), std::string::npos);
}

TEST(Grant, ColumnLevel_Rejected) {
  // Column-level grants are rejected (rather than silently broadened to a
  // table-wide grant) so the audit trail doesn't show an expanded scope.
  auto r = RunGrant("GRANT SELECT(c1, c2) ON tenant1.t TO u",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_NE(r.message.find("column-level"), std::string::npos);
}

// --- Reject: invalid request ---

TEST(Grant, UnknownLogical_InvalidRequest) {
  auto r = RunGrant("GRANT SELECT ON ghost.t TO u",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
  EXPECT_NE(r.message.find("ghost"), std::string::npos);
}

TEST(Grant, BareTable_NoUpstreamContext_InvalidRequest) {
  // No qualifier and no upstream_logical_database_in_context -> rewriter
  // can't pick a logical DB.
  auto r = RunGrant("GRANT SELECT ON t TO u",
                    {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
  EXPECT_NE(r.message.find("unqualified"), std::string::npos);
}

TEST(Grant, NoOptions_Unsupported) {
  // Without a TableNameRewrite/dynamic_args option the rewriter can't
  // validate or resolve, so it fails closed (mirrors CREATE/DROP DATABASE).
  auto r = RunGrantNoOpts("GRANT SELECT ON tenant1.t TO u");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

TEST(Revoke, NoOptions_Unsupported) {
  auto r = RunGrantNoOpts("REVOKE SELECT ON tenant1.t FROM u");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

// ============================================================================
// Optimize RPC: swap left-right JOIN sides based on caller-supplied physical
// table sizes.
//
// When the right table is larger than the left, the optimizer swaps them and
// adjusts the JOIN kind (Left↔Right) to preserve semantics. The goal is to
// keep the bigger table on the left (the streamed side in ClickHouse's
// hash-join execution).
//
// Test taxonomy:
//   * Optimize.* — tests for the RPC surface (codes, fallback contract).
//   * JoinSwap.*  — tests for the swap policy proper (kinds, sizes,
//                   skip rules). They go through the Optimize RPC too,
//                   but assert on swap behaviour rather than codes.
// ============================================================================

namespace {

rewriter::OptimizeResponse RunOptimize(
    const std::string &sql,
    const std::map<std::string, uint64_t> &table_sizes,
    uint64_t default_size = 0,
    double swap_threshold = 1.0) {
  RewriterServiceImpl service;
  rewriter::OptimizeRequest req;
  req.set_sql(sql);
  for (const auto &kv : table_sizes) {
    (*req.mutable_table_sizes())[kv.first] = kv.second;
  }
  if (default_size != 0) req.set_default_size(default_size);
  if (swap_threshold != 1.0) req.set_swap_threshold(swap_threshold);

  rewriter::OptimizeResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Optimize(&ctx, &req, &resp);
  EXPECT_TRUE(status.ok()) << "gRPC status not OK for: " << sql;
  return resp;
}

} // namespace

// ----- RPC contract -----

TEST(Optimize, SyntaxError_FallbackToOriginal) {
  // Invalid SQL — Optimize must return OptimizeSyntaxError and echo the
  // original SQL back unchanged, so the caller can fall back to running it
  // (or surfacing the error) without parsing the message.
  const std::string sql = "this is not SQL at all";
  const auto resp = RunOptimize(sql, {});
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeSyntaxError);
  EXPECT_EQ(resp.sql_after_optimize(), sql);
  EXPECT_FALSE(resp.swap_applied());
  EXPECT_FALSE(resp.message().empty()) << "expected a parser error message";
}

TEST(Optimize, SingleTable_Unoptimizable) {
  // Valid SQL with no JOIN — Optimize is a no-op. Returns Unoptimizable
  // (not OK) so callers can distinguish "I optimized something" from "I
  // had nothing to optimize." sql_after_optimize is the original SQL.
  const std::string sql = "SELECT * FROM a";
  const auto resp = RunOptimize(sql, {{"a", 1000}});
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeUnoptimizable);
  EXPECT_EQ(resp.sql_after_optimize(), sql);
  EXPECT_FALSE(resp.swap_applied());
}

TEST(Optimize, NonSelect_Unoptimizable) {
  // Non-SELECT (write / DDL) — currently out of scope, returns Unoptimizable.
  const std::string sql = "INSERT INTO a VALUES (1, 2)";
  const auto resp = RunOptimize(sql, {{"a", 1000}});
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeUnoptimizable);
  EXPECT_EQ(resp.sql_after_optimize(), sql);
  EXPECT_FALSE(resp.swap_applied());
}

// ----- Swap-policy tests -----

TEST(JoinSwap, NoSwap_RightSmaller) {
  // Right table (b) is smaller — no swap needed. Code is Unoptimizable
  // (nothing to do); sql_after_optimize echoes the input.
  const auto resp = RunOptimize(
    "SELECT * FROM a JOIN b ON a.x = b.x",
    {{"a", 1000}, {"b", 100}});
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeUnoptimizable);
  EXPECT_FALSE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_EQ(sql.find("RIGHT"), std::string::npos) << sql;
}

TEST(JoinSwap, InnerJoin_RightLarger) {
  // Right table (b) is larger — swap so a moves to the right.
  const auto resp = RunOptimize(
    "SELECT * FROM a JOIN b ON a.x = b.x",
    {{"a", 100}, {"b", 1000}});
  ASSERT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeOK);
  EXPECT_TRUE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  // After swap the FROM is b and JOIN target is a; INNER stays INNER.
  EXPECT_NE(sql.find("b"), std::string::npos) << sql;
  EXPECT_NE(sql.find("a"), std::string::npos) << sql;
  EXPECT_EQ(sql.find("RIGHT"), std::string::npos) << sql;
}

TEST(JoinSwap, LeftJoin_RightLarger_BecomesRightJoin) {
  const auto resp = RunOptimize(
    "SELECT * FROM a LEFT JOIN b ON a.x = b.x",
    {{"a", 100}, {"b", 1000}});
  ASSERT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeOK);
  EXPECT_TRUE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_NE(sql.find("RIGHT JOIN"), std::string::npos) << sql;
}

TEST(JoinSwap, RightJoin_RightLarger_BecomesLeftJoin) {
  const auto resp = RunOptimize(
    "SELECT * FROM a RIGHT JOIN b ON a.x = b.x",
    {{"a", 100}, {"b", 1000}});
  ASSERT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeOK);
  EXPECT_TRUE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_NE(sql.find("LEFT JOIN"), std::string::npos) << sql;
}

TEST(JoinSwap, NoSwap_EqualSizes) {
  const auto resp = RunOptimize(
    "SELECT * FROM a LEFT JOIN b ON a.x = b.x",
    {{"a", 500}, {"b", 500}});
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeUnoptimizable);
  EXPECT_FALSE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_NE(sql.find("LEFT JOIN"), std::string::npos) << sql;
}

TEST(JoinSwap, NoSwap_ThresholdPrevents) {
  // Ratio 150/100 = 1.5 < threshold 2.0 → no swap.
  const auto resp = RunOptimize(
    "SELECT * FROM a LEFT JOIN b ON a.x = b.x",
    {{"a", 100}, {"b", 150}},
    0,      // default_size
    2.0);   // swap_threshold
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeUnoptimizable);
  EXPECT_FALSE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_NE(sql.find("LEFT JOIN"), std::string::npos) << sql;
}

TEST(JoinSwap, CrossJoin_RightLarger) {
  // CROSS JOIN — symmetric, swap table positions, kind unchanged.
  const auto resp = RunOptimize(
    "SELECT * FROM a CROSS JOIN b",
    {{"a", 100}, {"b", 1000}});
  ASSERT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeOK);
  EXPECT_TRUE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_NE(sql.find("CROSS JOIN"), std::string::npos) << sql;
}

TEST(JoinSwap, SubqueryRight_Skip) {
  // Right side is a subquery — skip, no swap.
  const auto resp = RunOptimize(
    "SELECT * FROM a LEFT JOIN (SELECT * FROM b) AS s ON a.x = s.x",
    {{"a", 100}, {"b", 1000}});
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeUnoptimizable);
  EXPECT_FALSE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_NE(sql.find("LEFT JOIN"), std::string::npos) << sql;
}

TEST(JoinSwap, TableFunctionRight_Skip) {
  const auto resp = RunOptimize(
    "SELECT * FROM a JOIN numbers(10) AS n ON a.x = n.x",
    {{"a", 100}});
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeUnoptimizable);
  EXPECT_FALSE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_EQ(sql.find("RIGHT"), std::string::npos) << sql;
}

TEST(JoinSwap, DefaultSize_UnknownTable_Swap) {
  // a not in table_sizes → defaults to 0 → treated as smaller than b=1000 → swap.
  const auto resp = RunOptimize(
    "SELECT * FROM a JOIN b ON a.x = b.x",
    {{"b", 1000}});
  ASSERT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeOK);
  EXPECT_TRUE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_EQ(sql.find("RIGHT"), std::string::npos) << sql;
}

TEST(JoinSwap, DefaultSizeNonZero_UnknownIsLarge) {
  // a not in table_sizes but default_size=2000 > b=100 → a "appears large" → no swap.
  const auto resp = RunOptimize(
    "SELECT * FROM a JOIN b ON a.x = b.x",
    {{"b", 100}},
    2000);  // default_size
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeUnoptimizable);
  EXPECT_FALSE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_EQ(sql.find("RIGHT"), std::string::npos) << sql;
}

TEST(JoinSwap, MultipleJoins_OnlyFirstSwapped) {
  // First JOIN (a JOIN b) is the only swap candidate; second JOIN (c) is
  // untouched because its left side is the compound (a JOIN b).
  const auto resp = RunOptimize(
    "SELECT * FROM a JOIN b ON a.x = b.x JOIN c ON b.y = c.y",
    {{"a", 100}, {"b", 1000}, {"c", 500}});
  ASSERT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeOK);
  EXPECT_TRUE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_NE(sql.find("c"), std::string::npos) << sql;
}

TEST(JoinSwap, FullJoin_RightLarger) {
  // FULL JOIN — symmetric, swap table positions, kind unchanged.
  const auto resp = RunOptimize(
    "SELECT * FROM a FULL JOIN b ON a.x = b.x",
    {{"a", 100}, {"b", 1000}});
  ASSERT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeOK);
  EXPECT_TRUE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_EQ(sql.find("LEFT"), std::string::npos) << sql;
  EXPECT_EQ(sql.find("RIGHT"), std::string::npos) << sql;
  EXPECT_NE(sql.find("FULL"), std::string::npos) << sql;
}

TEST(JoinSwap, EmptyTableSizes_NoSwap) {
  // Empty map — all tables default to 0, so right == left == 0, no swap.
  const auto resp = RunOptimize(
    "SELECT * FROM a LEFT JOIN b ON a.x = b.x",
    {});
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeUnoptimizable);
  EXPECT_FALSE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_NE(sql.find("LEFT JOIN"), std::string::npos) << sql;
}

TEST(JoinSwap, WithAlias_RightLarger) {
  // Aliases must travel with their table_expression after a swap.
  const auto resp = RunOptimize(
    "SELECT * FROM a AS t1 JOIN b AS t2 ON t1.x = t2.x",
    {{"a", 100}, {"b", 1000}});
  ASSERT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeOK);
  EXPECT_TRUE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  const auto join_pos = sql.find("JOIN");
  ASSERT_NE(join_pos, std::string::npos);
  const auto before_join = sql.substr(0, join_pos);
  const auto after_join = sql.substr(join_pos);
  EXPECT_NE(before_join.find("b"), std::string::npos) << sql;
  EXPECT_NE(after_join.find("a"), std::string::npos) << sql;
}

TEST(JoinSwap, WithUsing_NotSwapped) {
  // USING is handled specially by CH's join algorithm — swap could break
  // it. Verify we leave USING joins alone even when right is larger.
  const auto resp = RunOptimize(
    "SELECT * FROM a LEFT JOIN b USING (x)",
    {{"a", 100}, {"b", 1000}});
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeUnoptimizable);
  EXPECT_FALSE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_EQ(sql.find("RIGHT"), std::string::npos) << sql;
  EXPECT_NE(sql.find("LEFT JOIN"), std::string::npos) << sql;
  EXPECT_NE(sql.find("USING"), std::string::npos) << sql;
}

TEST(JoinSwap, SemiJoin_NotSwapped) {
  // SEMI strictness — CH treats this as not-yet-validated for swap; we follow.
  // On Unoptimizable, sql_after_optimize echoes the input verbatim (no
  // re-formatting), so the assertion uses the user-written `LEFT SEMI JOIN`
  // form rather than CH formatter's canonical `SEMI LEFT JOIN`.
  const auto resp = RunOptimize(
    "SELECT * FROM a LEFT SEMI JOIN b ON a.x = b.x",
    {{"a", 100}, {"b", 1000}});
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeUnoptimizable);
  EXPECT_FALSE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_EQ(sql.find("RIGHT"), std::string::npos) << sql;
  EXPECT_NE(sql.find("LEFT SEMI JOIN"), std::string::npos) << sql;
}

TEST(JoinSwap, AntiJoin_NotSwapped) {
  // Same echo-vs-format note as SemiJoin_NotSwapped above.
  const auto resp = RunOptimize(
    "SELECT * FROM a LEFT ANTI JOIN b ON a.x = b.x",
    {{"a", 100}, {"b", 1000}});
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeUnoptimizable);
  EXPECT_FALSE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_EQ(sql.find("RIGHT"), std::string::npos) << sql;
  EXPECT_NE(sql.find("LEFT ANTI JOIN"), std::string::npos) << sql;
}

TEST(JoinSwap, CommaJoin) {
  // Comma (implicit cross) join: FROM a, b is symmetric — swap is allowed.
  const auto resp = RunOptimize(
    "SELECT * FROM a, b WHERE a.x = b.x",
    {{"a", 100}, {"b", 1000}});
  ASSERT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeOK);
  EXPECT_TRUE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_EQ(sql.find("RIGHT"), std::string::npos) << sql;
}

TEST(JoinSwap, AsofJoin_NotSwapped) {
  // ASOF JOIN inequality direction is bound to which side is the lookup
  // (right) table. Even if right is larger, swap would change semantics.
  const auto resp = RunOptimize(
    "SELECT * FROM a ASOF JOIN b ON a.k = b.k AND a.t >= b.t",
    {{"a", 100}, {"b", 1000}});
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeUnoptimizable);
  EXPECT_FALSE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  const auto from_pos = sql.find("FROM");
  ASSERT_NE(from_pos, std::string::npos);
  const auto asof_pos = sql.find("ASOF");
  ASSERT_NE(asof_pos, std::string::npos);
  const auto between = sql.substr(from_pos, asof_pos - from_pos);
  EXPECT_NE(between.find("a"), std::string::npos) << sql;
  EXPECT_EQ(between.find("b"), std::string::npos) << sql;
}

TEST(JoinSwap, LeftAsofJoin_NotSwapped) {
  // On Unoptimizable the SQL is echoed verbatim — assert on the
  // user-written `LEFT ASOF JOIN` form, not CH formatter's canonical
  // `ASOF LEFT JOIN`.
  const auto resp = RunOptimize(
    "SELECT * FROM a LEFT ASOF JOIN b ON a.k = b.k AND a.t >= b.t",
    {{"a", 100}, {"b", 1000}});
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeUnoptimizable);
  EXPECT_FALSE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  EXPECT_EQ(sql.find("RIGHT"), std::string::npos) << sql;
  EXPECT_NE(sql.find("LEFT ASOF"), std::string::npos) << sql;
}

TEST(JoinSwap, PasteJoin_NotSwapped) {
  // PASTE JOIN concatenates by row position with no ON/USING. Swap would
  // change projected column order — leave it alone.
  const auto resp = RunOptimize(
    "SELECT * FROM a PASTE JOIN b",
    {{"a", 100}, {"b", 1000}});
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeUnoptimizable);
  EXPECT_FALSE(resp.swap_applied());
  const auto sql = NormalizeWS(resp.sql_after_optimize());
  const auto from_pos = sql.find("FROM");
  ASSERT_NE(from_pos, std::string::npos);
  const auto paste_pos = sql.find("PASTE");
  ASSERT_NE(paste_pos, std::string::npos);
  const auto between = sql.substr(from_pos, paste_pos - from_pos);
  EXPECT_NE(between.find("a"), std::string::npos) << sql;
  EXPECT_EQ(between.find("b"), std::string::npos) << sql;
}

// ============================ View rewrite ============================

TEST(View, DropView_Dynamic_RewritesName) {
  auto resp = RunWithDynamicArgs(
    "DROP VIEW tenant1.v", /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_DROP_VIEW);
  EXPECT_NE(resp.sql_after_rewrite().find("physical_1"), std::string::npos);
  const auto *e = FindAccessed(resp, "tenant1", "v");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->physical_database(), "physical_1");
}

TEST(View, DropView_NoOptions_Passthrough) {
  auto resp = RunNoOptions("DROP VIEW mydb.v");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_DROP_VIEW);
  ASSERT_EQ(resp.original_accessed_tables_size(), 1);
}

TEST(View, DropView_IfExists_Dynamic_RewritesName) {
  auto resp = RunWithDynamicArgs(
    "DROP VIEW IF EXISTS tenant1.v", /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_DROP_VIEW);
  EXPECT_NE(resp.sql_after_rewrite().find("physical_1"), std::string::npos);
  const auto *e = FindAccessed(resp, "tenant1", "v");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->physical_database(), "physical_1");
}

TEST(View, CreateView_Dynamic_RewritesNameAndBody) {
  auto resp = RunWithDynamicArgs(
    "CREATE VIEW tenant1.v AS SELECT * FROM tenant1.events",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_CREATE_VIEW);
  EXPECT_NE(resp.sql_after_rewrite().find("physical_1"), std::string::npos);
  const auto *view = FindAccessed(resp, "tenant1", "v");
  ASSERT_NE(view, nullptr);
  EXPECT_EQ(view->physical_database(), "physical_1");
  const auto *body = FindAccessed(resp, "tenant1", "events");
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(body->physical_database(), "physical_1");
}

TEST(View, CreateView_Static_RewritesNameAndBody) {
  // RunSelectWithStaticArgs only populates static_args on the request; it
  // works for any statement kind — here a CREATE VIEW.
  auto resp = RunSelectWithStaticArgs(
    "CREATE VIEW v AS SELECT * FROM t",
    /*table_map*/ {{"t", "t_phys"}, {"v", "v_phys"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_CREATE_VIEW);
  EXPECT_NE(resp.sql_after_rewrite().find("t_phys"), std::string::npos);
  EXPECT_NE(resp.sql_after_rewrite().find("v_phys"), std::string::npos);
}

TEST(View, CreateView_NoOptions_Passthrough) {
  auto resp = RunNoOptions("CREATE VIEW mydb.v AS SELECT * FROM mydb.t");
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_CREATE_VIEW);
}

TEST(View, CreateOrReplaceView_RoundTrips) {
  auto resp = RunWithDynamicArgs(
    "CREATE OR REPLACE VIEW tenant1.v AS SELECT 1",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_CREATE_VIEW);
  EXPECT_NE(resp.sql_after_rewrite().find("REPLACE"), std::string::npos);
}

TEST(View, CreateView_BodyJoin_RewritesBothTables) {
  auto resp = RunWithDynamicArgs(
    "CREATE VIEW tenant1.v AS "
    "SELECT * FROM tenant1.a JOIN tenant1.b USING (id)",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  ASSERT_NE(FindAccessed(resp, "tenant1", "a"), nullptr);
  ASSERT_NE(FindAccessed(resp, "tenant1", "b"), nullptr);
  EXPECT_NE(resp.sql_after_rewrite().find("physical_1"), std::string::npos);
}

TEST(View, CreateMaterializedView_WithTo_RewritesNameTargetAndBody) {
  auto resp = RunWithDynamicArgs(
    "CREATE MATERIALIZED VIEW tenant1.mv TO tenant1.dest "
    "AS SELECT * FROM tenant1.src",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.statement_type(),
            rewriter::STATEMENT_TYPE_CREATE_MATERIALIZED_VIEW);
  ASSERT_NE(FindAccessed(resp, "tenant1", "mv"), nullptr);
  ASSERT_NE(FindAccessed(resp, "tenant1", "dest"), nullptr);
  ASSERT_NE(FindAccessed(resp, "tenant1", "src"), nullptr);
  EXPECT_EQ(FindAccessed(resp, "tenant1", "dest")->physical_database(),
            "physical_1");
}

TEST(View, CreateMaterializedView_NoTo_RewritesNameAndBody) {
  auto resp = RunWithDynamicArgs(
    "CREATE MATERIALIZED VIEW tenant1.mv ENGINE = MergeTree ORDER BY x "
    "AS SELECT x FROM tenant1.src",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.statement_type(),
            rewriter::STATEMENT_TYPE_CREATE_MATERIALIZED_VIEW);
  ASSERT_NE(FindAccessed(resp, "tenant1", "src"), nullptr);
  EXPECT_EQ(FindAccessed(resp, "tenant1", "src")->physical_database(),
            "physical_1");
}

TEST(View, CreateView_BodyRemoteLocalJoin_ForcesGlobal) {
  // tenant1 maps to a remote upstream; tenant2 is local. A JOIN across them
  // in the view body must become a GLOBAL JOIN via the cross-shard pass.
  auto resp = RunSelectWithDynamicArgs(
    "CREATE VIEW tenant2.v AS "
    "SELECT * FROM tenant1.a JOIN tenant2.b USING (id)",
    /*database_map*/ {{"tenant1", "physical_1"}, {"tenant2", "physical_2"}},
    /*known_physical*/ {},
    /*upstream_logical_in_context*/ "",
    /*remote_index*/ {{"tenant1", "up1"}},
    /*remote_upstreams*/ {{"up1", {"remote:9000", "u", "p"}}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_CREATE_VIEW);
  EXPECT_NE(resp.sql_after_rewrite().find("GLOBAL"), std::string::npos);
}

TEST(View, CreateLiveView_Unsupported) {
  auto resp = RunNoOptions("CREATE LIVE VIEW lv AS SELECT 1");
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
}

TEST(View, CreateLiveViewIfNotExists_ReportsExistenceClause) {
  const std::string sql = "CREATE LIVE VIEW IF NOT EXISTS lv AS SELECT 1";
  auto resp = RunNoOptions(sql);

  EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_EQ(resp.existence_clause(), rewriter::EXISTENCE_CLAUSE_IF_NOT_EXISTS);
  EXPECT_EQ(resp.sql_after_rewrite(), sql);
}

TEST(View, CreateLiveViewWithSQLSecurity_Unsupported) {
  auto resp = RunNoOptions(
    "CREATE SQL SECURITY DEFINER LIVE VIEW lv AS SELECT 1");

  EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
}

TEST(View, MaterializedViewColumnNamedLiveDoesNotTriggerCompatibility) {
  auto resp = RunNoOptions(
    "CREATE MATERIALIZED VIEW mv (live View) AS SELECT 1");

  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
  EXPECT_EQ(resp.statement_type(),
            rewriter::STATEMENT_TYPE_CREATE_MATERIALIZED_VIEW);
}

TEST(View, AttachLiveView_Unsupported) {
  auto resp = RunNoOptions("ATTACH LIVE VIEW lv AS SELECT 1");
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
}

TEST(View, MalformedCreateLiveView_RemainsSyntaxError) {
  auto resp = RunNoOptions("CREATE LIVE VIEW");
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::SyntaxError);
}

TEST(View, CreateWindowView_Unsupported) {
  auto resp = RunNoOptions(
    "CREATE WINDOW VIEW wv AS "
    "SELECT count(*) FROM t GROUP BY tumble(ts, INTERVAL 1 SECOND)");
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
}

TEST(View, CreateView_UnionBody_RewritesAllBranches) {
  // The view body is an ASTSelectWithUnionQuery — confirm both UNION
  // branches get their table references rewritten.
  auto resp = RunWithDynamicArgs(
    "CREATE VIEW tenant1.v AS "
    "SELECT x FROM tenant1.a UNION ALL SELECT x FROM tenant1.b",
    /*database_map*/ {{"tenant1", "physical_1"}});
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_CREATE_VIEW);
  ASSERT_NE(FindAccessed(resp, "tenant1", "a"), nullptr);
  ASSERT_NE(FindAccessed(resp, "tenant1", "b"), nullptr);
  EXPECT_NE(resp.sql_after_rewrite().find("physical_1"), std::string::npos);
}

TEST(View, CreateView_Dynamic_UnknownDb_Rejected) {
  // The view name's logical DB is not in database_map and not a known
  // physical DB — rewriteOneTarget must reject before acceptWrite.
  auto resp = RunWithDynamicArgs(
    "CREATE VIEW unknowndb.v AS SELECT 1",
    /*database_map*/ {{"tenant1", "physical_1"}});
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
}

TEST(View, CreateView_Static_RemoteTableMapHit_Unsupported) {
  // A view name cannot map to a remote() table function.
  auto resp = RunSelectWithStaticArgs(
    "CREATE VIEW v AS SELECT 1",
    /*table_map*/ {},
    /*remote_table_map*/ {{"v", {"remote_db", "remote_v"}}});
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
}

// ------------------------ MaterializeSQL ------------------------
static rewriter::MaterializeSQLResponse RunMaterialize(
    const rewriter::MaterializeSQLRequest &req) {
  RewriterServiceImpl service;
  rewriter::MaterializeSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.MaterializeSQL(&ctx, &req, &resp);
  EXPECT_TRUE(status.ok());
  return resp;
}

TEST(Materialize, PassesThroughDeterministicSQL) {
  rewriter::MaterializeSQLRequest req;
  req.set_sql("INSERT INTO db.t VALUES (1)");
  auto resp = RunMaterialize(req);
  EXPECT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSuccess);
  EXPECT_EQ(resp.sql_after_materialization(), "INSERT INTO db.t VALUES (1)");
  EXPECT_EQ(resp.replacements_size(), 0);
  EXPECT_EQ(resp.materializer_profile_id(), "sentio-p1-nondet-v1");
}

TEST(Materialize, SyntaxErrorEchoesAndFlags) {
  rewriter::MaterializeSQLRequest req;
  req.set_sql("INSERT INTO db.t VALUE (");  // malformed
  auto resp = RunMaterialize(req);
  EXPECT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSyntaxError);
  EXPECT_EQ(resp.sql_after_materialization(), resp.sql());
  EXPECT_FALSE(resp.message().empty());  // carries the parser's displayText()
}

TEST(Materialize, RejectsBadPolicy) {
  rewriter::MaterializeSQLRequest req;
  req.set_sql("SELECT 1");
  req.mutable_policy()->set_profile_id("other");
  auto resp = RunMaterialize(req);
  EXPECT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeInvalidRequest);
  EXPECT_EQ(resp.sql_after_materialization(), resp.sql());
  // Mirrors rewriter-go: a policy reject still echoes the default profile id.
  EXPECT_EQ(resp.materializer_profile_id(), "sentio-p1-nondet-v1");

  rewriter::MaterializeSQLRequest req2;
  req2.set_sql("SELECT 1");
  req2.mutable_policy()->set_timezone("PST");
  auto resp2 = RunMaterialize(req2);
  EXPECT_EQ(resp2.code(), rewriter::MaterializeCode::MaterializeInvalidRequest);
}

static rewriter::MaterializeSQLRequest MatReq(const std::string &sql) {
  rewriter::MaterializeSQLRequest req;
  req.set_sql(sql);
  return req;
}

TEST(Materialize, NowToDateTime) {
  auto req = MatReq("SELECT now()");
  req.mutable_inputs()->set_now_unix_ns(1710000000123456789LL);
  auto resp = RunMaterialize(req);
  ASSERT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSuccess) << resp.message();
  ASSERT_EQ(resp.replacements_size(), 1);
  EXPECT_EQ(resp.replacements(0).function_name(), "now");
  EXPECT_EQ(resp.replacements(0).ordinal(), 0u);
  EXPECT_EQ(resp.replacements(0).literal_sql(), "toDateTime(1710000000)");
  EXPECT_EQ(resp.replacements(0).value_type(), "DateTime");
  EXPECT_EQ(resp.sql_after_materialization().find("now("), std::string::npos);
}

TEST(Materialize, FailsClosedWhenNowInputMissing) {
  auto resp = RunMaterialize(MatReq("SELECT now()"));
  EXPECT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeInvalidRequest);
  EXPECT_EQ(resp.sql_after_materialization(), resp.sql());
}

TEST(Materialize, FailsClosedWhenKnownVolatileUnsupported) {
  auto resp = RunMaterialize(MatReq("SELECT generateSnowflakeID()"));
  EXPECT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeUnsupportedStatement);
  EXPECT_EQ(resp.sql_after_materialization(), resp.sql());
}

TEST(Materialize, FailsClosedWhenSupportedFunctionHasArguments) {
  auto req = MatReq("SELECT now('UTC')");
  req.mutable_inputs()->set_now_unix_ns(1710000000123456789LL);
  auto resp = RunMaterialize(req);
  EXPECT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeUnsupportedStatement);
  EXPECT_EQ(resp.sql_after_materialization(), resp.sql());  // fail-closed echo
}

// nowInBlock() is a real ClickHouse niladic function in the now-family branch.
TEST(Materialize, NowInBlockToDateTime) {
  auto req = MatReq("SELECT nowInBlock()");
  req.mutable_inputs()->set_now_unix_ns(1710000000123456789LL);
  auto resp = RunMaterialize(req);
  ASSERT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSuccess) << resp.message();
  ASSERT_EQ(resp.replacements_size(), 1);
  EXPECT_EQ(resp.replacements(0).literal_sql(), "toDateTime(1710000000)");
  EXPECT_EQ(resp.replacements(0).value_type(), "DateTime");
}

// current_timestamp() (with parens) is a ClickHouse alias of now() → ASTFunction,
// so it materializes the same as now().
TEST(Materialize, CurrentTimestampParensToDateTime) {
  auto req = MatReq("SELECT current_timestamp()");
  req.mutable_inputs()->set_now_unix_ns(1710000000123456789LL);
  auto resp = RunMaterialize(req);
  ASSERT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSuccess) << resp.message();
  ASSERT_EQ(resp.replacements_size(), 1);
  EXPECT_EQ(resp.replacements(0).literal_sql(), "toDateTime(1710000000)");
}

// ClickHouse-vs-polyglot divergence: bare SQL-standard keywords (no parens) are
// parsed as IDENTIFIERS, not functions (and error as UNKNOWN_IDENTIFIER at
// execution — they are not volatile in ClickHouse). The materializer correctly
// leaves them untouched (unlike the Go/polyglot materializer).
TEST(Materialize, BareKeywordIdentifiersLeftAlone) {
  auto req = MatReq("SELECT CURRENT_TIMESTAMP, CURRENT_DATE, localtime");
  req.mutable_inputs()->set_now_unix_ns(1710000000123456789LL);
  auto resp = RunMaterialize(req);
  EXPECT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSuccess);
  EXPECT_EQ(resp.replacements_size(), 0);
  EXPECT_EQ(resp.sql_after_materialization(), resp.sql());  // echoed verbatim
}

// Full Go-parity scalar set (parens forms — bare CURRENT_DATE is a CH identifier).
TEST(Materialize, AdditionalSafeScalarFunctions) {
  auto req = MatReq("SELECT now64(), today(), current_date(), curdate(), yesterday(), "
                    "UTCTimestamp(), rand32(), rand64(), randCanonical(), generateUUIDv4()");
  req.mutable_inputs()->set_now_unix_ns(1710000000123456789LL);
  req.mutable_inputs()->add_random_uint64_values(13);  // rand32
  req.mutable_inputs()->add_random_uint64_values(99);  // rand64
  req.mutable_inputs()->add_random_float64_values(0.125);
  req.mutable_inputs()->add_uuid_values("018f3c8a-3e8a-7d0a-b5b0-6e95e3f0b001");
  auto resp = RunMaterialize(req);
  ASSERT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSuccess) << resp.message();
  ASSERT_EQ(resp.replacements_size(), 10);
  const std::vector<std::string> want = {
    "fromUnixTimestamp64Milli(1710000000123)",
    "toDate(toDateTime(1710000000))",
    "toDate(toDateTime(1710000000))",
    "toDate(toDateTime(1710000000))",
    "toDate(toDateTime(1709913600))",
    "toDateTime(1710000000, 'UTC')",
    "13",
    "99",
    "toFloat64(0.125)",
    "toUUID('018f3c8a-3e8a-7d0a-b5b0-6e95e3f0b001')"};
  for (int i = 0; i < resp.replacements_size(); ++i)
    EXPECT_EQ(resp.replacements(i).literal_sql(), want[i]) << "ordinal " << i;
  EXPECT_EQ(resp.replacements(0).value_type(), "DateTime64(3)");
  EXPECT_EQ(resp.replacements(1).value_type(), "Date");   // today
  EXPECT_EQ(resp.replacements(4).value_type(), "Date");   // yesterday
  EXPECT_EQ(resp.replacements(6).value_type(), "UInt32");
  EXPECT_EQ(resp.replacements(7).value_type(), "UInt64");
  EXPECT_EQ(resp.replacements(8).value_type(), "Float64");
  EXPECT_EQ(resp.replacements(9).value_type(), "UUID");
}

TEST(Materialize, Rand32OverflowFailsClosed) {
  auto req = MatReq("SELECT rand32()");
  req.mutable_inputs()->add_random_uint64_values(uint64_t(1) << 32);
  auto resp = RunMaterialize(req);
  EXPECT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeInvalidRequest);
  EXPECT_EQ(resp.sql_after_materialization(), resp.sql());
}

TEST(Materialize, RandCanonicalOutOfRangeFailsClosed) {
  auto req = MatReq("SELECT randCanonical()");
  req.mutable_inputs()->add_random_float64_values(1.0);
  auto resp = RunMaterialize(req);
  EXPECT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeInvalidRequest);
  EXPECT_EQ(resp.sql_after_materialization(), resp.sql());
}

TEST(Materialize, BadUuidFailsClosed) {
  auto req = MatReq("SELECT generateUUIDv4()");
  req.mutable_inputs()->add_uuid_values("not-a-uuid");
  auto resp = RunMaterialize(req);
  EXPECT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeInvalidRequest);
  EXPECT_EQ(resp.sql_after_materialization(), resp.sql());
}

TEST(Materialize, RandValueExhaustionFailsClosed) {
  auto resp = RunMaterialize(MatReq("SELECT rand64()"));  // no inputs
  EXPECT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeInvalidRequest);
}

// ---- INSERT … VALUES data-section materialization (off-AST) ----
TEST(Materialize, InsertValuesSupportedFunctions) {
  auto req = MatReq("INSERT INTO db.t VALUES (now(), rand(), generateUUIDv4())");
  req.mutable_inputs()->set_now_unix_ns(1710000000123456789LL);
  req.mutable_inputs()->add_random_uint64_values(7);
  req.mutable_inputs()->add_uuid_values("018f3c8a-3e8a-7d0a-b5b0-6e95e3f0b001");
  auto resp = RunMaterialize(req);
  ASSERT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSuccess) << resp.message();
  EXPECT_EQ(resp.replacements_size(), 3);
  const std::string &out = resp.sql_after_materialization();
  EXPECT_EQ(out.find("now("), std::string::npos);
  EXPECT_EQ(out.find("rand("), std::string::npos);
  EXPECT_EQ(resp.replacements(1).literal_sql(), "7");  // rand()
  // Pin the exact reassembled output. `FORMAT Values(...)` (no space) is
  // ClickHouse's own round-trip form — verified to parse AND execute via
  // clickhouse-local, so it is the intended output, not a missing-space bug.
  EXPECT_EQ(out,
            "INSERT INTO db.t FORMAT Values(toDateTime(1710000000), 7, "
            "toUUID('018f3c8a-3e8a-7d0a-b5b0-6e95e3f0b001'))");
}

// Nested function-call parens inside a VALUES row: now() at depth 2 must be
// found and materialized (exercises splitValuesRows depth tracking + recursion).
TEST(Materialize, InsertValuesNestedParens) {
  auto req = MatReq("INSERT INTO db.t VALUES (toString(now()))");
  req.mutable_inputs()->set_now_unix_ns(1710000000123456789LL);
  auto resp = RunMaterialize(req);
  ASSERT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSuccess) << resp.message();
  ASSERT_EQ(resp.replacements_size(), 1);
  EXPECT_EQ(resp.replacements(0).literal_sql(), "toDateTime(1710000000)");
  const std::string &out = resp.sql_after_materialization();
  EXPECT_EQ(out.find("now("), std::string::npos);
  EXPECT_NE(out.find("toString(toDateTime(1710000000))"), std::string::npos);
}

TEST(Materialize, InsertValuesMultiRow) {
  auto req = MatReq("INSERT INTO db.t VALUES (rand64()), (rand64())");
  req.mutable_inputs()->add_random_uint64_values(11);
  req.mutable_inputs()->add_random_uint64_values(22);
  auto resp = RunMaterialize(req);
  ASSERT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSuccess) << resp.message();
  ASSERT_EQ(resp.replacements_size(), 2);
  EXPECT_EQ(resp.replacements(0).literal_sql(), "11");
  EXPECT_EQ(resp.replacements(1).literal_sql(), "22");
  const std::string &out = resp.sql_after_materialization();
  EXPECT_NE(out.find("(11)"), std::string::npos);
  EXPECT_NE(out.find("(22)"), std::string::npos);
}

TEST(Materialize, InsertValuesDeterministicEchoesVerbatim) {
  auto resp = RunMaterialize(MatReq("INSERT INTO db.t VALUES (1, 'a')"));
  EXPECT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSuccess);
  EXPECT_EQ(resp.replacements_size(), 0);
  EXPECT_EQ(resp.sql_after_materialization(), "INSERT INTO db.t VALUES (1, 'a')");
}

// ClickHouse resolves a FORMAT name case-insensitively, so `FORMAT values`
// rows run as Values and are materialized like VALUES rows.
TEST(Materialize, InsertValuesFormatNameIsCaseInsensitive) {
  for (const char *format : {"FORMAT values", "format VALUES", "FORMAT vAlUeS"}) {
    SCOPED_TRACE(format);
    auto req = MatReq(std::string("INSERT INTO db.t ") + format + " (now())");
    req.mutable_inputs()->set_now_unix_ns(1710000000123456789LL);
    auto resp = RunMaterialize(req);
    ASSERT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSuccess) << resp.message();
    ASSERT_EQ(resp.replacements_size(), 1);
    EXPECT_EQ(resp.replacements(0).literal_sql(), "toDateTime(1710000000)");
    EXPECT_EQ(resp.sql_after_materialization().find("now("), std::string::npos);
  }
  const auto unsupported = RunMaterialize(MatReq("INSERT INTO db.t FORMAT values (generateSnowflakeID())"));
  EXPECT_EQ(unsupported.code(), rewriter::MaterializeCode::MaterializeUnsupportedStatement);
}

TEST(Materialize, InsertValuesUnsupportedVolatileFailsClosed) {
  auto resp = RunMaterialize(MatReq("INSERT INTO db.t VALUES (generateSnowflakeID())"));
  EXPECT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeUnsupportedStatement);
  EXPECT_EQ(resp.sql_after_materialization(), resp.sql());
}

TEST(Materialize, InsertSelectMaterializes) {
  auto req = MatReq("INSERT INTO db.t SELECT now()");
  req.mutable_inputs()->set_now_unix_ns(1710000000123456789LL);
  auto resp = RunMaterialize(req);
  ASSERT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSuccess) << resp.message();
  ASSERT_EQ(resp.replacements_size(), 1);
  EXPECT_EQ(resp.replacements(0).literal_sql(), "toDateTime(1710000000)");
  EXPECT_EQ(resp.sql_after_materialization().find("now("), std::string::npos);
}

// ---- C++ extensions beyond the rewriter-go supported set ----
// generateUUIDv7 reuses the uuid_values channel (symmetric with v4). Go rejects
// it as unsupported; C++ accepts it (explicit divergence, same proto contract).
TEST(Materialize, ExtensionGenerateUUIDv7) {
  auto req = MatReq("SELECT generateUUIDv7()");
  req.mutable_inputs()->add_uuid_values("018f3c8a-3e8a-7d0a-b5b0-6e95e3f0b001");
  auto resp = RunMaterialize(req);
  ASSERT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSuccess) << resp.message();
  ASSERT_EQ(resp.replacements_size(), 1);
  EXPECT_EQ(resp.replacements(0).literal_sql(),
            "toUUID('018f3c8a-3e8a-7d0a-b5b0-6e95e3f0b001')");
  EXPECT_EQ(resp.replacements(0).value_type(), "UUID");
}

TEST(Materialize, ExtensionGenerateUUIDv7ChannelExhaustedFailsClosed) {
  auto resp = RunMaterialize(MatReq("SELECT generateUUIDv7()"));  // no uuid value
  EXPECT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeInvalidRequest);
  EXPECT_EQ(resp.sql_after_materialization(), resp.sql());
  EXPECT_EQ(resp.replacements_size(), 0);  // no partial replacement on error
}

// nowInBlock()/nowInBlock64() alias now()/now64(); randConstant() is UInt32.
TEST(Materialize, ExtensionNowInBlockAndRandConstant) {
  auto req = MatReq("SELECT nowInBlock(), nowInBlock64(), randConstant()");
  req.mutable_inputs()->set_now_unix_ns(1710000000123456789LL);
  req.mutable_inputs()->add_random_uint64_values(5);
  auto resp = RunMaterialize(req);
  ASSERT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSuccess) << resp.message();
  ASSERT_EQ(resp.replacements_size(), 3);
  EXPECT_EQ(resp.replacements(0).literal_sql(), "toDateTime(1710000000)");
  EXPECT_EQ(resp.replacements(0).value_type(), "DateTime");        // nowInBlock
  EXPECT_EQ(resp.replacements(1).literal_sql(), "fromUnixTimestamp64Milli(1710000000123)");
  EXPECT_EQ(resp.replacements(1).value_type(), "DateTime64(3)");   // nowInBlock64
  EXPECT_EQ(resp.replacements(2).literal_sql(), "5");
  EXPECT_EQ(resp.replacements(2).value_type(), "UInt32");
}

// ---- Storage-integrity contract proof (pre-parse, active option only) -------
namespace {

rewriter::StorageIntegrityArgs *AddSIContractOption(
  rewriter::RewriteSQLRequest &req,
  rewriter::StorageIntegrityContractVersion version =
    rewriter::STORAGE_INTEGRITY_CONTRACT_V1) {
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
  (*dyn->mutable_database_map())["db1"] = "phys";
  dyn->add_known_physical_databases("phys");
  auto *si = dyn->mutable_storage_integrity();
  si->set_contract_version(version);
  si->set_read_mode(rewriter::StorageIntegrityArgs::READ_MODE_SAFE);
  auto &table = (*si->mutable_tables())["db1.t"];
  table.set_safe_table("hg_safe.db1__t");
  table.set_unsafe_table("hg_unsafe.db1__t");
  return si;
}

void AddEmptyStaticOption(rewriter::RewriteSQLRequest &req) {
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  opt->mutable_table_name_args()->mutable_static_args();
}

rewriter::RewriteSQLResponse RunDirectRewrite(rewriter::RewriteSQLRequest req) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  EXPECT_TRUE(service.Rewrite(&ctx, &req, &resp).ok());
  return resp;
}

void ExpectRoundTrippedSIRewrite(const rewriter::RewriteSQLRequest &req) {
  const auto &tables = req.options(0)
                         .table_name_args()
                         .dynamic_args()
                         .storage_integrity()
                         .tables();
  ASSERT_NE(tables.find("db1.t"), tables.end());

  RewriterServiceImpl service;
  rewriter::RewriteSQLResponse resp;
  service.doRewrite(&req, &resp);

  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
  EXPECT_EQ(resp.storage_integrity_contract_version(),
            rewriter::STORAGE_INTEGRITY_CONTRACT_V1);
  EXPECT_EQ(resp.sql_after_rewrite(),
            "SELECT * FROM (SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t) AS `db1.t`");
}

} // namespace

TEST(StorageIntegrityContract, BinaryRoundTripPreservesMapAndRewrite) {
  rewriter::RewriteSQLRequest original;
  original.set_sql("SELECT * FROM db1.t");
  AddSIContractOption(original);

  std::string wire;
  ASSERT_TRUE(original.SerializeToString(&wire));
  rewriter::RewriteSQLRequest parsed;
  ASSERT_TRUE(parsed.ParseFromString(wire));

  ExpectRoundTrippedSIRewrite(parsed);
}

TEST(StorageIntegrityContract, JsonRoundTripPreservesMapAndRewrite) {
  rewriter::RewriteSQLRequest original;
  original.set_sql("SELECT * FROM db1.t");
  AddSIContractOption(original);

  std::string json;
  const auto write_status =
    google::protobuf::util::MessageToJsonString(original, &json);
  ASSERT_TRUE(write_status.ok()) << write_status.ToString();
  rewriter::RewriteSQLRequest parsed;
  const auto read_status =
    google::protobuf::util::JsonStringToMessage(json, &parsed);
  ASSERT_TRUE(read_status.ok()) << read_status.ToString();

  ExpectRoundTrippedSIRewrite(parsed);
}

TEST(StorageIntegrityContract, AcknowledgesValidAndSyntaxErrorPaths) {
  for (const std::string sql : {"SELECT 1", "SELECT ("}) {
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    AddSIContractOption(req);
    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.storage_integrity_contract_version(),
              rewriter::STORAGE_INTEGRITY_CONTRACT_V1);
    EXPECT_EQ(resp.code(), sql == "SELECT 1" ? rewriter::RewriteCode::Success
                                             : rewriter::RewriteCode::SyntaxError);
  }
}

TEST(StorageIntegrityContract, RemovedLiveViewPreservesPreflightMetadata) {
  rewriter::RewriteSQLRequest req;
  req.set_sql("CREATE LIVE VIEW analytics.lv AS SELECT * FROM db1.t");
  AddSIContractOption(req);

  const auto resp = RunDirectRewrite(std::move(req));

  EXPECT_EQ(resp.storage_integrity_contract_version(),
            rewriter::STORAGE_INTEGRITY_CONTRACT_V1);
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
  const auto *accessed = FindAccessed(resp, "db1", "t");
  ASSERT_NE(accessed, nullptr);
  EXPECT_TRUE(accessed->is_storage_integrity());
}

TEST(StorageIntegrityContract, RejectsMissingAndUnknownBeforeAcknowledgement) {
  for (const auto version : {
         rewriter::STORAGE_INTEGRITY_CONTRACT_UNSPECIFIED,
         static_cast<rewriter::StorageIntegrityContractVersion>(99)}) {
    rewriter::RewriteSQLRequest req;
    req.set_sql("SELECT 1");
    AddSIContractOption(req, version);
    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
    EXPECT_EQ(resp.storage_integrity_contract_version(),
              rewriter::STORAGE_INTEGRITY_CONTRACT_UNSPECIFIED);
    EXPECT_EQ(resp.sql_after_rewrite(), "SELECT 1");
    EXPECT_EQ(resp.message(), rewriter_handlers::kStorageIntegrityContractMessage);
  }
}

TEST(StorageIntegrityContract, EmptyOrAbsentSurfaceKeepsLegacyZero) {
  rewriter::RewriteSQLRequest absent;
  absent.set_sql("SELECT 1");
  EXPECT_EQ(RunDirectRewrite(std::move(absent)).storage_integrity_contract_version(),
            rewriter::STORAGE_INTEGRITY_CONTRACT_UNSPECIFIED);

  rewriter::RewriteSQLRequest empty;
  empty.set_sql("SELECT 1");
  auto *opt = empty.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  opt->mutable_table_name_args()->mutable_dynamic_args()
    ->mutable_storage_integrity()->set_contract_version(
      rewriter::STORAGE_INTEGRITY_CONTRACT_V1);
  EXPECT_EQ(RunDirectRewrite(std::move(empty)).storage_integrity_contract_version(),
            rewriter::STORAGE_INTEGRITY_CONTRACT_UNSPECIFIED);
}

TEST(StorageIntegrityContract, ActiveTableRewriteSelectionIsLastWins) {
  rewriter::RewriteSQLRequest shadowed;
  shadowed.set_sql("SELECT 1");
  AddSIContractOption(shadowed, rewriter::STORAGE_INTEGRITY_CONTRACT_UNSPECIFIED);
  AddEmptyStaticOption(shadowed);
  auto resp = RunDirectRewrite(std::move(shadowed));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.storage_integrity_contract_version(),
            rewriter::STORAGE_INTEGRITY_CONTRACT_UNSPECIFIED);

  rewriter::RewriteSQLRequest active;
  active.set_sql("SELECT 1");
  AddEmptyStaticOption(active);
  AddSIContractOption(active);
  resp = RunDirectRewrite(std::move(active));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.storage_integrity_contract_version(),
            rewriter::STORAGE_INTEGRITY_CONTRACT_V1);
}

TEST(StorageIntegrityContract, DBLevelUsesDynamicArgsButAcknowledgesOnlyActiveSI) {
  rewriter::RewriteSQLRequest shadowed;
  shadowed.set_sql("USE db1");
  AddSIContractOption(shadowed);
  AddEmptyStaticOption(shadowed);
  auto resp = RunDirectRewrite(std::move(shadowed));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.sql_after_rewrite(), "USE phys");
  EXPECT_EQ(resp.storage_integrity_contract_version(),
            rewriter::STORAGE_INTEGRITY_CONTRACT_UNSPECIFIED);

  rewriter::RewriteSQLRequest active;
  active.set_sql("USE db1");
  AddEmptyStaticOption(active);
  AddSIContractOption(active);
  resp = RunDirectRewrite(std::move(active));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.sql_after_rewrite(), "USE phys");
  EXPECT_EQ(resp.storage_integrity_contract_version(),
            rewriter::STORAGE_INTEGRITY_CONTRACT_V1);
}

TEST(StorageIntegrityCatchAll, UnmodelledStatementIsRefusedWithoutStorageIntegrity) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql("SYSTEM RELOAD CONFIG");
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
  (*dyn->mutable_database_map())["db1"] = "phys";
  dyn->add_known_physical_databases("phys");
  dyn->set_delim("_");

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  ASSERT_TRUE(service.Rewrite(&ctx, &req, &resp).ok());
  // Spec 2026-09-26 T7: an unmodelled class is refused in dynamic mode
  // whatever the SI state.
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
  EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
  EXPECT_EQ(resp.sql_after_rewrite(), "SYSTEM RELOAD CONFIG");
  EXPECT_EQ(resp.storage_integrity_contract_version(),
            rewriter::STORAGE_INTEGRITY_CONTRACT_UNSPECIFIED);
}

// ---- Storage-integrity contract V2 (dynamic table set, housegate sub-project 3)
namespace {

// Probe-shaped V2 request: db1 and other map to phys; the SI table map carries
// db1.t or is empty (H6).
rewriter::RewriteTableDynamicArgs *AddV2Option(
  rewriter::RewriteSQLRequest &req,
  rewriter::StorageIntegrityContractVersion version,
  bool with_table) {
  auto *si = AddSIContractOption(req, version);
  if (!with_table) si->mutable_tables()->clear();
  si->set_reserved_row_id_column("_hg_row_id");
  auto *dyn = req.mutable_options(req.options_size() - 1)
                ->mutable_table_name_args()->mutable_dynamic_args();
  (*dyn->mutable_database_map())["other"] = "phys";
  dyn->set_delim("_");
  return dyn;
}

}  // namespace

TEST(StorageIntegrityContractV2, EmptyMapActivatesCatchAll) {
  for (const char *sql : {"SYSTEM RELOAD CONFIG", "SET max_threads = 1"}) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    AddV2Option(req, rewriter::STORAGE_INTEGRITY_CONTRACT_V2, false);
    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
    EXPECT_EQ(resp.message(), rewriter_handlers::kStorageIntegrityUnmodelledMessage);
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    EXPECT_EQ(resp.storage_integrity_contract_version(),
              rewriter::STORAGE_INTEGRITY_CONTRACT_V2);
  }
}

TEST(StorageIntegrityContractV2, V1EmptyMapStaysLegacy) {
  // The inactive surface: no acknowledgement, the inactive-surface T7 text,
  // and SET as the one modelled pass-through (spec 2026-09-26 T7).
  rewriter::RewriteSQLRequest req;
  req.set_sql("SYSTEM RELOAD CONFIG");
  AddV2Option(req, rewriter::STORAGE_INTEGRITY_CONTRACT_V1, false);
  const auto resp = RunDirectRewrite(std::move(req));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
  EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
  EXPECT_EQ(resp.storage_integrity_contract_version(),
            rewriter::STORAGE_INTEGRITY_CONTRACT_UNSPECIFIED);

  rewriter::RewriteSQLRequest set;
  set.set_sql("SET max_threads = 1");
  AddV2Option(set, rewriter::STORAGE_INTEGRITY_CONTRACT_V1, false);
  const auto set_resp = RunDirectRewrite(std::move(set));
  EXPECT_EQ(set_resp.code(), rewriter::RewriteCode::Success) << set_resp.message();
  EXPECT_EQ(set_resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
  EXPECT_EQ(set_resp.sql_after_rewrite(), "SET max_threads = 1");
}

TEST(StorageIntegrityContractV2, AcknowledgesV2OnEveryPath) {
  struct Case {
    const char *sql;
    bool with_table;
    rewriter::RewriteCode code;
  };
  const std::vector<Case> cases = {
    {"SELECT a FROM db1.t", true, rewriter::RewriteCode::Success},
    {"SELECT a FROM db1.t", false, rewriter::RewriteCode::Success},
    {"TRUNCATE TABLE db1.t", true, rewriter::RewriteCode::UnsupportedStatement},
    {"SELECT (", false, rewriter::RewriteCode::SyntaxError},
  };
  for (const auto &c : cases) {
    SCOPED_TRACE(c.sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(c.sql);
    AddV2Option(req, rewriter::STORAGE_INTEGRITY_CONTRACT_V2, c.with_table);
    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), c.code) << resp.message();
    EXPECT_EQ(resp.storage_integrity_contract_version(),
              rewriter::STORAGE_INTEGRITY_CONTRACT_V2);
  }
}

TEST(StorageIntegrityContractV2, DropTableSucceeds) {
  rewriter::RewriteSQLRequest req;
  req.set_sql("DROP TABLE db1.t, other.u");
  AddV2Option(req, rewriter::STORAGE_INTEGRITY_CONTRACT_V2, true);
  const auto resp = RunDirectRewrite(std::move(req));
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_DROP_TABLE);
  EXPECT_EQ(resp.sql_after_rewrite(), "DROP TABLE phys.`db1.t`, phys.`other.u`");
  EXPECT_EQ(resp.storage_integrity_contract_version(),
            rewriter::STORAGE_INTEGRITY_CONTRACT_V2);
  const auto *si = FindAccessed(resp, "db1", "t");
  ASSERT_NE(si, nullptr);
  EXPECT_TRUE(si->is_storage_integrity());
  EXPECT_EQ(si->physical_database(), "phys");
  const auto *ordinary = FindAccessed(resp, "other", "u");
  ASSERT_NE(ordinary, nullptr);
  EXPECT_FALSE(ordinary->is_storage_integrity());
  const auto si_rewrite = resp.table_rewrites().find("db1.t");
  ASSERT_NE(si_rewrite, resp.table_rewrites().end());
  EXPECT_EQ(si_rewrite->second, "phys.db1.t");
  const auto ordinary_rewrite = resp.table_rewrites().find("other.u");
  ASSERT_NE(ordinary_rewrite, resp.table_rewrites().end());
  EXPECT_EQ(ordinary_rewrite->second, "phys.other.u");
}

TEST(StorageIntegrityContractV2, DropExemptionRequiresPlainGrammar) {
  const std::string signed_lane =
    "storage-integrity table db1.t accepts writes only through the signed statement lane";
  struct Case {
    const char *sql;
    bool session_db1;
    std::string message;
    rewriter::RewriteCode code = rewriter::RewriteCode::UnsupportedStatement;
  };
  const std::vector<Case> cases = {
    {"DROP TABLE db1.t ON CLUSTER c", false, signed_lane},
    {"DROP TABLE IF EMPTY db1.t", false, signed_lane},
    {"DROP TEMPORARY TABLE t", true, signed_lane},
    {"DROP VIEW db1.t", false, signed_lane},
    {"DROP TABLE other.u, other.v ON CLUSTER c", false,
      "multi-table DROP/TRUNCATE is not supported"},
    {"DROP TABLE other.u, {p:Identifier} UUID '00000000-0000-0000-0000-000000000001'",
      false, std::string(rewriter_handlers::kIdentifierParameterMessage),
      rewriter::RewriteCode::InvalidRewriteRequest},
  };
  for (const auto &c : cases) {
    SCOPED_TRACE(c.sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(c.sql);
    auto *dyn = AddV2Option(req, rewriter::STORAGE_INTEGRITY_CONTRACT_V2, true);
    if (c.session_db1) dyn->set_upstream_logical_database_in_context("db1");
    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), c.code);
    EXPECT_EQ(resp.message(), c.message);
    EXPECT_EQ(resp.sql_after_rewrite(), c.sql);
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    EXPECT_EQ(resp.storage_integrity_contract_version(),
              rewriter::STORAGE_INTEGRITY_CONTRACT_V2);
  }
}

// A query-parameter write target parses to an empty table name; rewriting it
// erased the target, so `DROP TABLE db1.{p:Identifier}` became
// `DROP DATABASE phys`. It must fail closed in every dynamic mode.
TEST(WriteTargetQueryParameter, RejectedBeforeRewriteInEveryMode) {
  enum class Mode { DynamicNoSI, V1, V2 };
  struct Case {
    const char *sql;
    bool session_db1;
  };
  const std::vector<Case> cases = {
    {"DROP TABLE db1.{p:Identifier}", false},
    {"DROP TABLE {p:Identifier}", true},
    {"TRUNCATE TABLE db1.{p:Identifier}", false},
    {"DROP VIEW db1.{p:Identifier}", false},
    {"DROP TABLE hg_safe.{p:Identifier}", false},
  };
  for (const Mode mode : {Mode::DynamicNoSI, Mode::V1, Mode::V2}) {
    for (const auto &c : cases) {
      SCOPED_TRACE(std::string(c.sql) + " mode=" + std::to_string(static_cast<int>(mode)));
      rewriter::RewriteSQLRequest req;
      req.set_sql(c.sql);
      auto *dyn = AddV2Option(req,
        mode == Mode::V1 ? rewriter::STORAGE_INTEGRITY_CONTRACT_V1
                         : rewriter::STORAGE_INTEGRITY_CONTRACT_V2,
        true);
      if (mode == Mode::DynamicNoSI) dyn->clear_storage_integrity();
      if (c.session_db1) dyn->set_upstream_logical_database_in_context("db1");
      const auto resp = RunDirectRewrite(std::move(req));
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
      // Under an active SI surface the reject annotator names hg_safe for
      // the hg_safe row; every other row keeps the parameter message.
      const bool annotated = mode != Mode::DynamicNoSI
        && std::string(c.sql).find("hg_safe") != std::string::npos;
      if (!annotated) {
        EXPECT_EQ(resp.message(), rewriter_handlers::kIdentifierParameterMessage);
      }
      EXPECT_EQ(resp.sql_after_rewrite(), c.sql);
      EXPECT_EQ(resp.sql_after_rewrite().find("DATABASE"), std::string::npos);
      EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
      EXPECT_EQ(resp.storage_integrity_contract_version(),
        mode == Mode::DynamicNoSI ? rewriter::STORAGE_INTEGRITY_CONTRACT_UNSPECIFIED
        : mode == Mode::V1        ? rewriter::STORAGE_INTEGRITY_CONTRACT_V1
                                  : rewriter::STORAGE_INTEGRITY_CONTRACT_V2);
    }
  }
}

TEST(StorageIntegrityContractV2, StaticSelectionShadowsDropExemption) {
  rewriter::RewriteSQLRequest req;
  req.set_sql("DROP TABLE db1.t, other.u");
  AddV2Option(req, rewriter::STORAGE_INTEGRITY_CONTRACT_V2, true);
  AddEmptyStaticOption(req);
  const auto resp = RunDirectRewrite(std::move(req));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_EQ(resp.message(), "multi-table DROP/TRUNCATE is not supported");
  EXPECT_EQ(resp.storage_integrity_contract_version(),
            rewriter::STORAGE_INTEGRITY_CONTRACT_UNSPECIFIED);
}

TEST(StorageIntegrityContractV2, DropRequiresAuthorizedLogicalDatabase) {
  rewriter::RewriteSQLRequest req;
  req.set_sql("DROP TABLE db1.t");
  auto *dyn = AddV2Option(req, rewriter::STORAGE_INTEGRITY_CONTRACT_V2, true);
  dyn->mutable_database_map()->erase("db1");
  const auto resp = RunDirectRewrite(std::move(req));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
  EXPECT_EQ(resp.message(),
            "storage-integrity logical database db1 is not authorized by database_map");
}

// ---- Storage-integrity contract V2: reserved_databases -----------------------
namespace {

// V1 learns its protected namespace from the table map: name every reserved
// database through a map entry.
rewriter::RewriteTableDynamicArgs *AddReservedV1Equivalent(rewriter::RewriteSQLRequest &req) {
  auto *dyn = AddV2Option(req, rewriter::STORAGE_INTEGRITY_CONTRACT_V1, true);
  auto &promote = (*dyn->mutable_storage_integrity()->mutable_tables())["db1.p"];
  promote.set_safe_table("hg_promote.db1__p");
  promote.set_unsafe_table("hg_unsafe.db1__p");
  return dyn;
}

rewriter::RewriteTableDynamicArgs *AddReservedV2(rewriter::RewriteSQLRequest &req, bool with_table) {
  auto *dyn = AddV2Option(req, rewriter::STORAGE_INTEGRITY_CONTRACT_V2, with_table);
  for (const char *database : {"hg_safe", "hg_unsafe", "hg_promote"})
    dyn->mutable_storage_integrity()->add_reserved_databases(database);
  return dyn;
}

const std::vector<std::string> kReservedNamespaceSQL = {
  "SELECT * FROM hg_safe.x",
  "SELECT * FROM hg_safe.db1__t",
  "INSERT INTO hg_unsafe.x VALUES (1)",
  "INSERT INTO hg_unsafe.x SELECT 1",
  "TRUNCATE TABLE hg_promote.x",
  "DROP TABLE hg_safe.x",
  "TRUNCATE DATABASE hg_safe",
  "SELECT * FROM merge($tag$hg_safe$tag$, 'x')",
  "USE hg_unsafe",
  "SHOW TABLES FROM hg_promote",
  "SYSTEM START MERGES hg_unsafe.x",
};

}  // namespace

TEST(StorageIntegrityReservedDatabases, V2EmptyMapMatchesV1Protection) {
  for (const auto &sql : kReservedNamespaceSQL) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest v1;
    v1.set_sql(sql);
    AddReservedV1Equivalent(v1);
    auto want = RunDirectRewrite(std::move(v1));
    rewriter::RewriteSQLRequest v2;
    v2.set_sql(sql);
    AddReservedV2(v2, false);
    const auto got = RunDirectRewrite(std::move(v2));
    EXPECT_NE(got.code(), rewriter::RewriteCode::Success) << got.sql_after_rewrite();
    EXPECT_EQ(got.storage_integrity_contract_version(), rewriter::STORAGE_INTEGRITY_CONTRACT_V2);
    want.set_storage_integrity_contract_version(rewriter::STORAGE_INTEGRITY_CONTRACT_V2);
    EXPECT_EQ(got.DebugString(), want.DebugString());
  }
}

TEST(StorageIntegrityReservedDatabases, UnionWithTableMap) {
  const std::vector<std::pair<std::string, std::string>> cases = {
    {"SELECT * FROM safe_x.y", "storage-integrity physical table safe_x.y is not directly addressable"},
    {"SELECT * FROM hg_safe.y", "storage-integrity physical table hg_safe.y is not directly addressable"},
  };
  for (const auto &[sql, message] : cases) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    auto *dyn = AddReservedV2(req, false);
    auto &table = (*dyn->mutable_storage_integrity()->mutable_tables())["db1.t"];
    table.set_safe_table("safe_x.db1__t");
    table.set_unsafe_table("unsafe_x.db1__t");
    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::RewriteError);
    EXPECT_EQ(resp.message(), message);
  }
}

TEST(StorageIntegrityReservedDatabases, V1IgnoresReservedDatabases) {
  for (const bool with_table : {false, true}) {
    for (const auto &sql : kReservedNamespaceSQL) {
      SCOPED_TRACE(sql);
      rewriter::RewriteSQLRequest plain;
      plain.set_sql(sql);
      AddV2Option(plain, rewriter::STORAGE_INTEGRITY_CONTRACT_V1, with_table);
      rewriter::RewriteSQLRequest reserved;
      reserved.set_sql(sql);
      auto *dyn = AddV2Option(reserved, rewriter::STORAGE_INTEGRITY_CONTRACT_V1, with_table);
      for (const char *database : {"not a name", "hg_safe", "hg_unsafe", "hg_promote"})
        dyn->mutable_storage_integrity()->add_reserved_databases(database);
      EXPECT_EQ(RunDirectRewrite(std::move(reserved)).DebugString(),
                RunDirectRewrite(std::move(plain)).DebugString());
    }
  }
}

TEST(StorageIntegrityReservedDatabases, RejectsMalformedEntryUnderV2) {
  rewriter::RewriteSQLRequest req;
  req.set_sql("SELECT 1");
  auto *dyn = AddV2Option(req, rewriter::STORAGE_INTEGRITY_CONTRACT_V2, false);
  dyn->mutable_storage_integrity()->add_reserved_databases("hg_safe");
  dyn->mutable_storage_integrity()->add_reserved_databases("hg-unsafe");
  const auto resp = RunDirectRewrite(std::move(req));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
  EXPECT_EQ(resp.message(),
            "storage-integrity reserved_databases entry \"hg-unsafe\" must be a simple identifier");
  EXPECT_EQ(resp.storage_integrity_contract_version(),
            rewriter::STORAGE_INTEGRITY_CONTRACT_UNSPECIFIED);
}

TEST(StorageIntegrityCatchAll, StructuredSystemTargetsSkipClusterDecoys) {
  struct Case {
    const char *sql;
    const char *message;
  };
  const std::vector<Case> cases = {
    {"SYSTEM STOP PULLING REPLICATION LOG ON CLUSTER hg_safe hg_unsafe.db1__t",
      "storage-integrity physical table hg_unsafe.db1__t is not directly addressable"},
    {"SYSTEM RELOAD CONFIG ON CLUSTER hg_safe",
      "storage-integrity is configured; statement class is not modelled by the rewriter and cannot be forwarded"},
  };

  for (const auto &c : cases) {
    SCOPED_TRACE(c.sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(c.sql);
    AddSIContractOption(req);

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    EXPECT_EQ(resp.sql_after_rewrite(), c.sql);
    EXPECT_EQ(resp.message(), c.message);
  }
}

TEST(StorageIntegrityCatchAll, SyntaxErrorMessageIsNeverLexicallyAnnotated) {
  constexpr const char *sql = "SELECT * FROM hg_safe.db1__t WHERE (";

  rewriter::RewriteSQLRequest plain;
  plain.set_sql(sql);
  const auto plain_resp = RunDirectRewrite(std::move(plain));
  ASSERT_EQ(plain_resp.code(), rewriter::RewriteCode::SyntaxError);

  rewriter::RewriteSQLRequest protected_req;
  protected_req.set_sql(sql);
  AddSIContractOption(protected_req);
  const auto protected_resp = RunDirectRewrite(std::move(protected_req));
  EXPECT_EQ(protected_resp.code(), rewriter::RewriteCode::SyntaxError);
  EXPECT_EQ(protected_resp.message(), plain_resp.message());
  EXPECT_EQ(protected_resp.sql_after_rewrite(), "Error parsing query");
}

TEST(StorageIntegrityShowDictionaries, GuardsDatabaseBeforePassthrough) {
  struct Case {
    const char *sql;
    rewriter::RewriteCode code;
    const char *message;
    const char *original_database;
    const char *logical_database;
    const char *physical_database;
  };
  const std::vector<Case> cases = {
    {"SHOW DICTIONARIES FROM hg_safe", rewriter::RewriteCode::UnsupportedStatement,
      "storage-integrity physical database hg_safe is not directly addressable",
      "hg_safe", "", "hg_safe"},
    {"SHOW DICTIONARIES FROM db1", rewriter::RewriteCode::UnsupportedStatement,
      "storage-integrity logical database db1 is not directly addressable",
      "db1", "db1", "phys"},
    // An ordinary database passes the namespace guard and then reads
    // system.dictionaries, outside the system-table allowlist.
    {"SHOW DICTIONARIES FROM other", rewriter::RewriteCode::UnsupportedStatement,
      "system table system.dictionaries is not accessible", "", "", ""},
  };

  for (const auto &c : cases) {
    SCOPED_TRACE(c.sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(c.sql);
    AddSIContractOption(req);
    auto *dyn = req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
    (*dyn->mutable_database_map())["other"] = "phys";

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), c.code) << resp.message();
    EXPECT_EQ(resp.message(), c.message);
    EXPECT_EQ(resp.sql_after_rewrite(), c.sql);
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    if (std::string(c.original_database).empty()) {
      EXPECT_EQ(resp.original_accessed_tables_size(), 0);
      continue;
    }
    ASSERT_EQ(resp.original_accessed_tables_size(), 1);
    const auto &access = resp.original_accessed_tables(0);
    EXPECT_EQ(access.original_database(), c.original_database);
    EXPECT_TRUE(access.original_table().empty());
    EXPECT_EQ(access.logical_database(), c.logical_database);
    EXPECT_EQ(access.physical_database(), c.physical_database);
    EXPECT_TRUE(access.is_storage_integrity());
    EXPECT_FALSE(access.is_remote());
  }
}

// ClickHouse spells this family with more keywords than polyglot does: FIELDS is
// a synonym of COLUMNS and INDICES joins INDEX / INDEXES / KEYS. rewriter-go
// classifies those two spellings as unknown SHOW kinds and refuses them through
// the Spec I D1 catch-all, so both engines fail closed but with different
// messages and the shared corpus cannot pin them. Keep the C++ behaviour pinned
// here instead.
//
// Pre-fix (measured on the unmodified tree with the CLI): both answered
// UnsupportedStatement with "storage-integrity physical database hg_safe is not
// directly addressable" — the Spec I D2 annotator naming only the database,
// after ASTShowColumnsQuery / ASTShowIndexesQuery fell through every handler to
// rejectUnmodelledStorageIntegrityStatement. The pair message below is new.
TEST(StorageIntegrityShowColumns, ClickHouseOnlySpellingsAreGatedAsThePair) {
  for (const char *sql : {"SHOW FIELDS FROM db1__t FROM hg_safe",
                          "SHOW INDICES FROM db1__t FROM hg_safe"}) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    AddSIContractOption(req);

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(),
      "storage-integrity physical table hg_safe.db1__t is not directly addressable");
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
    ASSERT_EQ(resp.original_accessed_tables_size(), 1);
    EXPECT_EQ(resp.original_accessed_tables(0).original_database(), "hg_safe");
    EXPECT_EQ(resp.original_accessed_tables(0).original_table(), "db1__t");
    EXPECT_TRUE(resp.original_accessed_tables(0).is_storage_integrity());
  }
}

// An ordinary namespace passes through with the ORIGINAL text, not formatAst.
// ASTShowIndexesQuery's formatter canonicalizes SHOW INDEX / KEYS to
// SHOW INDEXES and IN to FROM, which would make the two engines disagree on
// every non-canonical spelling.
//
// Pre-fix: Success but STATEMENT_TYPE_SELECT with the canonicalized SQL
// "SHOW INDEXES FROM u FROM other", because the statement reached the SELECT
// dispatch tail.
TEST(StorageIntegrityShowColumns, OrdinaryNamespacePassesThroughVerbatim) {
  for (const char *sql : {"SHOW INDEX FROM other.u", "SHOW FULL COLUMNS FROM u IN other"}) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    AddSIContractOption(req);
    auto *dyn = req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
    (*dyn->mutable_database_map())["other"] = "phys";

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_SHOW_TABLES);
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
    EXPECT_EQ(resp.original_accessed_tables_size(), 0);
  }
}

// Two inputs where ClickHouse's parser is strictly narrower than polyglot's, so
// the statement never reaches a handler and the engines answer different codes.
// Both refuse — this is a fail-closed divergence, not a bypass — but the corpus
// schema carries one want_code per case, so neither can be pinned there (plan
// deviation D-4). This test is the C++ half of that record.
//
//   SHOW SOMETHINGNEW FROM hg_safe            rewriter-go: UnsupportedStatement
//     (ParserShowTablesQuery takes a fixed keyword set; polyglot reports
//      ShowWhat=SOMETHINGNEW and the Go classifier refuses it as unknown)
//   SHOW COLUMNS FROM db1__t FROM {db:Ident}  rewriter-go: UnsupportedStatement
//     (ParserShowColumnsQuery parses the second clause with ParserIdentifier()
//      and never allows a query parameter)
//
// Neither is a new guard: both already answered SyntaxError before this change.
TEST(StorageIntegrityShowColumns, ParserNarrowerThanPolyglotStillFailsClosed) {
  for (const char *sql : {"SHOW SOMETHINGNEW FROM hg_safe",
                          "SHOW COLUMNS FROM db1__t FROM {db:Identifier}",
                          "SHOW COLUMNS FROM {t:Identifier} FROM hg_safe"}) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    AddSIContractOption(req);

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::SyntaxError) << resp.message();
  }
}

TEST(StorageIntegrityShowPrefixes, LegalPrefixesPreserveTargetSemantics) {
  const std::vector<std::string> prefixes = {
    "FULL ",
    "TEMPORARY ",
    "FULL TEMPORARY ",
  };

  auto rewrite = [](const std::string &sql) {
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    AddSIContractOption(req);
    auto *dyn = req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
    (*dyn->mutable_database_map())["other"] = "phys";
    return RunDirectRewrite(std::move(req));
  };

  const auto logical_tables_baseline = rewrite("SHOW TABLES FROM db1");
  ASSERT_EQ(logical_tables_baseline.code(), rewriter::RewriteCode::Success)
    << logical_tables_baseline.message();
  const auto ordinary_tables_baseline = rewrite("SHOW TABLES FROM other");
  ASSERT_EQ(ordinary_tables_baseline.code(), rewriter::RewriteCode::Success)
    << ordinary_tables_baseline.message();

  for (const auto &prefix : prefixes) {
    {
      const std::string sql = "SHOW " + prefix + "TABLES FROM hg_safe";
      SCOPED_TRACE(sql);
      const auto resp = rewrite(sql);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
      EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
      EXPECT_EQ(resp.message(),
        "storage-integrity physical database hg_safe is not directly addressable");
      ASSERT_EQ(resp.original_accessed_tables_size(), 1);
      EXPECT_EQ(resp.original_accessed_tables(0).original_database(), "hg_safe");
      EXPECT_EQ(resp.original_accessed_tables(0).physical_database(), "hg_safe");
      EXPECT_TRUE(resp.original_accessed_tables(0).is_storage_integrity());
    }
    // The SI namespace refusal above names the database first; a resolvable
    // target then reaches the synthetic enumeration, which models neither
    // FULL nor TEMPORARY: refused rather than answered as SHOW TABLES (Task
    // I3, rewriter-go dispatchShowTables).
    for (const std::string target : {"db1", "other"}) {
      const std::string sql = "SHOW " + prefix + "TABLES FROM " + target;
      SCOPED_TRACE(sql);
      const auto resp = rewrite(sql);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
      EXPECT_EQ(resp.database_rewrites_size(), 0);
    }
    {
      const std::string sql = "SHOW " + prefix + "DICTIONARIES FROM hg_safe";
      SCOPED_TRACE(sql);
      const auto resp = rewrite(sql);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
      EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
      EXPECT_EQ(resp.message(),
        "storage-integrity physical database hg_safe is not directly addressable");
      ASSERT_EQ(resp.original_accessed_tables_size(), 1);
      EXPECT_EQ(resp.original_accessed_tables(0).original_database(), "hg_safe");
      EXPECT_EQ(resp.original_accessed_tables(0).physical_database(), "hg_safe");
      EXPECT_TRUE(resp.original_accessed_tables(0).is_storage_integrity());
    }
    {
      const std::string sql = "SHOW " + prefix + "DICTIONARIES FROM db1";
      SCOPED_TRACE(sql);
      const auto resp = rewrite(sql);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
      EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
      EXPECT_EQ(resp.message(),
        "storage-integrity logical database db1 is not directly addressable");
      ASSERT_EQ(resp.original_accessed_tables_size(), 1);
      EXPECT_EQ(resp.original_accessed_tables(0).original_database(), "db1");
      EXPECT_EQ(resp.original_accessed_tables(0).logical_database(), "db1");
      EXPECT_EQ(resp.original_accessed_tables(0).physical_database(), "phys");
      EXPECT_TRUE(resp.original_accessed_tables(0).is_storage_integrity());
    }
    {
      const std::string sql = "SHOW " + prefix + "DICTIONARIES FROM other";
      SCOPED_TRACE(sql);
      const auto resp = rewrite(sql);
      // Every prefix reaches the same form: system.dictionaries, outside the
      // system-table allowlist.
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), "system table system.dictionaries is not accessible");
      EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
      EXPECT_EQ(resp.original_accessed_tables_size(), 0);
    }
  }
}

TEST(StorageIntegrityShowPrefixes, KeywordDatabaseTargetsUseStructuredAST) {
  const std::vector<std::string> databases = {
    "system", "default", "select", "from", "table", "settings",
  };

  for (const auto &database : databases) {
    for (const std::string prefix : {"", "FULL TEMPORARY "}) {
      const std::string sql = "SHOW " + prefix + "TABLES FROM " + database;
      SCOPED_TRACE(sql);
      rewriter::RewriteSQLRequest req;
      req.set_sql(sql);
      AddSIContractOption(req);
      auto *dyn = req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
      (*dyn->mutable_database_map())[database] = "phys";

      const auto resp = RunDirectRewrite(std::move(req));
      if (!prefix.empty()) {
        // The keyword database resolves structurally; the synthetic
        // enumeration then refuses FULL / TEMPORARY (Task I3).
        EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
        EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
        continue;
      }
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_SHOW_TABLES);
      EXPECT_NE(resp.sql_after_rewrite().find("database = 'phys'"),
                std::string::npos);
      EXPECT_NE(resp.sql_after_rewrite().find(
                  "startsWith(name, '" + database + ".')"),
                std::string::npos);
      ASSERT_EQ(resp.database_rewrites_size(), 1);
      EXPECT_EQ(resp.database_rewrites().at(database), "phys");
    }
    {
      const std::string sql =
        "SHOW FULL TEMPORARY DICTIONARIES FROM " + database;
      SCOPED_TRACE(sql);
      rewriter::RewriteSQLRequest req;
      req.set_sql(sql);
      AddSIContractOption(req);
      auto *dyn = req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
      (*dyn->mutable_database_map())[database] = "phys";

      const auto resp = RunDirectRewrite(std::move(req));
      // The keyword database is read structurally (no namespace refusal);
      // the form then reads system.dictionaries.
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), "system table system.dictionaries is not accessible");
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
      EXPECT_EQ(resp.original_accessed_tables_size(), 0);
    }
  }
}

TEST(StorageIntegrityShowPrefixes, ReversePrefixOrderRemainsSyntaxError) {
  const std::vector<std::string> cases = {
    "SHOW TEMPORARY FULL TABLES FROM hg_safe",
    "SHOW TEMPORARY FULL DICTIONARIES FROM hg_safe",
  };

  for (const auto &sql : cases) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest plain;
    plain.set_sql(sql);
    const auto plain_resp = RunDirectRewrite(std::move(plain));
    ASSERT_EQ(plain_resp.code(), rewriter::RewriteCode::SyntaxError);

    rewriter::RewriteSQLRequest protected_req;
    protected_req.set_sql(sql);
    AddSIContractOption(protected_req);
    const auto protected_resp = RunDirectRewrite(std::move(protected_req));
    EXPECT_EQ(protected_resp.code(), rewriter::RewriteCode::SyntaxError);
    EXPECT_EQ(protected_resp.message(), plain_resp.message());
    EXPECT_EQ(protected_resp.sql_after_rewrite(), "Error parsing query");
  }
}

TEST(StorageIntegrityShowNamespaces, UnresolvedExplicitDatabaseFailsClosed) {
  struct Case {
    const char *sql;
    const char *message;
  };
  // Spec 2026-09-26 T2 names the outcome before the SI SHOW guard: an
  // Identifier parameter is refused in every database position.
  const std::vector<Case> cases = {
    {"SHOW DICTIONARIES FROM {db:Identifier}", rewriter_handlers::kIdentifierParameterMessage.data()},
    {"SHOW TABLES FROM {db:Identifier}", rewriter_handlers::kIdentifierParameterMessage.data()},
  };

  for (const auto &c : cases) {
    SCOPED_TRACE(c.sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(c.sql);
    AddSIContractOption(req);
    req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args()
      ->set_upstream_logical_database_in_context("db1");

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    EXPECT_EQ(resp.sql_after_rewrite(), c.sql);
    EXPECT_EQ(resp.message(), c.message);
    EXPECT_EQ(resp.original_accessed_tables_size(), 0);
    EXPECT_EQ(resp.database_rewrites_size(), 0);
  }
}

TEST(StorageIntegrityShowNamespaces, EmptyStorageIntegritySurfaceKeepsDictionaryPassthrough) {
  const std::vector<std::string> cases = {
    "SHOW DICTIONARIES FROM hg_safe",
    "SHOW FULL DICTIONARIES FROM hg_safe",
    "SHOW TEMPORARY DICTIONARIES FROM hg_safe",
    "SHOW FULL TEMPORARY DICTIONARIES FROM hg_safe",
  };
  for (const auto &sql : cases) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    auto *opt = req.add_options();
    opt->set_op(rewriter::RewriteOp::TableNameRewrite);
    auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
    (*dyn->mutable_database_map())["db1"] = "phys";
    dyn->set_upstream_logical_database_in_context("db1");
    dyn->mutable_storage_integrity()->set_contract_version(
      rewriter::STORAGE_INTEGRITY_CONTRACT_V1);

    const auto resp = RunDirectRewrite(std::move(req));
    // No SI namespace refusal with the surface inactive; the form reads
    // system.dictionaries, outside the system-table allowlist.
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), "system table system.dictionaries is not accessible");
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
    EXPECT_EQ(resp.storage_integrity_contract_version(),
              rewriter::STORAGE_INTEGRITY_CONTRACT_UNSPECIFIED);
    EXPECT_EQ(resp.original_accessed_tables_size(), 0);
  }
}

TEST(StorageIntegrityShowNamespaces, ContextNamesAreAlreadySemantic) {
  constexpr const char *literal_context = "hg\\x5Fsafe";

  rewriter::RewriteSQLRequest dictionaries;
  dictionaries.set_sql("SHOW DICTIONARIES");
  AddSIContractOption(dictionaries);
  dictionaries.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args()
    ->set_upstream_logical_database_in_context(literal_context);
  const auto dictionaries_resp = RunDirectRewrite(std::move(dictionaries));
  // Not an SI namespace: the guard passes and the system-table allowlist
  // answers.
  EXPECT_EQ(dictionaries_resp.code(), rewriter::RewriteCode::UnsupportedStatement)
    << dictionaries_resp.message();
  EXPECT_EQ(dictionaries_resp.message(), "system table system.dictionaries is not accessible");
  EXPECT_EQ(dictionaries_resp.sql_after_rewrite(), "SHOW DICTIONARIES");
  EXPECT_EQ(dictionaries_resp.original_accessed_tables_size(), 0);

  rewriter::RewriteSQLRequest tables;
  tables.set_sql("SHOW TABLES");
  AddSIContractOption(tables);
  auto *dyn = tables.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
  dyn->set_upstream_logical_database_in_context(literal_context);
  (*dyn->mutable_database_map())[literal_context] = "phys";
  const auto tables_resp = RunDirectRewrite(std::move(tables));
  EXPECT_EQ(tables_resp.code(), rewriter::RewriteCode::Success)
    << tables_resp.message();
  ASSERT_EQ(tables_resp.database_rewrites_size(), 1);
  EXPECT_EQ(tables_resp.database_rewrites().at(literal_context), "phys");
  EXPECT_EQ(tables_resp.original_accessed_tables_size(), 0);
}

TEST(StorageIntegrityShowDictionaries, PhysicalContextGuardPrecedesLogicalContext) {
  struct Case {
    const char *physical_context;
    const char *logical_context;
  };
  const std::vector<Case> cases = {
    {"hg_safe", ""},
    {"hg_unsafe", "other"},
  };

  for (const auto &c : cases) {
    SCOPED_TRACE(c.physical_context);
    rewriter::RewriteSQLRequest req;
    req.set_sql("SHOW DICTIONARIES");
    AddSIContractOption(req);
    auto *dyn = req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
    dyn->set_upstream_physical_database_in_context(c.physical_context);
    dyn->set_upstream_logical_database_in_context(c.logical_context);

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    EXPECT_EQ(resp.sql_after_rewrite(), "SHOW DICTIONARIES");
    EXPECT_EQ(resp.message(),
      std::string("storage-integrity physical database ") + c.physical_context
        + " is not directly addressable");
    ASSERT_EQ(resp.original_accessed_tables_size(), 1);
    const auto &access = resp.original_accessed_tables(0);
    EXPECT_EQ(access.original_database(), c.physical_context);
    EXPECT_EQ(access.physical_database(), c.physical_context);
    EXPECT_TRUE(access.is_storage_integrity());
  }

  // An explicit ordinary database wins over both session contexts.
  rewriter::RewriteSQLRequest explicit_ordinary;
  explicit_ordinary.set_sql("SHOW DICTIONARIES FROM other");
  AddSIContractOption(explicit_ordinary);
  auto *explicit_dyn = explicit_ordinary.mutable_options(0)
    ->mutable_table_name_args()->mutable_dynamic_args();
  explicit_dyn->set_upstream_physical_database_in_context("hg_safe");
  explicit_dyn->set_upstream_logical_database_in_context("db1");
  const auto explicit_resp = RunDirectRewrite(std::move(explicit_ordinary));
  // The namespace guard passes; the form then reads system.dictionaries.
  EXPECT_EQ(explicit_resp.code(), rewriter::RewriteCode::UnsupportedStatement)
    << explicit_resp.message();
  EXPECT_EQ(explicit_resp.message(), "system table system.dictionaries is not accessible");
  EXPECT_EQ(explicit_resp.original_accessed_tables_size(), 0);

  // Context values are semantic already; a literal backslash-x spelling is
  // ordinary and must not be decoded into the reserved physical database.
  rewriter::RewriteSQLRequest literal_physical;
  literal_physical.set_sql("SHOW DICTIONARIES");
  AddSIContractOption(literal_physical);
  literal_physical.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args()
    ->set_upstream_physical_database_in_context("hg\\x5Fsafe");
  const auto literal_resp = RunDirectRewrite(std::move(literal_physical));
  // Not the reserved database: the guard passes and the system-table
  // allowlist answers.
  EXPECT_EQ(literal_resp.code(), rewriter::RewriteCode::UnsupportedStatement)
    << literal_resp.message();
  EXPECT_EQ(literal_resp.message(), "system table system.dictionaries is not accessible");
  EXPECT_EQ(literal_resp.original_accessed_tables_size(), 0);
}

TEST(StorageIntegrityCTEScope, NonRecursiveAliasesBecomeVisibleIncrementally) {
  constexpr const char *mutation_sql =
    "UPDATE other.u SET x=(WITH t AS (SELECT count() FROM t) "
    "SELECT count() FROM t) WHERE id=1";
  rewriter::RewriteSQLRequest mutation;
  mutation.set_sql(mutation_sql);
  AddSIContractOption(mutation);
  auto *mutation_dyn =
    mutation.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
  (*mutation_dyn->mutable_database_map())["other"] = "phys";
  mutation_dyn->set_upstream_logical_database_in_context("db1");

  const auto mutation_resp = RunDirectRewrite(std::move(mutation));
  EXPECT_EQ(mutation_resp.code(), rewriter::RewriteCode::UnsupportedStatement)
    << mutation_resp.message();
  EXPECT_EQ(mutation_resp.message(),
            "storage-integrity table db1.t accepts writes only through the signed statement lane");
  ASSERT_EQ(mutation_resp.original_accessed_tables_size(), 1);
  const auto &mutation_access = mutation_resp.original_accessed_tables(0);
  EXPECT_TRUE(mutation_access.original_database().empty());
  EXPECT_EQ(mutation_access.original_table(), "t");
  EXPECT_EQ(mutation_access.logical_database(), "db1");
  EXPECT_EQ(mutation_access.physical_database(), "phys");
  EXPECT_TRUE(mutation_access.is_storage_integrity());

  constexpr const char *select_sql =
    "WITH t AS (SELECT count() FROM t) SELECT count() FROM t";
  rewriter::RewriteSQLRequest select;
  select.set_sql(select_sql);
  AddSIContractOption(select);
  select.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args()
    ->set_upstream_logical_database_in_context("db1");

  const auto select_resp = RunDirectRewrite(std::move(select));
  EXPECT_EQ(select_resp.code(), rewriter::RewriteCode::Success) << select_resp.message();
  EXPECT_NE(select_resp.sql_after_rewrite().find("hg_safe.db1__t"), std::string::npos)
    << select_resp.sql_after_rewrite();
  const auto *select_access = FindAccessed(select_resp, "", "t");
  ASSERT_NE(select_access, nullptr);
  EXPECT_TRUE(select_access->is_storage_integrity());
}

// A lightweight UPDATE binds no WITH alias, recursive or not: ClickHouse 26.2
// and 25.8 count the table t here (measured), so the statement reads a table
// no rewrite reaches and is refused (spec 2026-09-26 R2). Deviation from the
// plan, which pinned Success on the assumption that the recursive CTE reads
// no table. With the SI surface active the unbound `t` is the Active table
// db1.t, so the SI write refusal names it first (rewriter-go v0.15.0 answers
// the same; before Task I3 the SI check bound the CTE and R2 answered).
TEST(StorageIntegrityCTEScope, RecursiveAliasesArePredeclared) {
  constexpr const char *sql =
    "UPDATE other.u SET x=(WITH RECURSIVE t AS (SELECT count() FROM t) "
    "SELECT count() FROM t) WHERE id=1";
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);
  AddSIContractOption(req);
  auto *dyn = req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
  (*dyn->mutable_database_map())["other"] = "phys";
  dyn->set_upstream_logical_database_in_context("db1");

  const auto resp = RunDirectRewrite(std::move(req));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
  EXPECT_EQ(resp.message(), "storage-integrity table db1.t accepts writes only through the signed statement lane");
  EXPECT_EQ(resp.sql_after_rewrite(), sql);
}

TEST(StorageIntegrityWrappers, PREWHEREUsesParsedMainTable) {
  const std::vector<std::string> cases = {
    "SELECT a FROM `\\x64b1`.t PREWHERE a > 1",
    "SELECT a FROM db1.t AS \"FROM\" PREWHERE a > 1",
  };
  for (const auto &sql : cases) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    AddSIContractOption(req);

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::RewriteError) << resp.message();
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
    EXPECT_EQ(resp.message(),
              std::string(rewriter_handlers::kStorageIntegrityWrapperMessage));
  }
}

TEST(StorageIntegrityWrappers, EscapedDatabaseUsesSemanticTarget) {
  const std::vector<std::string> cases = {
    "SELECT a FROM `\\x64b1`.t FINAL",
    "SELECT a FROM `\\x64b1`.t SAMPLE 0.1",
    "SELECT * FROM `\\x64b1`.t WITH OFFSET AS off",
    "SELECT * FROM `\\x64b1`.t AS x(a)",
  };
  for (const auto &sql : cases) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    AddSIContractOption(req);

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::RewriteError) << resp.message();
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
    EXPECT_EQ(resp.message(),
              std::string(rewriter_handlers::kStorageIntegrityWrapperMessage));
  }
}

TEST(StorageIntegrityReservedColumn, EscapedDatabaseUsesSemanticTarget) {
  const std::vector<std::string> cases = {
    "SELECT _hg_row_id FROM `\\x64b1`.t",
    "SELECT `\\x64b1`.t._hg_row_id FROM `\\x64b1`.t",
  };
  for (const auto &sql : cases) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    AddSIContractOption(req);

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::RewriteError) << resp.message();
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
    EXPECT_EQ(resp.message(), "reserved column _hg_row_id is not addressable");
  }
}

TEST(StorageIntegrityNamespaces, IdentifierEscapesAreSemanticButLiteralsAreNotDecodedAgain) {
  const std::vector<std::string> semantic_identifiers = {
    "SELECT * FROM merge(`\\x64b1`, t)",
    "SELECT * FROM remote('host', `\\x64b1`, t)",
  };
  for (const auto &sql : semantic_identifiers) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    AddSIContractOption(req);
    auto *dyn = req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
    (*dyn->mutable_database_map())["other"] = "phys";

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::RewriteError) << resp.message();
    const auto *access = FindAccessed(resp, "db1", "t");
    ASSERT_NE(access, nullptr);
    EXPECT_TRUE(access->is_storage_integrity());
  }

  // Spec 2026-09-26 T4: the escaped IN operand decodes to the Active db1.t
  // and is read through the derived safe read.
  {
    rewriter::RewriteSQLRequest req;
    req.set_sql("SELECT * FROM other.u WHERE id IN `\\x64b1`.t");
    AddSIContractOption(req);
    auto *dyn = req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
    (*dyn->mutable_database_map())["other"] = "phys";
    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    EXPECT_NE(resp.sql_after_rewrite().find("IN (SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t)"),
              std::string::npos) << resp.sql_after_rewrite();
    const auto *access = FindAccessed(resp, "db1", "t");
    ASSERT_NE(access, nullptr);
    EXPECT_TRUE(access->is_storage_integrity());
  }

  // A literal is not decoded again, so it names no SI namespace; the table
  // function itself is refused by the T5 allowlist after the SI policy.
  const std::vector<std::pair<std::string, std::string>> literal_backslash_x = {
    {"SELECT * FROM merge('db\\\\x31', 't')", "table function merge is not accepted"},
    {"SELECT * FROM remote('host', 'db\\\\x31', 't')", "table function remote is not accepted"},
  };
  for (const auto &[sql, message] : literal_backslash_x) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    AddSIContractOption(req);

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), message);
    for (const auto &access : resp.original_accessed_tables())
      EXPECT_FALSE(access.is_storage_integrity());
  }
}

TEST(StorageIntegrityAlterModifyQuery, RejectsEmbeddedLogicalAndPhysicalReads) {
  struct Case {
    const char *sql;
    const char *message;
    const char *database;
    const char *table;
  };
  const std::vector<Case> cases = {
    {"ALTER TABLE other.mv MODIFY QUERY SELECT * FROM db1.t",
      "storage-integrity table db1.t accepts writes only through the signed statement lane",
      "db1", "t"},
    {"ALTER TABLE other.mv MODIFY QUERY SELECT * FROM hg_safe.db1__t",
      "storage-integrity physical table hg_safe.db1__t is not directly addressable",
      "hg_safe", "db1__t"},
  };
  for (const auto &c : cases) {
    SCOPED_TRACE(c.sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(c.sql);
    AddSIContractOption(req);
    auto *dyn = req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
    (*dyn->mutable_database_map())["other"] = "phys";

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    EXPECT_EQ(resp.sql_after_rewrite(), c.sql);
    EXPECT_EQ(resp.message(), c.message);
    ASSERT_EQ(resp.original_accessed_tables_size(), 1);
    const auto &access = resp.original_accessed_tables(0);
    EXPECT_EQ(access.original_database(), c.database);
    EXPECT_EQ(access.original_table(), c.table);
    EXPECT_TRUE(access.is_storage_integrity());
  }
}

TEST(StorageIntegrityAlterExpressions, RejectsEmbeddedLogicalAndPhysicalReads) {
  struct Case {
    const char *sql;
    const char *message;
    const char *database;
    const char *table;
  };
  const std::vector<Case> cases = {
    {"ALTER TABLE other.u ADD COLUMN x UInt64 DEFAULT (SELECT max(a) FROM db1.t)",
      "storage-integrity table db1.t accepts writes only through the signed statement lane",
      "db1", "t"},
    {"ALTER TABLE other.u ADD COLUMN x UInt64 MATERIALIZED (SELECT max(a) FROM hg_safe.db1__t)",
      "storage-integrity physical table hg_safe.db1__t is not directly addressable",
      "hg_safe", "db1__t"},
    {"ALTER TABLE other.u ADD COLUMN x UInt64 ALIAS (SELECT max(a) FROM db1.t)",
      "storage-integrity table db1.t accepts writes only through the signed statement lane",
      "db1", "t"},
    {"ALTER TABLE other.u ADD COLUMN x UInt64 EPHEMERAL (SELECT max(a) FROM hg_unsafe.db1__t)",
      "storage-integrity physical table hg_unsafe.db1__t is not directly addressable",
      "hg_unsafe", "db1__t"},
    {"ALTER TABLE other.u ADD CONSTRAINT c CHECK x IN (SELECT a FROM hg_safe.db1__t)",
      "storage-integrity physical table hg_safe.db1__t is not directly addressable",
      "hg_safe", "db1__t"},
    {"ALTER TABLE other.u ADD COLUMN x UInt64 DEFAULT (SELECT max(a) FROM db1.t), "
      "ADD CONSTRAINT c CHECK x IN (SELECT a FROM hg_safe.db1__t)",
      "storage-integrity table db1.t accepts writes only through the signed statement lane",
      "db1", "t"},
  };

  for (const auto &c : cases) {
    SCOPED_TRACE(c.sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(c.sql);
    AddSIContractOption(req);
    auto *dyn = req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
    (*dyn->mutable_database_map())["other"] = "phys";

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    EXPECT_EQ(resp.sql_after_rewrite(), c.sql);
    EXPECT_EQ(resp.message(), c.message);
    ASSERT_EQ(resp.original_accessed_tables_size(), 1);
    const auto &access = resp.original_accessed_tables(0);
    EXPECT_EQ(access.original_database(), c.database);
    EXPECT_EQ(access.original_table(), c.table);
    EXPECT_TRUE(access.is_storage_integrity());
  }

  constexpr const char *ordinary_sql =
    "ALTER TABLE other.u ADD COLUMN x UInt64 DEFAULT (SELECT max(a) FROM other.v), "
    "ADD CONSTRAINT c CHECK x IN (SELECT a FROM other.w)";
  rewriter::RewriteSQLRequest ordinary;
  ordinary.set_sql(ordinary_sql);
  AddSIContractOption(ordinary);
  (*ordinary.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args()
      ->mutable_database_map())["other"] = "phys";

  const auto ordinary_resp = RunDirectRewrite(std::move(ordinary));
  // Spec 2026-09-26 R2: an ordinary subquery in an ALTER action is a read no
  // rewrite reaches, refused after the SI policy found nothing.
  EXPECT_EQ(ordinary_resp.code(), rewriter::RewriteCode::UnsupportedStatement)
    << ordinary_resp.message();
  EXPECT_EQ(ordinary_resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
  EXPECT_EQ(ordinary_resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
  for (const auto &access : ordinary_resp.original_accessed_tables())
    EXPECT_FALSE(access.is_storage_integrity());
}

TEST(StorageIntegrityCreateExpressions, RejectsEmbeddedColumnAndConstraintReads) {
  struct Case {
    const char *sql;
    const char *message;
    const char *database;
    const char *table;
  };
  const std::vector<Case> cases = {
    {"CREATE TABLE other.x (a UInt64 DEFAULT (SELECT max(x) FROM db1.t)) ENGINE=Memory",
      "storage-integrity table db1.t accepts writes only through the signed statement lane",
      "db1", "t"},
    {"CREATE TABLE other.x (a UInt64 MATERIALIZED (SELECT max(x) FROM hg_safe.db1__t)) ENGINE=Memory",
      "storage-integrity physical table hg_safe.db1__t is not directly addressable",
      "hg_safe", "db1__t"},
    {"CREATE TABLE other.x (a UInt64 ALIAS (SELECT max(x) FROM db1.t)) ENGINE=Memory",
      "storage-integrity table db1.t accepts writes only through the signed statement lane",
      "db1", "t"},
    {"CREATE TABLE other.x (a UInt64 EPHEMERAL (SELECT max(x) FROM hg_unsafe.db1__t)) ENGINE=Memory",
      "storage-integrity physical table hg_unsafe.db1__t is not directly addressable",
      "hg_unsafe", "db1__t"},
    {"CREATE TABLE other.x (a UInt64, CONSTRAINT c CHECK ((SELECT max(x) FROM db1.t) > 0)) ENGINE=Memory",
      "storage-integrity table db1.t accepts writes only through the signed statement lane",
      "db1", "t"},
  };

  for (const auto &c : cases) {
    SCOPED_TRACE(c.sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(c.sql);
    AddSIContractOption(req);
    auto *dyn = req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
    (*dyn->mutable_database_map())["other"] = "phys";

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    EXPECT_EQ(resp.sql_after_rewrite(), c.sql);
    EXPECT_EQ(resp.message(), c.message);
    ASSERT_EQ(resp.original_accessed_tables_size(), 1);
    const auto &access = resp.original_accessed_tables(0);
    EXPECT_EQ(access.original_database(), c.database);
    EXPECT_EQ(access.original_table(), c.table);
    EXPECT_TRUE(access.is_storage_integrity());
  }

  constexpr const char *ordinary_sql =
    "CREATE TABLE other.x (a UInt64 DEFAULT (SELECT max(x) FROM other.u), "
    "CONSTRAINT c CHECK ((SELECT count() FROM other.v) >= 0)) ENGINE=Memory";
  rewriter::RewriteSQLRequest ordinary;
  ordinary.set_sql(ordinary_sql);
  AddSIContractOption(ordinary);
  (*ordinary.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args()
      ->mutable_database_map())["other"] = "phys";

  const auto ordinary_resp = RunDirectRewrite(std::move(ordinary));
  // Spec 2026-09-26 R2: an ordinary subquery in a column default or a
  // constraint is a read no rewrite reaches, refused after the SI policy.
  EXPECT_EQ(ordinary_resp.code(), rewriter::RewriteCode::UnsupportedStatement)
    << ordinary_resp.message();
  EXPECT_EQ(ordinary_resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
  EXPECT_EQ(ordinary_resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
  for (const auto &access : ordinary_resp.original_accessed_tables())
    EXPECT_FALSE(access.is_storage_integrity());
}

TEST(StorageIntegrityLiterals, EscapeSqlLiteralEscapesBackslashBeforeQuote) {
  EXPECT_EQ(rewriter_handlers::escapeSqlLiteral("plain_1_1_0"), "plain_1_1_0");
  EXPECT_EQ(rewriter_handlers::escapeSqlLiteral("o'brien"), "o''brien");
  EXPECT_EQ(rewriter_handlers::escapeSqlLiteral("back\\slash"), "back\\\\slash");
  EXPECT_EQ(rewriter_handlers::escapeSqlLiteral("evil\\"), "evil\\\\");
  EXPECT_EQ(rewriter_handlers::escapeSqlLiteral("mix\\'ed"), "mix\\\\''ed");
}

TEST(StorageIntegrityLiterals, ProjectionRestoreSkipsStringLiterals) {
  EXPECT_EQ(rewriter_handlers::restoreStorageIntegrityProjectionSyntax(
              "SELECT * EXCEPT _hg_row_id FROM hg_safe.db1__t", "_hg_row_id"),
            "SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t");
  EXPECT_EQ(rewriter_handlers::restoreStorageIntegrityProjectionSyntax(
              "SELECT 'EXCEPT _hg_row_id' AS s FROM t", "_hg_row_id"),
            "SELECT 'EXCEPT _hg_row_id' AS s FROM t");
  EXPECT_EQ(rewriter_handlers::restoreStorageIntegrityProjectionSyntax(
              "SELECT 'a''EXCEPT _hg_row_id' AS s, * EXCEPT _hg_row_id FROM t", "_hg_row_id"),
            "SELECT 'a''EXCEPT _hg_row_id' AS s, * EXCEPT (_hg_row_id) FROM t");
}

TEST(StorageIntegrityMutationReads, RejectsEmbeddedSIReadsInGrammarOrder) {
  struct MutationCase {
    const char *name;
    const char *sql;
    const char *message;
    const char *access_database;
    const char *access_table;
  };
  const std::vector<MutationCase> cases = {
    {"update_assignment_logical",
      "UPDATE other.u SET x = (SELECT count() FROM db1.t) WHERE id = 1",
      "storage-integrity table db1.t accepts writes only through the signed statement lane",
      "db1", "t"},
    {"update_predicate_safe",
      "UPDATE other.u SET x = 1 WHERE id IN (SELECT id FROM hg_safe.db1__t)",
      "storage-integrity physical table hg_safe.db1__t is not directly addressable",
      "hg_safe", "db1__t"},
    {"delete_predicate_unsafe",
      "DELETE FROM other.u WHERE id IN (SELECT id FROM hg_unsafe.db1__t)",
      "storage-integrity physical table hg_unsafe.db1__t is not directly addressable",
      "hg_unsafe", "db1__t"},
    {"alter_update_assignment_logical",
      "ALTER TABLE other.u UPDATE x = (SELECT count() FROM db1.t) WHERE id = 1",
      "storage-integrity table db1.t accepts writes only through the signed statement lane",
      "db1", "t"},
    {"alter_update_predicate_safe",
      "ALTER TABLE other.u UPDATE x = 1 WHERE id IN (SELECT id FROM hg_safe.db1__t)",
      "storage-integrity physical table hg_safe.db1__t is not directly addressable",
      "hg_safe", "db1__t"},
    {"alter_delete_predicate_logical",
      "ALTER TABLE other.u DELETE WHERE id IN (SELECT id FROM db1.t)",
      "storage-integrity table db1.t accepts writes only through the signed statement lane",
      "db1", "t"},
  };

  for (const auto &c : cases) {
    SCOPED_TRACE(c.name);
    rewriter::RewriteSQLRequest req;
    req.set_sql(c.sql);
    AddSIContractOption(req);
    auto *dyn = req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
    (*dyn->mutable_database_map())["other"] = "phys";

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    EXPECT_EQ(resp.sql_after_rewrite(), c.sql);
    EXPECT_EQ(resp.message(), c.message);
    EXPECT_EQ(resp.storage_integrity_contract_version(),
              rewriter::STORAGE_INTEGRITY_CONTRACT_V1);
    ASSERT_EQ(resp.original_accessed_tables_size(), 1);
    const auto &access = resp.original_accessed_tables(0);
    EXPECT_EQ(access.original_database(), c.access_database);
    EXPECT_EQ(access.original_table(), c.access_table);
    EXPECT_TRUE(access.is_storage_integrity());
  }
}

TEST(StorageIntegrityMutationReads, UpdateAssignmentPrecedesPredicate) {
  constexpr const char *sql =
    "UPDATE other.u "
    "SET x = (SELECT count() FROM db1.t) "
    "WHERE id IN (SELECT id FROM hg_safe.db1__t)";

  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);
  AddSIContractOption(req);
  auto *dyn = req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
  (*dyn->mutable_database_map())["other"] = "phys";

  const auto resp = RunDirectRewrite(std::move(req));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
  EXPECT_EQ(resp.sql_after_rewrite(), sql);
  EXPECT_EQ(resp.message(),
            "storage-integrity table db1.t accepts writes only through the signed statement lane");
  EXPECT_EQ(resp.storage_integrity_contract_version(),
            rewriter::STORAGE_INTEGRITY_CONTRACT_V1);
  ASSERT_EQ(resp.original_accessed_tables_size(), 1);
  const auto &access = resp.original_accessed_tables(0);
  EXPECT_EQ(access.original_database(), "db1");
  EXPECT_EQ(access.original_table(), "t");
  EXPECT_TRUE(access.is_storage_integrity());
}

TEST(StorageIntegrityMutationReads, FirstSourceWinsAcrossTablesAndNamespaces) {
  struct OrderCase {
    const char *name;
    const char *sql;
    const char *message;
    const char *access_database;
    const char *access_table;
  };
  const std::vector<OrderCase> cases = {
    {"namespace_before_table",
      "UPDATE other.u SET x = (SELECT count() FROM merge('hg_safe', 'x')) + "
      "(SELECT count() FROM db1.t) WHERE id = 1",
      "storage-integrity physical table hg_safe.x is not directly addressable",
      "hg_safe", "x"},
    {"table_before_namespace",
      "UPDATE other.u SET x = (SELECT count() FROM db1.t) + "
      "(SELECT count() FROM merge('hg_safe', 'x')) WHERE id = 1",
      "storage-integrity table db1.t accepts writes only through the signed statement lane",
      "db1", "t"},
  };

  for (const auto &c : cases) {
    SCOPED_TRACE(c.name);
    rewriter::RewriteSQLRequest req;
    req.set_sql(c.sql);
    AddSIContractOption(req);
    auto *dyn = req.mutable_options(0)->mutable_table_name_args()->mutable_dynamic_args();
    (*dyn->mutable_database_map())["other"] = "phys";

    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    EXPECT_EQ(resp.sql_after_rewrite(), c.sql);
    EXPECT_EQ(resp.message(), c.message);
    ASSERT_EQ(resp.original_accessed_tables_size(), 1);
    const auto &access = resp.original_accessed_tables(0);
    EXPECT_EQ(access.original_database(), c.access_database);
    EXPECT_EQ(access.original_table(), c.access_table);
    EXPECT_TRUE(access.is_storage_integrity());
  }
}

// ============================================================================
// Storage-integrity read surface (housegate Spec G) — shared golden corpus.
// tests/testdata/storage_integrity_cases.json is a byte-identical copy of
// rewriter-go/internal/harness/testdata/storage_integrity_cases.json; the Go
// and C++ runners compare every successful sql_after_rewrite exactly after
// the shared identifier-quote normalization, plus every structured field.
// ============================================================================

TEST(StorageIntegrityCorpus, SatisfiesTheFrozenContract) {
  std::vector<std::string> violations;
  const auto cases = si_corpus::LoadCases(
      std::string(REWRITER_TEST_DATA_DIR) + "/storage_integrity_cases.json", &violations);
  ASSERT_FALSE(cases.empty()) << "the shared behaviour contract cannot be empty";
  for (const auto &v : si_corpus::ValidateCorpus(cases)) violations.push_back(v);
  std::string report;
  for (const auto &v : violations) report += "\n  " + v;
  EXPECT_TRUE(violations.empty())
      << "corpus contract violations (" << violations.size() << "):" << report;
}

TEST(StorageIntegrityCorpus, IsBytePinnedToRewriterGo) {
  std::ifstream in(std::string(REWRITER_TEST_DATA_DIR) + "/storage_integrity_cases.json",
                   std::ios::binary);
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string raw = buf.str();
  EXPECT_EQ(raw.size(), si_corpus::kCorpusBytes);
  EXPECT_EQ(si_corpus::Fingerprint(raw), si_corpus::kCorpusFingerprint)
      << "The shared corpus changed. Copy rewriter-go's file verbatim and update the pinned "
         "constants in BOTH tests/si_corpus.h and rewriter-go internal/harness/sicorpus_test.go.";
  std::vector<std::string> ignored;
  EXPECT_EQ(si_corpus::LoadCases(
                std::string(REWRITER_TEST_DATA_DIR) + "/storage_integrity_cases.json", &ignored)
                .size(),
            si_corpus::kCorpusCases);
}

TEST(SINormalize, SupportedCanonicalOutput) {
  using si_corpus::NormalizeSIIdentifierQuotes;
  EXPECT_EQ(NormalizeSIIdentifierQuotes("SELECT 1"), "SELECT 1");
  EXPECT_EQ(NormalizeSIIdentifierQuotes("SELECT a FROM `db1.t`"),
            "SELECT a FROM \"db1.t\"");
  EXPECT_EQ(NormalizeSIIdentifierQuotes("SELECT a FROM \"db1.t\""), "SELECT a FROM \"db1.t\"");
  EXPECT_EQ(NormalizeSIIdentifierQuotes("SELECT * FROM phys.`db1.t` AS `db1.t`"),
            "SELECT * FROM phys.\"db1.t\" AS \"db1.t\"");
  EXPECT_EQ(NormalizeSIIdentifierQuotes("SELECT '`raw`' FROM `t`"),
            "SELECT '`raw`' FROM \"t\"");
  EXPECT_EQ(NormalizeSIIdentifierQuotes("SELECT '\"' FROM \"t\""),
            "SELECT '\"' FROM \"t\"");
  EXPECT_EQ(NormalizeSIIdentifierQuotes("SELECT '-- /* # // `raw`' FROM `t`"),
            "SELECT '-- /* # // `raw`' FROM \"t\"");
  EXPECT_EQ(NormalizeSIIdentifierQuotes("SELECT 'INSERT FORMAT `raw`' FROM `t`"),
            "SELECT 'INSERT FORMAT `raw`' FROM \"t\"");
  EXPECT_EQ(NormalizeSIIdentifierQuotes("SELECT FORMAT FROM `t`"),
            "SELECT FORMAT FROM \"t\"");
  EXPECT_EQ(NormalizeSIIdentifierQuotes("INSERT INTO `t` VALUES ('`raw`')"),
            "INSERT INTO \"t\" VALUES ('`raw`')");
}

TEST(SINormalize, DoubledBacktickEscapeMatchesPolyglot) {
  EXPECT_EQ(si_corpus::NormalizeSIIdentifierQuotes("SELECT * FROM `a``b`"),
            "SELECT * FROM \"a`b\"");
}

TEST(SINormalize, BackslashBacktickEscapeMatchesPolyglot) {
  EXPECT_EQ(si_corpus::NormalizeSIIdentifierQuotes("SELECT * FROM `a\\`b`"),
            "SELECT * FROM \"a`b\"");
}

TEST(SINormalize, ReencodesANSIIdentifierDelimiter) {
  EXPECT_EQ(si_corpus::NormalizeSIIdentifierQuotes("SELECT * FROM `a\"b`"),
            "SELECT * FROM \"a\"\"b\"");
}

TEST(SINormalize, SupportedLiteralAndANSIEscapes) {
  using si_corpus::NormalizeSIIdentifierQuotes;
  EXPECT_EQ(NormalizeSIIdentifierQuotes("SELECT 'a\\'`raw`' FROM `t`"),
            "SELECT 'a\\'`raw`' FROM \"t\"");
  EXPECT_EQ(NormalizeSIIdentifierQuotes("SELECT 'a''`raw`' FROM `t`"),
            "SELECT 'a''`raw`' FROM \"t\"");
  EXPECT_EQ(NormalizeSIIdentifierQuotes("SELECT \"a\"\"`raw`\" FROM `t`"),
            "SELECT \"a\"\"`raw`\" FROM \"t\"");
  EXPECT_EQ(NormalizeSIIdentifierQuotes("SELECT \"a\\\" FROM `t`"),
            "SELECT \"a\\\" FROM \"t\"");
}

TEST(SINormalize, UnsupportedCommentsReturnOriginal) {
  using si_corpus::NormalizeSIIdentifierQuotes;
  const std::vector<std::string> cases = {
    "SELECT `t` -- keep `raw`\n",
    "SELECT `t` # keep `raw`\n",
    "SELECT `t` // keep `raw`\n",
    "SELECT `t` /* keep `raw` */",
    "SELECT `t` /*+ keep `raw` */",
  };
  for (const auto &sql : cases) EXPECT_EQ(NormalizeSIIdentifierQuotes(sql), sql);
}

TEST(SINormalize, UnsupportedDollarQuotesReturnOriginal) {
  using si_corpus::NormalizeSIIdentifierQuotes;
  const std::vector<std::string> cases = {
    "SELECT `t`, $$`raw`$$",
    "SELECT `t`, $tag$`raw`$tag$",
    "SELECT `t`, $name",
  };
  for (const auto &sql : cases) EXPECT_EQ(NormalizeSIIdentifierQuotes(sql), sql);
}

TEST(SINormalize, UnsupportedQuoteFormsReturnOriginal) {
  using si_corpus::NormalizeSIIdentifierQuotes;
  const std::vector<std::string> cases = {
    "SELECT `t`, \"\"\"`raw`\"\"\"",
    "SELECT `t`, '''`raw`'''",
    "SELECT `t`, ‘`raw`’",
    "SELECT `t`, “`raw`”",
    "SELECT `t`, E'`raw`'",
    "SELECT `t`, N'`raw`'",
    "SELECT `t`, U&'`raw`'",
  };
  for (const auto &sql : cases) EXPECT_EQ(NormalizeSIIdentifierQuotes(sql), sql);
}

TEST(SINormalize, InsertFormatReturnsOriginal) {
  const std::string sql =
    "INSERT INTO `db.t` FORMAT CSV\n1,`raw`\n\nSELECT `still_payload`";
  EXPECT_EQ(si_corpus::NormalizeSIIdentifierQuotes(sql), sql);
}

TEST(SINormalize, MalformedDelimitersReturnOriginal) {
  using si_corpus::NormalizeSIIdentifierQuotes;
  const std::vector<std::string> cases = {
    "SELECT `a",
    "SELECT `ok`, `bad",
    "SELECT `ok`, `a\\`b",
    "SELECT `ok`, \"bad",
    "SELECT `ok`, 'bad",
    "SELECT `ok`, 'bad\\",
  };
  for (const auto &sql : cases) EXPECT_EQ(NormalizeSIIdentifierQuotes(sql), sql);
}

// The comparison guard may deliberately leave a harmless identifier-style
// difference visible when it encounters unsupported syntax. It must never
// erase a semantic difference inside a literal and produce a false equality.
TEST(SINormalize, LiteralQuotingBugIsNotHidden) {
  EXPECT_NE(si_corpus::NormalizeSIIdentifierQuotes("SELECT * FROM t WHERE s = '`raw`'"),
            si_corpus::NormalizeSIIdentifierQuotes("SELECT * FROM t WHERE s = '\"raw\"'"));
}

namespace {

using SIGoldenCase = si_corpus::Case;

const std::map<std::string, rewriter::RewriteCode> kSICodeByName = {
  {"Success", rewriter::RewriteCode::Success},
  {"SyntaxError", rewriter::RewriteCode::SyntaxError},
  {"RewriteError", rewriter::RewriteCode::RewriteError},
  {"UnsupportedStatement", rewriter::RewriteCode::UnsupportedStatement},
  {"InvalidRewriteRequest", rewriter::RewriteCode::InvalidRewriteRequest},
};
const std::map<std::string, rewriter::StatementType> kSIStmtByName = {
  {"", rewriter::STATEMENT_TYPE_UNSPECIFIED}, {"SELECT", rewriter::STATEMENT_TYPE_SELECT},
  {"INSERT", rewriter::STATEMENT_TYPE_INSERT}, {"EXISTS_TABLE", rewriter::STATEMENT_TYPE_EXISTS_TABLE},
  {"SHOW_TABLES", rewriter::STATEMENT_TYPE_SHOW_TABLES}, {"DESCRIBE", rewriter::STATEMENT_TYPE_DESCRIBE},
  {"CREATE_TABLE", rewriter::STATEMENT_TYPE_CREATE_TABLE},
  {"DROP_TABLE", rewriter::STATEMENT_TYPE_DROP_TABLE},
  {"ALTER_TABLE", rewriter::STATEMENT_TYPE_ALTER_TABLE},
  {"CREATE_VIEW", rewriter::STATEMENT_TYPE_CREATE_VIEW},
  {"CREATE_MATERIALIZED_VIEW", rewriter::STATEMENT_TYPE_CREATE_MATERIALIZED_VIEW},
  {"SHOW_CREATE_TABLE", rewriter::STATEMENT_TYPE_SHOW_CREATE_TABLE},
};

void applyStorageIntegrityArgs(rewriter::RewriteTableDynamicArgs *dyn, const Poco::JSON::Object::Ptr &d,
                               const std::string &contract_version) {
  if (d->has("database_map")) {
    auto m = d->getObject("database_map");
    for (const auto &k : *m) (*dyn->mutable_database_map())[k.first] = k.second.toString();
  }
  for (const auto &p : si_corpus::JsonStrings(d, "known_physical_databases"))
    dyn->add_known_physical_databases(p);
  for (const auto &p : si_corpus::JsonStrings(d, "protected_databases"))
    dyn->add_protected_databases(p);
  if (d->has("upstream_logical_database_in_context"))
    dyn->set_upstream_logical_database_in_context(d->getValue<std::string>("upstream_logical_database_in_context"));
  if (d->has("upstream_physical_database_in_context"))
    dyn->set_upstream_physical_database_in_context(d->getValue<std::string>("upstream_physical_database_in_context"));
  if (d->has("delim")) dyn->set_delim(d->getValue<std::string>("delim"));
  if (d->has("logical_database_to_remote_upstream_index")) {
    auto m = d->getObject("logical_database_to_remote_upstream_index");
    for (const auto &k : *m)
      (*dyn->mutable_logical_database_to_remote_upstream_index())[k.first] = k.second.toString();
  }
  if (d->has("remote_upstreams")) {
    auto m = d->getObject("remote_upstreams");
    for (const auto &k : *m) {
      auto in = m->getObject(k.first);
      auto &out = (*dyn->mutable_remote_upstreams())[k.first];
      if (in->has("addr")) out.set_addr(in->getValue<std::string>("addr"));
      if (in->has("user")) out.set_user(in->getValue<std::string>("user"));
      if (in->has("password")) out.set_password(in->getValue<std::string>("password"));
    }
  }
  if (!d->has("storage_integrity")) return;
  auto si = d->getObject("storage_integrity");
  auto *args = dyn->mutable_storage_integrity();
  args->set_contract_version(contract_version == "V2"
                               ? rewriter::STORAGE_INTEGRITY_CONTRACT_V2
                               : rewriter::STORAGE_INTEGRITY_CONTRACT_V1);
  const std::string mode = si->has("read_mode") ? si->getValue<std::string>("read_mode") : "";
  args->set_read_mode(mode == "UNSAFE_LATEST" ? rewriter::StorageIntegrityArgs::READ_MODE_UNSAFE_LATEST
                    : mode == "SAFE" ? rewriter::StorageIntegrityArgs::READ_MODE_SAFE
                    : mode == "INVALID_99" ? static_cast<rewriter::StorageIntegrityArgs::ReadMode>(99)
                    : rewriter::StorageIntegrityArgs::READ_MODE_UNSPECIFIED);
  if (si->has("reserved_row_id_column")) args->set_reserved_row_id_column(si->getValue<std::string>("reserved_row_id_column"));
  for (const auto &database : si_corpus::JsonStrings(si, "reserved_databases"))
    args->add_reserved_databases(database);
  auto tables = si->getObject("tables");
  for (const auto &kv : *tables) {
    auto t = tables->getObject(kv.first);
    auto &out = (*args->mutable_tables())[kv.first];
    out.set_safe_table(t->getValue<std::string>("safe_table"));
    out.set_unsafe_table(t->getValue<std::string>("unsafe_table"));
    for (const auto &p : si_corpus::JsonStrings(t, "excluded_unsafe_parts"))
      out.add_excluded_unsafe_parts(p);
  }
}

std::vector<SIGoldenCase> loadSIGoldenCases() {
  std::vector<std::string> ignored;
  return si_corpus::LoadCases(
    std::string(REWRITER_TEST_DATA_DIR) + "/storage_integrity_cases.json", &ignored);
}

class StorageIntegrityGolden : public ::testing::TestWithParam<SIGoldenCase> {};

TEST_P(StorageIntegrityGolden, MatchesSharedCorpus) {
  const auto &c = GetParam();
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(c.sql);
  if (c.has_dynamic) {
    auto *opt = req.add_options();
    opt->set_op(rewriter::RewriteOp::TableNameRewrite);
    applyStorageIntegrityArgs(opt->mutable_table_name_args()->mutable_dynamic_args(), c.dynamic,
                              c.contract_version);
  }
  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  ASSERT_TRUE(service.Rewrite(&ctx, &req, &resp).ok());
  SCOPED_TRACE("sql_after_rewrite: " + resp.sql_after_rewrite() + " | message: " + resp.message());

  // The surface is active for any V2 case and for a V1 case with a non-empty
  // table map; an active case expects its own version acknowledged.
  const bool si_block = c.has_dynamic && c.dynamic->has("storage_integrity");
  const bool si_active = si_block && (c.contract_version == "V2"
    || c.dynamic->getObject("storage_integrity")->getObject("tables")->size() != 0);
  if (si_active && !c.want_no_contract_ack) {
    EXPECT_EQ(resp.storage_integrity_contract_version(),
              c.contract_version == "V2" ? rewriter::STORAGE_INTEGRITY_CONTRACT_V2
                                         : rewriter::STORAGE_INTEGRITY_CONTRACT_V1);
  } else {
    EXPECT_EQ(resp.storage_integrity_contract_version(), rewriter::STORAGE_INTEGRITY_CONTRACT_UNSPECIFIED);
  }

  if (!c.want_code.empty()) EXPECT_EQ(resp.code(), kSICodeByName.at(c.want_code));
  EXPECT_EQ(resp.statement_type(), kSIStmtByName.at(c.want_stmt));
  if (!c.want_message_contains.empty()) EXPECT_NE(resp.message().find(c.want_message_contains), std::string::npos);
  if (c.has_table_rewrites) {
    std::map<std::string, std::string> got(resp.table_rewrites().begin(), resp.table_rewrites().end());
    EXPECT_EQ(got, c.want_table_rewrites);
  }
  if (c.has_accessed) {
    // Spec 2026-09-26 §5 / rewriter-proto v0.4.0: the order is part of the
    // contract (a write target first, then key-sorted by the name as written).
    ASSERT_EQ(static_cast<size_t>(resp.original_accessed_tables_size()), c.want_accessed.size());
    for (size_t i = 0; i < c.want_accessed.size(); ++i) {
      const auto &w = c.want_accessed[i];
      const auto &g = resp.original_accessed_tables(static_cast<int>(i));
      SCOPED_TRACE("accessed[" + std::to_string(i) + "] want " + w.original_database + "." + w.original_table);
      EXPECT_EQ(g.original_database(), w.original_database);
      EXPECT_EQ(g.original_table(), w.original_table);
      EXPECT_EQ(g.logical_database(), w.logical_database);
      EXPECT_EQ(g.physical_database(), w.physical_database);
      EXPECT_EQ(g.is_remote(), w.is_remote);
      EXPECT_EQ(g.is_storage_integrity(), w.is_storage_integrity);
    }
  }
  const std::string normalized_sql = si_corpus::NormalizeSIIdentifierQuotes(resp.sql_after_rewrite());
  if (c.reject) {
    EXPECT_EQ(resp.sql_after_rewrite(), c.sql) << "a rejected statement must echo the input SQL";
  } else {
    // Spec J D3: every non-reject case is compared exactly after the shared
    // normalization. There is no sql_exact opt-in; twelve of the thirteen
    // want_sql strings used to be dead here.
    EXPECT_EQ(normalized_sql, si_corpus::NormalizeSIIdentifierQuotes(c.ExpectedSQL()))
      << "SQL pin mismatch for " << c.name;
  }
  for (const auto &s : c.want_sql_contains)
    EXPECT_NE(normalized_sql.find(s), std::string::npos) << "missing " << s;
  for (const auto &s : c.want_sql_not_contains)
    EXPECT_EQ(normalized_sql.find(s), std::string::npos) << "unexpected " << s;
}

INSTANTIATE_TEST_SUITE_P(SpecG, StorageIntegrityGolden, ::testing::ValuesIn(loadSIGoldenCases()),
  [](const ::testing::TestParamInfo<SIGoldenCase> &info) { return info.param.name; });

} // namespace

// ============================================================================
// Table-reference hardening (housegate spec 2026-09-26; Plan B v2).
// ============================================================================

namespace {

// rewriter-go's tablerefDynamic: db1 → phys, phys known and protected, the
// three hg_* databases protected, logical context db1; `si` adds contract V2
// with db1.t Active.
rewriter::RewriteTableDynamicArgs *AddTableRefOption(rewriter::RewriteSQLRequest &req, bool si) {
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
  (*dyn->mutable_database_map())["db1"] = "phys";
  dyn->add_known_physical_databases("phys");
  dyn->set_upstream_logical_database_in_context("db1");
  dyn->set_delim("_");
  for (const char *p : {"phys", "hg_safe", "hg_unsafe", "hg_promote"}) dyn->add_protected_databases(p);
  if (si) {
    auto *args = dyn->mutable_storage_integrity();
    args->set_contract_version(rewriter::STORAGE_INTEGRITY_CONTRACT_V2);
    args->set_read_mode(rewriter::StorageIntegrityArgs::READ_MODE_SAFE);
    args->set_reserved_row_id_column("_hg_row_id");
    for (const char *r : {"hg_safe", "hg_unsafe", "hg_promote"}) args->add_reserved_databases(r);
    auto &t = (*args->mutable_tables())["db1.t"];
    t.set_safe_table("hg_safe.db1__t");
    t.set_unsafe_table("hg_unsafe.db1__t");
  }
  return dyn;
}

rewriter::RewriteSQLResponse RunTableRef(const std::string &sql, bool si = false) {
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);
  AddTableRefOption(req, si);
  return RunDirectRewrite(std::move(req));
}

}  // namespace

// Inventory §6 (P): the SI compatibility shim took a clause keyword followed
// by "(" for an implicit alias with a column list and cut the list out of the
// parse SQL, so ClickHouse's parser refused the rest.
TEST(StorageIntegrityCompatibility, ClauseKeywordIsNotAnImplicitAlias) {
  for (const char *sql : {
         "SHOW COLUMNS FROM o LIMIT (SELECT count() FROM `db2.x`)",
         "SHOW COLUMNS FROM o WHERE (SELECT count() FROM `db2.x`) = 2",
         "SHOW INDEX FROM o WHERE (SELECT count() FROM `db2.x`) = 2",
       }) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql, /*si=*/true);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), "storage-integrity logical database db1 is not directly addressable");
  }
  for (const char *sql : {"SELECT * FROM db1.o WHERE (a, b) IN (1, 2)",
                          "SELECT * FROM db1.o JOIN db1.p USING (a)"}) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql, /*si=*/true);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
  }
}

// Spec 2026-09-26 T2: an Identifier parameter in any database or table
// position is refused before any rewrite, whatever the SI state.
TEST(TableRefParam, RefusedInEveryTablePosition) {
  for (const bool si : {false, true}) {
    for (const char *sql : {
           "SELECT * FROM {p:Identifier}", "SELECT * FROM db1.{p:Identifier}",
           "SELECT * FROM {d:Identifier}.t", "SELECT * FROM db1.o AS a JOIN {p:Identifier} AS b USING (a)",
           // T2 precedes the SI namespace policy: the reserved-database JOIN
           // side must not upgrade the parameter message under an active surface.
           "SELECT * FROM {p:Identifier} JOIN hg_safe.db1__t USING (a)",
           "SELECT * FROM (SELECT * FROM {p:Identifier})",
           "SELECT * FROM db1.o WHERE a IN {p:Identifier}", "SELECT * FROM db1.o WHERE a IN (({p:Identifier}))",
           "SELECT * FROM db1.o WHERE a IN db1.{p:Identifier}", "SELECT * FROM db1.o WHERE in(a, {p:Identifier})",
           "SELECT * FROM db1.o WHERE a IN +{p:Identifier}",
           "INSERT INTO db1.o SELECT * FROM {p:Identifier}",
           "CREATE TABLE db1.n ENGINE = Memory AS SELECT * FROM {p:Identifier}",
           "CREATE MATERIALIZED VIEW db1.mv TO db1.{p:Identifier} AS SELECT * FROM db1.o",
           "CREATE MATERIALIZED VIEW {p:Identifier} ENGINE = Memory AS SELECT * FROM db1.o",
           "DROP TABLE {p:Identifier}", "DROP TABLE db1.{p:Identifier}",
           // A multi-table DROP keeps its targets in database_and_tables, not in
           // database/table; the parameter in any entry is T2, whatever the
           // other entries name (a protected database must not answer first).
           "DROP TABLE db1.t, db1.{p:Identifier}", "DROP TABLE phys.x, db1.{p:Identifier}",
           "DROP TABLE db1.{p:Identifier}, db1.t",
           "INSERT INTO db1.{p:Identifier} VALUES (1)", "CREATE TABLE {p:Identifier} (a UInt64) ENGINE = Memory",
           "EXISTS TABLE db1.{p:Identifier}", "SHOW CREATE TABLE db1.{p:Identifier}",
           "DESCRIBE TABLE db1.{p:Identifier}", "RENAME TABLE db1.{p:Identifier} TO db1.z",
           "SHOW TABLES FROM {d:Identifier}", "USE {d:Identifier}",
           "CREATE DATABASE {d:Identifier}", "DROP DATABASE {d:Identifier}",
           "ALTER TABLE db1.{p:Identifier} UPDATE a = 1 WHERE 1",
           "ALTER TABLE db1.o UPDATE a = {c:Identifier} WHERE 1",
           "ALTER TABLE db1.o UPDATE b = (SELECT max(a) FROM {p:Identifier}) WHERE 1",
           "ALTER TABLE db1.o DELETE WHERE a IN {p:Identifier}",
           "ALTER TABLE db1.mv MODIFY QUERY SELECT * FROM {p:Identifier}",
           "EXPLAIN SELECT * FROM {p:Identifier}",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest) << resp.message();
      EXPECT_EQ(resp.message(), rewriter_handlers::kIdentifierParameterMessage);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
      EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    }
  }
}

// The preflight itself, not a later handler, must answer T2 for a multi-table
// DROP: every later rule (T3 protected databases, ...) is stacked behind it.
TEST(TableRefParam, PreflightSeesEveryTableOfAMultiTableDrop) {
  for (const char *sql : {"DROP TABLE db1.t, db1.{p:Identifier}", "DROP TABLE phys.x, db1.{p:Identifier}",
                          "DROP TABLE db1.{p:Identifier}, db1.t", "DROP TABLE db1.t, {p:Identifier}"}) {
    SCOPED_TRACE(sql);
    const std::string text(sql);
    DB::ParserQuery parser(text.data() + text.size(), false);
    const char *pos = text.data();
    DB::ASTPtr ast = DB::parseQuery(parser, pos, text.data() + text.size(), "", 0, 0, 0);
    rewriter::RewriteSQLRequest req;
    req.set_sql(text);
    AddTableRefOption(req, false);
    rewriter::RewriteSQLResponse resp;
    EXPECT_TRUE(rewriter_handlers::preflightTableReferences(ast, &req, &resp));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
    EXPECT_EQ(resp.message(), rewriter_handlers::kIdentifierParameterMessage);
  }
  // A table-only multi-table DROP stays the handlers' business.
  const std::string ok = "DROP TABLE db1.t, db1.u";
  DB::ParserQuery parser(ok.data() + ok.size(), false);
  const char *pos = ok.data();
  DB::ASTPtr ast = DB::parseQuery(parser, pos, ok.data() + ok.size(), "", 0, 0, 0);
  rewriter::RewriteSQLRequest req;
  req.set_sql(ok);
  AddTableRefOption(req, false);
  rewriter::RewriteSQLResponse resp;
  EXPECT_FALSE(rewriter_handlers::preflightTableReferences(ast, &req, &resp));
}

TEST(TableRefParam, ValueAndColumnParametersStayAllowed) {
  for (const char *sql : {"SELECT * FROM db1.o WHERE a = {v:UInt64}", "SELECT {c:Identifier} FROM db1.o"}) {
    SCOPED_TRACE(sql);
    EXPECT_EQ(RunTableRef(sql).code(), rewriter::RewriteCode::Success);
  }
}

// v0.15.0 segfaulted on these: setTable("") erased the parameter target and
// the formatter dereferenced the missing table. Every mode refuses them now.
TEST(TableRefParam, ShowCreateAndExistsNeverReachTheFormatterWithAnEmptyName) {
  for (const char *sql : {"EXISTS TABLE db1.{p:Identifier}", "SHOW CREATE TABLE db1.{p:Identifier}",
                          "EXISTS TABLE {p:Identifier}", "SHOW CREATE TABLE {p:Identifier}"}) {
    SCOPED_TRACE(sql);
    const auto none = RunWriteOp(sql);
    EXPECT_EQ(none.code, rewriter::RewriteCode::InvalidRewriteRequest);
    EXPECT_EQ(none.message, rewriter_handlers::kIdentifierParameterMessage);
    rewriter::RewriteSQLRequest st;
    st.set_sql(sql);
    AddEmptyStaticOption(st);
    const auto static_resp = RunDirectRewrite(std::move(st));
    EXPECT_EQ(static_resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
    EXPECT_EQ(static_resp.message(), rewriter_handlers::kIdentifierParameterMessage);
    for (const bool si : {false, true}) {
      const auto dynamic = RunTableRef(sql, si);
      EXPECT_EQ(dynamic.code(), rewriter::RewriteCode::InvalidRewriteRequest);
      EXPECT_EQ(dynamic.message(), rewriter_handlers::kIdentifierParameterMessage);
      EXPECT_EQ(dynamic.sql_after_rewrite(), sql);
    }
  }
}

// Spec 2026-09-26 T7: every unmodelled statement class is refused, after
// T2 and before T3, with the SI catch-all while the surface is active; a
// session SET is the one pass-through, and only while it is inactive.
TEST(TableRefClasses, UnmodelledClassesAreRefused) {
  for (const char *sql : {"SYSTEM RELOAD CONFIG", "EXPLAIN SELECT * FROM db1.o", "CHECK TABLE db1.o",
                          "CREATE USER u1", "KILL QUERY WHERE query_id = 'x'", "DETACH TABLE phys.x",
                          "OPTIMIZE TABLE phys.x", "ATTACH TABLE db1.x",
                          "DESCRIBE (SELECT * FROM phys.`db2.x`)"}) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
  }
  for (const char *sql : {"SYSTEM RELOAD CONFIG", "EXPLAIN SELECT * FROM db1.o", "KILL QUERY WHERE query_id = 'x'",
                          "DETACH TABLE db1.o", "ATTACH TABLE db1.x", "OPTIMIZE TABLE phys.x",
                          "SET max_threads = 1"}) {
    SCOPED_TRACE(std::string(sql) + " [V2]");
    const auto resp = RunTableRef(sql, /*si=*/true);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), rewriter_handlers::kStorageIntegrityUnmodelledMessage);
  }
  // The DETACH / ATTACH / OPTIMIZE / UNDROP target is reported (rewriter-go
  // RawTableRefs).
  const auto detach = RunTableRef("DETACH TABLE db1.o");
  ASSERT_EQ(detach.original_accessed_tables_size(), 1);
  EXPECT_EQ(detach.original_accessed_tables(0).original_table(), "o");
  EXPECT_EQ(detach.original_accessed_tables(0).physical_database(), "phys");

  const auto set = RunTableRef("SET max_threads = 1, max_block_size = 'a', use_query_cache = true");
  EXPECT_EQ(set.code(), rewriter::RewriteCode::Success) << set.message();
  EXPECT_EQ(set.message(), "success");
  EXPECT_EQ(set.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
  EXPECT_EQ(set.sql_after_rewrite(), "SET max_threads = 1, max_block_size = 'a', use_query_cache = true");
  // Mode None keeps the legacy pass-through for callers without rewrite options.
  EXPECT_EQ(RunWriteOp("SYSTEM RELOAD CONFIG").code, rewriter::RewriteCode::Success);
}

// Classes whose ClickHouse AST reports QueryKind::None but that rewriter-go
// models (measured against rewriter-go 248ecf2): the target-less SHOW forms and
// DESCRIBE FILESYSTEM CACHE are not unmodelled classes (T7). Since the
// system-table allowlist (Task I2, rewriter-go v0.15.0) SHOW PROCESSLIST /
// ACCESS read refused system tables and DESCRIBE FILESYSTEM CACHE is refused;
// ENGINES and PRIVILEGES keep the inactive-surface pass-through.
TEST(TableRefClasses, QueryKindNoneShowFormsAreAdmittedWhenInactive) {
  for (const char *sql : {"SHOW ENGINES", "SHOW PRIVILEGES", "SHOW ENGINES INTO OUTFILE 'x'",
                          "SHOW PRIVILEGES FORMAT JSON"}) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    EXPECT_EQ(resp.message(), "success");
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
    EXPECT_EQ(resp.original_accessed_tables_size(), 0);
  }
  for (const auto &[sql, table] : std::vector<std::pair<std::string, std::string>>{
         {"SHOW PROCESSLIST", "processes"}, {"SHOW PROCESSLIST FORMAT JSON", "processes"},
         {"SHOW ACCESS", "users"}, {"SHOW ACCESS FORMAT JSON", "users"}}) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), "system table system." + table + " is not accessible");
  }
  // DESCRIBE FILESYSTEM CACHE reads the cache registry
  // (system.filesystem_cache_settings); rewriter-go v0.15.0 refuses it.
  for (const char *sql : {"DESCRIBE FILESYSTEM CACHE 'c1'", "DESC FILESYSTEM CACHE 'c1'",
                          "DESCRIBE FILESYSTEM CACHE 'c1' FORMAT JSON"}) {
    for (const bool si : {false, true}) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
    }
  }
  // The admitted set is explicit: neighbours that rewriter-go refuses stay
  // refused, whatever the surface.
  for (const char *sql : {"WATCH db1.lv", "EXECUTE AS u1 SELECT 1", "BEGIN TRANSACTION", "CREATE WORKLOAD w1",
                          "CHECK ALL TABLES"}) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
  }
  // Surface active, as rewriter-go v0.15.0: ENGINES passes, PRIVILEGES is the
  // SI catch-all, PROCESSLIST / ACCESS read refused system tables.
  EXPECT_EQ(RunTableRef("SHOW ENGINES", /*si=*/true).code(), rewriter::RewriteCode::Success);
  const auto privileges = RunTableRef("SHOW PRIVILEGES", /*si=*/true);
  EXPECT_EQ(privileges.code(), rewriter::RewriteCode::UnsupportedStatement) << privileges.message();
  EXPECT_EQ(privileges.message(), rewriter_handlers::kStorageIntegrityUnmodelledMessage);
  EXPECT_EQ(RunTableRef("SHOW PROCESSLIST", /*si=*/true).message(), "system table system.processes is not accessible");
  EXPECT_EQ(RunTableRef("SHOW ACCESS", /*si=*/true).message(), "system table system.users is not accessible");
}

// Spec 2026-09-26 T3: a protected database named in a read position is
// refused. While the SI surface is active an SI physical or reserved name is
// left to the SI handlers, whose messages keep precedence.
TEST(TableRefProtected, RefusedInReadPositions) {
  for (const char *db : {"phys", "hg_safe", "hg_unsafe", "hg_promote"}) {
    for (const char *shape : {
           "SELECT * FROM %s.`db2.x`", "SELECT * FROM db1.o AS a JOIN %s.`db2.x` AS b USING (a)",
           "SELECT * FROM (SELECT * FROM %s.`db2.x`)", "WITH c AS (SELECT * FROM %s.`db2.x`) SELECT * FROM c",
           "SELECT * FROM db1.o WHERE a IN %s.`db2.x`", "SELECT * FROM db1.o WHERE a IN ((%s.`db2.x`))",
           "SELECT * FROM db1.o WHERE (a, b) IN %s.`db2.x`", "SELECT * FROM db1.o WHERE in(a, %s.`db2.x`)",
           "SELECT * FROM db1.o WHERE a GLOBAL IN %s.`db2.x`", "INSERT INTO db1.o SELECT * FROM %s.`db2.x`",
           "CREATE TABLE db1.n ENGINE = Memory AS SELECT * FROM %s.`db2.x`",
           "CREATE VIEW db1.v AS SELECT * FROM %s.`db2.x`", "DESCRIBE TABLE %s.`db2.x`",
           "ALTER TABLE db1.o DELETE WHERE a IN %s.`db2.x`",
           "ALTER TABLE db1.o UPDATE b = (SELECT count() FROM %s.`db2.x`) WHERE 1",
           "CREATE TABLE db1.n (a UInt64 DEFAULT (SELECT max(a) FROM %s.`db2.x`)) ENGINE = Memory",
           "SHOW COLUMNS FROM o WHERE (SELECT count() FROM %s.`db2.x`) = 2",
         }) {
      std::string sql = shape;
      sql.replace(sql.find("%s"), 2, db);
      for (const bool si : {false, true}) {
        SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
        const auto resp = RunTableRef(sql, si);
        EXPECT_NE(resp.code(), rewriter::RewriteCode::Success) << resp.message();
        EXPECT_EQ(resp.sql_after_rewrite(), sql);
        if (!si || std::string(db) == "phys") {
          EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
          EXPECT_EQ(resp.message(), std::string("protected database ") + db + " is not addressable");
        } else {
          EXPECT_NE(resp.message().find("storage-integrity"), std::string::npos) << resp.message();
        }
      }
    }
  }
}

// Carried from the Task 3 re-review: T2 (query parameters) beats T3 (protected
// databases) even when the protected table comes first in a multi-table DROP.
TEST(TableRefProtected, T2BeatsT3InAMultiTableDrop) {
  for (const char *sql : {"DROP TABLE phys.x, db1.{p:Identifier}", "DROP TABLE hg_safe.db1__t, db1.{p:Identifier}"}) {
    for (const bool si : {false, true}) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
      EXPECT_EQ(resp.message(), rewriter_handlers::kIdentifierParameterMessage);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
  }
}

// Carried from the Task 2 review: an IN operand over a protected name, in both
// the paren-less and the tuple forms, is refused exactly as rewriter-go
// answers (measured against cmd/rewrite): T3 while the surface is inactive,
// and for `phys` (not an SI object) also while it is active; the SI message
// for an SI physical name while it is active.
TEST(TableRefProtected, InOperandOverProtectedNameIsRefusedInTupleAndParenLessForms) {
  for (const char *lhs : {"a", "(a, b)"}) {
    for (const char *op : {"IN", "NOT IN", "GLOBAL IN"}) {
      for (const char *operand : {"%s", "(%s)", "((%s))"}) {
        for (const char *name : {"phys.x", "hg_safe.db1__t", "hg_unsafe.db1__t"}) {
          std::string spelled = operand;
          spelled.replace(spelled.find("%s"), 2, name);
          const std::string sql = std::string("SELECT * FROM db1.o WHERE ") + lhs + " " + op + " " + spelled;
          const std::string db = std::string(name).substr(0, std::string(name).find('.'));
          for (const bool si : {false, true}) {
            SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
            const auto resp = RunTableRef(sql, si);
            EXPECT_EQ(resp.sql_after_rewrite(), sql);
            if (!si || db == "phys") {
              EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
              EXPECT_EQ(resp.message(), "protected database " + db + " is not addressable");
            } else {
              EXPECT_EQ(resp.code(), rewriter::RewriteCode::RewriteError);
              EXPECT_EQ(resp.message(), "storage-integrity physical table " + std::string(name) +
                                            " is not directly addressable");
            }
          }
        }
      }
    }
  }
}

TEST(TableRefProtected, ColumnAliasAndLiteralUsesAreAllowed) {
  for (const char *sql : {"SELECT phys FROM db1.o", "SELECT a AS hg_safe FROM db1.o",
                          "SELECT * FROM db1.o WHERE s = 'phys'", "SELECT o.a FROM db1.o AS o"}) {
    SCOPED_TRACE(sql);
    EXPECT_EQ(RunTableRef(sql).code(), rewriter::RewriteCode::Success);
  }
}

TEST(TableRefProtected, ProtectedLogicalContextIsRefused) {
  rewriter::RewriteSQLRequest req;
  req.set_sql("SELECT * FROM o");
  AddTableRefOption(req, false)->set_upstream_logical_database_in_context("phys");
  const auto resp = RunDirectRewrite(std::move(req));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
  EXPECT_EQ(resp.message(), "protected database phys is not addressable");
}

TEST(TableRefProtected, EntriesMustBeSimpleIdentifiers) {
  rewriter::RewriteSQLRequest req;
  req.set_sql("SELECT 1");
  AddTableRefOption(req, false)->add_protected_databases("hg-promote");
  const auto resp = RunDirectRewrite(std::move(req));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
  EXPECT_EQ(resp.message(), "protected_databases entry \"hg-promote\" must be a simple identifier");
  EXPECT_EQ(resp.sql_after_rewrite(), "SELECT 1");
}

// A protected known_physical_databases entry is no longer a pass-through
// logical name (rewriter-proto v0.4.0 protected_databases comment).
TEST(TableRefProtected, ProtectedKnownPhysicalDatabaseIsNotAPassThrough) {
  rewriter::RewriteSQLRequest req;
  AddTableRefOption(req, false);
  const auto &args = req.options(0).table_name_args().dynamic_args();
  EXPECT_FALSE(rewriter_handlers::resolvePhysicalDatabase("phys", args).has_value());
  EXPECT_EQ(rewriter_handlers::resolvePhysicalDatabase("db1", args), std::optional<std::string>("phys"));
}

// Spec 2026-09-26 T3: a protected database named as a statement target is
// refused instead of being double-prefixed (``phys.`phys.x` ``) or passed
// through (`USE phys`).
TEST(TableRefProtected, RefusedInStatementTargets) {
  for (const char *db : {"phys", "hg_safe", "hg_unsafe", "hg_promote"}) {
    for (const char *shape : {
           "INSERT INTO %s.`db2.x` VALUES (1)", "DROP TABLE %s.`db2.x`",
           "CREATE TABLE %s.`db2.x` (a UInt64) ENGINE = Memory", "CREATE TABLE db1.n AS %s.`db2.x`",
           "RENAME TABLE %s.`db2.x` TO db1.z", "EXISTS TABLE %s.`db2.x`", "SHOW CREATE TABLE %s.`db2.x`",
           "SHOW TABLES FROM %s", "USE %s", "CREATE DATABASE %s", "DROP DATABASE %s",
           "CREATE MATERIALIZED VIEW db1.mv TO %s.`db2.x` AS SELECT * FROM db1.o",
           "ALTER TABLE db1.o REPLACE PARTITION tuple() FROM %s.`db2.x`",
         }) {
      std::string sql = shape;
      sql.replace(sql.find("%s"), 2, db);
      for (const bool si : {false, true}) {
        SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
        const auto resp = RunTableRef(sql, si);
        EXPECT_NE(resp.code(), rewriter::RewriteCode::Success) << resp.message();
        EXPECT_EQ(resp.sql_after_rewrite(), sql);
        if (!si || std::string(db) == "phys") {
          EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
          EXPECT_EQ(resp.message(), std::string("protected database ") + db + " is not addressable");
          ASSERT_EQ(resp.original_accessed_tables_size(), 1);
          EXPECT_EQ(resp.original_accessed_tables(0).original_database(), db);
          EXPECT_EQ(resp.original_accessed_tables(0).physical_database(), "");
        } else {
          EXPECT_NE(resp.message().find("storage-integrity"), std::string::npos) << resp.message();
        }
      }
    }
  }
}

// Spec 2026-09-26 T3 precedes T5 and T6: a protected database named in a
// table-function, table-engine or string-lookup argument is refused as
// protected. A lookup argument is SI-handler-blind, so it is refused as
// protected even while the SI surface is active.
TEST(TableRefProtected, RefusedInCarrierArguments) {
  struct Shape { const char *sql; bool blind; };
  for (const char *db : {"phys", "hg_safe", "hg_unsafe", "hg_promote"}) {
    for (const Shape shape : {
           Shape{"SELECT * FROM merge('%s', 'db2')", false},
           Shape{"SELECT * FROM remote('127.0.0.1:9000', '%s', 'db2.x')", false},
           Shape{"CREATE TABLE db1.n (a UInt64) ENGINE = Merge('%s', '^db2')", false},
           Shape{"SELECT joinGet('%s.`db2.x`', 'v', 1)", true},
           Shape{"SELECT dictGet('%s.`db2.x`', 'v', 1)", true},
           Shape{"SELECT hasColumnInTable('%s', 'db2', 'v')", true},
           Shape{"SELECT hasColumnInTable('h', '%s', 'db2', 'v')", true},
           Shape{"SELECT * FROM remoteSecure('127.0.0.1:9440', '%s', 'db2.x')", false},
           Shape{"SELECT * FROM cluster('c', '%s', 'x')", false},
           Shape{"SELECT * FROM clusterAllReplicas('c', '%s', 'x')", false},
           Shape{"SELECT * FROM dictionary('%s.x')", false},
           Shape{"CREATE TABLE db1.n (a UInt64) ENGINE = Buffer('%s', 'x', 1, 1, 1, 1, 1, 1, 1)", false},
           Shape{"CREATE TABLE db1.n (a UInt64) ENGINE = Distributed('c', '%s', 'x')", false},
           // A MATERIALIZED VIEW keeps its ENGINE in the view targets, not in
           // ASTCreateQuery::storage.
           Shape{"CREATE MATERIALIZED VIEW db1.mv ENGINE = Merge('%s', '^x') AS SELECT 1 AS a", false},
           Shape{"CREATE MATERIALIZED VIEW db1.mv ENGINE = Distributed('c', '%s', 'x') AS SELECT 1 AS a", false},
           Shape{"CREATE MATERIALIZED VIEW db1.mv ENGINE = Buffer('%s', 'x', 1, 1, 1, 1, 1, 1, 1) AS SELECT 1 AS a", false},
           // Function names are matched case-insensitively.
           Shape{"SELECT JOINGET('%s.`db2.x`', 'v', 1)", true},
           Shape{"SELECT DictGetString('%s.x', 'v', 1)", true},
           Shape{"SELECT dictHas('%s.x', 1)", true},
           Shape{"SELECT dictGetOrNull('%s.x', 'v', 1)", true},
         }) {
      std::string sql = shape.sql;
      sql.replace(sql.find("%s"), 2, db);
      for (const bool si : {false, true}) {
        SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
        const auto resp = RunTableRef(sql, si);
        EXPECT_NE(resp.code(), rewriter::RewriteCode::Success) << resp.message();
        EXPECT_EQ(resp.sql_after_rewrite(), sql);
        if (!si || shape.blind || std::string(db) == "phys") {
          EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
          EXPECT_EQ(resp.message(), std::string("protected database ") + db + " is not addressable");
          if (si && std::string(db) != "phys") {
            // A blind SI physical name is still reported as the SI object.
            ASSERT_EQ(resp.original_accessed_tables_size(), 1);
            EXPECT_TRUE(resp.original_accessed_tables(0).is_storage_integrity());
            EXPECT_EQ(resp.original_accessed_tables(0).physical_database(), db);
          }
        } else {
          EXPECT_NE(resp.message().find("storage-integrity"), std::string::npos) << resp.message();
        }
      }
    }
  }
}

// A logical database name in a carrier argument is not a T3 position: the
// protected check must leave it to the later reference checks (rewriter-go
// answers `joinGet target "db1.t" does not resolve` and `merge` a rejection
// of its own, neither of which is the protected-database message).
TEST(TableRefProtected, LogicalNameInCarrierArgumentPassesT3) {
  for (const char *sql : {"SELECT joinGet('db1.t', 'v', 1)", "SELECT joinGet('db2.x', 'v', 1)",
         "SELECT merge('db1', 'x')", "SELECT * FROM merge('db1', '^x')"}) {
    for (const bool si : {false, true}) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.message().find("protected database"), std::string::npos) << resp.message();
    }
  }
}

// An unquoted qualified identifier argument of a string lookup names its
// database structurally (rewriter-go qualifiedColumnArgTarget), and is just as
// SI-handler-blind as the literal form.
TEST(TableRefProtected, RefusedInLookupIdentifierArgument) {
  for (const char *db : {"phys", "hg_safe"}) for (const char *fn : {"joinGet", "dictGet"}) {
    const std::string sql = std::string("SELECT ") + fn + "(" + db + ".x, 'v', 1)";
    for (const bool si : {false, true}) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
      EXPECT_EQ(resp.message(), std::string("protected database ") + db + " is not addressable");
    }
  }
}

// Spec 2026-09-26 T4: an IN operand is a read source like FROM — rewritten,
// reported, and read through the derived safe read when Active. The order is
// key-sorted by the name as written (§5 "What the engines report").
TEST(TableRefSources, InOperandsAreRewrittenAndReported) {
  using Pairs = std::vector<std::pair<std::string, std::string>>;
  struct Case { const char *sql; const char *want_sql; Pairs accessed; bool si = false; };
  const std::vector<Case> cases = {
    {"SELECT * FROM db1.o WHERE a IN db1.p",
     "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE a IN (phys.`db1.p`)", {{"db1", "o"}, {"db1", "p"}}},
    {"SELECT * FROM db1.o WHERE a IN ((db1.p))",
     "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE a IN (phys.`db1.p`)", {{"db1", "o"}, {"db1", "p"}}},
    {"SELECT * FROM db1.o WHERE (a, b) IN db1.p",
     "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE (a, b) IN (phys.`db1.p`)", {{"db1", "o"}, {"db1", "p"}}},
    {"SELECT * FROM db1.o WHERE in(a, db1.p)",
     "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE in(a, phys.`db1.p`)", {{"db1", "o"}, {"db1", "p"}}},
    {"SELECT * FROM db1.o WHERE a GLOBAL IN db1.p",
     "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE a GLOBAL IN (phys.`db1.p`)", {{"db1", "o"}, {"db1", "p"}}},
    {"SELECT * FROM db1.o WHERE a IN p",
     "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE a IN (phys.`db1.p`)", {{"db1", "o"}, {"", "p"}}},
    {"SELECT * FROM db1.o WHERE a IN `db2.x`",
     "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE a IN (phys.`db1.db2.x`)", {{"db1", "o"}, {"", "db2.x"}}},
    {"SELECT * FROM db1.o WHERE a IN (b)",
     "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE a IN (phys.`db1.b`)", {{"", "b"}, {"db1", "o"}}},
    {"WITH c AS (SELECT 1 AS a) SELECT * FROM db1.o WHERE a IN c",
     "WITH c AS (SELECT 1 AS a) SELECT * FROM phys.`db1.o` AS `db1.o` WHERE a IN (c)", {{"db1", "o"}}},
    {"SELECT * FROM db1.o WHERE a IN system.tables",
     "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE a IN (system.tables)", {{"db1", "o"}, {"system", "tables"}}},
    {"SELECT * FROM db1.o WHERE a IN db1.t",
     "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE a IN (SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t)",
     {{"db1", "o"}, {"db1", "t"}}, true},
  };
  for (const auto &c : cases) {
    SCOPED_TRACE(c.sql);
    const auto resp = RunTableRef(c.sql, c.si);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    EXPECT_EQ(resp.sql_after_rewrite(), c.want_sql);
    Pairs got;
    for (const auto &a : resp.original_accessed_tables()) got.emplace_back(a.original_database(), a.original_table());
    EXPECT_EQ(got, c.accessed);
  }
}

// Preflight finding B4: decodeInFunction and the embedded SI read stream are
// unchanged, so while the SI surface is active every position outside the
// SELECT root (write bodies, views, mutations, column and ALTER expressions)
// still refuses an SI operand, and the SELECT root still refuses an SI
// physical one.
TEST(TableRefSources, InOperandChangeKeepsStorageIntegrityRefusals) {
  const std::string safe = "storage-integrity physical table hg_safe.db1__t is not directly addressable";
  struct Case { const char *sql; rewriter::RewriteCode code; std::string message; };
  const std::vector<Case> cases = {
    {"SELECT * FROM db1.o WHERE a IN hg_safe.db1__t", rewriter::RewriteCode::RewriteError, safe},
    {"SELECT * FROM db1.o WHERE a IN ((hg_safe.db1__t))", rewriter::RewriteCode::RewriteError, safe},
    {"SELECT * FROM db1.o WHERE in(a, (hg_safe.db1__t))", rewriter::RewriteCode::RewriteError, safe},
    {"SELECT * FROM db1.o WHERE a GLOBAL IN ((hg_unsafe.db1__t))", rewriter::RewriteCode::RewriteError,
     "storage-integrity physical table hg_unsafe.db1__t is not directly addressable"},
    {"SELECT * FROM db1.o WHERE a NOT IN ((hg_promote.x))", rewriter::RewriteCode::RewriteError,
     "storage-integrity physical table hg_promote.x is not directly addressable"},
    {"ALTER TABLE db1.o DELETE WHERE a IN hg_safe.db1__t", rewriter::RewriteCode::UnsupportedStatement, safe},
    {"ALTER TABLE db1.o UPDATE b = 1 WHERE a IN ((hg_safe.db1__t))", rewriter::RewriteCode::UnsupportedStatement, safe},
    {"DELETE FROM db1.o WHERE a IN ((hg_safe.db1__t))", rewriter::RewriteCode::UnsupportedStatement, safe},
    {"UPDATE db1.o SET b = 1 WHERE a IN ((hg_safe.db1__t))", rewriter::RewriteCode::UnsupportedStatement, safe},
    {"INSERT INTO db1.o SELECT * FROM db1.p WHERE a IN ((hg_safe.db1__t))", rewriter::RewriteCode::UnsupportedStatement, safe},
    {"CREATE TABLE db1.n ENGINE = Memory AS SELECT * FROM db1.p WHERE a IN ((hg_safe.db1__t))",
     rewriter::RewriteCode::UnsupportedStatement, safe},
    {"CREATE VIEW db1.v AS SELECT * FROM db1.p WHERE a IN ((hg_safe.db1__t))", rewriter::RewriteCode::UnsupportedStatement, safe},
    {"CREATE MATERIALIZED VIEW db1.mv TO db1.o AS SELECT * FROM db1.p WHERE a IN ((hg_safe.db1__t))",
     rewriter::RewriteCode::UnsupportedStatement, safe},
    {"CREATE TABLE db1.n (a UInt64, b UInt8 DEFAULT a IN ((hg_safe.db1__t))) ENGINE = Memory",
     rewriter::RewriteCode::UnsupportedStatement, safe},
    {"ALTER TABLE db1.o MODIFY COLUMN b UInt8 DEFAULT a IN ((hg_safe.db1__t))", rewriter::RewriteCode::UnsupportedStatement, safe},
    {"ALTER TABLE db1.o MODIFY TTL d + INTERVAL 1 DAY DELETE WHERE a IN ((hg_safe.db1__t))",
     rewriter::RewriteCode::UnsupportedStatement, safe},
    // An Active logical operand outside a rewritten SELECT body keeps its refusal.
    {"ALTER TABLE db1.o DELETE WHERE a IN db1.t", rewriter::RewriteCode::UnsupportedStatement,
     "storage-integrity table db1.t is not directly addressable through IN table target"},
    {"CREATE VIEW db1.v AS SELECT * FROM db1.p WHERE a IN db1.t", rewriter::RewriteCode::UnsupportedStatement,
     "storage-integrity table db1.t is not directly addressable through IN table target"},
  };
  for (const auto &c : cases) {
    SCOPED_TRACE(c.sql);
    const auto resp = RunTableRef(c.sql, /*si=*/true);
    EXPECT_EQ(resp.code(), c.code) << resp.message();
    EXPECT_EQ(resp.message(), c.message);
    EXPECT_EQ(resp.sql_after_rewrite(), c.sql);
  }
}

// Review C1 (task 8): the collected sources are keyed by identity, not by the
// name as written. A one-part quoted operand `hg_safe.db1__t` and the
// two-part FROM / IN `hg_safe.db1__t` share their spelling but not their
// meaning, so neither may erase the other: the two-part one is the SI physical
// table and must be refused, and every distinct table must be reported.
TEST(TableRefSources, QuotedTwinDoesNotHideADistinctReference) {
  for (const char *db : {"hg_safe", "hg_unsafe"}) {
    const std::string name = std::string(db) + ".db1__t";
    const std::string quoted = "`" + name + "`";
    for (const std::string sql : {
           "SELECT a IN " + quoted + " FROM " + name,
           "SELECT * FROM db1.o WHERE a IN " + quoted + " OR a IN " + name,
           "SELECT * FROM db1.o WHERE a IN " + name + " OR a IN " + quoted,
           "SELECT in(a, " + quoted + "), in(a, " + name + ") FROM db1.o",
         }) {
      SCOPED_TRACE(sql);
      const auto v2 = RunTableRef(sql, true);
      EXPECT_EQ(v2.code(), rewriter::RewriteCode::RewriteError) << v2.sql_after_rewrite();
      EXPECT_EQ(v2.message(), "storage-integrity physical table " + name + " is not directly addressable");
      EXPECT_EQ(v2.sql_after_rewrite(), sql);
      const auto off = RunTableRef(sql, false);
      EXPECT_EQ(off.code(), rewriter::RewriteCode::InvalidRewriteRequest) << off.sql_after_rewrite();
      EXPECT_EQ(off.message(), std::string("protected database ") + db + " is not addressable");
    }
  }
  {
    const std::string sql = "SELECT a IN `hg_promote.x` FROM hg_promote.x";
    const auto v2 = RunTableRef(sql, true);
    EXPECT_EQ(v2.code(), rewriter::RewriteCode::RewriteError) << v2.sql_after_rewrite();
    EXPECT_EQ(v2.message(), "storage-integrity physical table hg_promote.x is not directly addressable");
    const auto both = RunTableRef("SELECT * FROM db1.o WHERE a IN `hg_promote.x` OR a IN hg_promote.x", true);
    EXPECT_EQ(both.code(), rewriter::RewriteCode::RewriteError) << both.sql_after_rewrite();
    EXPECT_EQ(both.message(), "storage-integrity physical table hg_promote.x is not directly addressable");
  }

  // The lone quoted spelling is one unqualified table of the session's logical
  // database (rewriter-go decodeInOperand), so it is rewritten to that table:
  // the physical `hg_safe.db1__t` is never read through it.
  for (const bool si : {false, true}) {
    const auto lone = RunTableRef("SELECT * FROM db1.o WHERE a IN `hg_safe.db1__t`", si);
    EXPECT_EQ(lone.code(), rewriter::RewriteCode::Success) << lone.message();
    EXPECT_EQ(lone.sql_after_rewrite(),
              "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE a IN (phys.`db1.hg_safe.db1__t`)");
  }

  // Every distinct table is reported, in rewriter-go's order (written name,
  // then database, then table; measured against rewriter-go 584965a).
  using Pairs = std::vector<std::pair<std::string, std::string>>;
  struct Case { const char *sql; Pairs accessed; bool si_flag_on_db1_t; };
  for (const bool si : {false, true}) {
    for (const auto &c : std::vector<Case>{
           {"SELECT a IN `db1.t` FROM db1.t", {{"", "db1.t"}, {"db1", "t"}}, true},
           {"SELECT * FROM db1.o WHERE a IN `db1.t` AND b IN db1.t",
            {{"db1", "o"}, {"", "db1.t"}, {"db1", "t"}}, true},
           {"SELECT a IN `default.x` FROM default.x", {{"", "default.x"}, {"default", "x"}}, false},
           {"SELECT a IN db1.t FROM db1.t", {{"db1", "t"}}, true},
         }) {
      SCOPED_TRACE(std::string(c.sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(c.sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      Pairs got;
      bool si_flag = false;
      for (const auto &a : resp.original_accessed_tables()) {
        got.emplace_back(a.original_database(), a.original_table());
        if (a.original_database() == "db1" && a.original_table() == "t") si_flag = a.is_storage_integrity();
      }
      EXPECT_EQ(got, c.accessed);
      EXPECT_EQ(si_flag, si && c.si_flag_on_db1_t);
    }
  }
  // A refused twin reports both spellings too, the SI one flagged.
  {
    const auto resp = RunTableRef("SELECT a IN `hg_safe.db1__t` FROM hg_safe.db1__t", true);
    ASSERT_EQ(resp.original_accessed_tables_size(), 2);
    EXPECT_EQ(resp.original_accessed_tables(0).original_database(), "");
    EXPECT_EQ(resp.original_accessed_tables(0).original_table(), "hg_safe.db1__t");
    EXPECT_FALSE(resp.original_accessed_tables(0).is_storage_integrity());
    EXPECT_EQ(resp.original_accessed_tables(1).original_database(), "hg_safe");
    EXPECT_EQ(resp.original_accessed_tables(1).original_table(), "db1__t");
    EXPECT_TRUE(resp.original_accessed_tables(1).is_storage_integrity());
  }
}

// Fix round 2 (N1): a one-part IN operand reads through the current database.
// When that database is not resolvable (empty, unmapped `db9`) or is an SI
// physical one, the operand is refused before any forward, in every form —
// never forwarded verbatim to resolve in whatever database the session is in.
// Answers measured against rewriter-go (fix/in-operand-alias).
TEST(TableRefSources, OnePartInOperandNeedsAResolvableContext) {
  const std::vector<std::string> operands = {"t", "db1__t", "`hg_safe.db1__t`", "`db1.t`", "p"};
  struct Form { const char *pattern; const char *surface; };
  const std::vector<Form> forms = {
    {"SELECT * FROM db1.o WHERE a IN %", "IN"},
    {"SELECT * FROM db1.o WHERE a IN ((%))", "IN"},
    {"SELECT * FROM db1.o WHERE a IN (% AS z)", "IN"},
    {"SELECT * FROM db1.o WHERE a NOT IN %", "NOT IN"},
    {"SELECT * FROM db1.o WHERE a GLOBAL IN %", "GLOBAL IN"},
    {"SELECT * FROM db1.o WHERE a GLOBAL NOT IN (% AS z)", "GLOBAL NOT IN"},
    {"SELECT * FROM db1.o WHERE in(a, %)", "IN"},
    {"SELECT * FROM db1.o WHERE notIn(a, %)", "NOT IN"},
    {"SELECT * FROM db1.o WHERE globalIn(a, % AS z)", "GLOBAL IN"},
    {"SELECT * FROM db1.o WHERE globalNotIn(a, %)", "GLOBAL NOT IN"},
    {"SELECT * FROM db1.o WHERE inIgnoreSet(a, %)", "IN"},
    {"SELECT * FROM db1.o WHERE nullIn(a, %)", "NULL IN"},
    {"SELECT * FROM db1.o WHERE (a, b) IN %", "IN"},
  };
  const auto build = [](const std::string &pattern, const std::string &operand) {
    const auto at = pattern.find('%');
    return pattern.substr(0, at) + operand + pattern.substr(at + 1);
  };
  const auto run = [](const std::string &sql, const std::string &logical) {
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    auto *dyn = AddTableRefOption(req, true);
    dyn->set_upstream_logical_database_in_context(logical);
    return RunDirectRewrite(std::move(req));
  };
  for (const std::string logical : {"", "db9"}) for (const auto &operand : operands) for (const auto &form : forms) {
    const std::string sql = build(form.pattern, operand);
    SCOPED_TRACE(sql + " [context '" + logical + "']");
    const auto resp = run(sql, logical);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::RewriteError) << resp.sql_after_rewrite();
    EXPECT_EQ(resp.message(),
              std::string("storage-integrity ") + form.surface + " table target namespace is not statically resolvable");
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
    // The three SI physical databases are recorded, as rewriter-go does.
    std::set<std::string> recorded;
    for (const auto &a : resp.original_accessed_tables()) recorded.insert(a.original_database());
    for (const char *db : {"hg_safe", "hg_unsafe", "hg_promote"}) EXPECT_TRUE(recorded.count(db)) << db;
  }
  // An SI physical execution context names the physical table the operand
  // would read (measured: hg_safe.<whole name>), accessed as (hg_safe, name).
  for (const auto &[operand, physical] : std::vector<std::pair<std::string, std::string>>{
         {"t", "hg_safe.t"}, {"db1__t", "hg_safe.db1__t"}, {"`hg_safe.db1__t`", "hg_safe.hg_safe.db1__t"}}) {
    const std::string sql = "SELECT * FROM db1.o WHERE a IN " + operand;
    SCOPED_TRACE(sql + " [physical context hg_safe]");
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    AddTableRefOption(req, true)->set_upstream_physical_database_in_context("hg_safe");
    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::RewriteError) << resp.sql_after_rewrite();
    EXPECT_EQ(resp.message(), "storage-integrity physical table " + physical + " is not directly addressable");
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
    ASSERT_EQ(resp.original_accessed_tables_size(), 2);
    EXPECT_EQ(resp.original_accessed_tables(0).original_table(), "o");
    EXPECT_EQ(resp.original_accessed_tables(1).original_database(), "hg_safe");
    EXPECT_TRUE(resp.original_accessed_tables(1).is_storage_integrity());
  }
  // A resolvable context still reads (Active table through the derived read).
  for (const auto &operand : {"t", "p"}) {
    const auto resp = run(std::string("SELECT * FROM db1.o WHERE a IN ") + operand, "db1");
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
  }
}

// Review OOS (round 2): a one-part quoted name is one table of the current
// database, wherever it appears in a SELECT root — never split at its dot. C++
// used to split `db2.x` into (db2, x) and, db2 being unmapped, forward it
// verbatim, where ClickHouse resolves it in the physical database: tenant db2's
// table. Answers measured against rewriter-go.
TEST(TableRefSources, OnePartQuotedNameIsOneTableOfTheCurrentDatabase) {
  using Pairs = std::vector<std::pair<std::string, std::string>>;
  struct Case { const char *sql; std::string want; Pairs accessed; };
  for (const bool si : {false, true}) {
    for (const auto &c : std::vector<Case>{
           {"SELECT * FROM `db2.x`", "SELECT * FROM phys.`db1.db2.x` AS `db2.x`", {{"", "db2.x"}}},
           {"SELECT * FROM db1.o JOIN `db2.x` ON db1.o.a = 1",
            "SELECT * FROM phys.`db1.o` AS `db1.o` INNER JOIN phys.`db1.db2.x` AS `db2.x` ON db1.o.a = 1",
            {{"db1", "o"}, {"", "db2.x"}}},
           {"SELECT * FROM db1.o WHERE a IN (SELECT a FROM `db2.x`)",
            "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE a IN (SELECT a FROM phys.`db1.db2.x` AS `db2.x`)",
            {{"db1", "o"}, {"", "db2.x"}}},
           {"SELECT * FROM (SELECT * FROM `db2.x`)", "SELECT * FROM (SELECT * FROM phys.`db1.db2.x` AS `db2.x`)",
            {{"", "db2.x"}}},
           {"SELECT * FROM `db2.x` AS q", "SELECT * FROM phys.`db1.db2.x` AS q", {{"", "db2.x"}}},
           {"SELECT * FROM db1.o WHERE a IN `db2.x`",
            "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE a IN (phys.`db1.db2.x`)", {{"db1", "o"}, {"", "db2.x"}}},
           {"WITH c AS (SELECT * FROM `db2.x`) SELECT * FROM c",
            "WITH c AS (SELECT * FROM phys.`db1.db2.x` AS `db2.x`) SELECT * FROM c", {{"", "db2.x"}}},
           {"SELECT * FROM db1.o UNION ALL SELECT * FROM `db2.x`",
            "SELECT * FROM phys.`db1.o` AS `db1.o` UNION ALL SELECT * FROM phys.`db1.db2.x` AS `db2.x`",
            {{"db1", "o"}, {"", "db2.x"}}},
           {"SELECT * FROM `db2.x` UNION ALL SELECT * FROM db1.o",
            "SELECT * FROM phys.`db1.db2.x` AS `db2.x` UNION ALL SELECT * FROM phys.`db1.o` AS `db1.o`",
            {{"db1", "o"}, {"", "db2.x"}}},
           {"SELECT * FROM db1.o WHERE EXISTS (SELECT 1 FROM `db2.x`)",
            "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE exists((SELECT 1 FROM phys.`db1.db2.x` AS `db2.x`))",
            {{"db1", "o"}, {"", "db2.x"}}},
           {"SELECT * FROM `phys.x`", "SELECT * FROM phys.`db1.phys.x` AS `phys.x`", {{"", "phys.x"}}},
           {"SELECT * FROM `hg_safe.db1__t`", "SELECT * FROM phys.`db1.hg_safe.db1__t` AS `hg_safe.db1__t`",
            {{"", "hg_safe.db1__t"}}},
           {"SELECT * FROM db1.o JOIN `hg_safe.db1__t` ON db1.o.a = 1",
            "SELECT * FROM phys.`db1.o` AS `db1.o` INNER JOIN phys.`db1.hg_safe.db1__t` AS `hg_safe.db1__t` ON db1.o.a = 1",
            {{"db1", "o"}, {"", "hg_safe.db1__t"}}},
           {"SELECT * FROM `db1.t`", "SELECT * FROM phys.`db1.db1.t` AS `db1.t`", {{"", "db1.t"}}},
           {"SELECT * FROM `db1.o`", "SELECT * FROM phys.`db1.db1.o` AS `db1.o`", {{"", "db1.o"}}},
         }) {
      SCOPED_TRACE(std::string(c.sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(c.sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.sql_after_rewrite(), c.want);
      Pairs got;
      for (const auto &a : resp.original_accessed_tables()) got.emplace_back(a.original_database(), a.original_table());
      EXPECT_EQ(got, c.accessed);
    }
  }
}

// An INSERT … SELECT / CTAS body is rewritten by the SELECT pipeline, so a
// one-part IN operand there reads exactly as in a SELECT (spec 2026-09-26 T4;
// rewriter-go v0.15.0, measured by the Task I3 differential): the Active `t`
// becomes the derived safe read, and the quoted one-part `db1.t` is the
// tenant's own table "db1.t" (phys.`db1.db1.t`), never the SI table db1.t. A
// view body that reads the Active table is refused with the SI write message.
// A mutation forwards its predicate unrewritten (R2), so the operand stays
// refused there. (Before Task I3 the rewritten bodies kept an SI refusal here.)
TEST(TableRefSources, WriteBodiesReadAnActiveInOperandLikeASelect) {
  const std::string derived = "(SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t)";
  const std::vector<std::string> rewritten = {
    "INSERT INTO db1.o SELECT number FROM numbers(10) WHERE number IN %",
    "INSERT INTO db1.o SELECT number FROM numbers(10) WHERE number IN (%)",
    "INSERT INTO db1.o SELECT number FROM numbers(10) WHERE number IN (% AS z)",
    "INSERT INTO db1.o SELECT number FROM numbers(10) WHERE number GLOBAL IN %",
    "INSERT INTO db1.o SELECT number FROM numbers(10) WHERE notIn(number, %)",
    "INSERT INTO db1.o SELECT number FROM numbers(10) WHERE globalIn(number, % AS z)",
    "INSERT INTO db1.o SELECT number IN % FROM numbers(10)",
    "INSERT INTO db1.o SELECT number FROM numbers(10) WHERE number IN (SELECT 1 WHERE 1 IN %)",
    "CREATE TABLE db1.n ENGINE = Memory AS SELECT number FROM numbers(10) WHERE number IN %",
    "CREATE TABLE db1.n ENGINE = Memory AS SELECT number FROM numbers(10) WHERE number IN (% AS z)",
    "CREATE TABLE db1.n ENGINE = Memory AS SELECT number FROM numbers(10) WHERE notIn(number, %)",
    "CREATE TABLE db1.n ENGINE = Memory AS SELECT number IN % FROM numbers(10)",
  };
  const auto fill = [](const std::string &pattern, const std::string &operand) {
    const auto at = pattern.find('%');
    return pattern.substr(0, at) + operand + pattern.substr(at + 1);
  };
  for (const auto &pattern : rewritten) for (const std::string operand : {"t", "`db1.t`"}) {
    const std::string sql = fill(pattern, operand);
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql, true);
    ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    const bool active = operand == "t";
    EXPECT_NE(resp.sql_after_rewrite().find(active ? derived : "phys.`db1.db1.t`"), std::string::npos)
      << resp.sql_after_rewrite();
    bool reported = false;
    for (const auto &a : resp.original_accessed_tables())
      reported = reported || (a.original_database().empty() && a.original_table() == (active ? "t" : "db1.t"));
    EXPECT_TRUE(reported);
  }
  EXPECT_EQ(RunTableRef("INSERT INTO db1.o SELECT number FROM numbers(10) WHERE number IN t", true).sql_after_rewrite(),
            "INSERT INTO phys.`db1.o` SELECT number FROM numbers(10) WHERE number IN " + derived);

  // A view body: the Active table is the SI write refusal; `db1.t` is the
  // tenant's own table.
  const auto view_t = RunTableRef("CREATE VIEW db1.v AS SELECT number FROM numbers(10) WHERE number IN t", true);
  EXPECT_EQ(view_t.code(), rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_EQ(view_t.message(), "storage-integrity table db1.t accepts writes only through the signed statement lane");
  const auto view_q = RunTableRef("CREATE VIEW db1.v AS SELECT number FROM numbers(10) WHERE number IN `db1.t`", true);
  EXPECT_EQ(view_q.code(), rewriter::RewriteCode::Success) << view_q.message();
  EXPECT_EQ(view_q.sql_after_rewrite(),
            "CREATE VIEW phys.`db1.v` AS SELECT number FROM numbers(10) WHERE number IN (phys.`db1.db1.t`)");

  // Mutations forward the operand unrewritten: refused, with SI metadata.
  for (const std::string pattern : {"ALTER TABLE db1.o DELETE WHERE a IN %", "ALTER TABLE db1.o UPDATE b = 1 WHERE a IN (% AS z)",
                                    "DELETE FROM db1.o WHERE a IN %", "UPDATE db1.o SET b = 1 WHERE a IN %"})
    for (const std::string operand : {"t", "`db1.t`"}) {
      const std::string sql = fill(pattern, operand);
      SCOPED_TRACE(sql);
      const auto resp = RunTableRef(sql, true);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
      EXPECT_EQ(resp.message(), "storage-integrity table db1.t is not directly addressable through IN table target");
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
      bool si = false;
      for (const auto &a : resp.original_accessed_tables()) si = si || a.is_storage_integrity();
      EXPECT_TRUE(si);
    }
}

// Review NB1: caller-supplied CommonTableExprRewrite bodies are collected and
// rewritten with the query, so under an SI physical execution database their
// one-part IN operands are governed like the main query's — never erased
// without a matching namespace refusal.
TEST(TableRefSources, CteOptionBodyOnePartInOperandInPhysicalContextIsRefused) {
  for (const std::string logical : {"", "db1"}) for (const char *operand : {"t", "db1__t"}) {
    const std::string body = std::string("SELECT a FROM db1.o WHERE a IN ") + operand;
    SCOPED_TRACE(body + " [logical '" + logical + "']");
    rewriter::RewriteSQLRequest req;
    req.set_sql("SELECT * FROM c");
    auto *cte = req.add_options();
    cte->set_op(rewriter::RewriteOp::CommonTableExprRewrite);
    auto &msg = (*cte->mutable_common_table_expr_args()->mutable_cte_map())["c"];
    msg.set_alias("c");
    msg.set_sql(body);
    auto *dyn = AddTableRefOption(req, true);
    dyn->set_upstream_logical_database_in_context(logical);
    dyn->set_upstream_physical_database_in_context("hg_safe");
    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::RewriteError) << resp.sql_after_rewrite();
    EXPECT_EQ(resp.message(),
              std::string("storage-integrity physical table hg_safe.") + operand + " is not directly addressable");
  }
}

// Ledger ruling (release gate): an aliased IN operand is the same table operand
// as the unaliased one — `a IN (phys.x AS z)` must not read differently from
// `a IN (phys.x)` in either SI state, in any operator or callable form, at any
// paren/alias nesting, and in every position. Answers below were measured
// against rewriter-go (fix/in-operand-alias, ea20f0f/08bff5c).
TEST(TableRefSources, AliasedInOperandsAnswerLikeTheUnaliasedForm) {
  using Pairs = std::vector<std::pair<std::string, std::string>>;
  const auto accessed = [](const rewriter::RewriteSQLResponse &resp) {
    Pairs out;
    for (const auto &a : resp.original_accessed_tables()) out.emplace_back(a.original_database(), a.original_table());
    return out;
  };
  const std::vector<std::string> operands = {
    "db1.p", "`db2.x`", "db2.x", "p", "tt.b", "db1.t", "phys.x", "hg_safe.db1__t", "hg_unsafe.db1__t", "hg_promote.x",
  };
  // Each pattern holds `%` where the operand goes and is aliased by `%A`.
  const std::vector<std::pair<std::string, std::string>> forms = {
    {"SELECT * FROM db1.o WHERE a IN (%A)", "SELECT * FROM db1.o WHERE a IN (%)"},
    {"SELECT * FROM db1.o WHERE a IN ((%A))", "SELECT * FROM db1.o WHERE a IN (%)"},
    {"SELECT * FROM db1.o WHERE a IN ((%) AS z)", "SELECT * FROM db1.o WHERE a IN (%)"},
    {"SELECT * FROM db1.o WHERE a IN (((%) AS z))", "SELECT * FROM db1.o WHERE a IN (%)"},
    {"SELECT * FROM db1.o WHERE a IN ((%A) AS y)", "SELECT * FROM db1.o WHERE a IN (%)"},
    {"SELECT * FROM db1.o WHERE a NOT IN (%A)", "SELECT * FROM db1.o WHERE a NOT IN (%)"},
    {"SELECT * FROM db1.o WHERE a GLOBAL IN (%A)", "SELECT * FROM db1.o WHERE a GLOBAL IN (%)"},
    {"SELECT * FROM db1.o WHERE a GLOBAL NOT IN (%A)", "SELECT * FROM db1.o WHERE a GLOBAL NOT IN (%)"},
    {"SELECT * FROM db1.o WHERE (a, b) IN (%A)", "SELECT * FROM db1.o WHERE (a, b) IN (%)"},
    {"SELECT * FROM db1.o WHERE in(a, %A)", "SELECT * FROM db1.o WHERE in(a, %)"},
    {"SELECT * FROM db1.o WHERE notIn(a, %A)", "SELECT * FROM db1.o WHERE notIn(a, %)"},
    {"SELECT * FROM db1.o WHERE globalIn(a, %A)", "SELECT * FROM db1.o WHERE globalIn(a, %)"},
    {"SELECT * FROM db1.o WHERE globalNotIn(a, %A)", "SELECT * FROM db1.o WHERE globalNotIn(a, %)"},
    {"SELECT * FROM db1.o WHERE inIgnoreSet(a, %A)", "SELECT * FROM db1.o WHERE inIgnoreSet(a, %)"},
    {"SELECT * FROM db1.o WHERE notInIgnoreSet(a, %A)", "SELECT * FROM db1.o WHERE notInIgnoreSet(a, %)"},
    {"SELECT * FROM db1.o WHERE globalInIgnoreSet(a, %A)", "SELECT * FROM db1.o WHERE globalInIgnoreSet(a, %)"},
    {"SELECT * FROM db1.o WHERE nullIn(a, %A)", "SELECT * FROM db1.o WHERE nullIn(a, %)"},
    {"SELECT * FROM db1.o WHERE a IN (SELECT 1 FROM db1.p WHERE b IN (%A))",
     "SELECT * FROM db1.o WHERE a IN (SELECT 1 FROM db1.p WHERE b IN (%))"},
    {"SELECT * FROM (SELECT * FROM db1.o WHERE a IN (%A))", "SELECT * FROM (SELECT * FROM db1.o WHERE a IN (%))"},
    {"SELECT a IN (%A) FROM db1.o", "SELECT a IN (%) FROM db1.o"},
    {"SELECT * FROM db1.o WHERE a IN (%A) SETTINGS max_threads = 1",
     "SELECT * FROM db1.o WHERE a IN (%) SETTINGS max_threads = 1"},
  };
  const auto substitute = [](std::string pattern, const std::string &operand) {
    std::string out;
    for (size_t i = 0; i < pattern.size(); ++i) {
      if (pattern.compare(i, 2, "%A") == 0) { out += operand + " AS z"; ++i; }
      else if (pattern[i] == '%') out += operand;
      else out += pattern[i];
    }
    return out;
  };
  for (const bool si : {false, true}) for (const auto &operand : operands) for (const auto &[aliased, plain] : forms) {
    const std::string sql = substitute(aliased, operand);
    SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
    const auto with_alias = RunTableRef(sql, si);
    const auto without = RunTableRef(substitute(plain, operand), si);
    EXPECT_EQ(with_alias.code(), without.code()) << with_alias.message();
    EXPECT_EQ(with_alias.message(), without.message());
    EXPECT_EQ(accessed(with_alias), accessed(without));
    if (without.code() == rewriter::RewriteCode::Success) {
      // Whatever the operand was, no protected name reaches the output.
      EXPECT_EQ(with_alias.sql_after_rewrite().find("phys.x"), std::string::npos);
      EXPECT_EQ(with_alias.sql_after_rewrite().find("hg_unsafe"), std::string::npos);
      EXPECT_EQ(with_alias.sql_after_rewrite().find("hg_promote"), std::string::npos);
    } else {
      EXPECT_EQ(with_alias.sql_after_rewrite(), sql);
    }
  }
}

// The measured absolute answers (rewriter-go v0.14.0+ea20f0f) for the shapes
// the ledger names, so the equivalence above cannot be satisfied by both
// sides being wrong the same way.
TEST(TableRefSources, AliasedInOperandMeasuredAnswers) {
  using Pairs = std::vector<std::pair<std::string, std::string>>;
  const std::string prot_phys = "protected database phys is not addressable";
  const std::string prot_safe = "protected database hg_safe is not addressable";
  const std::string si_safe = "storage-integrity physical table hg_safe.db1__t is not directly addressable";
  const auto accessed = [](const rewriter::RewriteSQLResponse &resp) {
    Pairs out;
    for (const auto &a : resp.original_accessed_tables()) out.emplace_back(a.original_database(), a.original_table());
    return out;
  };
  const std::string scan = "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE ";
  for (const bool si : {false, true}) {
    SCOPED_TRACE(si ? "[V2]" : "[inactive]");
    // A protected physical database: T3 in both states.
    for (const char *sql : {
           "SELECT * FROM db1.o WHERE a IN (phys.x AS z)",
           "SELECT * FROM db1.o WHERE a IN (((phys.x) AS z))",
           "SELECT * FROM db1.o WHERE notIn(a, phys.x AS z)",
           "SELECT * FROM db1.o WHERE globalIn(a, phys.x AS z)",
           "SELECT * FROM db1.o WHERE globalNotIn(a, phys.x AS z)",
           "SELECT * FROM db1.o WHERE inIgnoreSet(a, phys.x AS z)",
           "DELETE FROM db1.o WHERE a IN (phys.x AS z)",
           "ALTER TABLE db1.o UPDATE b = 1 WHERE a IN (phys.x AS z)",
           "CREATE TABLE db1.n (a UInt64, b UInt8 DEFAULT a IN (phys.x AS z)) ENGINE = Memory",
         }) {
      SCOPED_TRACE(sql);
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest) << resp.message();
      EXPECT_EQ(resp.message(), prot_phys);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
    // hg_safe: T3 while the SI surface is inactive, the SI handlers' message
    // (and code) while it is active.
    struct SafeCase { const char *sql; rewriter::RewriteCode si_code; };
    for (const auto &c : std::vector<SafeCase>{
           {"SELECT * FROM db1.o WHERE a IN (hg_safe.db1__t AS z)", rewriter::RewriteCode::RewriteError},
           {"SELECT * FROM db1.o WHERE in(a, hg_safe.db1__t AS z)", rewriter::RewriteCode::RewriteError},
           {"DELETE FROM db1.o WHERE a IN (hg_safe.db1__t AS z)", rewriter::RewriteCode::UnsupportedStatement},
           {"ALTER TABLE db1.o UPDATE b = 1 WHERE a IN (hg_safe.db1__t AS z)", rewriter::RewriteCode::UnsupportedStatement},
           {"CREATE TABLE db1.n (a UInt64, b UInt8 DEFAULT a IN (hg_safe.db1__t AS z)) ENGINE = Memory",
            rewriter::RewriteCode::UnsupportedStatement},
         }) {
      SCOPED_TRACE(c.sql);
      const auto resp = RunTableRef(c.sql, si);
      EXPECT_EQ(resp.code(), si ? c.si_code : rewriter::RewriteCode::InvalidRewriteRequest) << resp.message();
      EXPECT_EQ(resp.message(), si ? si_safe : prot_safe);
      EXPECT_EQ(resp.sql_after_rewrite(), c.sql);
    }
    // A logical operand is rewritten and reported like the unaliased one; the
    // alias goes with the operand it named. A bare quoted `db2.x` is a table
    // of the session's own logical database, never tenant db2.
    const std::string self = si ? "(SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t)" : "(phys.`db1.t`)";
    struct Rewritten { const char *sql; std::string want; Pairs acc; };
    for (const auto &c : std::vector<Rewritten>{
           {"SELECT * FROM db1.o WHERE a IN (db1.p AS z)", scan + "a IN (phys.`db1.p`)", {{"db1", "o"}, {"db1", "p"}}},
           {"SELECT * FROM db1.o WHERE a IN ((db1.p) AS z)", scan + "a IN (phys.`db1.p`)", {{"db1", "o"}, {"db1", "p"}}},
           {"SELECT * FROM db1.o WHERE notIn(a, db1.p AS z)", scan + "notIn(a, phys.`db1.p`)", {{"db1", "o"}, {"db1", "p"}}},
           {"SELECT * FROM db1.o WHERE globalNotIn(a, db1.p AS z)", scan + "globalNotIn(a, phys.`db1.p`)",
            {{"db1", "o"}, {"db1", "p"}}},
           {"SELECT * FROM db1.o WHERE inIgnoreSet(a, db1.p AS z)", scan + "inIgnoreSet(a, phys.`db1.p`)",
            {{"db1", "o"}, {"db1", "p"}}},
           {"SELECT * FROM db1.o WHERE a IN (`db2.x` AS z)", scan + "a IN (phys.`db1.db2.x`)", {{"db1", "o"}, {"", "db2.x"}}},
           {"SELECT * FROM db1.o WHERE a IN (p AS z)", scan + "a IN (phys.`db1.p`)", {{"db1", "o"}, {"", "p"}}},
           {"SELECT * FROM db1.o WHERE a IN (db1.t AS z)", scan + "a IN " + self, {{"db1", "o"}, {"db1", "t"}}},
         }) {
      SCOPED_TRACE(c.sql);
      const auto resp = RunTableRef(c.sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.sql_after_rewrite(), c.want);
      EXPECT_EQ(accessed(resp), c.acc);
    }
  }
}

// An IN table operand in a position no rewrite reaches (mutations, column /
// partition / TTL expressions, SHOW … WHERE) is refused like rewriter-go's R2,
// aliased or not, in both SI states — never forwarded unrewritten, which would
// read the physical table (or another tenant's `db2.x`) verbatim.
TEST(TableRefSources, InOperandInUnrewrittenPositionIsRefused) {
  const std::vector<std::string> positions = {
    "ALTER TABLE db1.o DELETE WHERE a IN %",
    "ALTER TABLE db1.o UPDATE b = 1 WHERE a IN %",
    "ALTER TABLE db1.o UPDATE b = (a IN %) WHERE 1",
    "DELETE FROM db1.o WHERE a IN %",
    "UPDATE db1.o SET b = 1 WHERE a IN %",
    "CREATE TABLE db1.n (a UInt64, b UInt8 DEFAULT a IN %) ENGINE = Memory",
    "CREATE TABLE db1.n (a UInt64, b UInt8 MATERIALIZED a IN %) ENGINE = Memory",
    "CREATE TABLE db1.n (a UInt64) ENGINE = MergeTree ORDER BY a PARTITION BY a IN %",
    "CREATE TABLE db1.n (a UInt64) ENGINE = MergeTree ORDER BY a TTL d + 1 WHERE a IN %",
    "ALTER TABLE db1.o MODIFY COLUMN b UInt8 DEFAULT a IN %",
    "ALTER TABLE db1.o ADD COLUMN c UInt8 DEFAULT a IN %",
    "ALTER TABLE db1.o MODIFY TTL d + INTERVAL 1 DAY DELETE WHERE a IN %",
    "ALTER TABLE db1.o ADD INDEX i (a IN %) TYPE minmax GRANULARITY 1",
    "SHOW COLUMNS FROM o WHERE field IN %",
    // A CREATE with a SELECT body: the body is a rewritten position, the
    // column and ENGINE-clause expressions around it are not (review I1).
    "CREATE TABLE db1.n (a UInt64, b UInt8 DEFAULT a IN %) ENGINE = Memory AS SELECT 1 AS a",
    "CREATE TABLE db1.n (a UInt64, b UInt8 MATERIALIZED a IN %) ENGINE = Memory AS SELECT 1 AS a",
    "CREATE TABLE db1.n (a UInt64, b UInt8 ALIAS a IN %) ENGINE = Memory AS SELECT 1 AS a",
    "CREATE TABLE db1.n ENGINE = MergeTree ORDER BY a PARTITION BY a IN % AS SELECT 1 AS a",
    "CREATE TABLE db1.n ENGINE = MergeTree ORDER BY (a IN %) AS SELECT 1 AS a",
    "CREATE TABLE db1.n ENGINE = MergeTree ORDER BY a SAMPLE BY a IN % AS SELECT 1 AS a",
    "CREATE TABLE db1.n ENGINE = MergeTree ORDER BY a PRIMARY KEY a IN % AS SELECT 1 AS a",
    "CREATE TABLE db1.n ENGINE = MergeTree ORDER BY a TTL d + 1 WHERE a IN % AS SELECT 1 AS a",
    "CREATE MATERIALIZED VIEW db1.mv ENGINE = MergeTree ORDER BY a PARTITION BY a IN % AS SELECT 1 AS a",
    "CREATE MATERIALIZED VIEW db1.mv ENGINE = MergeTree ORDER BY (a IN %) AS SELECT 1 AS a",
    "CREATE MATERIALIZED VIEW db1.mv ENGINE = MergeTree ORDER BY a TTL d + 1 WHERE a IN % AS SELECT 1 AS a",
    // An IN operand inside an ENGINE argument, with and without a body.
    "CREATE TABLE db1.n (a UInt64) ENGINE = ReplacingMergeTree(a IN %) ORDER BY a",
    "CREATE TABLE db1.n ENGINE = ReplacingMergeTree(a IN %) ORDER BY a AS SELECT 1 AS a",
    "CREATE MATERIALIZED VIEW db1.mv ENGINE = ReplacingMergeTree(a IN %) ORDER BY a AS SELECT 1 AS a",
  };
  const auto wrap = [](const std::string &pattern, const std::string &operand) {
    const auto at = pattern.find('%');
    return pattern.substr(0, at) + operand + pattern.substr(at + 1);
  };
  // Operands no other rule names: only this refusal stands between them and
  // ClickHouse in either SI state.
  for (const bool si : {false, true}) for (const auto &position : positions)
  for (const char *operand : {"`db2.x`", "db2.x", "p", "b", "tt.b"}) for (const bool aliased : {false, true}) {
    const std::string body = aliased ? std::string("(") + operand + " AS z)" : std::string("(") + operand + ")";
    const std::string sql = wrap(position, body);
    SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
    const auto resp = RunTableRef(sql, si);
    if (si && position.rfind("SHOW COLUMNS", 0) == 0) {
      // The SI SHOW handler answers first, with its own refusal.
      EXPECT_NE(resp.code(), rewriter::RewriteCode::Success) << resp.sql_after_rewrite();
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
      continue;
    }
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), "statement is not supported");
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
    EXPECT_EQ(resp.original_accessed_tables_size(), 0);
    EXPECT_EQ(resp.table_rewrites_size(), 0);
  }
  // An operand the storage-integrity handlers or T3 name keeps its own,
  // more specific refusal.
  for (const bool si : {false, true}) for (const auto &position : positions)
  for (const char *operand : {"db1.p", "db1.t", "phys.x", "hg_safe.db1__t"}) for (const bool aliased : {false, true}) {
    const std::string body = aliased ? std::string("(") + operand + " AS z)" : std::string("(") + operand + ")";
    const std::string sql = wrap(position, body);
    SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
    const auto resp = RunTableRef(sql, si);
    EXPECT_NE(resp.code(), rewriter::RewriteCode::Success) << resp.sql_after_rewrite();
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
  }
}

// A value list is not a table operand, and a plain statement without an IN
// table operand is untouched by the refusal above.
TEST(TableRefSources, InOperandRefusalLeavesValuesAndOrdinaryStatementsAlone) {
  // A body-carrying CREATE without an IN operand is not touched by the guard.
  for (const bool si : {false, true}) for (const char *sql : {
         "CREATE TABLE db1.n ENGINE = MergeTree ORDER BY a AS SELECT 1 AS a",
         "CREATE TABLE db1.n (a UInt64) ENGINE = MergeTree ORDER BY a AS SELECT 1 AS a",
         "CREATE MATERIALIZED VIEW db1.mv ENGINE = MergeTree ORDER BY a AS SELECT 1 AS a",
       }) {
    SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
    const auto resp = RunTableRef(sql, si);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
  }
  for (const char *sql : {
         "ALTER TABLE db1.o DELETE WHERE a IN (1, 2)",
         "ALTER TABLE db1.o DELETE WHERE a = 1",
         "DELETE FROM db1.o WHERE a IN (1, 2)",
         "CREATE TABLE db1.n (a UInt64, b UInt8 DEFAULT 1) ENGINE = Memory",
       }) {
    SCOPED_TRACE(sql);
    EXPECT_EQ(RunTableRef(sql).code(), rewriter::RewriteCode::Success);
  }
}

// Spec 2026-09-26 T4: an INSERT … SELECT, CREATE TABLE … AS SELECT (EMPTY
// included) and view() body is rewritten and reported like FROM, its sources
// after the target; an Active source becomes the derived read unless the
// target itself is Active.
TEST(TableRefSources, EmbeddedBodiesAreRewrittenAndReported) {
  using Pairs = std::vector<std::pair<std::string, std::string>>;
  struct Case { const char *sql; const char *want_sql; Pairs accessed; bool si = false; };
  const std::vector<Case> cases = {
    {"INSERT INTO db1.o SELECT * FROM db1.p",
     "INSERT INTO phys.`db1.o` SELECT * FROM phys.`db1.p` AS `db1.p`", {{"db1", "o"}, {"db1", "p"}}},
    {"INSERT INTO db1.o SELECT * FROM p",
     "INSERT INTO phys.`db1.o` SELECT * FROM phys.`db1.p` AS p", {{"db1", "o"}, {"", "p"}}},
    {"INSERT INTO db1.o SELECT * FROM (SELECT * FROM db1.p WHERE a IN db1.q)",
     "INSERT INTO phys.`db1.o` SELECT * FROM (SELECT * FROM phys.`db1.p` AS `db1.p` WHERE a IN (phys.`db1.q`))",
     {{"db1", "o"}, {"db1", "p"}, {"db1", "q"}}},
    {"CREATE TABLE db1.n ENGINE = Memory AS SELECT * FROM db1.p",
     "CREATE TABLE phys.`db1.n` ENGINE = Memory AS SELECT * FROM phys.`db1.p` AS `db1.p`", {{"db1", "n"}, {"db1", "p"}}},
    {"CREATE TABLE db1.n ENGINE = Memory EMPTY AS SELECT * FROM db1.p",
     "CREATE TABLE phys.`db1.n` ENGINE = Memory EMPTY AS SELECT * FROM phys.`db1.p` AS `db1.p`",
     {{"db1", "n"}, {"db1", "p"}}},
    {"SELECT * FROM view(SELECT * FROM db1.o)",
     "SELECT * FROM view(SELECT * FROM phys.`db1.o` AS `db1.o`)", {{"db1", "o"}}},
    {"INSERT INTO db1.o SELECT * FROM db1.t",
     "INSERT INTO phys.`db1.o` SELECT * FROM (SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t) AS `db1.t`",
     {{"db1", "o"}, {"db1", "t"}}, true},
    {"CREATE TABLE db1.n ENGINE = Memory AS SELECT * FROM db1.t",
     "CREATE TABLE phys.`db1.n` ENGINE = Memory AS SELECT * FROM (SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t) AS `db1.t`",
     {{"db1", "n"}, {"db1", "t"}}, true},
    {"INSERT INTO db1.t SELECT * FROM db1.o",
     "INSERT INTO phys.`db1.t` SELECT * FROM phys.`db1.o` AS `db1.o`", {{"db1", "t"}, {"db1", "o"}}, true},
  };
  for (const auto &c : cases) {
    SCOPED_TRACE(c.sql);
    const auto resp = RunTableRef(c.sql, c.si);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    EXPECT_EQ(resp.sql_after_rewrite(), c.want_sql);
    Pairs got;
    for (const auto &a : resp.original_accessed_tables()) got.emplace_back(a.original_database(), a.original_table());
    EXPECT_EQ(got, c.accessed);
  }
  // While the SI surface is active an IN operand naming an SI object stays
  // refused in every write body (rewriter-go's write-path namespace policy),
  // even where the SELECT root would rewrite it.
  for (const auto &[sql, message] : std::vector<std::pair<std::string, std::string>>{
         {"INSERT INTO db1.o SELECT * FROM db1.p WHERE a IN db1.t",
          "storage-integrity table db1.t is not directly addressable through IN table target"},
         {"INSERT INTO db1.o SELECT * FROM (SELECT * FROM db1.p WHERE a IN db1.q)",
          "storage-integrity logical database db1 is not directly addressable through IN table target"},
         {"CREATE TABLE db1.n ENGINE = Memory AS SELECT * FROM db1.p WHERE a IN db1.t",
          "storage-integrity table db1.t is not directly addressable through IN table target"}}) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql, /*si=*/true);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), message);
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
  }
  // A CTAS into an Active target stays refused.
  EXPECT_EQ(RunTableRef("CREATE TABLE db1.t ENGINE = Memory AS SELECT * FROM db1.o", true).code(),
            rewriter::RewriteCode::UnsupportedStatement);
  // A refused physical source still reports and rewrites the target first.
  const auto physical = RunTableRef("CREATE TABLE db1.n ENGINE = Memory AS SELECT * FROM hg_safe.db1__t", true);
  EXPECT_EQ(physical.code(), rewriter::RewriteCode::UnsupportedStatement);
  ASSERT_EQ(physical.original_accessed_tables_size(), 2);
  EXPECT_EQ(physical.original_accessed_tables(0).original_table(), "n");
  EXPECT_EQ(physical.original_accessed_tables(1).original_database(), "hg_safe");
  EXPECT_EQ(physical.table_rewrites().at("db1.n"), "phys.db1.n");
}


// Ledger MUST (T8 → T9): a one-part quoted name in a rewritten write body is
// one table of the current database, as in the SELECT handler — never tenant
// db2's x. With a resolvable context it is rewritten into the session's
// logical database in both SI states; with none, an IN operand is refused as
// rewriter-go refuses it; through an SI physical execution database it is
// refused. The quoted `db1.t` is no exception (Task I3, rewriter-go v0.15.0):
// it is the tenant's table "db1.t", rewritten to phys.`db1.db1.t`, and with
// no resolvable logical context it is an unresolved unqualified name.
TEST(TableRefSources, WriteBodyOnePartQuotedNameIsOneTableOfTheCurrentDatabase) {
  using Pairs = std::vector<std::pair<std::string, std::string>>;
  struct Kind { const char *prefix, *suffix, *want_prefix, *want_suffix; };
  const std::vector<Kind> kinds = {
    {"INSERT INTO db1.o ", "", "INSERT INTO phys.`db1.o` ", ""},
    {"CREATE TABLE db1.n ENGINE = Memory AS ", "", "CREATE TABLE phys.`db1.n` ENGINE = Memory AS ", ""},
    {"CREATE TABLE db1.n ENGINE = Memory EMPTY AS ", "", "CREATE TABLE phys.`db1.n` ENGINE = Memory EMPTY AS ", ""},
    {"CREATE VIEW db1.v AS ", "", "CREATE VIEW phys.`db1.v` AS ", ""},
    {"CREATE MATERIALIZED VIEW db1.mv TO db1.o AS ", "", "CREATE MATERIALIZED VIEW phys.`db1.mv` TO phys.`db1.o` AS ", ""},
    {"SELECT * FROM view(", ")", "SELECT * FROM view(", ")"},
  };
  const std::vector<std::pair<std::string, std::string>> bodies = {
    {"SELECT * FROM `db2.x`", "SELECT * FROM phys.`db1.db2.x` AS `db2.x`"},
    {"SELECT * FROM db1.o JOIN `db2.x` AS j ON db1.o.a = j.a",
     "SELECT * FROM phys.`db1.o` AS `db1.o` INNER JOIN phys.`db1.db2.x` AS j ON db1.o.a = j.a"},
    {"SELECT * FROM (SELECT * FROM `db2.x`)", "SELECT * FROM (SELECT * FROM phys.`db1.db2.x` AS `db2.x`)"},
    {"SELECT a FROM db1.o WHERE a IN `db2.x`", "SELECT a FROM phys.`db1.o` AS `db1.o` WHERE a IN (phys.`db1.db2.x`)"},
    {"SELECT a FROM db1.o WHERE a IN (SELECT a FROM `db2.x`)",
     "SELECT a FROM phys.`db1.o` AS `db1.o` WHERE a IN (SELECT a FROM phys.`db1.db2.x` AS `db2.x`)"},
  };
  for (const bool si : {false, true}) for (const auto &kind : kinds) for (const auto &[body, want_body] : bodies) {
    const std::string sql = kind.prefix + body + kind.suffix;
    SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
    const auto resp = RunTableRef(sql, si);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    EXPECT_EQ(resp.sql_after_rewrite(), kind.want_prefix + want_body + kind.want_suffix);
    Pairs got;
    for (const auto &a : resp.original_accessed_tables()) got.emplace_back(a.original_database(), a.original_table());
    ASSERT_FALSE(got.empty());
    EXPECT_EQ(got.back(), (std::pair<std::string, std::string>{"", "db2.x"}));
    EXPECT_EQ(std::count(got.begin(), got.end(), std::pair<std::string, std::string>{"db2", "x"}), 0);
  }

  // No resolvable execution database (V2): the IN operand is refused as
  // rewriter-go refuses it, in every write body and operator form.
  const std::vector<std::pair<std::string, std::string>> forms = {
    {"SELECT a FROM db1.o WHERE a IN %", "IN"}, {"SELECT a FROM db1.o WHERE a IN (% AS z)", "IN"},
    {"SELECT a FROM db1.o WHERE a NOT IN %", "NOT IN"}, {"SELECT a FROM db1.o WHERE globalIn(a, %)", "GLOBAL IN"},
  };
  for (const std::string logical : {"", "db9"}) for (const auto &kind : kinds) {
    if (std::string(kind.prefix).starts_with("SELECT")) continue;  // the SELECT root, pinned by T8
    for (const auto &[form, surface] : forms) for (const char *operand : {"`db2.x`", "`phys.x`"}) {
      std::string body = form;
      body.replace(body.find('%'), 1, operand);
      const std::string sql = kind.prefix + body + kind.suffix;
      SCOPED_TRACE(sql + " [V2, logical '" + logical + "']");
      rewriter::RewriteSQLRequest req;
      req.set_sql(sql);
      AddTableRefOption(req, true)->set_upstream_logical_database_in_context(logical);
      const auto resp = RunDirectRewrite(std::move(req));
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
      EXPECT_EQ(resp.message(), "storage-integrity " + surface + " table target namespace is not statically resolvable");
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
      Pairs got;
      for (const auto &a : resp.original_accessed_tables()) got.emplace_back(a.original_database(), a.original_table());
      EXPECT_EQ(got, (Pairs{{"hg_promote", ""}, {"hg_safe", ""}, {"hg_unsafe", ""}}));
    }
  }

  // Through an SI physical execution database (V2, physical context hg_safe)
  // the name is refused as the physical table it would read. (PARITY:
  // rewriter-go reads a FROM name through the logical mapping here and
  // admits it; this configuration is never sent by housegate.)
  for (const auto &[sql, message] : std::vector<std::pair<std::string, std::string>>{
         {"INSERT INTO db1.o SELECT * FROM `db2.x`", "storage-integrity physical table hg_safe.db2.x is not directly addressable"},
         {"CREATE TABLE db1.n ENGINE = Memory AS SELECT * FROM db1.o JOIN `db2.x` AS j ON db1.o.a = j.a",
          "storage-integrity physical table hg_safe.db2.x is not directly addressable"},
         {"INSERT INTO db1.o SELECT a FROM db1.o WHERE a IN `db2.x`",
          "storage-integrity physical table hg_safe.db2.x is not directly addressable"},
         {"CREATE VIEW db1.v AS SELECT a FROM db1.o WHERE a IN `db2.x`",
          "storage-integrity physical table hg_safe.db2.x is not directly addressable"},
       }) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    AddTableRefOption(req, true)->set_upstream_physical_database_in_context("hg_safe");
    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
    EXPECT_EQ(resp.message(), message);
  }

  // `db1.t` quoted is one table of the current database, never the Active
  // table db1.t: with a logical context it is the tenant's table "db1.t"; with
  // only a physical one it is an unresolved unqualified name (rewriter-go
  // v0.15.0 answers both the same way).
  for (const std::string logical : {"db1", ""}) for (const auto &[sql, want] : std::vector<std::pair<std::string, std::string>>{
         {"INSERT INTO db1.o SELECT * FROM `db1.t`", "INSERT INTO phys.`db1.o` SELECT * FROM phys.`db1.db1.t` AS `db1.t`"},
         {"INSERT INTO db1.o SELECT * FROM db1.o JOIN `db1.t` AS j ON db1.o.a = j.a",
          "INSERT INTO phys.`db1.o` SELECT * FROM phys.`db1.o` AS `db1.o` INNER JOIN phys.`db1.db1.t` AS j ON db1.o.a = j.a"},
         {"CREATE TABLE db1.n ENGINE = Memory AS SELECT * FROM (SELECT * FROM `db1.t`)",
          "CREATE TABLE phys.`db1.n` ENGINE = Memory AS SELECT * FROM (SELECT * FROM phys.`db1.db1.t` AS `db1.t`)"},
       }) {
    SCOPED_TRACE(sql + " [logical '" + logical + "']");
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    auto *dyn = AddTableRefOption(req, true);
    dyn->set_upstream_logical_database_in_context(logical);
    if (logical.empty()) dyn->set_upstream_physical_database_in_context("phys");
    const auto resp = RunDirectRewrite(std::move(req));
    if (logical.empty()) {
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
      EXPECT_EQ(resp.message(), "unqualified table \"db1.t\" does not resolve through the session's logical database");
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
      continue;
    }
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    EXPECT_EQ(resp.sql_after_rewrite(), want);
    Pairs got;
    for (const auto &a : resp.original_accessed_tables()) got.emplace_back(a.original_database(), a.original_table());
    EXPECT_EQ(std::count(got.begin(), got.end(), std::pair<std::string, std::string>{"", "db1.t"}), 1);
    EXPECT_EQ(std::count(got.begin(), got.end(), std::pair<std::string, std::string>{"db1", "t"}), 0);
  }
}

namespace {

using AccessedPairs = std::vector<std::pair<std::string, std::string>>;

AccessedPairs accessedPairs(const rewriter::RewriteSQLResponse &resp) {
  AccessedPairs got;
  for (const auto &a : resp.original_accessed_tables()) got.emplace_back(a.original_database(), a.original_table());
  return got;
}

}  // namespace

// Review C1 (fix round 1): ClickHouse 26.2 does not resolve an outer WITH alias
// inside a view(), viewIfPermitted() or viewExplain() body — `WITH x AS (…)
// SELECT * FROM view(SELECT * FROM x)` reads the table x. A name there is a
// table: rewritten, reported and SI-checked, never skipped as a CTE reference
// (which forwarded `db2.x` verbatim, a cross-tenant read, and a shadowed Active
// table unrewritten). A want of Success pins the rewritten SQL, a refusal the
// message. rewriter-go shares the defect (fixed separately); these rows pin the
// correct answer, not Go's. viewIfPermitted and viewExplain are refused whole
// by the T5 allowlist (Task 11); with the SI surface active the body's table is
// still reported, as a table.
TEST(TableRefSources, OuterCteIsNotVisibleInAViewFamilyBody) {
  struct Case { const char *sql; bool si; rewriter::RewriteCode code; const char *want; AccessedPairs accessed; };
  const std::vector<Case> cases = {
    {"WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db2.x`)", true, rewriter::RewriteCode::Success,
     "WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db2.x` AS `db2.x`)",
     {{"", "db2.x"}}},
    {"WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db2.x`)", false, rewriter::RewriteCode::Success,
     "WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db2.x` AS `db2.x`)",
     {{"", "db2.x"}}},
    {"WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db1.t`)", true, rewriter::RewriteCode::Success,
     "WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db1.t` AS `db1.t`)",
     {{"", "db1.t"}}},
    {"WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db1.t`)", false, rewriter::RewriteCode::Success,
     "WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db1.t` AS `db1.t`)",
     {{"", "db1.t"}}},
    {"WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM t)", true, rewriter::RewriteCode::Success,
     "WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM (SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t) AS t)",
     {{"", "t"}}},
    {"WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM t)", false, rewriter::RewriteCode::Success,
     "WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.t` AS t)",
     {{"", "t"}}},
    {"WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM viewIfPermitted(SELECT * FROM `db2.x` ELSE null('a UInt64'))", true, rewriter::RewriteCode::UnsupportedStatement,
     "table function viewIfPermitted is not recognised",
     {{"", "db2.x"}}},
    {"WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM viewIfPermitted(SELECT * FROM `db2.x` ELSE null('a UInt64'))", false, rewriter::RewriteCode::UnsupportedStatement,
     "table function viewIfPermitted is not recognised",
     {}},
    {"WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM viewIfPermitted(SELECT * FROM `db1.t` ELSE null('a UInt64'))", true, rewriter::RewriteCode::UnsupportedStatement,
     "table function viewIfPermitted is not recognised",
     {{"", "db1.t"}}},
    {"WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM viewIfPermitted(SELECT * FROM `db1.t` ELSE null('a UInt64'))", false, rewriter::RewriteCode::UnsupportedStatement,
     "table function viewIfPermitted is not recognised",
     {}},
    {"WITH t AS (SELECT 7 AS a) SELECT * FROM viewIfPermitted(SELECT * FROM t ELSE null('a UInt64'))", true, rewriter::RewriteCode::UnsupportedStatement,
     "table function viewIfPermitted is not recognised",
     {{"", "t"}}},
    {"WITH t AS (SELECT 7 AS a) SELECT * FROM viewIfPermitted(SELECT * FROM t ELSE null('a UInt64'))", false, rewriter::RewriteCode::UnsupportedStatement,
     "table function viewIfPermitted is not recognised",
     {}},
    {"WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM viewExplain('EXPLAIN', '', (SELECT * FROM `db2.x`))", true, rewriter::RewriteCode::UnsupportedStatement,
     "table function viewExplain is not recognised",
     {{"", "db2.x"}}},
    {"WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM viewExplain('EXPLAIN', '', (SELECT * FROM `db2.x`))", false, rewriter::RewriteCode::UnsupportedStatement,
     "table function viewExplain is not recognised",
     {}},
    {"WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM viewExplain('EXPLAIN', '', (SELECT * FROM `db1.t`))", true, rewriter::RewriteCode::UnsupportedStatement,
     "table function viewExplain is not recognised",
     {{"", "db1.t"}}},
    {"WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM viewExplain('EXPLAIN', '', (SELECT * FROM `db1.t`))", false, rewriter::RewriteCode::UnsupportedStatement,
     "table function viewExplain is not recognised",
     {}},
    {"WITH t AS (SELECT 7 AS a) SELECT * FROM viewExplain('EXPLAIN', '', (SELECT * FROM t))", true, rewriter::RewriteCode::UnsupportedStatement,
     "table function viewExplain is not recognised",
     {{"", "t"}}},
    {"WITH t AS (SELECT 7 AS a) SELECT * FROM viewExplain('EXPLAIN', '', (SELECT * FROM t))", false, rewriter::RewriteCode::UnsupportedStatement,
     "table function viewExplain is not recognised",
     {}},
    {"INSERT INTO db1.o WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db2.x`)", true, rewriter::RewriteCode::Success,
     "INSERT INTO phys.`db1.o` WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db2.x` AS `db2.x`)",
     {{"db1", "o"}, {"", "db2.x"}}},
    {"INSERT INTO db1.o WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db2.x`)", false, rewriter::RewriteCode::Success,
     "INSERT INTO phys.`db1.o` WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db2.x` AS `db2.x`)",
     {{"db1", "o"}, {"", "db2.x"}}},
    // The quoted one-part `db1.t` is the tenant's table "db1.t" (Task I3,
    // rewriter-go v0.15.0), never the Active db1.t.
    {"INSERT INTO db1.o WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db1.t`)", true, rewriter::RewriteCode::Success,
     "INSERT INTO phys.`db1.o` WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db1.t` AS `db1.t`)",
     {{"db1", "o"}, {"", "db1.t"}}},
    {"INSERT INTO db1.o WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db1.t`)", false, rewriter::RewriteCode::Success,
     "INSERT INTO phys.`db1.o` WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db1.t` AS `db1.t`)",
     {{"db1", "o"}, {"", "db1.t"}}},
    {"INSERT INTO db1.o WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM t)", true, rewriter::RewriteCode::Success,
     "INSERT INTO phys.`db1.o` WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM (SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t) AS t)",
     {{"db1", "o"}, {"", "t"}}},
    {"INSERT INTO db1.o WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM t)", false, rewriter::RewriteCode::Success,
     "INSERT INTO phys.`db1.o` WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.t` AS t)",
     {{"db1", "o"}, {"", "t"}}},
    {"CREATE TABLE db1.n ENGINE = Memory AS WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db2.x`)", true, rewriter::RewriteCode::Success,
     "CREATE TABLE phys.`db1.n` ENGINE = Memory AS WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db2.x` AS `db2.x`)",
     {{"db1", "n"}, {"", "db2.x"}}},
    {"CREATE TABLE db1.n ENGINE = Memory AS WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db2.x`)", false, rewriter::RewriteCode::Success,
     "CREATE TABLE phys.`db1.n` ENGINE = Memory AS WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db2.x` AS `db2.x`)",
     {{"db1", "n"}, {"", "db2.x"}}},
    // The quoted one-part `db1.t` is the tenant's table "db1.t" (Task I3,
    // rewriter-go v0.15.0), never the Active db1.t.
    {"CREATE TABLE db1.n ENGINE = Memory AS WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db1.t`)", true, rewriter::RewriteCode::Success,
     "CREATE TABLE phys.`db1.n` ENGINE = Memory AS WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db1.t` AS `db1.t`)",
     {{"db1", "n"}, {"", "db1.t"}}},
    {"CREATE TABLE db1.n ENGINE = Memory AS WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db1.t`)", false, rewriter::RewriteCode::Success,
     "CREATE TABLE phys.`db1.n` ENGINE = Memory AS WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db1.t` AS `db1.t`)",
     {{"db1", "n"}, {"", "db1.t"}}},
    {"CREATE TABLE db1.n ENGINE = Memory AS WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM t)", true, rewriter::RewriteCode::Success,
     "CREATE TABLE phys.`db1.n` ENGINE = Memory AS WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM (SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t) AS t)",
     {{"db1", "n"}, {"", "t"}}},
    {"CREATE TABLE db1.n ENGINE = Memory AS WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM t)", false, rewriter::RewriteCode::Success,
     "CREATE TABLE phys.`db1.n` ENGINE = Memory AS WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.t` AS t)",
     {{"db1", "n"}, {"", "t"}}},
    {"CREATE VIEW db1.v AS WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db2.x`)", true, rewriter::RewriteCode::Success,
     "CREATE VIEW phys.`db1.v` AS WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db2.x` AS `db2.x`)",
     {{"db1", "v"}, {"", "db2.x"}}},
    {"CREATE VIEW db1.v AS WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db2.x`)", false, rewriter::RewriteCode::Success,
     "CREATE VIEW phys.`db1.v` AS WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db2.x` AS `db2.x`)",
     {{"db1", "v"}, {"", "db2.x"}}},
    {"CREATE VIEW db1.v AS WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db1.t`)", true, rewriter::RewriteCode::Success,
     "CREATE VIEW phys.`db1.v` AS WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db1.t` AS `db1.t`)",
     {{"db1", "v"}, {"", "db1.t"}}},
    {"CREATE VIEW db1.v AS WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db1.t`)", false, rewriter::RewriteCode::Success,
     "CREATE VIEW phys.`db1.v` AS WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db1.t` AS `db1.t`)",
     {{"db1", "v"}, {"", "db1.t"}}},
    {"CREATE VIEW db1.v AS WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM t)", true, rewriter::RewriteCode::UnsupportedStatement,
     "storage-integrity table db1.t accepts writes only through the signed statement lane",
     {{"db1", "v"}, {"", "t"}}},
    {"CREATE VIEW db1.v AS WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM t)", false, rewriter::RewriteCode::Success,
     "CREATE VIEW phys.`db1.v` AS WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.t` AS t)",
     {{"db1", "v"}, {"", "t"}}},
    {"CREATE MATERIALIZED VIEW db1.mv TO db1.o AS WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db2.x`)", true, rewriter::RewriteCode::Success,
     "CREATE MATERIALIZED VIEW phys.`db1.mv` TO phys.`db1.o` AS WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db2.x` AS `db2.x`)",
     {{"db1", "mv"}, {"db1", "o"}, {"", "db2.x"}}},
    {"CREATE MATERIALIZED VIEW db1.mv TO db1.o AS WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db2.x`)", false, rewriter::RewriteCode::Success,
     "CREATE MATERIALIZED VIEW phys.`db1.mv` TO phys.`db1.o` AS WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db2.x` AS `db2.x`)",
     {{"db1", "mv"}, {"db1", "o"}, {"", "db2.x"}}},
    {"CREATE MATERIALIZED VIEW db1.mv TO db1.o AS WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db1.t`)", true, rewriter::RewriteCode::Success,
     "CREATE MATERIALIZED VIEW phys.`db1.mv` TO phys.`db1.o` AS WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db1.t` AS `db1.t`)",
     {{"db1", "mv"}, {"db1", "o"}, {"", "db1.t"}}},
    {"CREATE MATERIALIZED VIEW db1.mv TO db1.o AS WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM `db1.t`)", false, rewriter::RewriteCode::Success,
     "CREATE MATERIALIZED VIEW phys.`db1.mv` TO phys.`db1.o` AS WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db1.t` AS `db1.t`)",
     {{"db1", "mv"}, {"db1", "o"}, {"", "db1.t"}}},
    {"CREATE MATERIALIZED VIEW db1.mv TO db1.o AS WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM t)", true, rewriter::RewriteCode::UnsupportedStatement,
     "storage-integrity table db1.t accepts writes only through the signed statement lane",
     {{"db1", "mv"}, {"db1", "o"}, {"", "t"}}},
    {"CREATE MATERIALIZED VIEW db1.mv TO db1.o AS WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM t)", false, rewriter::RewriteCode::Success,
     "CREATE MATERIALIZED VIEW phys.`db1.mv` TO phys.`db1.o` AS WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.t` AS t)",
     {{"db1", "mv"}, {"db1", "o"}, {"", "t"}}},
    {"WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT a FROM db1.o WHERE a IN `db2.x`)", true, rewriter::RewriteCode::Success,
     "WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT a FROM phys.`db1.o` AS `db1.o` WHERE a IN (phys.`db1.db2.x`))",
     {{"db1", "o"}, {"", "db2.x"}}},
    {"WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT a FROM db1.o WHERE a IN `db2.x`)", false, rewriter::RewriteCode::Success,
     "WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT a FROM phys.`db1.o` AS `db1.o` WHERE a IN (phys.`db1.db2.x`))",
     {{"db1", "o"}, {"", "db2.x"}}},
    {"WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT a FROM db1.o WHERE a IN `db1.t`)", true, rewriter::RewriteCode::Success,
     "WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT a FROM phys.`db1.o` AS `db1.o` WHERE a IN (phys.`db1.db1.t`))",
     {{"db1", "o"}, {"", "db1.t"}}},
    {"WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT a FROM db1.o WHERE a IN `db1.t`)", false, rewriter::RewriteCode::Success,
     "WITH `db1.t` AS (SELECT 7 AS a) SELECT * FROM view(SELECT a FROM phys.`db1.o` AS `db1.o` WHERE a IN (phys.`db1.db1.t`))",
     {{"db1", "o"}, {"", "db1.t"}}},
    {"WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT a FROM db1.o WHERE a IN t)", true, rewriter::RewriteCode::Success,
     "WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT a FROM phys.`db1.o` AS `db1.o` WHERE a IN (SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t))",
     {{"db1", "o"}, {"", "t"}}},
    {"WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT a FROM db1.o WHERE a IN t)", false, rewriter::RewriteCode::Success,
     "WITH t AS (SELECT 7 AS a) SELECT * FROM view(SELECT a FROM phys.`db1.o` AS `db1.o` WHERE a IN (phys.`db1.t`))",
     {{"db1", "o"}, {"", "t"}}},
  };
  for (const auto &c : cases) {
    SCOPED_TRACE(std::string(c.sql) + (c.si ? " [V2]" : " [inactive]"));
    const auto resp = RunTableRef(c.sql, c.si);
    EXPECT_EQ(resp.code(), c.code) << resp.message();
    if (c.code == rewriter::RewriteCode::Success) EXPECT_EQ(resp.sql_after_rewrite(), c.want);
    else EXPECT_EQ(resp.message(), c.want);
    EXPECT_EQ(accessedPairs(resp), c.accessed);
  }
  // A CTE stays a CTE in an ordinary subquery: only the view family is opaque.
  EXPECT_EQ(RunTableRef("WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM (SELECT * FROM `db2.x`)", true).sql_after_rewrite(),
            "WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM (SELECT * FROM `db2.x`)");
}

// Review I1 (fix round 1): an INSERT … SELECT / CTAS body that reads an Active
// table may not address the reserved row-id column, exactly as the SELECT
// handler refuses it (rewriter-go's message, code and accessed list). An
// ordinary source under an Active target, and the inactive surface, are
// unaffected.
TEST(TableRefSources, WriteBodyRefusesTheReservedRowIdColumn) {
  for (const auto &[sql, accessed] : std::vector<std::pair<std::string, AccessedPairs>>{
         {"INSERT INTO db1.o SELECT _hg_row_id FROM db1.t", {{"db1", "o"}, {"db1", "t"}}},
         {"INSERT INTO db1.o SELECT _hg_row_id FROM t", {{"db1", "o"}, {"", "t"}}},
         {"INSERT INTO db1.o SELECT t._hg_row_id FROM t", {{"db1", "o"}, {"", "t"}}},
         {"INSERT INTO db1.o SELECT x._hg_row_id FROM db1.t AS x", {{"db1", "o"}, {"db1", "t"}}},
         {"INSERT INTO db1.o SELECT a FROM db1.t WHERE _hg_row_id = ''", {{"db1", "o"}, {"db1", "t"}}},
         {"INSERT INTO db1.o SELECT * FROM (SELECT _hg_row_id FROM db1.t)", {{"db1", "o"}, {"db1", "t"}}},
         {"INSERT INTO db1.o SELECT _hg_row_id FROM db1.o JOIN db1.t USING a", {{"db1", "o"}, {"db1", "o"}, {"db1", "t"}}},
         {"CREATE TABLE db1.n ENGINE = Memory AS SELECT _hg_row_id FROM db1.t", {{"db1", "n"}, {"db1", "t"}}},
         {"CREATE TABLE db1.n ENGINE = Memory AS SELECT _hg_row_id FROM t", {{"db1", "n"}, {"", "t"}}},
         {"CREATE TABLE db1.n ENGINE = Memory EMPTY AS SELECT _hg_row_id FROM db1.t", {{"db1", "n"}, {"db1", "t"}}},
         {"CREATE OR REPLACE TABLE db1.n ENGINE = Memory AS SELECT t._hg_row_id FROM t", {{"db1", "n"}, {"", "t"}}},
       }) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql, true);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
    EXPECT_EQ(resp.message(), "reserved column _hg_row_id is not addressable");
    EXPECT_EQ(accessedPairs(resp), accessed);
    EXPECT_EQ(RunTableRef(sql, false).code(), rewriter::RewriteCode::Success);
  }
  for (const char *sql : {"INSERT INTO db1.t SELECT _hg_row_id FROM db1.o", "INSERT INTO db1.o SELECT _hg_row_id FROM db1.p"}) {
    SCOPED_TRACE(sql);
    EXPECT_EQ(RunTableRef(sql, true).code(), rewriter::RewriteCode::Success);
  }
}

// Review I2 (fix round 1): an INSERT … SELECT / CTAS body admits an Active
// table as the derived read only where it reads it through view() or an
// ordinary table expression. Inside viewIfPermitted / viewExplain /
// executable it stays refused, with no table_rewrites entry for a rewrite that
// never happened; with the SI surface active the SI message precedes T5. Every
// other body naming those functions is refused whole by the T5 allowlist
// (Task 11), in both SI states, so `db2.x` there (a cross-tenant read) is never
// forwarded.
TEST(TableRefSources, WriteBodyTableFunctionBodies) {
  const std::vector<std::pair<std::string, std::string>> kinds = {
    {"INSERT INTO db1.o ", "o"}, {"CREATE TABLE db1.n ENGINE = Memory AS ", "n"},
    {"CREATE OR REPLACE TABLE db1.n ENGINE = Memory AS ", "n"},
  };
  const std::vector<std::pair<std::string, std::string>> bodies = {
    {"SELECT * FROM viewIfPermitted(SELECT * FROM % ELSE null('a UInt64'))",
     "table function viewIfPermitted is not recognised"},
    {"SELECT * FROM viewExplain('EXPLAIN', '', (SELECT * FROM %))",
     "table function viewExplain is not recognised"},
    {"SELECT * FROM executable('s.sh', 'TSV', 'a UInt64', (SELECT * FROM %))",
     "table function executable is not accepted"},
  };
  auto fill = [](std::string pattern, const std::string &operand) {
    pattern.replace(pattern.find('%'), 1, operand);
    return pattern;
  };
  for (const auto &[body, refused] : bodies) {
    for (const auto &[prefix, target] : kinds) {
      for (const char *operand : {"t", "db1.t"}) {
        const std::string sql = prefix + fill(body, operand);
        SCOPED_TRACE(sql);
        const auto resp = RunTableRef(sql, true);
        EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
        EXPECT_EQ(resp.message(), "storage-integrity table db1.t accepts writes only through the signed statement lane");
        EXPECT_EQ(accessedPairs(resp),
                  (AccessedPairs{{"db1", target}, {std::string(operand) == "t" ? "" : "db1", "t"}}));
        EXPECT_EQ(resp.table_rewrites().count(operand), 0u);
      }
      for (const bool si : {false, true}) {
        const std::string sql = prefix + fill(body, "`db2.x`");
        SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
        const auto resp = RunTableRef(sql, si);
        EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
        EXPECT_EQ(resp.message(), refused);
        EXPECT_EQ(accessedPairs(resp), AccessedPairs{});
      }
    }
    // The SELECT root refuses the function as well; with the SI surface
    // active the body's table was already reported, as rewriter-go reports it.
    for (const bool si : {false, true}) {
      const std::string sql = fill(body, "`db2.x`");
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
      EXPECT_EQ(resp.message(), refused);
      EXPECT_EQ(accessedPairs(resp), (si ? AccessedPairs{{"", "db2.x"}} : AccessedPairs{}));
    }
  }
  // view() is the one table function whose body is admitted as the derived read.
  const auto view = RunTableRef("INSERT INTO db1.o SELECT * FROM view(SELECT * FROM t)", true);
  EXPECT_EQ(view.code(), rewriter::RewriteCode::Success) << view.message();
  EXPECT_EQ(view.sql_after_rewrite(),
            "INSERT INTO phys.`db1.o` SELECT * FROM view(SELECT * FROM (SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t) AS t)");
  // Review M1: a refused INSERT INTO FUNCTION reports no empty target.
  for (const auto &[sql, accessed] : std::vector<std::pair<std::string, AccessedPairs>>{
         {"INSERT INTO FUNCTION null('a UInt64') SELECT * FROM hg_safe.db1__t", {{"hg_safe", "db1__t"}}},
         {"INSERT INTO FUNCTION null('a UInt64') SELECT * FROM db1.t", {{"db1", "t"}}},
       }) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql, true);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
    EXPECT_EQ(accessedPairs(resp), accessed);
  }
}

// Review M4: `db2.x` in a CTE body and in either UNION arm of a write body, and
// CREATE OR REPLACE TABLE … AS SELECT, read the session's logical database.
TEST(TableRefSources, WriteBodyCteAndUnionArmsReadTheCurrentDatabase) {
  const std::vector<std::pair<std::string, std::string>> kinds = {
    {"INSERT INTO db1.o ", "INSERT INTO phys.`db1.o` "},
    {"CREATE TABLE db1.n ENGINE = Memory AS ", "CREATE TABLE phys.`db1.n` ENGINE = Memory AS "},
    {"CREATE OR REPLACE TABLE db1.n ENGINE = Memory AS ", "CREATE OR REPLACE TABLE phys.`db1.n` ENGINE = Memory AS "},
  };
  const std::vector<std::pair<std::string, std::string>> bodies = {
    {"WITH c AS (SELECT * FROM `db2.x`) SELECT * FROM c",
     "WITH c AS (SELECT * FROM phys.`db1.db2.x` AS `db2.x`) SELECT * FROM c"},
    {"SELECT * FROM `db2.x` UNION ALL SELECT * FROM db1.o",
     "SELECT * FROM phys.`db1.db2.x` AS `db2.x` UNION ALL SELECT * FROM phys.`db1.o` AS `db1.o`"},
    {"SELECT * FROM db1.o UNION ALL SELECT * FROM `db2.x`",
     "SELECT * FROM phys.`db1.o` AS `db1.o` UNION ALL SELECT * FROM phys.`db1.db2.x` AS `db2.x`"},
    {"SELECT * FROM `db2.x`", "SELECT * FROM phys.`db1.db2.x` AS `db2.x`"},
  };
  for (const bool si : {false, true}) for (const auto &[prefix, want_prefix] : kinds) for (const auto &[body, want] : bodies) {
    const std::string sql = prefix + body;
    SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
    const auto resp = RunTableRef(sql, si);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    EXPECT_EQ(resp.sql_after_rewrite(), want_prefix + want);
    const auto got = accessedPairs(resp);
    EXPECT_EQ(std::count(got.begin(), got.end(), std::pair<std::string, std::string>{"", "db2.x"}), 1);
    EXPECT_EQ(std::count(got.begin(), got.end(), std::pair<std::string, std::string>{"db2", "x"}), 0);
  }
}

// Re-review N1 (fix round 2): ClickHouse 26.2 does not bind an outer WITH alias
// inside executable()'s input query, nor inside a view() / viewIfPermitted() /
// viewExplain() call at any depth — including one nested in remote /
// remoteSecure / cluster / clusterAllReplicas (measured: the real table is
// read). A CTE-shadowed name there is a table: reported as one and never
// forwarded verbatim. The T5 allowlist (Task 11) refuses these table functions
// whole, as rewriter-go does; with the SI surface active the SI message comes
// first, and at the SELECT root the body's table was already reported.
TEST(TableRefSources, OuterCteIsNotVisibleInExecutableOrANestedView) {
  struct Case { std::string sql; bool si; rewriter::RewriteCode code; std::string want; AccessedPairs accessed; };
  const std::string exe = "SELECT * FROM executable('s.sh', 'TSV', 'a UInt64', (SELECT * FROM %))";
  auto with = [](const std::string &name, const std::string &body) {
    return "WITH " + name + " AS (SELECT 7 AS a) " + body;
  };
  auto fill = [](std::string pattern, const std::string &name) {
    pattern.replace(pattern.find('%'), 1, name);
    return pattern;
  };
  const std::string signed_lane = "storage-integrity table db1.t accepts writes only through the signed statement lane";
  const std::string exe_refused = "table function executable is not accepted";
  std::vector<Case> cases;
  for (const bool si : {false, true}) {
    // executable() at the SELECT root.
    for (const std::string name : {"`db2.x`", "`db1.t`", "t"}) {
      const std::string bare = name == "`db2.x`" ? "db2.x" : name == "`db1.t`" ? "db1.t" : "t";
      cases.push_back({with(name, fill(exe, name)), si, rewriter::RewriteCode::UnsupportedStatement, exe_refused,
                       si ? AccessedPairs{{"", bare}} : AccessedPairs{}});
    }
    // executable() in an INSERT … SELECT body.
    cases.push_back({"INSERT INTO db1.o " + with("`db2.x`", fill(exe, "`db2.x`")), si,
                     rewriter::RewriteCode::UnsupportedStatement, exe_refused, {}});
    // remote / cluster family around a nested view(): an unresolved namespace
    // under V2 (the SI message precedes T5), the T5 refusal otherwise.
    for (const char *carrier : {"remote('h', %)", "remoteSecure('h', %)", "cluster('c', %)", "clusterAllReplicas('c', %)"}) {
      const std::string function(carrier, std::string_view(carrier).find('('));
      for (const std::string name : {"`db2.x`", "`db1.t`", "t"}) {
        const std::string sql = with(name, "SELECT * FROM " + fill(carrier, "view(SELECT * FROM " + name + ")"));
        const std::string bare = name == "`db2.x`" ? "db2.x" : name == "`db1.t`" ? "db1.t" : "t";
        if (si) {
          // The view body's table is reported (a table, not the CTE) ahead of
          // the refusal's physical databases; rewriter-go reports only the latter.
          cases.push_back({sql, si, rewriter::RewriteCode::RewriteError,
                           "storage-integrity table function namespace is not statically resolvable",
                           {{"", bare}, {"hg_promote", ""}, {"hg_safe", ""}, {"hg_unsafe", ""}}});
        } else {
          cases.push_back({sql, si, rewriter::RewriteCode::UnsupportedStatement,
                           "table function " + function + " is not accepted", {}});
        }
      }
    }
  }
  // With the SI surface active an INSERT body refuses an Active table inside
  // executable() with the SI message ahead of T5 (rewriter-go answers T5:
  // message only). The quoted one-part `db1.t` is the tenant's table "db1.t",
  // not an SI table, so T5 names the function (as rewriter-go does).
  cases.push_back({"INSERT INTO db1.o " + with("t", fill(exe, "t")), true, rewriter::RewriteCode::UnsupportedStatement,
                   signed_lane, {{"db1", "o"}, {"", "t"}}});
  cases.push_back({"INSERT INTO db1.o " + with("`db1.t`", fill(exe, "`db1.t`")), true,
                   rewriter::RewriteCode::UnsupportedStatement, "table function executable is not accepted", {}});
  for (const auto &c : cases) {
    SCOPED_TRACE(c.sql + (c.si ? " [V2]" : " [inactive]"));
    const auto resp = RunTableRef(c.sql, c.si);
    EXPECT_EQ(resp.code(), c.code) << resp.message();
    if (c.code == rewriter::RewriteCode::Success) EXPECT_EQ(resp.sql_after_rewrite(), c.want);
    else EXPECT_EQ(resp.message(), c.want);
    EXPECT_EQ(accessedPairs(resp), c.accessed);
  }
}

// Re-review N2: view() is matched case-insensitively everywhere, as
// ClickHouse accepts `VIEW(…)`: an Active table in its body is the derived
// read in a write body (rewriter-go's answer), not a refusal.
TEST(TableRefSources, ViewTableFunctionIsMatchedCaseInsensitively) {
  for (const char *fn : {"VIEW", "View", "view"}) {
    const std::string sql = std::string("INSERT INTO db1.o SELECT * FROM ") + fn + "(SELECT * FROM t)";
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql, true);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    EXPECT_EQ(resp.sql_after_rewrite(),
              "INSERT INTO phys.`db1.o` SELECT * FROM view(SELECT * FROM (SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t) AS t)");
    EXPECT_EQ(accessedPairs(resp), (AccessedPairs{{"db1", "o"}, {"", "t"}}));
  }
  const auto shadowed = RunTableRef("WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM VIEW(SELECT * FROM `db2.x`)", false);
  EXPECT_EQ(shadowed.sql_after_rewrite(),
            "WITH `db2.x` AS (SELECT 7 AS a) SELECT * FROM view(SELECT * FROM phys.`db1.db2.x` AS `db2.x`)");
}

// Re-review N3: ClickHouse binds an outer CTE in an IN subquery, a bare IN
// operand, a scalar subquery and a scalar table-function argument, so the
// name stays the CTE there (rewriter-go agrees): not rewritten, not reported.
TEST(TableRefSources, OuterCteStaysACteOutsideTheViewFamily) {
  for (const bool si : {false, true}) for (const std::string name : {"`db2.x`", "`db1.t`", "t"}) {
    const std::string with = "WITH " + name + " AS (SELECT 7 AS a) ";
    for (const auto &[body, want, accessed] : std::vector<std::tuple<std::string, std::string, AccessedPairs>>{
           {"SELECT * FROM db1.o WHERE a IN (SELECT a FROM " + name + ")",
            "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE a IN (SELECT a FROM " + name + ")", {{"db1", "o"}}},
           {"SELECT * FROM db1.o WHERE a IN " + name,
            "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE a IN (" + name + ")", {{"db1", "o"}}},
           {"SELECT (SELECT max(a) FROM " + name + ") FROM db1.o",
            "SELECT (SELECT max(a) FROM " + name + ") FROM phys.`db1.o` AS `db1.o`", {{"db1", "o"}}},
           {"SELECT * FROM numbers((SELECT count() FROM " + name + "))",
            "SELECT * FROM numbers((SELECT count() FROM " + name + "))", {}},
         }) {
      SCOPED_TRACE(with + body + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(with + body, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.sql_after_rewrite(), with + want);
      EXPECT_EQ(accessedPairs(resp), accessed);
    }
    // The same in an INSERT … SELECT body's IN subquery.
    if (name != "`db1.t`" || !si) {
      const std::string sql = "INSERT INTO db1.o " + with + "SELECT * FROM db1.p WHERE a IN (SELECT a FROM " + name + ")";
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.sql_after_rewrite(), "INSERT INTO phys.`db1.o` " + with
                + "SELECT * FROM phys.`db1.p` AS `db1.p` WHERE a IN (SELECT a FROM " + name + ")");
      EXPECT_EQ(accessedPairs(resp), (AccessedPairs{{"db1", "o"}, {"db1", "p"}}));
    }
  }
}

// Spec 2026-09-26 T12: a materialized view writes into its TO / APPEND TO
// target, so an Active target is refused with rewriter-go's message.
TEST(TableRefParity, MaterializedViewToActiveTableIsRefused) {
  for (const char *sql : {"CREATE MATERIALIZED VIEW db1.mv TO db1.t AS SELECT * FROM db1.o",
                          "CREATE MATERIALIZED VIEW db1.mv REFRESH EVERY 1 HOUR APPEND TO db1.t AS SELECT * FROM db1.o"}) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql, /*si=*/true);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), rewriter_handlers::storageIntegrityWriteRejectMessage("db1.t"));
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
  }
  const auto ok = RunTableRef("CREATE MATERIALIZED VIEW db1.mv TO db1.o AS SELECT * FROM db1.p", /*si=*/true);
  EXPECT_EQ(ok.code(), rewriter::RewriteCode::Success) << ok.message();
  // An SI physical target is refused as the physical table it names.
  for (const char *db : {"hg_safe", "hg_unsafe", "hg_promote"}) {
    const std::string sql = std::string("CREATE MATERIALIZED VIEW db1.mv TO ") + db + ".`db2.x` AS SELECT * FROM db1.o";
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql, /*si=*/true);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), std::string("storage-integrity physical table ") + db + ".db2.x is not directly addressable");
    ASSERT_EQ(resp.original_accessed_tables_size(), 1);
    EXPECT_TRUE(resp.original_accessed_tables(0).is_storage_integrity());
  }
}

// Spec 2026-09-26 T12, measured against rewriter-go for the plain TO form: the TO / REFRESH TO / REFRESH
// APPEND TO target is a write target, in both surface states. A REFRESH view is also R2-refused (it reruns its
// query outside any session), after the target refusals: a REFRESH form answers as TO where TO refuses, and
// `statement is not supported` where TO succeeds. rewriter-go refuses every REFRESH view before any other check,
// so where TO refuses the two engines differ only in the message.
TEST(TableRefParity, MaterializedViewTargetAnswersAreTheSameForEveryToForm) {
  struct Row {
    const char *target;
    bool si;
    rewriter::RewriteCode code;
    const char *message;  // exact; "" = Success
    AccessedPairs accessed;
  };
  const auto write = rewriter_handlers::storageIntegrityWriteRejectMessage("db1.t");
  const std::vector<Row> rows = {
    {"db1.t", true, rewriter::RewriteCode::UnsupportedStatement, write.c_str(), {{"db1", "t"}}},
    {"hg_safe.db1__t", true, rewriter::RewriteCode::UnsupportedStatement,
     "storage-integrity physical table hg_safe.db1__t is not directly addressable", {{"hg_safe", "db1__t"}}},
    {"phys.x", true, rewriter::RewriteCode::InvalidRewriteRequest, "protected database phys is not addressable", {{"phys", ""}}},
    {"db1.o", true, rewriter::RewriteCode::Success, "", {{"db1", "mv"}, {"db1", "o"}, {"db1", "p"}}},
    {"`db2.x`", true, rewriter::RewriteCode::Success, "", {{"db1", "mv"}, {"", "db2.x"}, {"db1", "p"}}},
    {"db1.t", false, rewriter::RewriteCode::Success, "", {{"db1", "mv"}, {"db1", "t"}, {"db1", "p"}}},
    {"hg_safe.db1__t", false, rewriter::RewriteCode::InvalidRewriteRequest, "protected database hg_safe is not addressable", {{"hg_safe", ""}}},
    {"phys.x", false, rewriter::RewriteCode::InvalidRewriteRequest, "protected database phys is not addressable", {{"phys", ""}}},
    {"db1.o", false, rewriter::RewriteCode::Success, "", {{"db1", "mv"}, {"db1", "o"}, {"db1", "p"}}},
    {"`db2.x`", false, rewriter::RewriteCode::Success, "", {{"db1", "mv"}, {"", "db2.x"}, {"db1", "p"}}},
  };
  for (const char *head : {"CREATE MATERIALIZED VIEW db1.mv TO ", "CREATE MATERIALIZED VIEW db1.mv REFRESH EVERY 1 HOUR TO ",
                           "CREATE MATERIALIZED VIEW db1.mv REFRESH EVERY 1 HOUR APPEND TO "}) {
    const bool refresh = std::string_view(head).find("REFRESH") != std::string_view::npos;
    for (const auto &row : rows) {
      const std::string sql = std::string(head) + row.target + " AS SELECT * FROM db1.p";
      SCOPED_TRACE(sql + (row.si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, row.si);
      if (refresh && row.code == rewriter::RewriteCode::Success) {
        EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
        EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
        EXPECT_EQ(resp.sql_after_rewrite(), sql);
        EXPECT_EQ(accessedPairs(resp), AccessedPairs{});
        continue;
      }
      EXPECT_EQ(resp.code(), row.code) << resp.message();
      if (row.code == rewriter::RewriteCode::Success) {
        // A one-part quoted name is one table of the current database, never database db2.
        if (std::string(row.target) == "`db2.x`")
          EXPECT_NE(resp.sql_after_rewrite().find("phys.`db1.db2.x`"), std::string::npos) << resp.sql_after_rewrite();
      } else {
        EXPECT_EQ(resp.message(), row.message);
        EXPECT_EQ(resp.sql_after_rewrite(), sql);
      }
      EXPECT_EQ(accessedPairs(resp), row.accessed);
    }
  }
}

// Spec 2026-09-26 T5: the table-function allowlist, matched
// case-insensitively; the message names the function as written.
TEST(TableRefAllowlist, TableFunctions) {
  for (const char *fn : {"merge('db1', 'o')", "remote('h', 'db1', 'o')", "remoteSecure('h', 'db1', 'o')",
                         "cluster('c', db1.o)", "clusterAllReplicas('c', db1.o)", "loop('db1', 'o')",
                         "dictionary(db1.d)", "mergeTreeIndex('db1', 'o')", "mergeTreeProjection('db1', 'o', 'p')",
                         "timeSeriesData('db1', 'o')", "prometheusQuery('db1', 'o', 'up')", "clickhouse('db1.o')",
                         "mysql('h', 'db1', 'o', 'u', 'p')", "postgresql('h', 'db1', 'o', 'u', 'p')",
                         "jdbc('ds', 'db1', 'o')", "odbc('ds', 'db1', 'o')", "executable('x.sh', 'TSV', 'a UInt8')",
                         "fuzzQuery('SELECT 1')", "MERGE('db1', 'o')"}) {
    const std::string sql = std::string("SELECT * FROM ") + fn;
    const std::string name(fn, std::string_view(fn).find('('));
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), "table function " + name + " is not accepted");
  }
  const auto unknown = RunTableRef("SELECT * FROM frobnicate('x')");
  EXPECT_EQ(unknown.code(), rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_EQ(unknown.message(), "table function frobnicate is not recognised");
  const auto insert_function = RunTableRef("INSERT INTO FUNCTION remote('h', 'db1', 'o') VALUES (1)");
  EXPECT_EQ(insert_function.message(), "table function remote is not accepted");
  for (const char *sql : {"SELECT * FROM numbers(10)", "SELECT * FROM url('http://127.0.0.1/x', CSV)",
                          "INSERT INTO db1.o SELECT * FROM input('a UInt8')"}) {
    SCOPED_TRACE(sql);
    EXPECT_EQ(RunTableRef(sql).code(), rewriter::RewriteCode::Success);
  }
}

// While the SI surface is active T5 runs after the SI namespace policy, so an
// SI-owned message wins (rewriter-go rejectDisallowedCarriers call sites).
TEST(TableRefAllowlist, StorageIntegrityMessagesPrecedeAllowlistsWhenActive) {
  const auto si = RunTableRef("SELECT * FROM merge('hg_safe', 'db1__t')", /*si=*/true);
  EXPECT_EQ(si.code(), rewriter::RewriteCode::RewriteError) << si.message();
  EXPECT_EQ(si.message(), "storage-integrity physical table hg_safe.db1__t is not directly addressable");
  const auto ordinary = RunTableRef("SELECT * FROM merge('other', 'x')", /*si=*/true);
  EXPECT_EQ(ordinary.code(), rewriter::RewriteCode::UnsupportedStatement) << ordinary.message();
  EXPECT_EQ(ordinary.message(), "table function merge is not accepted");
}

// Spec 2026-09-26 §5 precedence: T2, then T7, then T3, then T5.
TEST(TableRefPrecedence, ParameterThenClassThenProtectedThenAllowlist) {
  struct Case { const char *sql; rewriter::RewriteCode code; std::string message; };
  const std::vector<Case> cases = {
    {"EXPLAIN SELECT * FROM {p:Identifier}", rewriter::RewriteCode::InvalidRewriteRequest,
     std::string(rewriter_handlers::kIdentifierParameterMessage)},
    {"DETACH TABLE phys.x", rewriter::RewriteCode::UnsupportedStatement,
     std::string(rewriter_handlers::kUnsupportedStatementMessage)},
    {"SELECT * FROM merge('phys', 'db2')", rewriter::RewriteCode::InvalidRewriteRequest,
     "protected database phys is not addressable"},
    {"SELECT * FROM merge('db1', 'o')", rewriter::RewriteCode::UnsupportedStatement,
     "table function merge is not accepted"},
  };
  for (const auto &c : cases) {
    SCOPED_TRACE(c.sql);
    const auto resp = RunTableRef(c.sql);
    EXPECT_EQ(resp.code(), c.code) << resp.message();
    EXPECT_EQ(resp.message(), c.message);
    EXPECT_EQ(resp.sql_after_rewrite(), c.sql);
    EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
  }
}

// T5 reaches every source position of every handled statement in both SI
// states: the preflight while the surface is inactive, the SELECT and write
// handlers after their SI checks while it is active. A non-literal database
// argument (`remote('h', currentDatabase(), 'x')`) is refused with the
// function. Measured against rewriter-go db2e246: the same code, message and
// accessed list on every row.
TEST(TableRefAllowlist, EverySourcePositionInBothSurfaceStates) {
  const std::string fn = "merge('other', 'x')";
  const std::string merge_refused = "table function merge is not accepted";
  std::vector<std::pair<std::string, std::string>> rows;
  for (const char *pattern : {
         "SELECT * FROM db1.o JOIN % AS z USING a", "SELECT * FROM (SELECT * FROM %)",
         "WITH c AS (SELECT * FROM %) SELECT * FROM c", "SELECT * FROM db1.o UNION ALL SELECT * FROM %",
         "SELECT * FROM db1.o INTERSECT SELECT * FROM %", "SELECT * FROM db1.o WHERE a IN (SELECT a FROM %)",
         "SELECT (SELECT count() FROM %)", "WITH (SELECT count() FROM %) AS c SELECT c",
         "SELECT * FROM db1.o WHERE EXISTS (SELECT 1 FROM %)", "SELECT * FROM view(SELECT * FROM %)",
         "INSERT INTO db1.o SELECT * FROM %", "INSERT INTO db1.o (a) SELECT a FROM %",
         "INSERT INTO db1.o WITH c AS (SELECT * FROM %) SELECT * FROM c",
         "CREATE TABLE db1.n ENGINE = Memory AS SELECT * FROM %", "CREATE TABLE db1.n ENGINE = Memory EMPTY AS SELECT * FROM %",
         "CREATE OR REPLACE TABLE db1.n ENGINE = Memory AS SELECT * FROM %", "CREATE VIEW db1.v AS SELECT * FROM %",
         "CREATE VIEW db1.v AS SELECT * FROM view(SELECT * FROM %)",
         "CREATE MATERIALIZED VIEW db1.mv TO db1.o AS SELECT * FROM %",
         "CREATE MATERIALIZED VIEW db1.mv ENGINE = Memory AS SELECT * FROM %", "INSERT INTO FUNCTION % SELECT * FROM db1.o",
         "CREATE TABLE db1.n AS %", "DELETE FROM db1.o WHERE a IN (SELECT a FROM %)",
         "UPDATE db1.o SET a = 1 WHERE a IN (SELECT a FROM %)", "DESCRIBE TABLE %", "DESC %"}) {
    std::string sql = pattern;
    sql.replace(sql.find('%'), 1, fn);
    rows.push_back({sql, merge_refused});
  }
  for (const std::string function : {"remote", "remoteSecure", "clusterAllReplicas"}) {
    const std::string call = function + (function == "clusterAllReplicas" ? "('c'" : "('h'") + ", currentDatabase(), 'x')";
    const std::string refused = "table function " + function + " is not accepted";
    rows.push_back({"SELECT * FROM " + call, refused});
    rows.push_back({"INSERT INTO db1.o SELECT * FROM " + call, refused});
    rows.push_back({"DESCRIBE TABLE " + call, refused});
  }
  for (const bool si : {false, true}) {
    for (const auto &[sql, message] : rows) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), message);
      // Only an ordinary table the SELECT handler had already read is reported.
      for (const auto &access : resp.original_accessed_tables())
        EXPECT_EQ(access.original_database() + "." + access.original_table(), "db1.o");
    }
  }
  // With the SI surface active an SI namespace in the function keeps its SI
  // message in the write positions too.
  for (const char *sql : {"INSERT INTO db1.o SELECT * FROM merge('hg_safe', 'db1__t')",
                          "CREATE TABLE db1.n ENGINE = Memory AS SELECT * FROM merge('hg_safe', 'db1__t')",
                          "CREATE VIEW db1.v AS SELECT * FROM merge('hg_safe', 'db1__t')",
                          "INSERT INTO FUNCTION merge('hg_safe', 'db1__t') SELECT * FROM db1.o",
                          "CREATE TABLE db1.n AS merge('hg_safe', 'db1__t')",
                          "DELETE FROM db1.o WHERE a IN (SELECT a FROM merge('hg_safe', 'db1__t'))"}) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql, /*si=*/true);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), "storage-integrity physical table hg_safe.db1__t is not directly addressable");
    EXPECT_EQ(accessedPairs(resp), (AccessedPairs{{"hg_safe", "db1__t"}}));
  }
}

// A DESCRIBE whose target is a table function: T5 names a refused or unknown
// one in both SI states (rewriter-go runs the allowlist on the target in
// both); an admitted one whose arguments carry a query is an unmodelled shape
// (the DESCRIBE handler would forward its body unrewritten); any other
// admitted one is forwarded. Measured against rewriter-go db2e246.
TEST(TableRefAllowlist, DescribeTargetFunction) {
  const std::string unmodelled_active(rewriter_handlers::kStorageIntegrityUnmodelledMessage);
  const std::string unmodelled(rewriter_handlers::kUnsupportedStatementMessage);
  for (const bool si : {false, true}) {
    for (const auto &[sql, message] : std::vector<std::pair<std::string, std::string>>{
           {"DESCRIBE TABLE executable('x.sh', 'TSV', 'a UInt8')", "table function executable is not accepted"},
           {"DESCRIBE TABLE frobnicate('x')", "table function frobnicate is not recognised"},
           {"DESC viewExplain('EXPLAIN', '', (SELECT * FROM db1.o))", "table function viewExplain is not recognised"},
           {"DESCRIBE TABLE view(SELECT * FROM db1.o)", si ? unmodelled_active : unmodelled},
           {"DESCRIBE TABLE VIEW(SELECT * FROM `db2.x`)", si ? unmodelled_active : unmodelled},
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
      EXPECT_EQ(resp.message(), message);
      EXPECT_EQ(accessedPairs(resp), AccessedPairs{});
    }
    for (const char *sql : {"DESCRIBE TABLE numbers(10)", "DESC url('http://127.0.0.1/x', CSV)"}) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
  }
  // With the SI surface active the allowlist names the function ahead of the
  // DESCRIBE handler's SI checks (rewriter-go's order).
  const auto merge = RunTableRef("DESCRIBE TABLE merge('hg_safe', 'db1__t')", /*si=*/true);
  EXPECT_EQ(merge.code(), rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_EQ(merge.message(), "table function merge is not accepted");
}

// Spec 2026-09-26 T5: the table-engine allowlist (case-sensitive: a list
// name in another case is "not recognised"; Replicated* only without
// arguments) and the disk / storage_policy table settings, for CREATE TABLE,
// a materialized view's own storage and ALTER … MODIFY SETTING.
TEST(TableRefAllowlist, EnginesAndTableSettings) {
  // `both`: also refused as T5 while the SI surface is active. A row whose
  // engine argument names the SI logical database db1 is left out there: the
  // SI namespace policy answers it first (TableRefAllowlist.StorageIntegrity…).
  struct Case { const char *sql; std::string message; bool both; };
  const std::vector<Case> refused = {
    {"CREATE TABLE db1.n (a UInt64) ENGINE = Merge('db1', '^o')", "table engine Merge is not accepted", false},
    {"CREATE TABLE db1.n (a UInt64) ENGINE = Buffer(db1.o, 16, 10, 100, 10000, 1000000, 10000000, 100000000)",
     "table engine Buffer is not accepted", false},
    {"CREATE TABLE db1.n (a UInt64) ENGINE = Distributed(default, db1.o)", "table engine Distributed is not accepted", false},
    {"CREATE TABLE db1.n (a UInt64) ENGINE = URL('http://127.0.0.1/x', CSV)", "table engine URL is not accepted", true},
    {"CREATE TABLE db1.n (a UInt64) ENGINE = KeeperMap('/x')", "table engine KeeperMap is not accepted", true},
    {"CREATE TABLE db1.n (a UInt64) ENGINE = Frob", "table engine Frob is not accepted", true},
    {"CREATE TABLE db1.n (a UInt64) ENGINE = ReplicatedMergeTree('/clickhouse/tables/x', 'r1') ORDER BY a",
     "table engine ReplicatedMergeTree is not accepted", true},
    {"CREATE TABLE db1.n (a UInt64) ENGINE = memory", "table engine memory is not recognised", true},
    {"CREATE TABLE db1.n (a UInt64) ENGINE = MergeTree ORDER BY a SETTINGS storage_policy = 's3'",
     "table setting storage_policy is not accepted", true},
    {"CREATE TABLE db1.n (a UInt64) ENGINE = MergeTree ORDER BY a SETTINGS disk = 'd'",
     "table setting disk is not accepted", true},
    {"ALTER TABLE db1.o MODIFY SETTING disk = 'd'", "table setting disk is not accepted", true},
    {"CREATE MATERIALIZED VIEW db1.mv ENGINE = Merge(currentDatabase(), '^db2') AS SELECT * FROM db1.o",
     "table engine Merge is not accepted", true},
    {"CREATE MATERIALIZED VIEW db1.mv ENGINE = MergeTree ORDER BY a SETTINGS storage_policy = 'p' AS SELECT * FROM db1.o",
     "table setting storage_policy is not accepted", true},
  };
  for (const bool si : {false, true}) {
    for (const auto &c : refused) {
      if (si && !c.both) continue;
      SCOPED_TRACE(std::string(c.sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(c.sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), c.message);
    }
  }
  for (const char *sql : {"CREATE TABLE db1.n (a UInt64) ENGINE = MergeTree ORDER BY a",
                          "CREATE TABLE db1.n (a UInt64) ENGINE = ReplicatedMergeTree ORDER BY a",
                          "CREATE TABLE db1.n (a UInt64) ENGINE = Memory"}) {
    SCOPED_TRACE(sql);
    EXPECT_EQ(RunTableRef(sql).code(), rewriter::RewriteCode::Success);
  }
  // rewriter-go refuses the inner-table UUID clause and a database ENGINE
  // clause as unsupported statements (its parser has no grammar for them);
  // the answer is kept level even when the engine itself is on the list.
  for (const bool si : {false, true}) {
    for (const char *sql : {
           "CREATE MATERIALIZED VIEW db1.mv TO INNER UUID '00000000-0000-0000-0000-000000000001' "
           "ENGINE = MergeTree ORDER BY a AS SELECT * FROM db1.o",
           "CREATE MATERIALIZED VIEW db1.mv TO INNER UUID '00000000-0000-0000-0000-000000000001' "
           "ENGINE = MergeTree ORDER BY a SETTINGS index_granularity = 1 AS SELECT * FROM db1.o",
           "CREATE TABLE db1.ts ENGINE = TimeSeries DATA INNER UUID '00000000-0000-0000-0000-000000000001'",
           "CREATE DATABASE dbx ENGINE = MergeTree",
           "CREATE DATABASE dbx ENGINE = Atomic"}) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), "statement is not supported");
    }
  }
  // While the SI surface is active the SI namespace policy reads a
  // materialized view's own engine first.
  const auto regexp = RunTableRef(
    "CREATE MATERIALIZED VIEW db1.mv ENGINE = Merge(REGEXP('hg_.*'),'.*') AS SELECT * FROM db1.o", /*si=*/true);
  EXPECT_EQ(regexp.code(), rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_EQ(regexp.message(), "storage-integrity Merge table engine namespace is not statically resolvable");
}

// Spec 2026-09-26 T6 as merged in rewriter-go: joinGet / joinGetOrNull and
// the dictGet family are refused statement-wide (no ClickHouse spelling
// addresses a dotted-name tenant object); hasColumnInTable is rewritten in a
// rewritten SELECT body and refused elsewhere.
TEST(TableRefLookup, ResolvesOrRefuses) {
  struct Case { const char *sql; bool si; std::string message; };
  const std::vector<Case> refused = {
    {"SELECT joinGet('db1.j', 'v', 1)", false, "joinGet target \"db1.j\""},
    {"SELECT joinGet(db1.j, 'v', 1)", false, "joinGet target \"db1.j\""},
    {"SELECT joinGet(concat('db1', '.j'), 'v', 1)", false, "joinGet target \"\""},
    {"SELECT dictGet('db1.d', 'v', 1)", false, "dictGet target \"db1.d\""},
    {"SELECT joinGet('db1.t', 'v', 1)", true, "joinGet target \"db1.t\""},
    {"SELECT dictGet(currentDatabase() || '.d', 'v', 1)", true, "dictGet target \"\""},
    {"UPDATE db1.o SET a = hasColumnInTable('db1', 'j', 'v') WHERE 1", false, "hasColumnInTable target \"db1.j\""},
    {"ALTER TABLE db1.o UPDATE a = `joinGet`('default.j', 'v', 1) WHERE 1", false, "joinGet target \"default.j\""},
    {"INSERT INTO db1.o VALUES (joinGet('db1.j', 'v', 1))", false, "joinGet target \"db1.j\""},
    {"INSERT INTO db1.o VALUES (1), (dictGet('db1.d', 'v', 1))", true, "dictGet target \"db1.d\""},
  };
  for (const auto &c : refused) {
    SCOPED_TRACE(c.sql);
    const auto resp = RunTableRef(c.sql, c.si);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest) << resp.message();
    EXPECT_EQ(resp.message(), c.message + " does not resolve through the caller's databases");
    EXPECT_EQ(resp.sql_after_rewrite(), c.sql);
  }
  const auto rewritten = RunTableRef("SELECT hasColumnInTable('db1', 'j', 'v')");
  EXPECT_EQ(rewritten.code(), rewriter::RewriteCode::Success) << rewritten.message();
  EXPECT_EQ(rewritten.sql_after_rewrite(), "SELECT hasColumnInTable('phys', 'db1.j', 'v')");
  ASSERT_EQ(rewritten.original_accessed_tables_size(), 1);
  EXPECT_EQ(rewritten.original_accessed_tables(0).original_table(), "j");
  EXPECT_EQ(rewritten.table_rewrites().at("db1.j"), "phys.db1.j");
  const auto once = RunTableRef(
    "SELECT hasColumnInTable('db1', 'o', 'a'), hasColumnInTable('db1', 'o', 'b') FROM db1.o");
  EXPECT_EQ(once.code(), rewriter::RewriteCode::Success) << once.message();
  EXPECT_EQ(once.original_accessed_tables_size(), 1);
}

// T6 release-gate shapes, each pinned to rewriter-go db2e246 (cmd/rewrite,
// FFI v0.14.0) in both SI states: a non-literal or three-part lookup target,
// every lookup spelling and position (VALUES rows included, in any row
// separator form), hasColumnInTable pairs that do not resolve, and the
// positions where hasColumnInTable is rewritten.
TEST(TableRefLookup, MatchesRewriterGo) {
  struct Refused { const char *sql; std::string message; };
  const std::vector<Refused> refused = {
    {"SELECT joinGet({p:String}, 'v', 1)", "joinGet target \"\""},
    {"SELECT joinGet(concat('phys', '.x'), 'v', 1)", "joinGet target \"\""},
    {"SELECT joinGet(1, 'v', 1)", "joinGet target \"\""},
    {"SELECT joinGet('', 'v', 1)", "joinGet target \"\""},
    {"SELECT dictGet(phys.x.y, 'v', 1)", "dictGet target \"phys.x.y\""},
    {"SELECT joinGet(phys.x.y, 'v', 1)", "joinGet target \"phys.x.y\""},
    {"SELECT joinGet(db1.x.y, 'v', 1)", "joinGet target \"db1.x.y\""},
    {"SELECT joinGet(`db1.j`, 'v', 1)", "joinGet target \"db1.j\""},
    {"SELECT JOINGET('db1.j', 'v', 1)", "JOINGET target \"db1.j\""},
    {"SELECT joinGetOrNull('db1.j', 'v', 1)", "joinGetOrNull target \"db1.j\""},
    {"SELECT dictHas('db1.d', 1)", "dictHas target \"db1.d\""},
    {"SELECT dictGetOrDefault('db1.d', 'v', 1, 0)", "dictGetOrDefault target \"db1.d\""},
    {"SELECT hasColumnInTable('db1.x', 'j', 'v')", "hasColumnInTable target \"db1.x.j\""},
    {"SELECT hasColumnInTable('', 'j', 'v')", "hasColumnInTable target \".j\""},
    {"SELECT hasColumnInTable('db1', '', 'v')", "hasColumnInTable target \"db1.\""},
    {"SELECT hasColumnInTable('db9', 'j', 'v')", "hasColumnInTable target \"db9.j\""},
    {"SELECT hasColumnInTable(currentDatabase(), 'j', 'v')", "hasColumnInTable target \"\""},
    {"SELECT hasColumnInTable('db1', 'j')", "hasColumnInTable target \"\""},
    {"SELECT * FROM db1.o WHERE a IN (SELECT joinGet('db1.j', 'v', 1))", "joinGet target \"db1.j\""},
    {"WITH joinGet('db1.j','v',1) AS x SELECT x", "joinGet target \"db1.j\""},
    {"SHOW TABLES WHERE name = joinGet('db1.j', 'v', 1)", "joinGet target \"db1.j\""},
    // Command text (an ALTER action, a SHOW trailing clause): any call counts.
    {"SHOW TABLES WHERE name = joinGet()", "joinGet target \"\""},
    {"ALTER TABLE db1.o UPDATE a = joinGet() WHERE 1", "joinGet target \"\""},
    {"ALTER TABLE db1.mv MODIFY QUERY SELECT joinGet() AS a", "joinGet target \"\""},
    {"ALTER TABLE db1.o UPDATE a = dictGet('phys.d', 'v', 1) WHERE 1", "dictGet target \"phys.d\""},
    {"ALTER TABLE db1.o UPDATE a = hasColumnInTable('db1', 'j', 'v') WHERE 1", "hasColumnInTable target \"db1.j\""},
    {"ALTER TABLE db1.o DELETE WHERE dictGet('db1.d', 'v', 1) = 1", "dictGet target \"db1.d\""},
    {"ALTER TABLE db1.o MODIFY COLUMN a UInt64 DEFAULT dictGet('db1.d', 'v', 1)", "dictGet target \"db1.d\""},
    {"CREATE TABLE db1.n (a UInt64 DEFAULT joinGet('db1.j', 'v', 1)) ENGINE = Memory", "joinGet target \"db1.j\""},
    {"DELETE FROM db1.o WHERE dictGet('db1.d', 'v', 1) = 1", "dictGet target \"db1.d\""},
    {"INSERT INTO db1.o VALUES (1) (dictGet('db1.d', 'v', 1))", "dictGet target \"db1.d\""},
    {"INSERT INTO db1.o VALUES (dictGet('db1.d', 'v', 1));", "dictGet target \"db1.d\""},
    {"INSERT INTO db1.o VALUES (1, dictGet('db1.d', 'v', 1)), (2, 3)", "dictGet target \"db1.d\""},
    {"INSERT INTO db1.o VALUES (1), ('a' || dictGet('db1.d', 'v', 1))", "dictGet target \"db1.d\""},
    {"INSERT INTO db1.o FORMAT Values (dictGet('db1.d', 'v', 1))", "dictGet target \"db1.d\""},
    {"INSERT INTO db1.o SETTINGS max_threads=1 VALUES (dictGet('db1.d','v',1))", "dictGet target \"db1.d\""},
    {"INSERT INTO db1.o (a) VALUES (dictGet('db1.d','v',1))", "dictGet target \"db1.d\""},
    {"INSERT INTO db1.o VALUES (dictGet(phys.x.y, 'v', 1))", "dictGet target \"phys.x.y\""},
    {"INSERT INTO db1.o VALUES ((SELECT dictGet('db1.d','v',1)))", "dictGet target \"db1.d\""},
    {"INSERT INTO db1.o VALUES ((SELECT hasColumnInTable('db1','j','v')))", "hasColumnInTable target \"db1.j\""},
    {"INSERT INTO db1.o VALUES (hasColumnInTable('db1', 'j', 'v'))", "hasColumnInTable target \"db1.j\""},
  };
  for (const bool si : {false, true}) {
    for (const auto &c : refused) {
      SCOPED_TRACE(std::string(c.sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(c.sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest) << resp.message();
      EXPECT_EQ(resp.message(), c.message + " does not resolve through the caller's databases");
      EXPECT_EQ(resp.sql_after_rewrite(), c.sql);
      EXPECT_EQ(resp.original_accessed_tables_size(), 0);
    }
    // A protected database named in a VALUES-row lookup is T3, which precedes
    // T6; an opaque or unreadable VALUES text holding a lookup is R2.
    for (const char *sql : {"INSERT INTO db1.o (a) VALUES (dictGet('phys.d','v',1))",
                            "INSERT INTO db1.o VALUES (hasColumnInTable('phys', 'j', 'v'))"}) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest) << resp.message();
      EXPECT_EQ(resp.message(), "protected database phys is not addressable");
      ASSERT_EQ(resp.original_accessed_tables_size(), 1);
      EXPECT_EQ(resp.original_accessed_tables(0).original_database(), "phys");
      EXPECT_EQ(resp.original_accessed_tables(0).original_table(), "");
    }
    for (const char *sql : {"INSERT INTO db1.o (a) SETTINGS max_threads=1 VALUES (dictGet('db1.d','v',1))",
                            "INSERT INTO db1.o VALUES (dictGet('db1.d', 'v', 1)) SETTINGS x = 1"}) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), "statement is not supported");
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
    // Refused on both engines with a different message (fail-closed; message
    // parity is tracked for T15 / T22): C++ refuses DESCRIBE of a subquery as
    // an unmodelled class (T7) before T6.
    const auto describe = RunTableRef("DESCRIBE TABLE (SELECT joinGet('db1.j','v',1))", si);
    EXPECT_EQ(describe.code(), rewriter::RewriteCode::UnsupportedStatement) << describe.message();
    // An unresolved pair in an INSERT … SELECT / CTAS body answers
    // UnsupportedStatement, in a view body InvalidRewriteRequest.
    for (const auto &[sql, code] : std::vector<std::pair<const char *, rewriter::RewriteCode>>{
           {"INSERT INTO db1.o SELECT hasColumnInTable('db9', 'j', 'v')", rewriter::RewriteCode::UnsupportedStatement},
           {"CREATE TABLE db1.n ENGINE = Memory AS SELECT hasColumnInTable('db9', 'j', 'v') AS c",
            rewriter::RewriteCode::UnsupportedStatement},
           {"CREATE VIEW db1.vv AS SELECT hasColumnInTable('db9', 'j', 'v') AS c",
            rewriter::RewriteCode::InvalidRewriteRequest},
           {"CREATE MATERIALIZED VIEW db1.mv TO db1.o AS SELECT hasColumnInTable('db9', 'j', 'v') AS a",
            rewriter::RewriteCode::InvalidRewriteRequest}}) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), code) << resp.message();
      EXPECT_EQ(resp.message(), "hasColumnInTable target \"db9.j\" does not resolve through the caller's databases");
    }
    const auto bare = RunTableRef("SELECT joinGet()", si);
    EXPECT_EQ(bare.code(), rewriter::RewriteCode::Success) << bare.message();
    EXPECT_EQ(bare.sql_after_rewrite(), "SELECT joinGet()");
  }
  // A lookup in a VALUES row of an INSERT into an Active table: T6 in both
  // SI states. An Active INSERT target is not a refusal cause, so the SI
  // reject annotator keeps T6 (rewriter-go parity).
  const std::string si_target = "INSERT INTO db1.t VALUES (dictGet('db1.d', 'v', 1))";
  for (const bool si : {false, true}) {
    const auto resp = RunTableRef(si_target, si);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
    EXPECT_EQ(resp.message(), "dictGet target \"db1.d\" does not resolve through the caller's databases");
  }
  // An Active pair is not rewritten: refused with the SI surface active only.
  const auto active = RunTableRef("SELECT hasColumnInTable('db1', 't', 'v')", /*si=*/true);
  EXPECT_EQ(active.code(), rewriter::RewriteCode::InvalidRewriteRequest) << active.message();
  EXPECT_EQ(active.message(), "hasColumnInTable target \"db1.t\" does not resolve through the caller's databases");
  EXPECT_EQ(active.sql_after_rewrite(), "SELECT hasColumnInTable('db1', 't', 'v')");
  const auto ordinary = RunTableRef("SELECT hasColumnInTable('db1', 't', 'v')");
  EXPECT_EQ(ordinary.code(), rewriter::RewriteCode::Success) << ordinary.message();
  EXPECT_EQ(ordinary.sql_after_rewrite(), "SELECT hasColumnInTable('phys', 'db1.t', 'v')");

  // Rewritten positions: the pair is replaced, recorded in table_rewrites and
  // reported after the statement's own tables unless one is the same table.
  struct Rewritten { const char *sql; const char *call; std::vector<std::pair<std::string, std::string>> accessed; };
  const std::vector<Rewritten> rewritten = {
    {"SELECT hasColumnInTable('h', 'db1', 'j', 'v')", "hasColumnInTable('h', 'phys', 'db1.j', 'v')", {{"db1", "j"}}},
    {"SELECT HASCOLUMNINTABLE('db1', 'j', 'v')", "HASCOLUMNINTABLE('phys', 'db1.j', 'v')", {{"db1", "j"}}},
    {"SELECT * FROM db1.o WHERE hasColumnInTable('db1', 'z', 'a')", "hasColumnInTable('phys', 'db1.z', 'a')", {{"db1", "o"}, {"db1", "z"}}},
    {"SELECT hasColumnInTable('db1', 'j', 'v') FROM db1.o", "hasColumnInTable('phys', 'db1.j', 'v')", {{"db1", "o"}, {"db1", "j"}}},
    {"SELECT hasColumnInTable('db1', 'o', 'a') FROM `db1.o`", "hasColumnInTable('phys', 'db1.o', 'a')", {{"", "db1.o"}, {"db1", "o"}}},
    {"SELECT * FROM view(SELECT hasColumnInTable('db1', 'j', 'v'))", "hasColumnInTable('phys', 'db1.j', 'v')", {{"db1", "j"}}},
    {"INSERT INTO db1.o SELECT hasColumnInTable('db1', 'j', 'v')", "hasColumnInTable('phys', 'db1.j', 'v')", {{"db1", "o"}, {"db1", "j"}}},
    {"CREATE TABLE db1.n ENGINE = Memory AS SELECT hasColumnInTable('db1', 'j', 'v') AS c", "hasColumnInTable('phys', 'db1.j', 'v')", {{"db1", "n"}, {"db1", "j"}}},
    {"CREATE VIEW db1.vv AS SELECT hasColumnInTable('db1', 'j', 'v') AS c", "hasColumnInTable('phys', 'db1.j', 'v')", {{"db1", "vv"}, {"db1", "j"}}},
  };
  for (const bool si : {false, true}) {
    for (const auto &c : rewritten) {
      SCOPED_TRACE(std::string(c.sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(c.sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_NE(resp.sql_after_rewrite().find(c.call), std::string::npos) << resp.sql_after_rewrite();
      std::vector<std::pair<std::string, std::string>> accessed;
      for (const auto &a : resp.original_accessed_tables())
        accessed.emplace_back(a.original_database(), a.original_table());
      EXPECT_EQ(accessed, c.accessed);
    }
  }
}

// ClickHouse resolves a FORMAT name case-insensitively, so `FORMAT values`
// runs the rows as Values: the T6 and T3 row checks apply to every spelling
// (rewriter-go db2e246, both SI states). A string that merely names a lookup
// is not a call.
TEST(TableRefLookup, ValuesFormatNameIsCaseInsensitive) {
  const std::string unresolved = " does not resolve through the caller's databases";
  for (const bool si : {false, true}) {
    for (const char *format : {"values", "VALUES", "vAlUeS"}) {
      const std::string prefix = std::string("INSERT INTO db1.o FORMAT ") + format + " ";
      const std::string lookup = prefix + "(joinGet('db1.j', 'v', 1))";
      SCOPED_TRACE(lookup + (si ? " [V2]" : " [inactive]"));
      const auto refused = RunTableRef(lookup, si);
      EXPECT_EQ(refused.code(), rewriter::RewriteCode::InvalidRewriteRequest) << refused.message();
      EXPECT_EQ(refused.message(), "joinGet target \"db1.j\"" + unresolved);
      EXPECT_EQ(refused.sql_after_rewrite(), lookup);
      for (const std::string row : {"(dictGet('phys.d', 'v', 1))", "(hasColumnInTable('phys', 'j', 'v'))"}) {
        const std::string sql = prefix + row;
        SCOPED_TRACE(sql);
        const auto resp = RunTableRef(sql, si);
        EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest) << resp.message();
        EXPECT_EQ(resp.message(), "protected database phys is not addressable");
        ASSERT_EQ(resp.original_accessed_tables_size(), 1);
        EXPECT_EQ(resp.original_accessed_tables(0).original_database(), "phys");
        EXPECT_EQ(resp.original_accessed_tables(0).original_table(), "");
      }
      const auto plain = RunTableRef(prefix + "(1, 'joinGet')", si);
      EXPECT_EQ(plain.code(), rewriter::RewriteCode::Success) << plain.message();
      ASSERT_EQ(plain.original_accessed_tables_size(), 1);
      EXPECT_EQ(plain.original_accessed_tables(0).original_table(), "o");
    }
  }
}

// Inventory §6 (P), continued: a join keyword directly after a table is not
// an implicit alias with a column list. The shim read `FROM db1.o JOIN (…)`
// as alias `JOIN` with columns, cut the subquery out of the parse SQL and
// refused the statement under V2 (released v0.15.0); rewriter-go db2e246
// answers Success for every shape below in both SI states.
TEST(StorageIntegrityCompatibility, JoinKeywordIsNotAnImplicitAlias) {
  using Accessed = std::vector<std::pair<std::string, std::string>>;
  const Accessed o{{"db1", "o"}};
  const std::vector<std::pair<const char *, Accessed>> rows = {
    {"SELECT * FROM db1.o JOIN (SELECT 1 AS c) AS s ON 1", o},
    {"SELECT * FROM db1.o JOIN (SELECT 1 AS c) s ON 1", o},
    {"SELECT * FROM db1.o JOIN (SELECT 1 AS c) ON 1", o},
    {"SELECT * FROM db1.o LEFT JOIN (SELECT 1 AS c) AS s ON 1", o},
    {"SELECT * FROM db1.o INNER JOIN (SELECT 1 AS c) AS s ON 1", o},
    {"SELECT * FROM db1.o RIGHT JOIN (SELECT 1 AS c) AS s ON 1", o},
    {"SELECT * FROM db1.o FULL OUTER JOIN (SELECT 1 AS c) AS s ON 1", o},
    {"SELECT * FROM db1.o CROSS JOIN (SELECT 1 AS c) AS s", o},
    {"SELECT * FROM db1.o GLOBAL JOIN (SELECT 1 AS c) AS s ON 1", o},
    {"SELECT * FROM db1.o ANY JOIN (SELECT 1 AS c) AS s ON 1", o},
    {"SELECT * FROM db1.o ALL INNER JOIN (SELECT 1 AS c) AS s ON 1", o},
    {"SELECT * FROM db1.o ASOF JOIN (SELECT 1 AS c, 1 AS d) AS s ON a = c AND a >= d", o},
    {"SELECT * FROM db1.o SEMI LEFT JOIN (SELECT 1 AS c) AS s ON 1", o},
    {"SELECT * FROM db1.o ANTI LEFT JOIN (SELECT 1 AS c) AS s ON 1", o},
    {"SELECT * FROM db1.o PASTE JOIN (SELECT 1 AS c) AS s", o},
    {"SELECT * FROM db1.o ARRAY JOIN [1] AS z", o},
    {"SELECT * FROM db1.o LEFT ARRAY JOIN [1] AS z", o},
    {"SELECT * FROM db1.o x JOIN (SELECT 1 AS c) AS s ON 1", o},
    {"SELECT * FROM db1.o AS x JOIN (SELECT 1 AS c) AS s ON 1", o},
    {"SELECT * FROM db1.o WHERE (a, a) IN (SELECT 1, 1)", o},
    {"SELECT * FROM db1.o PREWHERE (a = 1)", o},
    {"SELECT * FROM db1.o JOIN db1.p USING (a)", {{"db1", "o"}, {"db1", "p"}}},
    {"SELECT * FROM db1.o JOIN db1.p ON (a = a)", {{"db1", "o"}, {"db1", "p"}}},
    {"SELECT * FROM db1.o SAMPLE 1 / 2", o},
    {"SELECT * FROM db1.o FINAL", o},
    {"SELECT * FROM db1.o LIMIT (SELECT 1)", o},
    {"SELECT * FROM db1.o UNION ALL (SELECT * FROM db1.o)", o},
    {"SELECT * FROM db1.o JOIN (SELECT * FROM db1.t) AS s ON 1", {{"db1", "o"}, {"db1", "t"}}},
    {"SELECT * FROM db1.t JOIN (SELECT 1 AS c) ON 1", {{"db1", "t"}}},
    {"SELECT * FROM db1.t JOIN (SELECT 1 AS c) AS s ON 1", {{"db1", "t"}}},
  };
  for (const bool si : {false, true}) {
    for (const auto &[sql, accessed] : rows) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      Accessed got;
      for (const auto &a : resp.original_accessed_tables())
        got.emplace_back(a.original_database(), a.original_table());
      EXPECT_EQ(got, accessed);
      // An Active table read keeps its derived read under V2.
      const bool reads_active = std::string(sql).find("db1.t") != std::string::npos;
      EXPECT_EQ(resp.sql_after_rewrite().find("hg_safe.db1__t") != std::string::npos, si && reads_active)
        << resp.sql_after_rewrite();
    }
  }
}

// A quoted alias is exempt from ClickHouse's restricted keyword list, and
// LOCAL / OUTER are not on it: each is a real implicit alias with a column
// list, which the storage-integrity surface refuses on an Active table as
// rewriter-go db2e246 does under V2 (quoted forms; Go's parser refuses the
// unquoted local / outer forms outright). With the surface inactive the
// quoted forms are ordinary aliases.
TEST(StorageIntegrityCompatibility, QuotedKeywordAliasWithColumnsIsRefused) {
  const std::vector<std::string> quoted = {
    "SELECT * FROM db1.t `join` (a, b)", "SELECT * FROM db1.t `only` (a, b)",
    "SELECT * FROM db1.t `local` (a, b)", "SELECT * FROM db1.t `outer` (a, b)",
    "SELECT * FROM db1.t `where` (a)"};
  std::vector<std::string> refused = quoted;
  refused.push_back("SELECT * FROM db1.t local (a)");
  refused.push_back("SELECT * FROM db1.t outer (a)");
  for (const auto &sql : refused) {
    SCOPED_TRACE(sql + " [V2]");
    const auto resp = RunTableRef(sql, /*si=*/true);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::RewriteError) << resp.message();
    EXPECT_EQ(resp.message(),
              "FINAL/SAMPLE/PREWHERE/WITH OFFSET/column aliases on storage-integrity tables are not supported");
    EXPECT_EQ(resp.sql_after_rewrite(), sql);
    ASSERT_EQ(resp.original_accessed_tables_size(), 1);
    EXPECT_EQ(resp.original_accessed_tables(0).original_database(), "db1");
    EXPECT_EQ(resp.original_accessed_tables(0).original_table(), "t");
  }
  for (const auto &sql : quoted) {
    SCOPED_TRACE(sql + " [inactive]");
    const auto resp = RunTableRef(sql);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
  }
  // An ordinary table keeps a quoted keyword alias with columns under V2.
  const auto ordinary = RunTableRef("SELECT * FROM db1.o `join` (a, b)", /*si=*/true);
  EXPECT_EQ(ordinary.code(), rewriter::RewriteCode::Success) << ordinary.message();
}

// Spec 2026-09-26 §5 "settings values" (rewriter-go R5): a SQL-bearing or
// dialect setting is a T5 table-setting refusal, named as written; a value
// that is not a numeric, string or boolean literal is `statement is not
// supported`. The session-SET carve-out applies the same rule.
TEST(TableRefSettings, SqlBearingDialectAndValueRules) {
  struct Case { const char *sql; std::string message; };
  const std::vector<Case> refused = {
    {"SELECT * FROM db1.o SETTINGS additional_table_filters = {'db1.o': 'a > 1'}",
     "table setting additional_table_filters is not accepted"},
    {"SELECT * FROM db1.o SETTINGS additional_result_filter = 'a = 1'",
     "table setting additional_result_filter is not accepted"},
    {"SELECT * FROM db1.o SETTINGS parallel_replicas_custom_key = 'a'",
     "table setting parallel_replicas_custom_key is not accepted"},
    {"SELECT * FROM db1.o SETTINGS dialect = 1", "table setting dialect is not accepted"},
    {"SELECT * FROM db1.o SETTINGS allow_experimental_kusto_dialect = 1",
     "table setting allow_experimental_kusto_dialect is not accepted"},
    {"INSERT INTO db1.o SELECT * FROM db1.p SETTINGS additional_table_filters = {'db1.p': 'a > 1'}",
     "table setting additional_table_filters is not accepted"},
    {"INSERT INTO db1.o (a) SETTINGS `POLYGLOT_DIALECT` = 'sqlite' SELECT 1",
     "table setting POLYGLOT_DIALECT is not accepted"},
    {"SELECT * FROM db1.o SETTINGS max_threads = {'a': 'b'}", std::string(rewriter_handlers::kUnsupportedStatementMessage)},
    {"SELECT * FROM db1.o SETTINGS enable_scopes_for_with_statement = 0",
     "table setting enable_scopes_for_with_statement is not accepted"},
    // ClickHouse 26.8: SQL text the server wraps around the query, the
    // current database, format overrides and the other switches
    // sqlBearingSetting lists.
    {"SELECT * FROM db1.o SETTINGS filter = 'a IN (SELECT a FROM db2.x)'", "table setting filter is not accepted"},
    {"SELECT * FROM db1.o SETTINGS select = '(SELECT 1 FROM db2.x)'", "table setting select is not accepted"},
    {"SELECT * FROM db1.o SETTINGS `order` = 'a'", "table setting order is not accepted"},
    {"SELECT * FROM db1.o SETTINGS sort = 'a'", "table setting sort is not accepted"},
    {"SELECT * FROM db1.o SETTINGS database = 'db2'", "table setting database is not accepted"},
    {"SELECT * FROM db1.o SETTINGS DATABASE = 'db2'", "table setting DATABASE is not accepted"},
    {"INSERT INTO db1.o SETTINGS input_format = 'Values' FORMAT CSV 1", "table setting input_format is not accepted"},
    {"INSERT INTO db1.o SETTINGS format = 'Values' FORMAT CSV 1", "table setting format is not accepted"},
    {"SELECT * FROM db1.o SETTINGS output_format = 'JSON'", "table setting output_format is not accepted"},
    {"SELECT * FROM db1.o SETTINGS default_format = 'JSON'", "table setting default_format is not accepted"},
    {"SELECT * FROM db1.o SETTINGS asterisk_include_virtual_columns = 1",
     "table setting asterisk_include_virtual_columns is not accepted"},
    {"SELECT * FROM db1.o SETTINGS s3_allow_server_credentials_in_user_queries = 1",
     "table setting s3_allow_server_credentials_in_user_queries is not accepted"},
    {"SELECT * FROM db1.o SETTINGS dynamic_disk_allow_from_env = 1",
     "table setting dynamic_disk_allow_from_env is not accepted"},
    {"SELECT * FROM db1.o SETTINGS url_base = 'file://'", "table setting url_base is not accepted"},
    {"SELECT * FROM db1.o SETTINGS analyzer_compatibility_allow_cte_redefinition = 1",
     "table setting analyzer_compatibility_allow_cte_redefinition is not accepted"},
    {"SELECT * FROM db1.o SETTINGS analyzer_inline_views = 1", "table setting analyzer_inline_views is not accepted"},
    {"SELECT * FROM db1.o SETTINGS enable_streaming_queries = 1",
     "table setting enable_streaming_queries is not accepted"},
  };
  // A session SET is modelled only while the SI surface is inactive.
  for (const char *sql : {"SET filter = 'a IN (SELECT a FROM db2.x)'", "SET database = 'db2'"}) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql, false);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_NE(resp.message().find(" is not accepted"), std::string::npos) << resp.message();
  }
  for (const bool si : {false, true}) {
    for (const auto &c : refused) {
      SCOPED_TRACE(std::string(c.sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(c.sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), c.message);
    }
  }
  for (const auto &[sql, message] : std::vector<std::pair<std::string, std::string>>{
         {"SET dialect = 1", "table setting dialect is not accepted"},
         {"SET polyglot_dialect = 'sqlite'", "table setting polyglot_dialect is not accepted"},
         {"SET max_threads = {'a': 'b'}", std::string(rewriter_handlers::kUnsupportedStatementMessage)}}) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), message);
  }
  EXPECT_EQ(RunTableRef("SELECT * FROM db1.o SETTINGS max_threads = 1, join_algorithm = 'hash', use_query_cache = true").code(),
            rewriter::RewriteCode::Success);
}

// R5 beyond the plain SELECT / INSERT / SET forms, each measured against
// rewriter-go: a CREATE's own storage settings refuse the SQL-bearing names
// (ALTER … MODIFY SETTING does not), `name = DEFAULT` counts, a name is refused
// before any value is judged, the statement classes with no query hook of
// their own are refused after the storage-integrity handlers accept them, and
// the literal forms the parse tree folds away (hex, binary, signed inf / nan)
// are read from the tokens.
TEST(TableRefSettings, StorageDefaultNameOrderAndLiteralForms) {
  const std::string table_setting_dialect = "table setting dialect is not accepted";
  const std::string unsupported(rewriter_handlers::kUnsupportedStatementMessage);
  struct Case { const char *sql; std::string message; };
  const std::vector<Case> refused = {
    {"CREATE TABLE db1.n (a Int) ENGINE = MergeTree ORDER BY a SETTINGS dialect = 1", table_setting_dialect},
    {"CREATE TABLE db1.n (a Int) ENGINE = MergeTree ORDER BY a SETTINGS a = 1, dialect = DEFAULT", table_setting_dialect},
    {"SELECT * FROM db1.o SETTINGS dialect = DEFAULT", table_setting_dialect},
    {"SELECT * FROM db1.o SETTINGS max_threads = {'a': 'b'}, dialect = 1", table_setting_dialect},
    {"SELECT * FROM db1.o SETTINGS max_threads = -inf", unsupported},
    {"SELECT * FROM db1.o SETTINGS max_threads = -0x10", unsupported},
    {"INSERT INTO db1.o (a) SETTINGS max_threads = 0x10 SELECT 1", unsupported},
    {"INSERT INTO db1.o (a) SETTINGS max_threads = +inf VALUES (1)", unsupported},
    {"DESCRIBE TABLE db1.o SETTINGS dialect = 1", table_setting_dialect},
    {"SHOW TABLES FROM db1 SETTINGS dialect = 1", table_setting_dialect},
    {"SHOW CREATE TABLE db1.o SETTINGS dialect = 1", table_setting_dialect},
    {"EXISTS TABLE db1.o SETTINGS dialect = 1", table_setting_dialect},
    {"DESCRIBE TABLE db1.o SETTINGS max_threads = 0x10", unsupported},
  };
  for (const bool si : {false, true}) {
    for (const auto &c : refused) {
      SCOPED_TRACE(std::string(c.sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(c.sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), c.message);
    }
  }
  // DROP TABLE … SETTINGS is refused with the storage-integrity surface active
  // (the statement carries no other hook).
  {
    const auto resp = RunTableRef("DROP TABLE db1.o SETTINGS dialect = 1", /*si=*/true);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), table_setting_dialect);
  }
  for (const auto &[sql, message] : std::vector<std::pair<std::string, std::string>>{
         {"SET dialect = DEFAULT", table_setting_dialect},
         {"SET max_threads = 0x10", unsupported},
         {"SET max_threads = +inf", unsupported},
         {"SET param_x = {'a': 'b'}", unsupported}}) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), message);
  }
  for (const bool si : {false, true}) {
    for (const char *sql : {
           "ALTER TABLE db1.o MODIFY SETTING dialect = 1",
           "SELECT * FROM db1.o SETTINGS max_threads = 0x10",
           "SELECT * FROM db1.o SETTINGS max_threads = inf, max_block_size = nan, use_query_cache = +1",
           "INSERT INTO db1.o SELECT * FROM db1.p SETTINGS max_threads = 0x10",
           "CREATE TABLE db1.n (a Int) ENGINE = MergeTree ORDER BY a SETTINGS index_granularity = {'a': 'b'}"}) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    }
  }
  EXPECT_EQ(RunTableRef("SET max_threads = 1, max_block_size = 'a', use_query_cache = true, x = DEFAULT").code(),
            rewriter::RewriteCode::Success);
}

// rewriter-go parity (TestAnnotateStorageIntegrityReject_*): only a proven
// statement target or a classified table / namespace read upgrades a
// rejection's message; a lexical lookalike keeps the caller's message.
TEST(TableRefAnnotation, OnlyProvenNamesUpgradeTheMessage) {
  rewriter::RewriteSQLRequest options;
  AddTableRefOption(options, /*si=*/true);
  const auto &args = options.options(0).table_name_args().dynamic_args();
  auto annotate = [&](const std::string &sql) {
    const char *begin = sql.data();
    const char *end = begin + sql.size();
    DB::ParserQuery parser(end, false);
    const auto ast = DB::parseQuery(parser, begin, end, "", 0, 0, 0);
    rewriter::RewriteSQLResponse resp;
    resp.set_code(rewriter::RewriteCode::UnsupportedStatement);
    resp.set_message("original");
    rewriter_handlers::annotateStorageIntegrityReject(&resp, ast, args);
    return resp.message();
  };
  for (const char *sql : {
         "SELECT hg_safe.x FROM other.u AS hg_safe",
         "SELECT x FROM other.u AS hg_safe",
         "SELECT x AS hg_safe FROM other.u",
         "UPDATE other.u SET x = (WITH RECURSIVE t AS (SELECT count() FROM t) SELECT count() FROM t) WHERE id = 1",
         "SYSTEM RELOAD CONFIG ON CLUSTER hg_safe",
         "OPTIMIZE TABLE other.u FINAL SETTINGS hg_safe = 1",
       }) {
    SCOPED_TRACE(sql);
    EXPECT_EQ(annotate(sql), "original");
  }
  EXPECT_EQ(annotate("DESCRIBE (SELECT * FROM hg_safe.db1__t)"),
            "storage-integrity physical table hg_safe.db1__t is not directly addressable");
  EXPECT_EQ(annotate("CHECK TABLE db1.t"),
            "storage-integrity table db1.t accepts writes only through the signed statement lane");
  EXPECT_EQ(annotate("DETACH DATABASE hg_unsafe"),
            "storage-integrity physical database hg_unsafe is not directly addressable");
}

// The annotator's structural target list: RENAME sides, every table of a
// multi-table DROP and a BACKUP / RESTORE database element each name an hg_*
// object; an Active target is named only for an opaque class or the SI
// catch-all. Called directly so no handler answers first.
TEST(TableRefAnnotation, StatementTargetsNameHgObjects) {
  rewriter::RewriteSQLRequest options;
  AddTableRefOption(options, /*si=*/true);
  const auto &args = options.options(0).table_name_args().dynamic_args();
  auto annotate = [&](const std::string &sql, const std::string &message = "original") {
    const char *begin = sql.data();
    const char *end = begin + sql.size();
    DB::ParserQuery parser(end, false);
    const auto ast = DB::parseQuery(parser, begin, end, "", 0, 0, 0);
    rewriter::RewriteSQLResponse resp;
    resp.set_code(rewriter::RewriteCode::UnsupportedStatement);
    resp.set_message(message);
    rewriter_handlers::annotateStorageIntegrityReject(&resp, ast, args);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
    return resp.message();
  };
  const std::vector<std::pair<std::string, std::string>> rows = {
    {"RENAME TABLE db1.o TO hg_safe.x", "storage-integrity physical table hg_safe.x is not directly addressable"},
    {"RENAME TABLE hg_safe.db1__t TO db1.z",
     "storage-integrity physical table hg_safe.db1__t is not directly addressable"},
    {"RENAME TABLE db1.o TO db1.z, db1.p TO hg_unsafe.y",
     "storage-integrity physical table hg_unsafe.y is not directly addressable"},
    {"RENAME DATABASE hg_safe TO z", "storage-integrity physical database hg_safe is not directly addressable"},
    {"DROP TABLE db1.o, hg_safe.db1__t",
     "storage-integrity physical table hg_safe.db1__t is not directly addressable"},
    {"DROP TABLE hg_unsafe.db1__t, db1.o",
     "storage-integrity physical table hg_unsafe.db1__t is not directly addressable"},
    {"BACKUP DATABASE hg_safe TO Disk('d', 'x')",
     "storage-integrity physical database hg_safe is not directly addressable"},
    {"BACKUP DATABASE hg_unsafe TO Disk('d', 'x')",
     "storage-integrity physical database hg_unsafe is not directly addressable"},
    {"RESTORE DATABASE db1 AS hg_promote FROM Disk('d', 'x')",
     "storage-integrity physical database hg_promote is not directly addressable"},
    // Not SI objects: the caller's message stands.
    {"RENAME TABLE db1.o TO db1.z", "original"},
    {"DROP TABLE db1.o, db1.p", "original"},
    {"BACKUP DATABASE db1 TO Disk('d', 'x')", "original"},
    // An Active target is not a refusal cause outside the SI catch-all.
    {"DROP TABLE db1.o, db1.t", "original"},
    {"RENAME TABLE db1.t TO db1.z", "original"},
  };
  for (const auto &[sql, want] : rows) {
    SCOPED_TRACE(sql);
    EXPECT_EQ(annotate(sql), want);
  }
  const std::string catch_all(rewriter_handlers::kStorageIntegrityUnmodelledMessage);
  EXPECT_EQ(annotate("DROP TABLE db1.o, db1.t", catch_all),
            "storage-integrity table db1.t accepts writes only through the signed statement lane");
  EXPECT_EQ(annotate("BACKUP DATABASE db1 TO Disk('d', 'x')", catch_all), catch_all);
}

// The annotator runs from a destructor, so nothing may escape it. A
// multi-table DROP whose table list holds a non-identifier makes ClickHouse's
// getRewrittenASTsOfSingleTable throw; the caller's refusal is kept intact.
TEST(TableRefAnnotation, AnInternalErrorKeepsTheCallersRefusal) {
  rewriter::RewriteSQLRequest options;
  AddTableRefOption(options, /*si=*/true);
  const auto &args = options.options(0).table_name_args().dynamic_args();
  const std::string sql = "DROP TABLE db1.o, hg_safe.db1__t";
  const char *begin = sql.data();
  const char *end = begin + sql.size();
  DB::ParserQuery parser(end, false);
  const auto ast = DB::parseQuery(parser, begin, end, "", 0, 0, 0);
  auto *drop = ast->as<DB::ASTDropQuery>();
  ASSERT_NE(drop, nullptr);
  ASSERT_NE(drop->database_and_tables, nullptr);
  drop->database_and_tables->children[0] = DB::make_intrusive<DB::ASTLiteral>(DB::Field(1));
  rewriter::RewriteSQLResponse resp;
  resp.set_code(rewriter::RewriteCode::InvalidRewriteRequest);
  resp.set_message("original");
  EXPECT_NO_THROW(rewriter_handlers::annotateStorageIntegrityReject(&resp, ast, args));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest);
  EXPECT_EQ(resp.message(), "original");
}

// The T3 arm for the function an INSERT INTO FUNCTION writes through and a
// CREATE TABLE … AS fn() copies, in both SI states; a function that names no
// database keeps its class refusal (rewriter-go's answers).
TEST(TableRefPreflight, StatementFunctionDatabaseIsExamined) {
  const std::string class_insert = "INSERT INTO FUNCTION(...) is not supported";
  const std::string class_create = "CREATE TABLE AS table_function(...) is not supported";
  for (const bool si : {false, true}) {
    for (const char *sql : {"INSERT INTO FUNCTION remote('h', 'phys', 'x') SELECT 1",
                            "INSERT INTO FUNCTION merge('phys', 'x') SELECT 1",
                            "CREATE TABLE db1.n AS merge('phys', 'x')",
                            "CREATE TABLE db1.n AS remote('h', 'phys', 'x')"}) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest) << resp.message();
      EXPECT_EQ(resp.message(), "protected database phys is not addressable");
    }
    const std::vector<std::pair<const char *, std::string>> not_t3 = {
      {"INSERT INTO FUNCTION numbers(10) SELECT 1", class_insert},
      {"INSERT INTO FUNCTION values('a Int', 1) SELECT 1", class_insert},
      {"INSERT INTO FUNCTION null('a Int') SELECT 1", class_insert},
      {"CREATE TABLE db1.n AS numbers(10)", class_create},
      {"CREATE TABLE db1.n AS values('a Int', 1)", class_create},
    };
    for (const auto &[sql, want] : not_t3) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), want);
    }
  }
  // An hg_* database in the function is the SI handlers' while the surface
  // is active, T3 while it is not.
  EXPECT_EQ(RunTableRef("CREATE TABLE db1.n AS remote('h', 'hg_safe', 'db1__t')").message(),
            "protected database hg_safe is not addressable");
  EXPECT_EQ(RunTableRef("CREATE TABLE db1.n AS remote('h', 'hg_safe', 'db1__t')", /*si=*/true).message(),
            "storage-integrity physical table hg_safe.db1__t is not directly addressable");
}

// Message precedence through the whole pipeline with contract V2 active (the
// items carried to Task 15). Rows marked Go are rewriter-go's exact answer;
// rows marked §5 pin spec 2026-09-26 §5 ("T3, with the SI messages first
// while the surface is active") where rewriter-go still answers the later
// T3 / T5 / T6 / R5 refusal (a rewriter-go follow-up).
TEST(TableRefAnnotation, MessagePrecedenceUnderAnActiveSurface) {
  const std::string phys = "protected database phys is not addressable";
  const std::string hg_safe_t = "storage-integrity physical table hg_safe.db1__t is not directly addressable";
  const std::string active_t = "storage-integrity table db1.t accepts writes only through the signed statement lane";
  const std::string catch_all(rewriter_handlers::kStorageIntegrityUnmodelledMessage);
  struct Row {
    const char *sql;
    rewriter::RewriteCode code;
    std::string message;
  };
  const std::vector<Row> rows = {
    // Go: an Active table or a word `t` never replaces T3 (T5 M1).
    {"SELECT * FROM db1.t JOIN phys.x ON 1", rewriter::RewriteCode::InvalidRewriteRequest, phys},
    {"SELECT t FROM phys.x", rewriter::RewriteCode::InvalidRewriteRequest, phys},
    {"SELECT hg_safe FROM phys.x", rewriter::RewriteCode::InvalidRewriteRequest, phys},
    {"INSERT INTO db1.t SELECT * FROM phys.x", rewriter::RewriteCode::InvalidRewriteRequest, phys},
    {"ALTER TABLE db1.t DELETE WHERE a IN (SELECT a FROM phys.x)", rewriter::RewriteCode::InvalidRewriteRequest, phys},
    // Go: an INSERT into an Active table keeps T6 (T13) and R5 (T14).
    {"INSERT INTO db1.t VALUES (joinGet({p:String}, 'a', 1))", rewriter::RewriteCode::InvalidRewriteRequest,
     "joinGet target \"\" does not resolve through the caller's databases"},
    {"INSERT INTO t SELECT dictGet(concat('a','b'), 'a', 1)", rewriter::RewriteCode::InvalidRewriteRequest,
     "dictGet target \"\" does not resolve through the caller's databases"},
    {"INSERT INTO db1.t FORMAT Values (dictGet('db1.x.y', 'a', 1))", rewriter::RewriteCode::InvalidRewriteRequest,
     "dictGet target \"db1.x.y\" does not resolve through the caller's databases"},
    {"INSERT INTO db1.t SETTINGS dialect = 1 VALUES (1)", rewriter::RewriteCode::UnsupportedStatement,
     "table setting dialect is not accepted"},
    {"INSERT INTO db1.t (a) SETTINGS additional_table_filters = 1 SELECT 1", rewriter::RewriteCode::UnsupportedStatement,
     "table setting additional_table_filters is not accepted"},
    // Go: the annotator reads the statement as written, not a handler's
    // hg_safe rewrite of its target.
    {"EXISTS TABLE db1.t SETTINGS dialect = 1", rewriter::RewriteCode::UnsupportedStatement,
     "table setting dialect is not accepted"},
    // Go: T5 keeps its message beside an Active target or read (T11 M2).
    {"INSERT INTO db1.t SELECT * FROM executable('x', 'CSV', 'a Int')", rewriter::RewriteCode::UnsupportedStatement,
     "table function executable is not accepted"},
    {"CREATE VIEW db1.v AS SELECT * FROM db1.t JOIN merge('other','x') ON 1", rewriter::RewriteCode::UnsupportedStatement,
     "table function merge is not accepted"},
    // Go: lexical lookalikes keep the caller's message.
    {"SELECT t.x FROM db1.o AS t WHERE dictGet(concat('a','b'),'a',1) = 1", rewriter::RewriteCode::InvalidRewriteRequest,
     "dictGet target \"\" does not resolve through the caller's databases"},
    {"SELECT hg_safe.x FROM other.u AS hg_safe WHERE dictGet(concat('a','b'),'a',1) = 1",
     rewriter::RewriteCode::InvalidRewriteRequest, "dictGet target \"\" does not resolve through the caller's databases"},
    {"WITH t AS (SELECT 1 AS x) SELECT * FROM t SETTINGS dialect = 1", rewriter::RewriteCode::UnsupportedStatement,
     "table setting dialect is not accepted"},
    {"OPTIMIZE TABLE db1.o FINAL SETTINGS t = 1", rewriter::RewriteCode::UnsupportedStatement, catch_all},
    {"SYSTEM RELOAD CONFIG ON CLUSTER hg_safe", rewriter::RewriteCode::UnsupportedStatement, catch_all},
    // Go: EXPLAIN is command text; what it explains names nothing (T4 note).
    {"EXPLAIN SELECT * FROM db1.t", rewriter::RewriteCode::UnsupportedStatement, catch_all},
    {"EXPLAIN SELECT * FROM hg_safe.db1__t", rewriter::RewriteCode::UnsupportedStatement, catch_all},
    {"DETACH DATABASE db1", rewriter::RewriteCode::UnsupportedStatement, catch_all},
    // Go: the target of a class refused with the SI catch-all is named.
    {"UNDROP TABLE db1.t", rewriter::RewriteCode::UnsupportedStatement, active_t},
    {"BACKUP TABLE hg_safe.db1__t TO Disk('d', 'x')", rewriter::RewriteCode::UnsupportedStatement, hg_safe_t},
    {"CHECK TABLE db1.t", rewriter::RewriteCode::UnsupportedStatement, active_t},
    {"SYSTEM STOP MERGES t", rewriter::RewriteCode::UnsupportedStatement, active_t},
    // Go: T3 precedes T5 for an INSERT INTO FUNCTION / CREATE … AS fn()
    // target (T7 M3).
    {"INSERT INTO FUNCTION remote('h', 'phys', 'x') SELECT 1", rewriter::RewriteCode::InvalidRewriteRequest, phys},
    {"CREATE TABLE db1.n AS merge('phys', 'x')", rewriter::RewriteCode::InvalidRewriteRequest, phys},
    // §5: an hg_* name the statement addresses names the outcome ahead of a
    // later refusal of the same statement.
    {"INSERT INTO hg_safe.db1__t SELECT * FROM phys.x", rewriter::RewriteCode::InvalidRewriteRequest, hg_safe_t},
    {"INSERT INTO hg_safe.db1__t VALUES (joinGet({p:String}, 'a', 1))", rewriter::RewriteCode::InvalidRewriteRequest,
     hg_safe_t},
    {"CREATE MATERIALIZED VIEW db1.mv TO hg_safe.db1__t AS SELECT * FROM phys.x",
     rewriter::RewriteCode::InvalidRewriteRequest, hg_safe_t},
    {"DESCRIBE TABLE hg_safe.db1__t SETTINGS dialect = 1", rewriter::RewriteCode::UnsupportedStatement, hg_safe_t},
    {"SELECT * FROM hg_safe.db1__t JOIN merge('other','x') ON 1", rewriter::RewriteCode::RewriteError, hg_safe_t},
    {"SELECT * FROM merge('other','x') UNION ALL SELECT * FROM hg_safe.db1__t", rewriter::RewriteCode::RewriteError,
     hg_safe_t},
    {"SELECT * FROM frobnicate('x') WHERE a IN (SELECT a FROM hg_safe.db1__t)", rewriter::RewriteCode::RewriteError,
     hg_safe_t},
    // Go: RENAME and multi-table DROP name their hg_* side through the
    // handlers.
    {"RENAME TABLE db1.o TO hg_safe.x", rewriter::RewriteCode::UnsupportedStatement,
     "storage-integrity physical table hg_safe.x is not directly addressable"},
    {"RENAME TABLE db1.o TO db1.z, db1.p TO hg_unsafe.y", rewriter::RewriteCode::UnsupportedStatement,
     "storage-integrity physical table hg_unsafe.y is not directly addressable"},
    {"DROP TABLE db1.o, hg_safe.db1__t", rewriter::RewriteCode::UnsupportedStatement, hg_safe_t},
    // §5: a BACKUP / RESTORE database element names the hg_* database
    // (rewriter-go answers the SI catch-all).
    {"BACKUP DATABASE hg_safe TO Disk('d', 'x')", rewriter::RewriteCode::UnsupportedStatement,
     "storage-integrity physical database hg_safe is not directly addressable"},
    {"RESTORE DATABASE db1 AS hg_promote FROM Disk('d', 'x')", rewriter::RewriteCode::UnsupportedStatement,
     "storage-integrity physical database hg_promote is not directly addressable"},
    // §5: T3 precedes T5 on a DESCRIBE target (rewriter-go answers T5).
    {"DESCRIBE TABLE remote('h', 'phys', 'x')", rewriter::RewriteCode::InvalidRewriteRequest, phys},
    // Go: an aliased JOIN to a subquery stays admitted (fixed in 7e6afbd).
    {"SELECT * FROM db1.o JOIN (SELECT 1 AS c) AS s ON 1", rewriter::RewriteCode::Success, "success"},
  };
  for (const auto &row : rows) {
    SCOPED_TRACE(row.sql);
    const auto resp = RunTableRef(row.sql, /*si=*/true);
    EXPECT_EQ(resp.code(), row.code) << resp.message();
    EXPECT_EQ(resp.message(), row.message);
  }
}

// Spec 2026-09-26 R2 for ALTER actions (rewriter-go OpaqueTextIsUngoverned):
// a subquery, a table, a cross-table partition action, MODIFY QUERY, or an
// IN-family occurrence whose operand region is not literal-only is
// `statement is not supported`. The infix region is the right operand; the
// callable region is the whole argument list.
TEST(TableRefUngoverned, AlterActions) {
  for (const bool si : {false, true}) {
    for (const char *sql : {
           "ALTER TABLE db1.o DELETE WHERE a IN `db2.x`",
           "ALTER TABLE db1.o DELETE WHERE a IN +`db2.x`",
           "ALTER TABLE db1.o UPDATE b = (SELECT max(a) FROM `db2.x`) WHERE 1",
           "ALTER TABLE db1.o UPDATE b = (SELECT count() FROM remote('127.0.0.1', 'phys', 'db2.x')) WHERE 1",
           "ALTER TABLE db1.mv MODIFY QUERY SELECT * FROM `db2.x`",
           "ALTER TABLE db1.o FETCH PARTITION tuple() FROM '/clickhouse/tables/x'",
           "ALTER TABLE db1.o ATTACH PARTITION tuple() FROM db1.p",
           "ALTER TABLE db1.o MOVE PARTITION tuple() TO TABLE db1.p",
           "ALTER TABLE db1.o MODIFY TTL d + INTERVAL 1 DAY DELETE WHERE a IN `db2.x`",
           "ALTER TABLE db1.o ADD COLUMN c UInt8 DEFAULT a IN `db2.x`",
           "ALTER TABLE db1.o DELETE WHERE in(a, (1, 2))",
           "ALTER TABLE db1.o DELETE WHERE a IN (1, b)",
           "ALTER TABLE db1.o DELETE WHERE `in`((a, 0), `db2.x`)",
           "ALTER TABLE db1.o DELETE WHERE CASE WHEN a THEN 1 END IN (`db2.x`)",
           "ALTER TABLE db1.o ADD PROJECTION p (SELECT a ORDER BY date IN (`db2.x`))",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
    for (const char *sql : {
           "ALTER TABLE db1.o DELETE WHERE a IN (1, 2)", "ALTER TABLE db1.o DELETE WHERE in(42, (1, 2))",
           "ALTER TABLE db1.o DELETE WHERE in(42, [1, 2])", "ALTER TABLE db1.o DELETE WHERE a IN -1",
           "ALTER TABLE db1.o DELETE WHERE a IN (+1, -2)", "ALTER TABLE db1.o DELETE WHERE a NOT IN ('x', 'y')",
           "ALTER TABLE db1.o MODIFY TTL d + INTERVAL 1 DAY DELETE WHERE a IN [1, 2]",
           "ALTER TABLE db1.o ADD PROJECTION p (SELECT a ORDER BY b)",
           "ALTER TABLE db1.o UPDATE b = 1 WHERE a IN (1, 2)",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_ALTER_TABLE);
    }
  }
  // While the SI surface is active an SI object named in an ALTER action
  // keeps the SI message: the SI write preflight runs before R2.
  const auto physical = RunTableRef("ALTER TABLE db1.o DELETE WHERE a IN hg_safe.`db2.x`", /*si=*/true);
  EXPECT_EQ(physical.code(), rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_EQ(physical.message(), "storage-integrity physical table hg_safe.db2.x is not directly addressable");
}

// R2 in ALTER actions, measured against rewriter-go (db2e246): its IN rule is
// lexical, so the spelling of an operand region decides, not the AST. These
// pin Go's answer in both SI states; the refused table function inside ALTER
// mutation text (a Plan B release gate) is one of them.
TEST(TableRefUngoverned, AlterActionsFollowRewriterGo) {
  for (const bool si : {false, true}) {
    for (const char *sql : {
           // Review T8 M2: an alias or a subquery is not a literal region.
           "ALTER TABLE db1.o DELETE WHERE a IN (1 AS z)",
           "ALTER TABLE db1.o DELETE WHERE a IN (SELECT 1)",
           "ALTER TABLE db1.o DELETE WHERE a IN (SELECT a FROM remote('h', currentDatabase(), 'x'))",
           "ALTER TABLE db1.o UPDATE b = 1 WHERE a IN (SELECT a FROM merge('db9', 'o'))",
           "ALTER TABLE db1.o DELETE WHERE a IN (0x1F)",
           "ALTER TABLE db1.o DELETE WHERE a IN (0b1)",
           "ALTER TABLE db1.o DELETE WHERE a IN (inf)",
           "ALTER TABLE db1.o DELETE WHERE a IN (-nan)",
           "ALTER TABLE db1.o DELETE WHERE a IN (1::UInt8)",
           "ALTER TABLE db1.o DELETE WHERE a IN (toUInt8(1))",
           "ALTER TABLE db1.o DELETE WHERE a IN (1, 2) OR a IN (SELECT 1)",
           "ALTER TABLE db1.o DELETE WHERE a IN -`db2.x`",
           "ALTER TABLE db1.o DELETE WHERE notIn = 1",
           "ALTER TABLE db1.o DELETE WHERE t.in = 1",
           "ALTER TABLE db1.o UPDATE `in` = 1 WHERE 1",
           "ALTER TABLE db1.o UPDATE b = 1 WHERE `notIn`(a, (1, 2))",
           "ALTER TABLE db1.o MODIFY COLUMN b UInt8 DEFAULT a IN (1, b)",
           "ALTER TABLE db1.o ADD INDEX i (a IN (1, b)) TYPE minmax GRANULARITY 1",
           "ALTER TABLE db1.o ADD PROJECTION p (WITH 1 AS x SELECT a)",
           "ALTER TABLE db1.o REPLACE PARTITION tuple() FROM db1.p",
           // Review T16 M1: the lexical scan skips comments, keeps a heredoc
           // one token, reads past ON CLUSTER and every comma-separated action.
           "ALTER TABLE db1.o DELETE WHERE a IN/*x*/`db2.x`",
           "ALTER TABLE db1.o DELETE WHERE a IN -- c\n`db2.x`",
           "ALTER TABLE db1.o DELETE WHERE a IN # c\n db2.x",
           "ALTER TABLE db1.o DELETE WHERE a IN $$db2.x$$",
           "ALTER TABLE db1.o DELETE WHERE a IN \u201cdb2.x\u201d",
           "ALTER TABLE db1.o DELETE WHERE a IN `db2\\x2ex`",
           "ALTER TABLE db1.o ON CLUSTER c DELETE WHERE a IN `db2.x`",
           "ALTER TABLE db1.o ADD COLUMN c UInt8, UPDATE b = 1 WHERE a IN `db2.x`",
           "ALTER TABLE db1.o DROP COLUMN c, DELETE WHERE a IN (SELECT a FROM `db2.x`)",
           "ALTER TABLE db1.o ATTACH PARTITION ID 'all' FROM `db2.x`",
           "ALTER TABLE db1.o REPLACE PARTITION ID 'all' FROM `db2.x`",
           "ALTER TABLE db1.o MOVE PARTITION ID 'all' TO TABLE `db2.x`",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
      EXPECT_EQ(resp.original_accessed_tables_size(), 0);
    }
    for (const char *sql : {
           "ALTER TABLE db1.o DELETE WHERE a IN (1 + 1)", "ALTER TABLE db1.o DELETE WHERE a IN (1 - 2)",
           "ALTER TABLE db1.o DELETE WHERE a IN tuple(1, 2)", "ALTER TABLE db1.o DELETE WHERE a IN ((1, 2), (3, 4))",
           "ALTER TABLE db1.o DELETE WHERE a IN (TRUE, NULL, 'x')", "ALTER TABLE db1.o DELETE WHERE a IN (1e-3, .5)",
           "ALTER TABLE db1.o DELETE WHERE a IN (- - 1)", "ALTER TABLE db1.o DELETE WHERE a IN [1, -2] AND b NOT IN ('x')",
           "ALTER TABLE db1.o CLEAR COLUMN c IN PARTITION tuple()", "ALTER TABLE db1.o FREEZE WITH NAME 'x'",
           "ALTER TABLE db1.o MODIFY COMMENT 'SELECT 1 IN t'", "ALTER TABLE db1.o MODIFY SETTING max_parts_in_total = 1",
           "ALTER TABLE db1.o ATTACH PARTITION tuple()", "ALTER TABLE db1.o MOVE PARTITION tuple() TO DISK 'd'",
           "ALTER TABLE db1.o DELETE WHERE a IN/*x*/(1, 2)", "ALTER TABLE db1.o DELETE WHERE a IN -- c\n(1, 2)",
           "ALTER TABLE db1.o DELETE WHERE a IN 'db2.x'", "ALTER TABLE db1.o DELETE WHERE a IN \u2018db2.x\u2019",
           "ALTER TABLE db1.o ON CLUSTER c DELETE WHERE a IN (1, 2)",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_ALTER_TABLE);
    }
  }
}

// Review T16 C1: ClickHouse decodes a quoted function name (`\xHH` and the
// other backslash escapes, and English Unicode double quotes) before it looks
// the function up, so `\x69n`(a, t) is IN. Every lexical scanner reads the
// decoded name: an escaped IN-family or lookup name answers exactly what its
// plain spelling answers (rewriter-go refuses every one of these), in both SI
// states.
TEST(TableRefUngoverned, EscapedFunctionNamesAreDecoded) {
  const auto spellings = [](const std::string &name) {
    char first[8], middle[8];
    std::snprintf(first, sizeof(first), "\\x%02x", static_cast<unsigned char>(name[0]));
    const size_t mid = name.size() / 2;
    std::snprintf(middle, sizeof(middle), "\\x%02X", static_cast<unsigned char>(name[mid]));
    return std::vector<std::string>{
      "`" + std::string(first) + name.substr(1) + "`",
      "\"" + std::string(first) + name.substr(1) + "\"",
      "`" + name.substr(0, mid) + middle + name.substr(mid + 1) + "`",
      "“" + name + "”",
    };
  };
  const auto wrap = [](const std::string &pattern, const std::string &name) {
    std::string out = pattern;
    for (size_t at; (at = out.find('%')) != std::string::npos;) out.replace(at, 1, name);
    return out;
  };
  struct Family {
    std::vector<std::string> names;
    std::vector<std::string> positions;
  };
  const std::vector<Family> families = {
    {{"in", "notIn", "globalIn", "globalNotIn", "nullIn", "notNullIn", "globalNullIn", "globalNotNullIn",
      "inIgnoreSet", "notInIgnoreSet", "globalInIgnoreSet", "nullInIgnoreSet"},
     {"ALTER TABLE db1.o DELETE WHERE %(a, `db2.x`)", "ALTER TABLE db1.o UPDATE b = 1 WHERE %(a, `db2.x`)",
      "ALTER TABLE db1.o UPDATE b = %(a, (1, b)) WHERE 1", "ALTER TABLE db1.o DELETE WHERE %(a, phys.`db2.x`)"}},
    {{"joinGet", "joinGetOrNull", "dictGet", "dictGetString", "dictGetOrDefault", "dictHas", "dictGetHierarchy",
      "dictIsIn", "dictGetChildren", "dictGetDescendants"},
     {"ALTER TABLE db1.o DELETE WHERE %('db2.j', 'v', 1) = 1", "ALTER TABLE db1.o UPDATE b = %('db2.j', 'v', 1) WHERE 1",
      "INSERT INTO db1.o VALUES (%('db2.j', 'v', 1))",
      "INSERT INTO db1.o (a) SETTINGS max_threads = 1 VALUES (%('db2.j', 'v', 1))"}},
    {{"hasColumnInTable"},
     {"ALTER TABLE db1.o DELETE WHERE %('db2', 'x', 'a') = 1", "INSERT INTO db1.o VALUES (%('db2', 'x', 'a'))",
      "INSERT INTO db1.o (a) SETTINGS max_threads = 1 VALUES (%('db2', 'x', 'a'))"}},
  };
  for (const bool si : {false, true}) for (const auto &family : families) for (const auto &name : family.names)
  for (const auto &position : family.positions) {
    const std::string plain_sql = wrap(position, name);
    const auto plain = RunTableRef(plain_sql, si);
    for (const auto &spelled : spellings(name)) {
      const std::string sql = wrap(position, spelled);
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_NE(resp.code(), rewriter::RewriteCode::Success) << resp.sql_after_rewrite();
      EXPECT_EQ(resp.code(), plain.code());
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
      if (name.rfind("dict", 0) != 0 && name.rfind("join", 0) != 0 && name.rfind("hasColumn", 0) != 0)
        EXPECT_EQ(resp.message(), plain.message());
    }
  }
}

// Spec 2026-09-26 R2 for structured UPDATE / DELETE, column / constraint /
// storage expressions and REFRESH views: a table, a table function or an IN
// operand naming a table there is `statement is not supported` in both SI
// states; a table-free subquery, a CTE name and an IN operand that is a tuple
// of expressions are not reads.
TEST(TableRefUngoverned, MutationsCreateExpressionsAndRefresh) {
  for (const bool si : {false, true}) {
    for (const char *sql : {
           "UPDATE db1.o SET b = 1 WHERE a IN `db2.x`",
           "UPDATE db1.o SET b = (SELECT max(a) FROM db1.p) WHERE 1",
           "UPDATE db1.o SET b = (SELECT count() FROM numbers(3)) WHERE 1",
           "UPDATE db1.o SET b = (WITH u AS (SELECT count() FROM u) SELECT count() FROM u) WHERE 1",
           // Deviation from the plan (fix round 1): ClickHouse reads the table t
           // here, not the recursive CTE (26.2 / 25.8, measured).
           "UPDATE db1.o SET b = (WITH RECURSIVE t AS (SELECT 1) SELECT count() FROM t) WHERE 1",
           "DELETE FROM db1.o WHERE a IN (SELECT a FROM `db2.x`)",
           "DELETE FROM db1.o WHERE in(a, `db2.x`)",
           "CREATE TABLE db1.n (a UInt64 DEFAULT (SELECT max(a) FROM db1.p)) ENGINE = Memory",
           "CREATE TABLE db1.n (a UInt64, CONSTRAINT c CHECK a IN (SELECT a FROM db1.p)) ENGINE = Memory",
           "CREATE TABLE db1.n (a UInt64) ENGINE = MergeTree ORDER BY a IN `db2.x`",
           "CREATE TABLE db1.n (d Date, a UInt64) ENGINE = MergeTree ORDER BY a TTL d + INTERVAL 1 DAY DELETE WHERE a IN `db2.x`",
           "CREATE MATERIALIZED VIEW db1.mv REFRESH EVERY 1 HOUR TO db1.p AS SELECT * FROM db1.o",
           "CREATE MATERIALIZED VIEW db1.`refresh` REFRESH AFTER 1 HOUR TO db1.p AS SELECT * FROM db1.o",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      // Under V2 the unbound recursive `t` is the Active db1.t: the SI write
      // refusal names it first (rewriter-go v0.15.0 answers the same).
      EXPECT_EQ(resp.message(), si && std::string(sql).find("RECURSIVE t") != std::string::npos
                                  ? "storage-integrity table db1.t accepts writes only through the signed statement lane"
                                  : rewriter_handlers::kUnsupportedStatementMessage);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
    for (const char *sql : {
           "UPDATE db1.o SET b = 1 WHERE a IN (1, 2)",
           "DELETE FROM db1.o WHERE a NOT IN (-1, 2)",
           "DELETE FROM db1.o WHERE a IN (1, b)",
           "UPDATE db1.o SET b = a IN (1, c) WHERE 1",
           "UPDATE db1.o SET b = (SELECT 1) WHERE 1",
           "CREATE TABLE db1.n (a UInt64 DEFAULT 1, CONSTRAINT c CHECK a IN (1, 2)) ENGINE = Memory",
           "CREATE TABLE db1.n (a UInt64) ENGINE = MergeTree ORDER BY a",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    }
  }
}

// Spec 2026-09-26 R2, measured against rewriter-go (db2e246) in both SI
// states: the structured positions follow its ExpressionPositionHasReads. A
// `values` list of literals, a column-position parameter and an IN operand that is not a name (`numbers(3)`,
// `-`db2.x``, `(1 AS z)`) are not reads; neither is a table-free subquery in an
// ALTER that rewriter-go structures throughout (ADD COLUMN, ADD CONSTRAINT,
// DROP COLUMN), which it walks like a CREATE column.
TEST(TableRefUngoverned, MutationsCreateExpressionsFollowRewriterGo) {
  for (const bool si : {false, true}) {
    for (const char *sql : {
           "UPDATE db1.o SET b = (SELECT count() FROM view(SELECT 1)) WHERE 1",
           "UPDATE db1.o SET b = exists(SELECT 1 FROM db1.p) WHERE 1",
           "UPDATE db1.o SET b = (WITH u AS (SELECT count() FROM db1.p) SELECT count() FROM u) WHERE 1",
           "UPDATE db1.o SET b = (SELECT count() FROM numbers(3) WHERE number IN (1, a)) WHERE 1",
           "UPDATE db1.o SET b = a IN `db2.x` WHERE 1",
           "DELETE FROM db1.o WHERE (SELECT count() FROM db1.p AS p1 JOIN db1.o AS o1 ON 1)",
           "DELETE FROM db1.o WHERE a IN (SELECT a FROM numbers(3))",
           "DELETE FROM db1.o WHERE a IN (SELECT a FROM url('http://h/x'))",
           "DELETE FROM db1.o WHERE a IN (SELECT a FROM file('x'))",
           "DELETE FROM o WHERE a GLOBAL IN db2.x",
           "DELETE FROM db1.o WHERE a IN p",
           "DELETE FROM db1.o WHERE `\\x69n`(a, `db2.x`)",
           "CREATE TABLE db1.n (a UInt64, INDEX i a IN `db2.x` TYPE minmax) ENGINE = MergeTree ORDER BY a",
           "CREATE TABLE db1.n (a UInt64, b UInt64 ALIAS (SELECT count() FROM file('x'))) ENGINE = Memory",
           "CREATE TABLE db1.n (a UInt64, CONSTRAINT c ASSUME a IN (SELECT a FROM numbers(3))) ENGINE = Memory",
           "CREATE TABLE db1.n (a UInt64) ENGINE = MergeTree PARTITION BY (SELECT count() FROM numbers(3)) ORDER BY a",
           "CREATE TABLE db1.n (a UInt64) ENGINE = MergeTree ORDER BY a SAMPLE BY a GLOBAL IN db2.x",
           "CREATE TABLE db1.n (a UInt64) ENGINE = MergeTree PRIMARY KEY a IN `db2.x` ORDER BY a",
           "CREATE TABLE db1.n ENGINE = MergeTree ORDER BY notIn(a, `db2.x`) AS SELECT * FROM db1.o",
           "CREATE MATERIALIZED VIEW db1.mv ENGINE = MergeTree ORDER BY a IN `db2.x` AS SELECT a FROM db1.o",
           "ALTER TABLE db1.o ADD COLUMN c UInt8 DEFAULT (SELECT max(a) FROM db1.p)",
           "ALTER TABLE db1.o ADD CONSTRAINT c CHECK a IN `db2.x`",
           "ALTER TABLE db1.o ADD COLUMN c UInt8 DEFAULT `\\x69n`(a, `db2.x`), DROP COLUMN b",
           "ALTER TABLE db1.o ADD COLUMN c UInt8, ADD CONSTRAINT k CHECK exists(SELECT 1 FROM numbers(3))",
           "ALTER TABLE db1.o ON CLUSTER k ADD COLUMN c UInt8 DEFAULT (WITH u AS (SELECT count() FROM u) SELECT count() FROM u)",
           "CREATE MATERIALIZED VIEW db1.mv REFRESH EVERY 1 HOUR APPEND TO db1.p AS SELECT * FROM db1.o",
           "CREATE MATERIALIZED VIEW db1.mv REFRESH EVERY 1 HOUR ENGINE = MergeTree ORDER BY a AS SELECT a FROM db1.o",
           "CREATE MATERIALIZED VIEW db1.mv REFRESH EVERY 1 HOUR DEPENDS ON db1.p TO db1.p AS SELECT * FROM db1.o",
           "CREATE MATERIALIZED VIEW db1.mv REFRESH EVERY 1 HOUR TO db1.p AS SELECT * FROM numbers(3)",
           "CREATE MATERIALIZED VIEW mv REFRESH EVERY 1 HOUR TO p AS SELECT 1 AS a",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
      EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
    }
    for (const char *sql : {
           "DELETE FROM db1.o WHERE a IN (SELECT a FROM values('a UInt8', 1))",
           "UPDATE db1.o SET b = (SELECT count() FROM VALUES('a UInt8', 1)) WHERE 1",
           "UPDATE db1.o SET b = {c:Identifier} WHERE 1",
           "DELETE FROM db1.o WHERE a IN numbers(3)",
           "DELETE FROM db1.o WHERE a IN -`db2.x`",
           "DELETE FROM db1.o WHERE a IN (1 AS z)",
           "DELETE FROM db1.o WHERE a IN ((1, 2), (3, b))",
           "UPDATE db1.o SET b = exists(SELECT 1) WHERE 1",
           "UPDATE db1.o SET b = (SELECT count() FROM (SELECT 1)) WHERE 1",
           "UPDATE db1.o SET b = (SELECT 1 UNION ALL SELECT 2) WHERE 1",
           "CREATE TABLE db1.n (a UInt64 MATERIALIZED (WITH 1 AS y SELECT y)) ENGINE = Memory",
           "CREATE TABLE db1.n (a UInt64) ENGINE = MergeTree PARTITION BY (SELECT 1) ORDER BY a",
           "CREATE TABLE db1.n (a UInt64, INDEX i a IN (1, b) TYPE minmax) ENGINE = MergeTree ORDER BY a",
           "ALTER TABLE db1.o ADD COLUMN c UInt8 DEFAULT (SELECT 1)",
           "ALTER TABLE db1.o ADD CONSTRAINT c CHECK a IN (1, b)",
           "ALTER TABLE db1.o ADD COLUMN c UInt8 DEFAULT a IN (0x1F), DROP COLUMN b",
           "ALTER TABLE db1.o ADD COLUMN c UInt8, ADD CONSTRAINT k CHECK a IN (toUInt8(1))",
           "ALTER TABLE db1.o ADD COLUMN c UInt8 DEFAULT a IN -`db2.x`",
           "CREATE MATERIALIZED VIEW db1.refresh TO db1.p AS SELECT * FROM db1.o",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    }
  }
}

// A materialized view's column expressions are R2 positions like a table's.
// rewriter-go does not walk them (it admits every row below, protected and
// storage-integrity names included); this engine refuses the reads.
TEST(TableRefUngoverned, MaterializedViewColumnExpressions) {
  for (const bool si : {false, true}) {
    for (const char *expr : {"(SELECT max(a) FROM db1.p)", "a IN `db2.x`", "(SELECT count() FROM numbers(3))"}) {
      const std::string sql = std::string("CREATE MATERIALIZED VIEW db1.mv (a UInt64 DEFAULT ") + expr
        + ") ENGINE = MergeTree ORDER BY a AS SELECT a FROM db1.o";
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
    }
    const auto ok = RunTableRef(
      "CREATE MATERIALIZED VIEW db1.mv (a UInt64 DEFAULT (SELECT 1)) ENGINE = MergeTree ORDER BY a AS SELECT a FROM db1.o", si);
    EXPECT_EQ(ok.code(), rewriter::RewriteCode::Success) << ok.message();
  }
}

// The post-handler guard, on its own: it keeps every IN-operand refusal (an
// enclosing WITH alias is no exemption), re-checks R2's structured positions
// with R2's predicate, and is the only guard of an IN operand in a CREATE's
// `AS <table function>` source.
TEST(TableRefUngoverned, PostHandlerGuardAgreesWithR2) {
  const auto guard = [](const std::string &text) {
    DB::ParserQuery parser(text.data() + text.size(), false);
    const char *pos = text.data();
    const DB::ASTPtr ast = DB::parseQuery(parser, pos, text.data() + text.size(), "", 0, 0, 0);
    return rewriter_handlers::inOperandInUnrewrittenPosition(ast);
  };
  for (const char *sql : {
         "DELETE FROM db1.o WHERE a IN `db2.x`",
         "UPDATE db1.o SET b = (SELECT max(a) FROM db1.p) WHERE 1",
         "CREATE TABLE db1.n (a UInt64 DEFAULT a IN `db2.x`) ENGINE = Memory",
         "CREATE MATERIALIZED VIEW db1.mv ENGINE = MergeTree ORDER BY a IN `db2.x` AS SELECT a FROM db1.o",
         "CREATE TABLE db1.n AS numbers(1 IN `db2.x`)",
         "ALTER TABLE db1.o ADD COLUMN c UInt8 DEFAULT (SELECT max(a) FROM db1.p)",
         "ALTER TABLE db1.o DELETE WHERE a IN `db2.x`",
         "DELETE FROM db1.o WHERE a IN (WITH c AS (SELECT 1 AS x) SELECT x FROM c WHERE x IN c)",
         "CREATE TABLE db1.n (a UInt64, CONSTRAINT c CHECK a IN (WITH c AS (SELECT 1 AS x) SELECT x FROM c WHERE x IN c)) ENGINE = Memory",
         "UPDATE db1.o SET b = (WITH RECURSIVE t AS (SELECT 1) SELECT count() FROM t) WHERE 1",
       }) {
    SCOPED_TRACE(sql);
    EXPECT_TRUE(guard(sql));
  }
  for (const char *sql : {
         "UPDATE db1.o SET b = a IN (1, c) WHERE 1",
         "ALTER TABLE db1.o ADD COLUMN c UInt8 DEFAULT (SELECT 1)",
         "CREATE TABLE db1.n (a UInt64) ENGINE = MergeTree ORDER BY a AS SELECT * FROM db1.o WHERE a IN `db2.x`",
       }) {
    SCOPED_TRACE(sql);
    EXPECT_FALSE(guard(sql));
  }
}

// No WITH alias binds a FROM / IN name in R2's structured positions. Measured
// on ClickHouse 26.2 and 25.8: `DELETE … WHERE a IN (WITH c AS (SELECT 1 AS x)
// SELECT x FROM c)` deletes by the rows of table c; a lightweight UPDATE's
// `(WITH cc AS … SELECT count() FROM cc)` and `(WITH RECURSIVE t AS …
// FROM t)` count the tables; a CHECK / ASSUME constraint is stored with the
// name qualified by the current database and enforced against that table;
// TTL and (25.8) INDEX expressions resolve the name as a table. With a quoted
// name such as `db2.my-t` that is a membership oracle on another tenant's
// physical table, so every such shape is refused in both SI states
// (rewriter-go admits them; its fix is separate).
TEST(TableRefUngoverned, StructuredPositionsBindNoCTE) {
  for (const bool si : {false, true}) {
    for (const char *sql : {
           "DELETE FROM db1.o WHERE a IN (WITH c AS (SELECT 1 AS x) SELECT x FROM c)",
           "DELETE FROM db1.o WHERE a IN (WITH c AS (SELECT 1 AS x) SELECT x FROM c WHERE x IN c)",
           "DELETE FROM db1.o WHERE a IN (WITH `db2.my-t` AS (SELECT 1 AS x) SELECT x FROM (SELECT arrayJoin(range(100)) AS x) WHERE x IN `db2.my-t`)",
           "DELETE FROM db1.o WHERE a IN (WITH `db2.my-t` AS (SELECT 1 AS x) SELECT x FROM `db2.my-t`)",
           "DELETE FROM db1.o WHERE a IN `db2.my-t`",
           "UPDATE db1.o SET b = (WITH cc AS (SELECT 1 AS x) SELECT count() FROM cc WHERE 1 IN cc) WHERE 1",
           "UPDATE db1.o SET b = 1 WHERE a IN (WITH c AS (SELECT 1 AS x) SELECT x FROM c)",
           "UPDATE db1.o SET b = (WITH RECURSIVE r AS (SELECT 1) SELECT count() FROM r) WHERE 1",
           "CREATE TABLE `db1.n` (a UInt64, CONSTRAINT k CHECK a IN (WITH `db2.my-t` AS (SELECT 1 AS x) SELECT x FROM (SELECT arrayJoin(range(100)) AS x) WHERE x IN `db2.my-t`)) ENGINE = Memory",
           "CREATE TABLE db1.n (a UInt64, CONSTRAINT k CHECK a IN (WITH `db2.my-t` AS (SELECT 1 AS x) SELECT x FROM (SELECT arrayJoin(range(100)) AS x) WHERE x IN `db2.my-t`)) ENGINE = Memory",
           "CREATE TABLE db1.n (a UInt64, CONSTRAINT k CHECK a IN (WITH c AS (SELECT 1 AS x) SELECT x FROM c)) ENGINE = Memory",
           "CREATE TABLE db1.n (a UInt64, CONSTRAINT k ASSUME a IN (WITH c AS (SELECT 1 AS x) SELECT x FROM c)) ENGINE = Memory",
           "CREATE TABLE db1.n (d Date, a UInt64) ENGINE = MergeTree ORDER BY a TTL d + INTERVAL 1 DAY DELETE WHERE (WITH c AS (SELECT 1 AS x) SELECT count() FROM c WHERE 1 IN c)",
           "CREATE TABLE db1.n (a UInt64, INDEX i a IN (WITH c AS (SELECT 1 AS x) SELECT x FROM c) TYPE minmax) ENGINE = MergeTree ORDER BY a",
           "CREATE MATERIALIZED VIEW db1.mv ENGINE = MergeTree ORDER BY a TTL toDate(a) + INTERVAL 1 DAY DELETE WHERE a IN (WITH c AS (SELECT 1 AS x) SELECT x FROM c) AS SELECT a FROM db1.o",
           "ALTER TABLE db1.o ADD CONSTRAINT k CHECK a IN (WITH c AS (SELECT 1 AS x) SELECT x FROM c)",
           "ALTER TABLE db1.o ADD CONSTRAINT k ASSUME a IN (WITH `db2.my-t` AS (SELECT 1 AS x) SELECT x FROM (SELECT 1 AS x) WHERE x IN `db2.my-t`)",
           "ALTER TABLE db1.o ADD COLUMN c UInt8 DEFAULT (WITH RECURSIVE r AS (SELECT 1) SELECT count() FROM r)",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
  }
}

// Spec 2026-09-26 R2 for INSERT: FROM INFILE, a read in a VALUES row, and a
// read in the rows or query of an INSERT whose column list is followed by a
// SETTINGS clause (rewriter-go keeps that text opaque and applies the
// literal-only IN rule) are `statement is not supported` in both SI states.
TEST(TableRefUngoverned, InsertInfileValuesAndHeaderSettings) {
  for (const bool si : {false, true}) {
    for (const char *sql : {
           "INSERT INTO db1.o FROM INFILE 'x.csv' FORMAT CSV",
           "INSERT INTO db1.o VALUES ((SELECT max(a) FROM db1.p))",
           "INSERT INTO db1.o (a) SETTINGS max_threads = 1 VALUES (in(42, `db2.x`))",
           "INSERT INTO db1.o (a) SETTINGS max_threads = 1 FORMAT Values (not in(1, `db2.x`))",
           "INSERT INTO db1.o (a) SETTINGS max_threads = 1 VALUES (3 IN +`db2.y`)",
           "INSERT INTO db1.o (a) SETTINGS max_threads = 1 SELECT a FROM `db2.x`",
           "INSERT INTO db1.o (a) SETTINGS max_threads = 1 SELECT in(42, `db2.x`)",
           "INSERT INTO db1.o (a) SETTINGS max_threads = 1 SELECT CASE WHEN 1 THEN 42 END IN (`db2.x`)",
           "INSERT INTO db1.o VALUES (in(42, `db2.x`))",
           "INSERT INTO db1.o (a) SETTINGS max_threads = 1 VALUES (1 IN (1, b))",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
    }
    for (const char *sql : {
           "INSERT INTO db1.o VALUES (1, 'x', NULL, -2, [1, 2], (3, 4))",
           "INSERT INTO db1.o VALUES (1 IN (1, 2))",
           "INSERT INTO db1.o VALUES (1 IN (1, b))",
           "INSERT INTO db1.o VALUES ({p:UInt8})",
           "INSERT INTO db1.o (a) SETTINGS max_threads = 1 SELECT 1",
           "INSERT INTO db1.o (a) SELECT a FROM db1.p SETTINGS max_threads = 1",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    }
  }
}

// The INSERT rows rewriter-go reads, measured against rewriter-go db2e246 in
// both SI states: a structured VALUES row takes T2 / T3 / T5 / T6 and the
// structural R2 rule (a table, a table function, an IN operand naming a
// table, a parameter there; a table-free subquery and a tuple or aliased IN
// operand are not reads) and, with the surface active, the SI namespace
// policy; the rows or query that follow a column list and a header SETTINGS
// take T2 / T3 over their text and the lexical rule (one SELECT, no FROM /
// JOIN / WITH, literal-only IN regions, no lookup call). A quoted IN-family
// name is matched as ClickHouse decodes it.
TEST(TableRefUngoverned, InsertRowsFollowRewriterGo) {
  const std::string phys = "protected database phys is not addressable";
  const std::string param = "query parameters are not supported in a database or table position";
  const std::string unsupported(rewriter_handlers::kUnsupportedStatementMessage);
  struct Row {
    const char *sql;
    rewriter::RewriteCode code;
    std::string inactive;  // message with the SI surface inactive
    std::string active;    // message under V2 ("" = the same answer)
    rewriter::RewriteCode active_code = rewriter::RewriteCode::UnsupportedStatement;
  };
  using C = rewriter::RewriteCode;
  const std::vector<Row> rows = {
    {"INSERT INTO db1.o (a) VALUES ((SELECT count() FROM phys.x))", C::InvalidRewriteRequest, phys, ""},
    {"INSERT INTO db1.o VALUES (1 IN phys.x)", C::InvalidRewriteRequest, phys, ""},
    {"INSERT INTO db1.o VALUES ((SELECT * FROM merge('phys', 'x')))", C::InvalidRewriteRequest, phys, ""},
    {"INSERT INTO db1.o (a) SETTINGS max_threads = 1 VALUES (phys.x.a)", C::InvalidRewriteRequest, phys, ""},
    {"INSERT INTO db1.o (a) SETTINGS max_threads = 1 SELECT phys.a", C::InvalidRewriteRequest, phys, ""},
    {"INSERT INTO db1.o VALUES (1 IN {p:Identifier})", C::InvalidRewriteRequest, param, ""},
    {"INSERT INTO db1.o (a) SETTINGS max_threads = 1 VALUES ({p:Identifier})", C::InvalidRewriteRequest, param, ""},
    {"INSERT INTO db1.o (a) SETTINGS max_threads = 1 SELECT {p:Identifier}", C::InvalidRewriteRequest, param, ""},
    {"INSERT INTO db1.o VALUES ((SELECT count() FROM db1.t))", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.o VALUES (1 IN `db2.x`)", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.o VALUES (in(1, `db2.x`))", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.o VALUES (`in`(1, `db2.x`))", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.o VALUES (\xE2\x80\x9Cin\xE2\x80\x9D(1, `db2.x`))", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.o VALUES (`\\x69n`(1, `db2.x`))", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.o VALUES (notIn(1, db2.x))", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.o VALUES (globalNotInIgnoreSet(1, db2.x))", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.o VALUES ((SELECT * FROM numbers(1)))", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.o VALUES ((SELECT a FROM view(SELECT 1)))", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.o VALUES (exists(SELECT 1 FROM db1.p))", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.o VALUES ((SELECT count() FROM t))", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.o VALUES (1), ((SELECT a FROM db1.p))", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.o FORMAT values ((SELECT a FROM db1.p))", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.t VALUES ((SELECT count() FROM `db2.x`))", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.o (a) SETTINGS max_threads = 1 VALUES ((SELECT 1), (SELECT 2))", C::UnsupportedStatement, unsupported, ""},
    // A header SETTINGS makes the query opaque even when it has its own.
    {"INSERT INTO db1.o (a) SETTINGS max_threads = 1 SELECT a FROM db1.p SETTINGS max_threads = 2",
     C::UnsupportedStatement, unsupported, ""},
    // Text that does not parse as rows is read lexically and fails closed
    // (rewriter-go cannot parse it either).
    {"INSERT INTO db1.o VALUES (1) SELECT 1 FROM db2.x", C::UnsupportedStatement, unsupported, ""},
    {"INSERT INTO db1.o VALUES ((SELECT a FROM db1.p)", C::UnsupportedStatement, unsupported, ""},
    // The SI surface names an SI object first while it is active.
    {"INSERT INTO db1.o VALUES ((SELECT count() FROM hg_safe.db1__t))", C::InvalidRewriteRequest,
     "protected database hg_safe is not addressable",
     "storage-integrity physical table hg_safe.db1__t is not directly addressable"},
    {"INSERT INTO db1.o VALUES (1 IN db1.t)", C::UnsupportedStatement, unsupported,
     "storage-integrity table db1.t is not directly addressable through IN table target"},
    {"INSERT INTO db1.o VALUES ((SELECT * FROM remote('h', 'db1', 'o')))", C::UnsupportedStatement,
     "table function remote is not accepted",
     "storage-integrity logical database db1 is not directly addressable through remote table function"},
    {"INSERT INTO db1.o (a) SETTINGS max_threads = 1 VALUES (hg_safe.x.a)", C::InvalidRewriteRequest,
     "protected database hg_safe is not addressable", "storage-integrity physical table hg_safe.x is not directly addressable"},
  };
  for (const bool si : {false, true}) {
    for (const auto &row : rows) {
      SCOPED_TRACE(std::string(row.sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(row.sql, si);
      const bool differs = si && !row.active.empty();
      EXPECT_EQ(resp.code(), differs ? row.active_code : row.code) << resp.message();
      EXPECT_EQ(resp.message(), differs ? row.active : row.inactive);
      EXPECT_EQ(resp.sql_after_rewrite(), row.sql);
    }
    // Admitted in both engines: no table is read.
    for (const char *sql : {
           "INSERT INTO db1.o VALUES ((SELECT 1))",
           "INSERT INTO db1.o VALUES (1 IN (SELECT 1))",
           "INSERT INTO db1.o VALUES (1 IN (1 AS z))",
           "INSERT INTO db1.o VALUES (1 IN numbers(3))",
           "INSERT INTO db1.o VALUES (1) (2); ",
           "INSERT INTO db1.o (a) SETTINGS max_threads = 1 VALUES ((SELECT 1))",
           "INSERT INTO db1.o (a) SETTINGS max_threads = 1 FORMAT CSV 1",
           "INSERT INTO db1.o FORMAT CSV",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    }
    // Admitted here, refused by rewriter-go only because its parser cannot
    // read them (no table is read): a VALUES clause whose rows follow on the
    // wire (the shape native-protocol clients send), and `inf`, which
    // ClickHouse reads as a number.
    for (const char *sql : {
           "INSERT INTO db1.o VALUES",
           "INSERT INTO db1.o (a) VALUES",
           "INSERT INTO db1.o VALUES (1 IN (inf))",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    }
  }
}

// R5 over the statement text (Plan B v2 T14 review M1 / M2): a SETTINGS list
// is read from the tokens even where the AST holds no settings clause (the
// rows of a VALUES data tail), and a name counts only when `=` follows it, so
// a column named `settings` with an alias is not a setting.
TEST(TableRefSettings, TextNamesNeedAnAssignment) {
  for (const bool si : {false, true}) {
    SCOPED_TRACE(si ? "[V2]" : "[inactive]");
    const auto tail = RunTableRef("INSERT INTO db1.o VALUES (1) SETTINGS dialect = 1", si);
    EXPECT_EQ(tail.code(), rewriter::RewriteCode::UnsupportedStatement) << tail.message();
    EXPECT_EQ(tail.message(), "table setting dialect is not accepted");
    const auto alias = RunTableRef("SELECT settings dialect FROM db1.o SETTINGS max_threads = 1", si);
    EXPECT_EQ(alias.code(), rewriter::RewriteCode::Success) << alias.message();
    const auto both = RunTableRef("SELECT settings dialect FROM db1.o SETTINGS dialect = 'x'", si);
    EXPECT_EQ(both.code(), rewriter::RewriteCode::UnsupportedStatement) << both.message();
    EXPECT_EQ(both.message(), "table setting dialect is not accepted");
  }
}

// C1 (fix round 1): a comment in a VALUES payload must not hide a read from
// the literal gate. ClickHouse's Values reader treats `'\x'` as an unfinished
// escape and keeps reading past the `'`, so `'\x'--'` is string content plus a
// live second value, while the C++ SQL lexer closes `'\x'` and reads `--` as a
// comment to end of input. The literal gate now treats any comment as
// non-literal, the rows are parsed with ClickHouse's parser (a payload that no
// longer balances fails closed), and the opaque header scan fails closed on a
// comment. rewriter-go refuses every row below and admits the comment rows
// that carry no read (both measured against db2e246).
TEST(TableRefUngoverned, InsertValuesCommentsCannotHideReads) {
  const std::string unsupported(rewriter_handlers::kUnsupportedStatementMessage);
  for (const bool si : {false, true}) {
    for (const char *sql : {
           "INSERT INTO db1.o VALUES ('\\x'--', (SELECT max(a) FROM db2.x))",
           "INSERT INTO db1.o VALUES ('\\x'#', (SELECT max(a) FROM db2.x))",
           "INSERT INTO db1.o (a) SETTINGS max_threads = 1 VALUES ('\\x'--', (SELECT max(a) FROM db2.x))",
           "INSERT INTO db1.o VALUES ('\\'' , (SELECT max(a) FROM db2.x))",
           "INSERT INTO db1.o VALUES ('\\\\', (SELECT max(a) FROM db2.x))",
           "INSERT INTO db1.o VALUES ((SELECT max(a) FROM db2.x)) -- x",
           "INSERT INTO db1.o VALUES ((SELECT max(a) FROM db2.x)) /* c */",
           "INSERT INTO db1.o VALUES (1), ((SELECT max(a) FROM db2.x)) -- c",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), unsupported);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
    // Fix round 3: any comment in the VALUES payload is refused, even one that
    // carries no read. The SQL view of a commented payload does not prove what
    // ClickHouse's Values reader executes. rewriter-go admits these (fail-closed
    // PARITY, T22).
    for (const char *sql : {
           "INSERT INTO db1.o VALUES (1) -- ok",
           "INSERT INTO db1.o VALUES (1) /* c */, (2)",
           "INSERT INTO db1.o VALUES (1 /* x */)",
           "INSERT INTO db1.o VALUES (1 /* c */, 2)",
           "INSERT INTO db1.o VALUES ('a''b' /* c */)",
           "INSERT INTO db1.o VALUES ('\\'' /* c */)",
           "INSERT INTO db1.o VALUES ('\\\\' /* c */)",
           "INSERT INTO db1.o VALUES (1) # c",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), unsupported);
    }
    // A comment outside the VALUES payload is SQL the parser reads: before
    // VALUES, and in an INSERT … SELECT. A doubled quote is one string.
    for (const char *sql : {
           "INSERT INTO db1.o /* c */ VALUES (1)",
           "INSERT INTO db1.o SELECT 1 -- c",
           "INSERT INTO db1.o VALUES ('a''b')",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    }
  }
}

// Fix round 2: ClickHouse's Values reader consumes two bytes after `\x`, its
// SQL lexer one, so a `\x` escape without two hex digits is read differently by
// the two. Every such form is refused, in the plain and the header-SETTINGS
// VALUES text; well-formed escapes keep today's answer.
TEST(TableRefUngoverned, InsertValuesMalformedHexEscapeIsRefused) {
  const std::string unsupported(rewriter_handlers::kUnsupportedStatementMessage);
  for (const bool si : {false, true}) {
    for (const char *prefix : {"INSERT INTO db1.o VALUES ", "INSERT INTO db1.o (a) SETTINGS max_threads = 1 VALUES "}) {
      for (const char *row : {"('\\x')", "('\\x4')", "('\\xZZ')", "('a\\x', 1)", "('\\x4', 'b')"}) {
        const std::string sql = std::string(prefix) + row;
        SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
        const auto resp = RunTableRef(sql, si);
        EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
        EXPECT_EQ(resp.message(), unsupported);
      }
      for (const char *row : {"('\\x41')", "('\\n')", "('\\N')", "('\\0')", "('\\\\')", "('\\'')", "('a''b')",
                              "('\\q')", "('\\\\x')"}) {
        const std::string sql = std::string(prefix) + row;
        SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
        const auto resp = RunTableRef(sql, si);
        EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      }
    }
  }
}

// Spec 2026-09-26 R7 for SHOW trailing clauses: a subquery, a function call,
// a non-literal IN region, a parameter or a quoted dotted name in WHERE /
// LIMIT is `statement is not supported`; with the SI surface active an SI
// physical name there carries the SI message first.
TEST(TableRefShow, TrailingClausesAreGoverned) {
  // With the SI surface active the session's logical database db1 is an SI
  // logical database, so a SHOW scoped to it is refused by the SI handler
  // first; the cluster-wide families still reach R7.
  for (const char *sql : {
         "SHOW CLUSTERS LIKE 'x' LIMIT (SELECT count() FROM `db2.x`)",
         "SHOW MERGES LIMIT (SELECT count() FROM `db2.x`)",
       }) {
    SCOPED_TRACE(std::string(sql) + " [V2]");
    const auto resp = RunTableRef(sql, /*si=*/true);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
  }
  EXPECT_EQ(RunTableRef("SHOW CLUSTERS LIKE 'x' LIMIT 3", /*si=*/true).code(), rewriter::RewriteCode::Success);
  {
    const bool si = false;  // SI surface inactive
    for (const char *sql : {
           "SHOW COLUMNS FROM o LIMIT (SELECT count() FROM `db2.x`)",
           "SHOW FIELDS FROM o LIMIT 1 + (SELECT count() FROM `db2.x`)",
           "SHOW COLUMNS FROM o LIMIT throwIf(1)",
           "SHOW INDEX FROM o WHERE (SELECT count() FROM `db2.x`) = 2",
           "SHOW DICTIONARIES WHERE (SELECT count() FROM `db2.x`) = 2",
           "SHOW DICTIONARIES FROM default WHERE in(7, `db2.x`)",
           "SHOW CLUSTERS LIKE 'x' LIMIT (SELECT count() FROM `db2.x`)",
           "SHOW MERGES LIMIT (SELECT count() FROM `db2.x`)",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
    }
    for (const char *sql : {"SHOW CLUSTERS LIKE 'x' LIMIT 3"}) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
    // The body passes R7; the form then reads system.dictionaries.
    const auto dictionaries = RunTableRef("SHOW DICTIONARIES LIKE 'a%'", si);
    EXPECT_EQ(dictionaries.code(), rewriter::RewriteCode::UnsupportedStatement) << dictionaries.message();
    EXPECT_EQ(dictionaries.message(), "system table system.dictionaries is not accessible");
  }
  const auto physical = RunTableRef("SHOW DICTIONARIES FROM default WHERE in(7, hg_safe.db1__t)", /*si=*/true);
  EXPECT_EQ(physical.code(), rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_EQ(physical.message(), "storage-integrity physical table hg_safe.db1__t is not directly addressable");
  const auto inactive = RunTableRef("SHOW DICTIONARIES FROM default WHERE in(7, hg_safe.db1__t)");
  EXPECT_EQ(inactive.code(), rewriter::RewriteCode::InvalidRewriteRequest);
  EXPECT_EQ(inactive.message(), "protected database hg_safe is not addressable");
}

// Spec 2026-09-26 R7: an unqualified SHOW COLUMNS / INDEX or DESCRIBE target
// resolves like a FROM reference — a bare quoted `db2.x` is the table named
// db2.x in the session's logical database. A qualified target stays verbatim.
TEST(TableRefShow, UnqualifiedTargetsResolveLikeFrom) {
  struct Case { const char *sql; bool si; const char *want; };
  for (const auto &c : std::vector<Case>{
         // ClickHouse's formatter spells the database as a FROM clause.
         {"SHOW COLUMNS FROM `db2.x`", false, "SHOW COLUMNS FROM `db1.db2.x` FROM phys"},
         {"SHOW COLUMNS FROM o WHERE name LIKE 'a%'", false, "SHOW COLUMNS FROM `db1.o` FROM phys WHERE name LIKE 'a%'"},
         {"SHOW COLUMNS FROM o LIMIT 5", false, "SHOW COLUMNS FROM `db1.o` FROM phys LIMIT 5"},
         {"DESCRIBE TABLE `db2.x`", false, "DESCRIBE TABLE phys.`db1.db2.x`"},
         {"DESCRIBE TABLE `db2.x`", true, "DESCRIBE TABLE phys.`db1.db2.x`"},
       }) {
    SCOPED_TRACE(std::string(c.sql) + (c.si ? " [V2]" : " [inactive]"));
    const auto resp = RunTableRef(c.sql, c.si);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    EXPECT_EQ(resp.sql_after_rewrite(), c.want);
    ASSERT_EQ(resp.table_rewrites_size(), 1);
    EXPECT_EQ(resp.table_rewrites().begin()->second.rfind("phys.db1.", 0), 0u);
  }
}

// Task 19 release gates (measured against rewriter-go db2e246).
//  (a) R7 holds for every SHOW trailing clause, the families rewritten into a
//      synthetic SELECT (SHOW TABLES / DATABASES) included: rewriter-go
//      admits their bodies because the rewrite drops them.
//  (b) an Identifier parameter in a SHOW trailing clause is T2.
//  (c) an unqualified DESCRIBE / SHOW COLUMNS target with no logical context
//      is refused, never forwarded to bind in the physical database; the
//      DESCRIBE target is read from the AST, whatever surrounds it.
//  (d) `SHOW TABLES WHERE name IN p` is refused by R7 (rewriter-go admits it:
//      the synthetic enumeration drops the WHERE).
TEST(TableRefShow, CarriedReleaseGates) {
  const std::string unsupported(rewriter_handlers::kUnsupportedStatementMessage);
  const std::string parameter(rewriter_handlers::kIdentifierParameterMessage);
  for (const bool si : {false, true}) {
    for (const char *sql : {
           "SHOW TABLES WHERE name IN (SELECT name FROM merge('phys','^db2'))",
           "SHOW TABLES FROM db1 WHERE name IN (SELECT name FROM merge('db1', 'o'))",
           "SHOW FULL TABLES WHERE (SELECT count() FROM `db2.x`) = 2",
           "SHOW TABLES LIMIT (SELECT count() FROM `db2.x`)",
           "SHOW DATABASES LIMIT (SELECT count() FROM `db2.x`)",
           "SHOW TABLES WHERE name IN p",
           "SHOW TABLES WHERE name IN `db2.x`",
           "SHOW TABLES WHERE name = {v:String}",
           // The text rule: calls, rewriter-go's IN regions and quoted dotted
           // names, whatever the AST folds.
           "SHOW DICTIONARIES FROM default WHERE lower(name) = 'a'",
           "SHOW DICTIONARIES FROM default WHERE `lower`(name) = 'a'",
           "SHOW DICTIONARIES FROM default WHERE EXTRACT(DAY FROM now()) = 1",
           "SHOW DICTIONARIES FROM default WHERE name IN (inf)",
           "SHOW DICTIONARIES FROM default WHERE name IN (0x1F)",
           "SHOW DICTIONARIES FROM default WHERE name IN (1 AS z)",
           "SHOW DICTIONARIES FROM default WHERE name IN tuple(1)",
           "SHOW DICTIONARIES FROM default WHERE `\\x69n`(name, (1, 2))",
           "SHOW DICTIONARIES FROM default WHERE name = 'a' AS `x.y`",
           "SHOW CLUSTERS LIKE 'x' LIMIT 1 + (SELECT 1)",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), unsupported);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
    for (const char *sql : {
           "SHOW COLUMNS FROM db1.o WHERE name = {p:Identifier}",
           "SHOW TABLES WHERE name = {p:Identifier}",
           "SHOW DICTIONARIES FROM default WHERE name IN {p:Identifier}",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest) << resp.message();
      EXPECT_EQ(resp.message(), parameter);
    }
    // Keyword syntax is not a call (rewriter-go admits these), and a
    // literal-only IN region is not a read. The body passes R7; a SHOW
    // DICTIONARIES then reads system.dictionaries, outside the system-table
    // allowlist (Task I2), so it answers that refusal, not R7's.
    for (const char *sql : {
           "SHOW DICTIONARIES FROM default WHERE CASE WHEN 1 THEN 1 END = 1",
           "SHOW DICTIONARIES FROM default WHERE CAST(name AS String) = 'a'",
           "SHOW DICTIONARIES FROM default WHERE IF(1, 1, 0) = 1",
           "SHOW DICTIONARIES FROM default WHERE name IN (1, 2) AND NOT (name = 'b')",
           "SHOW DICTIONARIES FROM default WHERE name IN (1 + 1)",
           "SHOW DICTIONARIES FROM default WHERE db2.x = 1",
           "SHOW DICTIONARIES FROM default WHERE name = 'a' LIMIT 5",
         }) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), "system table system.dictionaries is not accessible");
    }
    // A SHOW TABLES body that passes R7 is then refused by the synthetic
    // enumeration, which would drop it (Task I3, rewriter-go).
    for (const char *sql : {"SHOW TABLES WHERE name IN ('a', 'b') LIMIT 5", "SHOW TABLES LIKE 'a%'"}) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
    }
  }

  // (c) No logical context: the physical context would bind the name.
  for (const bool si : {false, true}) {
    for (const char *sql : {"DESCRIBE TABLE t", "DESC `db2.x`", "DESCRIBE TABLE /*c*/ `db2.x`",
                            "SHOW COLUMNS FROM o", "SHOW INDEX FROM `db2.x`", "SHOW COLUMNS /*c*/ FROM db1__t"}) {
      SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]"));
      rewriter::RewriteSQLRequest req;
      req.set_sql(sql);
      auto *dyn = AddTableRefOption(req, si);
      dyn->set_upstream_logical_database_in_context("");
      dyn->set_upstream_physical_database_in_context("phys");
      const auto resp = RunDirectRewrite(std::move(req));
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest) << resp.sql_after_rewrite();
      EXPECT_NE(resp.message().find("is unqualified and no upstream_logical_database_in_context is set"),
                std::string::npos) << resp.message();
    }
  }
  // A remote-mapped logical context cannot carry a remote() here.
  for (const char *sql : {"DESCRIBE TABLE o", "SHOW COLUMNS FROM o"}) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest req;
    req.set_sql(sql);
    auto *dyn = AddTableRefOption(req, false);
    (*dyn->mutable_logical_database_to_remote_upstream_index())["db1"] = "u";
    auto &up = (*dyn->mutable_remote_upstreams())["u"];
    up.set_addr("h:9000");
    up.set_user("u");
    up.set_password("p");
    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
    EXPECT_NE(resp.message().find("remote upstream"), std::string::npos) << resp.message();
  }
  // The DESCRIBE target is ClickHouse's: a comment or line break around it,
  // or an escaped spelling, does not hide it.
  struct Case { const char *sql; bool si; rewriter::RewriteCode code; const char *want; };
  for (const auto &c : std::vector<Case>{
         {"DESCRIBE TABLE /*c*/ `db2.x`", false, rewriter::RewriteCode::Success, "DESCRIBE TABLE phys.`db1.db2.x`"},
         {"/*c*/ DESCRIBE TABLE o", false, rewriter::RewriteCode::Success, "DESCRIBE TABLE phys.`db1.o`"},
         {"DESCRIBE  TABLE\n`db2.x`", true, rewriter::RewriteCode::Success, "DESCRIBE TABLE phys.`db1.db2.x`"},
         {"describe table `\\x6f`", false, rewriter::RewriteCode::Success, "DESCRIBE TABLE phys.`db1.o`"},
         {"DESCRIBE TABLE /*c*/ hg_safe.db1__t", true, rewriter::RewriteCode::UnsupportedStatement,
          "storage-integrity physical table hg_safe.db1__t is not directly addressable"},
         {"DESCRIBE TABLE db1.o", false, rewriter::RewriteCode::Success, "DESCRIBE TABLE db1.o"},
       }) {
    SCOPED_TRACE(std::string(c.sql) + (c.si ? " [V2]" : " [inactive]"));
    const auto resp = RunTableRef(c.sql, c.si);
    EXPECT_EQ(resp.code(), c.code) << resp.message();
    EXPECT_EQ(c.code == rewriter::RewriteCode::Success ? resp.sql_after_rewrite() : resp.message(), c.want);
  }
  // Under V2 a comment before an Active table's name keeps the SI metadata
  // answer instead of the unmodelled-statement refusal.
  const auto active = RunTableRef("/*c*/ DESCRIBE TABLE t", /*si=*/true);
  EXPECT_EQ(active.code(), rewriter::RewriteCode::Success) << active.message();
  EXPECT_NE(active.sql_after_rewrite().find("hg_safe"), std::string::npos) << active.sql_after_rewrite();
}

// ============================================================================
// Interstitial I1: rewriter-go v0.15.0 rules.
// ============================================================================

namespace {

// RunTableRef with the logical context replaced (`ctx`) and, when `phys` is
// non-null, an upstream_physical_database_in_context.
rewriter::RewriteSQLResponse RunTableRefContext(const std::string &sql, bool si, const std::string &ctx,
                                                const char *phys = nullptr) {
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);
  auto *dyn = AddTableRefOption(req, si);
  dyn->set_upstream_logical_database_in_context(ctx);
  if (phys) dyn->set_upstream_physical_database_in_context(phys);
  return RunDirectRewrite(std::move(req));
}

std::string UnresolvedUnqualifiedMessage(const std::string &table) {
  return "unqualified table \"" + table + "\" does not resolve through the session's logical database";
}

}  // namespace

// rewriter-go #49 (TestTableRef_UnresolvedUnqualifiedNameIsRefused): in
// dynamic mode an unqualified read-position table name — the one-part dotted
// quoted `db1.t` included — whose logical context is empty or unmapped is
// refused, never forwarded to bind in the physical database. Embedded
// INSERT … SELECT / CTAS bodies answer UnsupportedStatement. With the SI
// surface active and no physical context, an IN operand keeps the SI
// namespace refusal.
TEST(TableRefUnresolved, UnqualifiedNameIsRefused) {
  const auto invalid = rewriter::RewriteCode::InvalidRewriteRequest;
  const auto unsupported = rewriter::RewriteCode::UnsupportedStatement;
  struct Pos { const char *sql; const char *table; rewriter::RewriteCode code; };
  const std::vector<Pos> positions = {
    {"SELECT * FROM t", "t", invalid},
    {"SELECT * FROM `db1.t`", "db1.t", invalid},
    {"SELECT * FROM \"other.x\"", "other.x", invalid},
    {"SELECT * FROM db1.o AS a JOIN t AS b USING (a)", "t", invalid},
    {"SELECT * FROM (SELECT * FROM t)", "t", invalid},
    {"SELECT (SELECT max(a) FROM t)", "t", invalid},
    {"WITH c AS (SELECT * FROM t) SELECT * FROM c", "t", invalid},
    {"SELECT a FROM db1.o UNION ALL SELECT a FROM t", "t", invalid},
    {"SELECT * FROM db1.o WHERE a IN (SELECT a FROM t)", "t", invalid},
    {"SELECT * FROM view(SELECT * FROM t)", "t", invalid},
    {"CREATE VIEW db1.v AS SELECT * FROM t", "t", invalid},
    {"CREATE MATERIALIZED VIEW db1.mv TO db1.x AS SELECT * FROM t", "t", invalid},
    {"INSERT INTO db1.o SELECT * FROM t", "t", unsupported},
    {"INSERT INTO db1.o SELECT * FROM `db1.t`", "db1.t", unsupported},
    {"CREATE TABLE db1.n ENGINE = Memory AS SELECT * FROM t", "t", unsupported},
  };
  const std::vector<Pos> in_operands = {
    {"SELECT * FROM db1.o WHERE a IN t", "t", invalid},
    {"SELECT * FROM db1.o WHERE a IN (t)", "t", invalid},
    {"SELECT * FROM db1.o WHERE a IN `db1.t`", "db1.t", invalid},
    {"SELECT * FROM db1.o WHERE in(a, t)", "t", invalid},
  };
  for (const bool si : {false, true}) {
    for (const std::string ctx : {"", "db9"}) {
      for (const bool phys : {false, true}) {
        auto all = positions;
        if (!si || phys) all.insert(all.end(), in_operands.begin(), in_operands.end());
        for (const auto &p : all) {
          SCOPED_TRACE(std::string(p.sql) + (si ? " [V2]" : " [inactive]") + " ctx=" + ctx + (phys ? " phys" : ""));
          const auto resp = RunTableRefContext(p.sql, si, ctx, phys ? "phys" : nullptr);
          EXPECT_EQ(resp.code(), p.code) << resp.message();
          EXPECT_EQ(resp.message(), UnresolvedUnqualifiedMessage(p.table));
          EXPECT_EQ(resp.sql_after_rewrite(), p.sql);
          EXPECT_EQ(resp.table_rewrites_size(), 0);
        }
        if (si && !phys) {
          for (const auto &p : in_operands) {
            SCOPED_TRACE(std::string(p.sql) + " [V2] SI message first, ctx=" + ctx);
            const auto resp = RunTableRefContext(p.sql, si, ctx);
            EXPECT_EQ(resp.code(), rewriter::RewriteCode::RewriteError) << resp.message();
            EXPECT_EQ(resp.message(), "storage-integrity IN table target namespace is not statically resolvable");
          }
        }
      }
    }
  }
}

// rewriter-go TestTableRef_UnresolvedUnqualifiedRuleLeavesOthersAlone: CTE
// names, table aliases, qualified unmapped names and FROM-less statements are
// untouched; a mapped context still rewrites the name; none and static modes
// forward it.
TEST(TableRefUnresolved, RuleLeavesOthersAlone) {
  for (const bool si : {false, true}) {
    for (const std::string ctx : {"", "db9"}) {
      for (const auto &[sql, want] : std::vector<std::pair<std::string, std::string>>{
             {"WITH c AS (SELECT 1 AS a) SELECT * FROM c", "WITH c AS (SELECT 1 AS a) SELECT * FROM c"},
             {"SELECT b.a FROM db1.o AS b", "SELECT b.a FROM phys.`db1.o` AS b"},
             {"SELECT * FROM db2.x", "SELECT * FROM db2.x"},
             {"SELECT 1", "SELECT 1"},
           }) {
        SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]") + " ctx=" + ctx);
        const auto resp = RunTableRefContext(sql, si, ctx);
        EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
        EXPECT_EQ(resp.sql_after_rewrite(), want);
      }
      // A CTE-named IN operand and a qualified unmapped one stay admitted.
      for (const char *sql : {"WITH c AS (SELECT 1 AS a) SELECT * FROM db1.o WHERE a IN c",
                              "SELECT * FROM db1.o WHERE a IN db2.x"}) {
        SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]") + " ctx=" + ctx);
        const auto resp = RunTableRefContext(sql, si, ctx, "phys");
        EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      }
    }
  }
  for (const bool si : {false, true}) {
    const auto resp = RunTableRef("SELECT * FROM `other.x`", si);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    EXPECT_EQ(resp.sql_after_rewrite(), "SELECT * FROM phys.`db1.other.x` AS `other.x`");
  }
  for (const char *sql : {"SELECT * FROM t", "SELECT * FROM `db1.t`", "INSERT INTO db1.o SELECT * FROM t"}) {
    SCOPED_TRACE(sql);
    rewriter::RewriteSQLRequest none;
    none.set_sql(sql);
    EXPECT_EQ(RunDirectRewrite(none).code(), rewriter::RewriteCode::Success);
    rewriter::RewriteSQLRequest stat;
    stat.set_sql(sql);
    AddEmptyStaticOption(stat);
    EXPECT_EQ(RunDirectRewrite(stat).code(), rewriter::RewriteCode::Success);
  }
}

// rewriter-go TestTableRef_UnresolvedUnqualifiedRejectCarriesNoTableRewrites,
// …Precedence and …DoesNotStripSIWriteRefusalMap: an empty table_rewrites map
// in every family (a write statement's own target included); the first
// unresolved name in document order; SI read-surface refusals outrank it; the
// SI write refusal of a view body keeps its own map.
TEST(TableRefUnresolved, PrecedenceAndEmptyMap) {
  for (const bool si : {false, true}) {
    for (const std::string ctx : {"", "db9"}) {
      for (const char *sql : {
             "SELECT * FROM db1.o AS a JOIN t AS b USING (a)",
             "SELECT * FROM db1.t JOIN t2 USING (a)",
             "SELECT * FROM db1.o WHERE a IN t",
             "INSERT INTO db1.o SELECT * FROM t",
             "CREATE TABLE db1.n ENGINE = Memory AS SELECT * FROM db1.o JOIN t USING (a)",
             "CREATE VIEW db1.v AS SELECT * FROM db1.o JOIN t USING (a)",
             "CREATE MATERIALIZED VIEW db1.mv TO db1.x AS SELECT * FROM t",
           }) {
        SCOPED_TRACE(std::string(sql) + (si ? " [V2]" : " [inactive]") + " ctx=" + ctx);
        const auto resp = RunTableRefContext(sql, si, ctx, "phys");
        EXPECT_EQ(resp.message().rfind("unqualified table ", 0), 0u) << resp.message();
        EXPECT_EQ(resp.table_rewrites_size(), 0);
      }
    }
  }
  struct Case { const char *sql; bool si; bool phys; rewriter::RewriteCode code; std::string message; };
  const auto invalid = rewriter::RewriteCode::InvalidRewriteRequest;
  for (const auto &c : std::vector<Case>{
         {"SELECT * FROM b JOIN a USING (x)", false, true, invalid, UnresolvedUnqualifiedMessage("b")},
         {"SELECT * FROM (SELECT * FROM inner_t) AS q JOIN outer_t USING (a)", false, true, invalid,
          UnresolvedUnqualifiedMessage("inner_t")},
         {"SELECT (SELECT max(a) FROM s) FROM f", false, true, invalid, UnresolvedUnqualifiedMessage("s")},
         {"WITH c AS (SELECT * FROM x) SELECT * FROM y JOIN c USING (a)", false, true, invalid,
          UnresolvedUnqualifiedMessage("x")},
         {"SELECT * FROM f WHERE a IN (SELECT a FROM s)", false, true, invalid, UnresolvedUnqualifiedMessage("f")},
         {"SELECT a FROM u1 UNION ALL SELECT a FROM u2", false, true, invalid, UnresolvedUnqualifiedMessage("u1")},
         {"SELECT * FROM t2 JOIN db1.t FINAL USING (a)", true, true, rewriter::RewriteCode::RewriteError,
          "FINAL/SAMPLE/PREWHERE/WITH OFFSET/column aliases on storage-integrity tables are not supported"},
         {"SELECT * FROM db1.o WHERE a IN t", true, false, rewriter::RewriteCode::RewriteError,
          "storage-integrity IN table target namespace is not statically resolvable"},
         {"SELECT * FROM db1.o WHERE a IN t", true, true, invalid, UnresolvedUnqualifiedMessage("t")},
         {"SELECT * FROM `a\"b`", false, true, invalid, UnresolvedUnqualifiedMessage("a\"b")},
       }) {
    for (const std::string ctx : {"", "db9"}) {
      SCOPED_TRACE(std::string(c.sql) + (c.si ? " [V2]" : " [inactive]") + " ctx=" + ctx);
      const auto resp = RunTableRefContext(c.sql, c.si, ctx, c.phys ? "phys" : nullptr);
      EXPECT_EQ(resp.code(), c.code) << resp.message();
      EXPECT_EQ(resp.message(), c.message);
    }
  }
  const std::string si_write = "storage-integrity table db1.t accepts writes only through the signed statement lane";
  for (const std::string ctx : {"", "db9"}) {
    for (const auto &[base, with_unresolved] : std::vector<std::pair<std::string, std::string>>{
           {"CREATE VIEW db1.v AS SELECT * FROM db1.t", "CREATE VIEW db1.v AS SELECT * FROM db1.t JOIN t2 USING (a)"},
           {"CREATE MATERIALIZED VIEW db1.mv TO db1.x AS SELECT * FROM db1.t",
            "CREATE MATERIALIZED VIEW db1.mv TO db1.x AS SELECT * FROM db1.t JOIN t2 USING (a)"},
         }) {
      SCOPED_TRACE(with_unresolved + " ctx=" + ctx);
      const auto a = RunTableRefContext(base, true, ctx, "phys");
      const auto b = RunTableRefContext(with_unresolved, true, ctx, "phys");
      EXPECT_EQ(a.code(), rewriter::RewriteCode::UnsupportedStatement) << a.message();
      EXPECT_EQ(a.message(), si_write);
      EXPECT_EQ(b.code(), rewriter::RewriteCode::UnsupportedStatement) << b.message();
      EXPECT_EQ(b.message(), si_write);
      ASSERT_EQ(a.table_rewrites().count("db1.t"), 1u);
      EXPECT_EQ(a.table_rewrites().at("db1.t"), "hg_safe.db1__t");
      using Map = std::map<std::string, std::string>;
      EXPECT_EQ(Map(a.table_rewrites().begin(), a.table_rewrites().end()),
                Map(b.table_rewrites().begin(), b.table_rewrites().end()));
    }
  }
}

// rewriter-go #51 round 6/7 (TestTableRef_NameResolutionSettingsAreRefused):
// the settings that change what a name binds to — enable_global_with_statement,
// compatibility, implicit_table_at_top_level — and promql_table /
// promql_database are refused like the SQL-bearing and dialect settings, any
// value, any case, in every query-level SETTINGS clause (a view / MV body's,
// an INSERT header's, a command's tail included) and in a session SET. The
// analyzer switches stay accepted.
TEST(TableRefSettings, NameBindingSettingsAreRefused) {
  const auto msg = [](const std::string &name) { return "table setting " + name + " is not accepted"; };
  for (const bool si : {false, true}) {
    for (const auto &[sql, name] : std::vector<std::pair<std::string, std::string>>{
           {R"(CREATE VIEW db1.v AS WITH 1 AS "other.secret" SELECT a FROM db1.o WHERE 0 UNION ALL SELECT a FROM db1.o WHERE a IN "other.secret" SETTINGS enable_global_with_statement = 0)", "enable_global_with_statement"},
           {R"(CREATE MATERIALIZED VIEW db1.mv ENGINE = Memory POPULATE AS WITH 1 AS "other.secret" SELECT a FROM db1.o WHERE 0 UNION ALL SELECT a FROM db1.o WHERE a IN "other.secret" SETTINGS enable_global_with_statement = 0)", "enable_global_with_statement"},
           {R"(WITH "other.secret" AS (SELECT toUInt64(1) AS a) SELECT a FROM db1.o WHERE a IN (SELECT a FROM "other.secret") SETTINGS enable_global_with_statement = 0)", "enable_global_with_statement"},
           {R"(WITH 1 AS "other.secret" SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE a IN "other.secret") SETTINGS enable_global_with_statement = 0)", "enable_global_with_statement"},
           {R"(INSERT INTO db1.q WITH toUInt64(1) AS "other.secret" SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE a IN "other.secret") SETTINGS enable_global_with_statement = 0)", "enable_global_with_statement"},
           {R"(CREATE TABLE db1.n ENGINE = Memory AS WITH toUInt64(1) AS "other.secret" SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE a IN "other.secret") SETTINGS enable_global_with_statement = 0)", "enable_global_with_statement"},
           {"SELECT a FROM db1.o SETTINGS enable_global_with_statement = 1", "enable_global_with_statement"},
           {"SELECT a FROM db1.o SETTINGS max_threads = 1, Enable_Global_With_Statement = false", "Enable_Global_With_Statement"},
           {"INSERT INTO db1.q SETTINGS enable_global_with_statement = 0 SELECT a FROM db1.o", "enable_global_with_statement"},
           {"INSERT INTO db1.q (a) SETTINGS compatibility = '20.8' SELECT a", "compatibility"},
           {R"(WITH 1 AS "other.secret" SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE a IN "other.secret") SETTINGS compatibility = '20.8')", "compatibility"},
           {"SELECT a FROM db1.o SETTINGS compatibility = '25.1'", "compatibility"},
           {"SELECT a SETTINGS implicit_table_at_top_level = 'z'", "implicit_table_at_top_level"},
           {"SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.p) SETTINGS implicit_table_at_top_level = 'other.secret'", "implicit_table_at_top_level"},
           {"CREATE VIEW db1.v AS SELECT a SETTINGS implicit_table_at_top_level = 'z'", "implicit_table_at_top_level"},
           {"SELECT a FROM db1.o SETTINGS promql_table = 'z'", "promql_table"},
           {"SELECT a FROM db1.o SETTINGS promql_database = 'z'", "promql_database"},
           {"SELECT a FROM db1.o SETTINGS COMPATIBILITY = DEFAULT", "COMPATIBILITY"},
           {"DESCRIBE TABLE db1.o SETTINGS enable_global_with_statement = 0", "enable_global_with_statement"},
           {"ALTER TABLE db1.o UPDATE a = 1 WHERE 1 SETTINGS implicit_table_at_top_level = 'z'", "implicit_table_at_top_level"},
           {"DELETE FROM db1.o WHERE 1 SETTINGS enable_global_with_statement = 0", "enable_global_with_statement"},
           {"CREATE TABLE db1.n (a UInt8) ENGINE = MergeTree ORDER BY a SETTINGS compatibility = '20.1'", "compatibility"},
           // A name ClickHouse decodes to a refused setting is that setting.
           {"SELECT a FROM db1.o SETTINGS `\\Nenable_global_with_statement` = 0", "enable_global_with_statement"},
           {"SELECT a FROM db1.o SETTINGS `\\x65nable_global_with_statement` = 0", "enable_global_with_statement"},
           {"INSERT INTO db1.q (a) SETTINGS max_threads = 1, `impl\\Nicit_table_at_top_level` = 'phys.z' SELECT a", "implicit_table_at_top_level"},
           {"DESCRIBE TABLE db1.o SETTINGS \"\\Ncompatibility\" = '20.1'", "compatibility"},
           {"SELECT a FROM db1.o SETTINGS \xE2\x80\x9C" "compatibility\xE2\x80\x9D = '20.1'", "compatibility"},
           // Task I3 fix round 1: the old analyzer is refused (user ruling
           // 2026-10-01). The analyzer switch unless its value is a true
           // literal; legacy_column_name_of_tuple_literal and profile for any
           // value. Every settings position, both SI states.
           {"SELECT a FROM db1.o SETTINGS enable_analyzer = 0", "enable_analyzer"},
           {"SELECT a FROM db1.o SETTINGS allow_experimental_analyzer = 0, enable_scopes_for_with_statement = 0, "
            "prefer_column_name_to_alias = 1", "allow_experimental_analyzer"},
           {"SELECT a FROM db1.o SETTINGS enable_analyzer = 'false'", "enable_analyzer"},
           {"SELECT a FROM db1.o SETTINGS enable_analyzer = 0x1", "enable_analyzer"},
           {"SELECT a FROM db1.o SETTINGS enable_analyzer = 1.0", "enable_analyzer"},
           {"SELECT a FROM db1.o SETTINGS enable_analyzer = +1", "enable_analyzer"},
           {"SELECT a FROM db1.o SETTINGS legacy_column_name_of_tuple_literal = 0", "legacy_column_name_of_tuple_literal"},
           {"SELECT a FROM db1.o SETTINGS legacy_column_name_of_tuple_literal = 1", "legacy_column_name_of_tuple_literal"},
           {"SELECT a FROM db1.o SETTINGS profile = 'default'", "profile"},
           {"DESCRIBE TABLE db1.o SETTINGS enable_analyzer = 0", "enable_analyzer"},
           {"CREATE TABLE db1.n (a UInt8) ENGINE = MergeTree ORDER BY a SETTINGS enable_analyzer = 0", "enable_analyzer"},
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), msg(name));
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
    for (const auto &[sql, want] : std::vector<std::pair<std::string, std::string>>{
           // The analyzer switch with a true literal, and the other analyzer
           // settings ClickHouse keeps accepting, stay admitted. The engine's
           // embedded 26.3 formatter renders the boolean keyword lowercase
           // (`true`); the corpus pin has `TRUE`, so si_tr_setting_analyzer_on_*
           // pin want_sql_cpp like si_tr_settings_plain_values_* for the same
           // formatting divergence.
           {"SELECT a FROM db1.o SETTINGS enable_analyzer = 1, allow_experimental_analyzer = true",
            "SELECT a FROM phys.`db1.o` AS `db1.o` SETTINGS enable_analyzer = 1, allow_experimental_analyzer = true"},
           {"SELECT a FROM db1.o SETTINGS enable_analyzer = '1'",
            "SELECT a FROM phys.`db1.o` AS `db1.o` SETTINGS enable_analyzer = '1'"},
           {"SELECT a FROM db1.o SETTINGS enable_scopes_for_with_statement = 0, prefer_column_name_to_alias = 1",
            "SELECT a FROM phys.`db1.o` AS `db1.o` SETTINGS enable_scopes_for_with_statement = 0, prefer_column_name_to_alias = 1"},
           // A table-setting position: ClickHouse has no such MergeTree
           // setting (UNKNOWN_SETTING); rewriter-go admits it too.
           {"ALTER TABLE db1.o MODIFY SETTING max_parts_in_total = 100",
            "ALTER TABLE phys.`db1.o` (MODIFY SETTING max_parts_in_total = 100)"},
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.sql_after_rewrite(), want);
    }
  }
  // A session SET: the carve-out exists only with the SI surface inactive.
  for (const auto &[sql, name] : std::vector<std::pair<std::string, std::string>>{
         {"SET enable_global_with_statement = 0", "enable_global_with_statement"},
         {"SET max_threads = 1, enable_global_with_statement = 0", "enable_global_with_statement"},
         {"SET implicit_table_at_top_level = 'z'", "implicit_table_at_top_level"},
         {"SET compatibility = '20.1'", "compatibility"},
         {"SET promql_table = 'z'", "promql_table"},
         {"SET promql_database = 'z'", "promql_database"},
         {"SET `\\Nenable_global_with_statement` = 0", "enable_global_with_statement"},
         {"SET max_threads = 1, \"\\Ncompatibility\" = '20.8'", "compatibility"},
         {"SET `impl\\Nicit_table_at_top_level` = 'other.z';", "implicit_table_at_top_level"},
       }) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql, false);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), msg(name));
    EXPECT_EQ(RunTableRef(sql, true).code(), rewriter::RewriteCode::UnsupportedStatement);
  }
  // A SHOW COLUMNS tail (under V2 the SI logical-database refusal answers
  // first).
  const auto show = RunTableRef("SHOW COLUMNS FROM db1.o SETTINGS compatibility = '20.1'", false);
  EXPECT_EQ(show.code(), rewriter::RewriteCode::UnsupportedStatement) << show.message();
  EXPECT_EQ(show.message(), msg("compatibility"));
}

// rewriter-go #51 rounds 7–9 (TestTableRef_EscapedSettingNamesAreRefused,
// …UnparseableSettingsClausesFailClosed, …ShowSettingsSkipAndTableSettingNames).
// ClickHouse's parser decodes every settings list the engine forwards, so the
// checks above see the decoded names. On top of that, as rewriter-go does:
// a quoted setting name whose source spelling is not its plain text (a
// backslash escape, a doubled quote, Unicode quotes) followed by `=` after a
// SETTINGS / SETTING keyword or in a SET list, and a compound (dotted)
// setting name anywhere, are refused as `statement is not supported`; ALTER …
// RESET SETTING refuses a refused table setting. HouseGate's own keys stay
// accepted.
TEST(TableRefSettings, SettingNameSpellingAndTableSettingKeys) {
  const std::string unsupported = "statement is not supported";
  for (const bool si : {false, true}) {
    for (const std::string sql : {
           "SELECT a FROM db1.o SETTINGS `\\Nmax_threads` = 1",
           "SELECT a FROM db1.o SETTINGS `max``threads` = 1",
           "SELECT a FROM db1.o SETTINGS \"max\"\"threads\" = 1",
           "SELECT a FROM db1.o SETTINGS \xE2\x80\x9Cmax_threads\xE2\x80\x9D = 1",
           "DESCRIBE TABLE db1.o SETTINGS `\\Nmax_threads` = 1",
           "INSERT INTO db1.q (a) SETTINGS `\\Nmax_threads` = 1 SELECT a",
           "SELECT a FROM db1.o SETTINGS SQL_a.b = 1",
           "SELECT a FROM db1.o SETTINGS `SQL_a`.`b` = 1",
           "INSERT INTO db1.q SETTINGS SQL_a.b = 1 SELECT a FROM db1.o",
           "INSERT INTO db1.q (a) SETTINGS SQL_a.b = 1 SELECT a",
           "INSERT INTO db1.q (a) SETTINGS max_threads = 1, SQL_a . b = 1 SELECT a",
           "INSERT INTO db1.q (a) SETTINGS max_threads = 1 SELECT a SETTINGS SQL_a.b = 1",
           "DESCRIBE TABLE db1.o SETTINGS SQL_a.b.c = 1",
           "ALTER TABLE db1.o MODIFY SETTING `storage``policy` = 'default'",
           "ALTER TABLE db1.o MODIFY SETTING `\\Nmax_threads` = 1",
           "ALTER TABLE db1.o RESET SETTING `\\Ncompatibility`",
           "ALTER TABLE db1.o RESET SETTING max_parts_in_total, \"\\Nmax_threads\"",
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), unsupported);
    }
    for (const auto &[sql, name] : std::vector<std::pair<std::string, std::string>>{
           {"ALTER TABLE db1.o RESET SETTING storage_policy", "storage_policy"},
           {"ALTER TABLE db1.o RESET SETTING disk, max_parts_in_total", "disk"},
           // Decoded by ClickHouse's parser: the refused table setting itself
           // (rewriter-go answers `statement is not supported`).
           {"ALTER TABLE db1.o RESET SETTING `\\Nstorage_policy`", "storage_policy"},
           {"ALTER TABLE db1.o MODIFY SETTING `\\Nstorage_policy` = 'default'", "storage_policy"},
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), "table setting " + name + " is not accepted");
    }
    for (const auto &[sql, want] : std::vector<std::pair<std::string, std::string>>{
           {"SELECT a FROM db1.o SETTINGS `max_threads` = 1", "SELECT a FROM phys.`db1.o` AS `db1.o` SETTINGS max_threads = 1"},
           {"SELECT a FROM db1.o SETTINGS SQL_x_auth_token = 'x', SQL_x_statement_token = 'y', SQL_x_payer = 'p', "
            "SQL_x_read_mode = 'safe', SQL_sentio_driver = 1, SQL_sentio_maintenance = 1, SQL_sentio_platform_operator = 1",
            "SELECT a FROM phys.`db1.o` AS `db1.o` SETTINGS SQL_x_auth_token = 'x', SQL_x_statement_token = 'y', "
            "SQL_x_payer = 'p', SQL_x_read_mode = 'safe', SQL_sentio_driver = 1, SQL_sentio_maintenance = 1, "
            "SQL_sentio_platform_operator = 1"},
           {"INSERT INTO db1.q (a) SETTINGS SQL_x_payer = 'p', SQL_x_statement_token = 'y' SELECT a",
            "INSERT INTO phys.`db1.q` (a) SETTINGS SQL_x_payer = 'p', SQL_x_statement_token = 'y' SELECT a"},
           {"ALTER TABLE db1.o UPDATE settings = 1 WHERE 1", "ALTER TABLE phys.`db1.o` (UPDATE  settings = 1 WHERE 1)"},
           {"ALTER TABLE db1.o RESET SETTING max_parts_in_total",
            "ALTER TABLE phys.`db1.o` (RESET SETTING  max_parts_in_total)"},
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.sql_after_rewrite(), want);
    }
  }
  // The session SET list (surface inactive): the same spelling rule, and a
  // compound name is not a modelled SET; HouseGate's keys stay accepted.
  for (const std::string sql : {"SET `\\Nmax_threads` = 1", "SET `max``threads` = 1", "SET \"max\"\"threads\" = 1",
                                "SET SQL_a.b = 1", "SET `SQL_a`.b = 1", "SET \"SQL_a\".\"b\" = 1",
                                "SET SQL_x_read_mode", "SET SQL_x_read_mode;", "SET SQL_x_read_mode, max_threads = 1",
                                "SET max_threads = 1, SQL_x_read_mode"}) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql, false);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), unsupported);
  }
  for (const std::string sql : {"SET max_threads = 1", "SET `max_threads` = 1",
                                "SET SQL_x_read_mode = 'safe', SQL_sentio_maintenance = 1",
                                "SET SQL_x_auth_token = 'x', SQL_x_statement_token = 'y', SQL_x_payer = 'p', "
                                "SQL_sentio_driver = 1, SQL_sentio_platform_operator = 1"}) {
    SCOPED_TRACE(sql);
    EXPECT_EQ(RunTableRef(sql, false).code(), rewriter::RewriteCode::Success);
  }
  for (const std::string sql : {"SHOW SETTINGS LIKE 'max%'", "/* c */ SHOW CHANGED SETTINGS ILIKE 'max%'"}) {
    SCOPED_TRACE(sql);
    EXPECT_EQ(RunTableRef(sql, false).code(), rewriter::RewriteCode::Success);
  }
}

// Fidelity (I1 differential): ClickHouse's parser keeps a WINDOW clause
// definition and an APPLY transformer's lambda outside `children`, so no
// policy check saw a read there and the rewrite left it verbatim
// (`WINDOW w AS (PARTITION BY (SELECT 1 FROM hg_unsafe.x))` answered Success).
// A parametrised alias is refused: the formatter cannot re-emit it.
TEST(TableRefFidelity, FieldOnlySubtreesAreGoverned) {
  for (const bool si : {false, true}) {
    struct Case { const char *sql; rewriter::RewriteCode code; std::string message; };
    const std::string hg_unsafe = si ? "storage-integrity physical table hg_unsafe.x is not directly addressable"
                                     : "protected database hg_unsafe is not addressable";
    const auto hg_code = si ? rewriter::RewriteCode::RewriteError : rewriter::RewriteCode::InvalidRewriteRequest;
    for (const auto &c : std::vector<Case>{
           {"SELECT a FROM db1.o WINDOW w AS (PARTITION BY (SELECT 1 FROM hg_unsafe.x))", hg_code, hg_unsafe},
           {"SELECT row_number() OVER w AS rn WINDOW w AS (PARTITION BY (SELECT 1 FROM hg_unsafe.x)) "
            "QUALIFY EXISTS(SELECT 1 FROM db1.t)", hg_code, hg_unsafe},
           {"SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.p WINDOW w AS (PARTITION BY (SELECT 1 FROM hg_unsafe.x)))",
            hg_code, hg_unsafe},
           {"SELECT a FROM db1.o WINDOW w AS (ORDER BY a ROWS BETWEEN (SELECT 1 FROM hg_unsafe.x) PRECEDING AND CURRENT ROW)",
            hg_code, hg_unsafe},
           {"SELECT a FROM db1.o WINDOW w AS (ORDER BY a IN phys.x)", rewriter::RewriteCode::InvalidRewriteRequest,
            "protected database phys is not addressable"},
           {"SELECT a FROM db1.o WINDOW w AS (PARTITION BY a IN {p:Identifier})",
            rewriter::RewriteCode::InvalidRewriteRequest,
            "query parameters are not supported in a database or table position"},
           {"SELECT a FROM db1.o WINDOW w AS (PARTITION BY (SELECT 1 FROM remote('h', 'db', 't')))",
            rewriter::RewriteCode::UnsupportedStatement, "table function remote is not accepted"},
           {"SELECT a FROM db1.o WINDOW w AS (PARTITION BY joinGet('db2.j', 'v', a))",
            rewriter::RewriteCode::InvalidRewriteRequest,
            "joinGet target \"db2.j\" does not resolve through the caller's databases"},
           // A lambda holding a query is refused before the other checks (Task
           // I3, as rewriter-go); under V2 the reject annotator names the SI
           // object.
           {"SELECT * APPLY(x -> (SELECT 1 FROM hg_unsafe.x)) FROM db1.o", rewriter::RewriteCode::UnsupportedStatement,
            si ? "storage-integrity physical table hg_unsafe.x is not directly addressable" : "statement is not supported"},
           {"SELECT a FROM db1.o AS {alias:Identifier}", rewriter::RewriteCode::UnsupportedStatement,
            "statement is not supported"},
           {"SELECT a AS {alias:Identifier} FROM db1.o", rewriter::RewriteCode::UnsupportedStatement,
            "statement is not supported"},
         }) {
      SCOPED_TRACE(std::string(c.sql) + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(c.sql, si);
      EXPECT_EQ(resp.code(), c.code) << resp.message();
      EXPECT_EQ(resp.message(), c.message);
    }
    // COLUMNS(…) APPLY(lambda): the lambda's IN operand is a read.
    const auto apply = RunTableRef("SELECT COLUMNS('a') APPLY(x -> x IN hg_unsafe.x) FROM db1.o", si);
    EXPECT_NE(apply.code(), rewriter::RewriteCode::Success) << apply.sql_after_rewrite();
    // Reads in a WINDOW definition are rewritten like any other.
    const auto read = RunTableRef("SELECT a FROM db1.o WINDOW w AS (PARTITION BY (SELECT 1 FROM db1.t))", si);
    EXPECT_EQ(read.code(), rewriter::RewriteCode::Success) << read.message();
    EXPECT_EQ(read.sql_after_rewrite(),
              std::string("SELECT a FROM phys.`db1.o` AS `db1.o` WINDOW w AS (PARTITION BY (SELECT 1 FROM ")
                + (si ? "(SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t)" : "phys.`db1.t`") + " AS `db1.t`))");
    const auto plain = RunTableRef("SELECT sum(a) OVER w FROM db1.o WINDOW w AS (PARTITION BY a ORDER BY b)", si);
    EXPECT_EQ(plain.code(), rewriter::RewriteCode::Success) << plain.message();
    EXPECT_EQ(plain.sql_after_rewrite(),
              "SELECT sum(a) OVER w FROM phys.`db1.o` AS `db1.o` WINDOW w AS (PARTITION BY a ORDER BY b ASC)");
    const auto unresolved =
      RunTableRefContext("SELECT a FROM db1.o WINDOW w AS (PARTITION BY (SELECT 1 FROM t))", si, "", "phys");
    EXPECT_EQ(unresolved.code(), rewriter::RewriteCode::InvalidRewriteRequest) << unresolved.message();
    EXPECT_EQ(unresolved.message(), UnresolvedUnqualifiedMessage("t"));
  }
}

// rewriter-go #54 (decode names like ClickHouse before every check), C++
// side: the SI compatibility scan reads table names from the statement text
// before parsing. It decoded quoted names by hand (`\N` kept as N, no
// Unicode quotes), so an Active table spelled `db\N1`.t or “db1”.“t” was an
// ordinary table to it: its alias column list or WITH OFFSET was stripped
// for the parser, the table became the derived read, and the wrapper was put
// back on it (Success; rewriter-go refuses).
TEST(TableRefDecoding, StorageIntegrityScanDecodesLikeClickHouse) {
  const std::string wrapper =
    "FINAL/SAMPLE/PREWHERE/WITH OFFSET/column aliases on storage-integrity tables are not supported";
  for (const std::string name : {"`db\\N1`.t", "db1.`\\Nt`", "\"db\\N1\".t", "`db1`.`\\x74`",
                                 "\xE2\x80\x9C" "db1\xE2\x80\x9D.\xE2\x80\x9Ct\xE2\x80\x9D", "db1.\xE2\x80\x9Ct\xE2\x80\x9D",
                                 "`\\Nt`", "`t\\N`", "\xE2\x80\x9Ct\xE2\x80\x9D"}) {
    for (const std::string sql : {"SELECT * FROM " + name + " AS x (c1, c2)", "SELECT * FROM " + name + " x (c1, c2)",
                                  "SELECT * FROM db1.o JOIN " + name + " AS x (c1) ON 1",
                                  "/* /* nested */ */ SELECT * FROM " + name + " AS x (c1, c2)",
                                  "SELECT * FROM " + name + " AS x (c1, c2) # it's",
                                  "SELECT * FROM " + name + " WITH OFFSET AS off",
                                  "SELECT * FROM db1.o, " + name + " WITH OFFSET off"}) {
      SCOPED_TRACE(sql);
      const auto resp = RunTableRefContext(sql, /*si=*/true, "db1", "phys");
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::RewriteError) << resp.message() << " | " << resp.sql_after_rewrite();
      EXPECT_EQ(resp.message(), wrapper);
    }
  }
  // An ordinary table keeps its alias column list and WITH OFFSET.
  const auto ordinary = RunTableRefContext("SELECT * FROM `db1`.`\\x6f` AS x (c1, c2)", true, "db1", "phys");
  EXPECT_EQ(ordinary.code(), rewriter::RewriteCode::Success) << ordinary.message();
  EXPECT_EQ(ordinary.sql_after_rewrite(), "SELECT * FROM phys.`db1.o` AS x(c1, c2)");
}

// I1 fix round 1 (review C1 / I1 / I2 / M1–M4). The structural enumeration of
// the ClickHouse AST fields kept outside `children` (report, fix round 1)
// found two more read-bearing ones: a TTL element's GROUP BY … SET keys and
// assignments and its RECOMPRESS CODEC, and ALTER … MODIFY REFRESH's refresh
// strategy. Each is now walked like any other TTL / ALTER expression.
TEST(TableRefFidelity, TtlGroupBySetAndRecompressAreGoverned) {
  const std::string t7 = "statement is not supported";
  for (const bool si : {false, true}) {
    struct Case { std::string tail; rewriter::RewriteCode code; std::string message; };
    const std::string hg = si ? "storage-integrity physical table hg_unsafe.x is not directly addressable"
                              : "protected database hg_unsafe is not addressable";
    const auto hg_code = si ? rewriter::RewriteCode::UnsupportedStatement : rewriter::RewriteCode::InvalidRewriteRequest;
    const auto invalid = rewriter::RewriteCode::InvalidRewriteRequest;
    const auto unsupported = rewriter::RewriteCode::UnsupportedStatement;
    const std::vector<Case> cases = {
      {"GROUP BY a SET v = max(v) + (SELECT max(v) FROM phys.`db2.secret`)", invalid,
       "protected database phys is not addressable"},
      {"GROUP BY a SET v = max(v) + (SELECT 1 FROM hg_unsafe.x)", hg_code, hg},
      {"GROUP BY a, a IN (SELECT 1 FROM phys.x) SET v = max(v)", invalid, "protected database phys is not addressable"},
      {"GROUP BY a SET v = max(v) + (SELECT max(v) FROM db1.t)", unsupported, t7},
      {"GROUP BY a SET v = max(v) + (SELECT max(v) FROM db1.p)", unsupported, t7},
      {"GROUP BY a SET v = max(v IN db2.secret)", unsupported, t7},
      {"GROUP BY a SET v = max(v) + (SELECT count() FROM remote('h','db','t'))", unsupported,
       "table function remote is not accepted"},
      {"GROUP BY a SET v = max(v) + dictGet('db2.dict', 'v', toUInt64(a))", invalid,
       "dictGet target \"db2.dict\" does not resolve through the caller's databases"},
      {"GROUP BY a SET v = max(v) + joinGet('db2.j', 'v', a)", invalid,
       "joinGet target \"db2.j\" does not resolve through the caller's databases"},
      {"GROUP BY a SET v = max(v) + hasColumnInTable('db2', 'x', 'a')", invalid,
       "hasColumnInTable target \"db2.x\" does not resolve through the caller's databases"},
      {"GROUP BY a SET v = max(v IN {p:Identifier})", invalid,
       "query parameters are not supported in a database or table position"},
      {"RECOMPRESS CODEC(ZSTD((SELECT 1 FROM hg_unsafe.x)))", hg_code, hg},
    };
    for (const auto &c : cases) {
      for (const std::string sql : {
             "CREATE TABLE db1.n (a UInt8, d DateTime, v UInt64) ENGINE = MergeTree ORDER BY a TTL d + INTERVAL 1 SECOND " + c.tail,
             "ALTER TABLE db1.o MODIFY TTL d + INTERVAL 1 SECOND " + c.tail,
             "CREATE MATERIALIZED VIEW db1.mv ENGINE = MergeTree ORDER BY a TTL d + INTERVAL 1 SECOND " + c.tail
               + " AS SELECT a, d, v FROM db1.o"}) {
        SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
        const auto resp = RunTableRef(sql, si);
        EXPECT_EQ(resp.code(), c.code) << resp.message();
        EXPECT_EQ(resp.sql_after_rewrite(), sql);
        // Engine-local precedence in an ALTER action (pre-existing, PARITY):
        // a table function there is an R2 read (the action is opaque text to
        // rewriter-go, which names T5), and under V2 the SI write preflight
        // names an Active table the action reads before R2.
        const bool alter = sql.rfind("ALTER", 0) == 0;
        if (alter && c.message == "table function remote is not accepted") {
          EXPECT_EQ(resp.message(), t7);
        } else if (alter && si && c.tail.find("db1.t)") != std::string::npos) {
          EXPECT_EQ(resp.message(), "storage-integrity table db1.t accepts writes only through the signed statement lane");
        } else {
          EXPECT_EQ(resp.message(), c.message);
        }
      }
    }
    // An unresolved name in the SET subquery is an R2 read first.
    const auto unresolved = RunTableRefContext(
      "CREATE TABLE db1.n (a UInt8, d DateTime, v UInt64) ENGINE = MergeTree ORDER BY a TTL d GROUP BY a "
      "SET v = max(v IN (SELECT v FROM t))", si, "", "phys");
    EXPECT_EQ(unresolved.code(), rewriter::RewriteCode::UnsupportedStatement) << unresolved.message();
    EXPECT_EQ(unresolved.message(), t7);
    // Controls: a read-free GROUP BY … SET and RECOMPRESS stay accepted.
    for (const auto &[sql, want] : std::vector<std::pair<std::string, std::string>>{
           {"CREATE TABLE db1.n (a UInt8, d DateTime, v UInt64) ENGINE = MergeTree ORDER BY a TTL d GROUP BY a SET v = max(v)",
            "CREATE TABLE phys.`db1.n` (`a` UInt8, `d` DateTime, `v` UInt64) ENGINE = MergeTree ORDER BY a "
            "TTL d GROUP BY a SET v = max(v)"},
           {"ALTER TABLE db1.o MODIFY TTL d GROUP BY a SET v = max(v)",
            "ALTER TABLE phys.`db1.o` (MODIFY TTL d GROUP BY a SET v = max(v))"},
           {"CREATE TABLE db1.n (a UInt8, d DateTime) ENGINE = MergeTree ORDER BY a TTL d RECOMPRESS CODEC(ZSTD(1))",
            "CREATE TABLE phys.`db1.n` (`a` UInt8, `d` DateTime) ENGINE = MergeTree ORDER BY a "
            "TTL d RECOMPRESS CODEC(ZSTD(1))"},
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.sql_after_rewrite(), want);
    }
  }
}

// ALTER … MODIFY REFRESH … DEPENDS ON: protected / SI / parameter names are
// refused like anywhere else; any other DEPENDS ON target is refused as an
// ungoverned position (the engine does not rewrite it; rewriter-go forwards
// it unrewritten, PARITY). A plain MODIFY REFRESH stays accepted.
TEST(TableRefFidelity, ModifyRefreshDependsOnIsGoverned) {
  for (const bool si : {false, true}) {
    const std::string hg = si ? "storage-integrity physical table hg_unsafe.x is not directly addressable"
                              : "protected database hg_unsafe is not addressable";
    for (const auto &[sql, code, message] : std::vector<std::tuple<std::string, rewriter::RewriteCode, std::string>>{
           // Under V2 the SI namespace defers hg_* to the SI handlers, which do
           // not read a DEPENDS ON list: R2 answers (rewriter-go names the SI
           // physical table; both refuse).
           {"ALTER TABLE db1.mv MODIFY REFRESH EVERY 1 HOUR DEPENDS ON hg_unsafe.x",
            si ? rewriter::RewriteCode::UnsupportedStatement : rewriter::RewriteCode::InvalidRewriteRequest,
            si ? "statement is not supported" : hg},
           {"ALTER TABLE db1.mv MODIFY REFRESH EVERY 1 HOUR DEPENDS ON phys.x",
            rewriter::RewriteCode::InvalidRewriteRequest, "protected database phys is not addressable"},
           {"ALTER TABLE db1.mv MODIFY REFRESH EVERY 1 HOUR DEPENDS ON db1.x",
            rewriter::RewriteCode::UnsupportedStatement, "statement is not supported"},
           {"ALTER TABLE db1.mv MODIFY REFRESH EVERY 1 HOUR DEPENDS ON x",
            rewriter::RewriteCode::UnsupportedStatement, "statement is not supported"},
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), code) << resp.message();
      EXPECT_EQ(resp.message(), message);
    }
    const auto plain = RunTableRef("ALTER TABLE db1.mv MODIFY REFRESH EVERY 1 HOUR", si);
    EXPECT_EQ(plain.code(), rewriter::RewriteCode::Success) << plain.message();
    EXPECT_EQ(plain.sql_after_rewrite(), "ALTER TABLE phys.`db1.mv` (MODIFY REFRESH EVERY 1 HOUR)");
  }
}

// A table ENGINE's arguments are a storage expression: a read there is
// refused in both SI states (under V2 an hg_* name gets the SI message).
TEST(TableRefFidelity, EngineArgumentsAreGoverned) {
  for (const bool si : {false, true}) {
    for (const auto &[sql, code, message] : std::vector<std::tuple<std::string, rewriter::RewriteCode, std::string>>{
           {"CREATE TABLE db1.n (a UInt8) ENGINE = MergeTree((SELECT 1 FROM hg_unsafe.x)) ORDER BY a",
            si ? rewriter::RewriteCode::UnsupportedStatement : rewriter::RewriteCode::InvalidRewriteRequest,
            si ? "storage-integrity physical table hg_unsafe.x is not directly addressable"
               : "protected database hg_unsafe is not addressable"},
           {"CREATE TABLE db1.n (a UInt8) ENGINE = Memory((SELECT 1 FROM hg_safe.db1__t))",
            si ? rewriter::RewriteCode::UnsupportedStatement : rewriter::RewriteCode::InvalidRewriteRequest,
            si ? "storage-integrity physical table hg_safe.db1__t is not directly addressable"
               : "protected database hg_safe is not addressable"},
           {"CREATE TABLE db1.n (a UInt8) ENGINE = MergeTree((SELECT 1 FROM db1.p)) ORDER BY a",
            rewriter::RewriteCode::UnsupportedStatement, "statement is not supported"},
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), code) << resp.message();
      EXPECT_EQ(resp.message(), message);
    }
    const auto plain = RunTableRef("CREATE TABLE db1.n (a UInt8, v Int8) ENGINE = CollapsingMergeTree(v) ORDER BY a", si);
    EXPECT_EQ(plain.code(), rewriter::RewriteCode::Success) << plain.message();
  }
}

// Review I2 / M1: reads inside WINDOW are reported in table_rewrites (the
// unresolved-name check no longer runs on a clone); APPLY's own lookup
// function is T6. A lambda (APPLY's or any other) that holds a query or an
// operator-form IN is refused with T7 (the SI catch-all under V2, which the
// reject annotator upgrades when it can name an SI object), as rewriter-go
// refuses it (Task I3); a lambda without one keeps working.
TEST(TableRefFidelity, WindowAndApplyReadsAreReported) {
  using Map = std::map<std::string, std::string>;
  for (const bool si : {false, true}) {
    const std::string t_safe = si ? "hg_safe.db1__t" : "phys.db1.t";
    for (const std::string sql : {"SELECT * APPLY(x -> x IN (SELECT a FROM db1.t)) FROM db1.o",
                                  "CREATE VIEW db1.w AS SELECT * APPLY(x -> x IN (SELECT a FROM db1.p)) FROM db1.o",
                                  "INSERT INTO db1.q SELECT * APPLY(x -> x IN (SELECT a FROM db1.p)) FROM db1.o",
                                  "SELECT * APPLY(x -> x IN (1, 2)) FROM db1.o", "SELECT * APPLY(x -> x NOT IN (1)) FROM db1.o",
                                  "SELECT * APPLY(x -> (SELECT 1)) FROM db1.o", "SELECT * APPLY(x -> exists(SELECT 1)) FROM db1.o",
                                  "SELECT arrayMap(x -> x IN t, [a]) FROM db1.o",
                                  "SELECT a FROM db1.o WHERE arrayExists(x -> x GLOBAL IN (SELECT 1), [a])",
                                  "SELECT * APPLY(x -> arrayMap(y -> y IN t, [x])) FROM db1.o",
                                  // Task I3 fix round 1 (review I1): a callable IN with a table
                                  // operand inside a lambda is refused like the operator form.
                                  "SELECT * APPLY(x -> in(x, t)) FROM db1.o",
                                  "SELECT arrayMap(y -> in(y, p), [a]) FROM db1.o",
                                  "SELECT arrayMap(y -> globalIn(y, p), [a]) FROM db1.o",
                                  "SELECT * APPLY(x -> notIn(x, db1.p)) FROM db1.o"}) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
      EXPECT_EQ(resp.message(), si ? rewriter_handlers::kStorageIntegrityUnmodelledMessage
                                   : rewriter_handlers::kUnsupportedStatementMessage);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
    for (const std::string sql : {"SELECT * APPLY(x -> x + 1) FROM db1.o", "SELECT * APPLY(x -> in(x, (1, 2))) FROM db1.o",
                                  "SELECT * APPLY(toString) FROM db1.o", "SELECT arrayMap(x -> x + 1, [a]) FROM db1.o",
                                  // A callable IN over a literal inside a lambda stays admitted (matches Go).
                                  "SELECT arrayMap(y -> in(y, (1, 2)), [a]) FROM db1.o"}) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      EXPECT_EQ(RunTableRef(sql, si).code(), rewriter::RewriteCode::Success) << RunTableRef(sql, si).message();
    }
    for (const std::string sql : {"SELECT a FROM db1.o WINDOW w AS (PARTITION BY a IN (SELECT a FROM db1.t))"}) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      const Map got(resp.table_rewrites().begin(), resp.table_rewrites().end());
      if (sql.find("db1.t") != std::string::npos) {
        ASSERT_EQ(got.count("db1.t"), 1u) << resp.sql_after_rewrite();
        EXPECT_EQ(got.at("db1.t"), t_safe);
      } else {
        ASSERT_EQ(got.count("db1.p"), 1u) << resp.sql_after_rewrite();
        EXPECT_EQ(got.at("db1.p"), "phys.db1.p");
      }
    }
    for (const auto &[sql, message] : std::vector<std::pair<std::string, std::string>>{
           {"SELECT * APPLY(dictGet('db2.dict', 'v')) FROM db1.o",
            "dictGet target \"db2.dict\" does not resolve through the caller's databases"},
           {"SELECT * APPLY(joinGet('db2.j', 'v')) FROM db1.o",
            "joinGet target \"db2.j\" does not resolve through the caller's databases"},
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest) << resp.message();
      EXPECT_EQ(resp.message(), message);
    }
    const auto columns = RunTableRef("SELECT COLUMNS('a') APPLY(x -> x IN hg_unsafe.x) FROM db1.o", si);
    EXPECT_EQ(columns.code(), rewriter::RewriteCode::UnsupportedStatement) << columns.message();
    EXPECT_EQ(columns.message(), si ? "storage-integrity physical table hg_unsafe.x is not directly addressable"
                                    : "statement is not supported");
    // Review M3: several windows are not a table's alias column list.
    const auto windows = RunTableRef("SELECT a FROM db1.o WINDOW w AS (PARTITION BY a), w2 AS (w ORDER BY a)", si);
    EXPECT_EQ(windows.code(), rewriter::RewriteCode::Success) << windows.message();
  }
}

// Review M1: SHOW SETTINGS PROFILES is not a settings list; it reaches the
// SHOW handler (the system-table refusal rewriter-go gives it arrives with the
// allowlist, Task I2). A context mapped to a missing remote upstream is an
// unresolved context.
TEST(TableRefFidelity, ShowSettingsProfilesAndMissingRemoteUpstream) {
  // SHOW SETTINGS PROFILES reads system.settings_profiles, outside the
  // system-table allowlist (Task I2): refused in both SI states.
  for (const bool si : {false, true}) {
    const auto resp = RunTableRef("SHOW SETTINGS PROFILES", si);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    EXPECT_EQ(resp.message(), "system table system.settings_profiles is not accessible");
  }
  for (const bool si : {false, true}) {
    rewriter::RewriteSQLRequest req;
    req.set_sql("SELECT * FROM t");
    auto *dyn = AddTableRefOption(req, si);
    (*dyn->mutable_database_map())["db9"] = "phys";
    (*dyn->mutable_logical_database_to_remote_upstream_index())["db9"] = "up1";
    dyn->set_upstream_logical_database_in_context("db9");
    dyn->set_upstream_physical_database_in_context("phys");
    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest) << resp.message();
    EXPECT_EQ(resp.message(), UnresolvedUnqualifiedMessage("t"));
  }
}

// Controller ruling (I1 fix round 1, a deviation from brief rule 2): the
// name-binding settings are query settings, not MergeTree table settings, so
// ALTER … MODIFY / RESET SETTING admits them as rewriter-go v0.15.0 does;
// ClickHouse refuses them there as UNKNOWN_SETTING.
TEST(TableRefSettings, NameBindingSettingsInTableSettingPositionsFollowGo) {
  for (const bool si : {false, true}) {
    for (const std::string sql : {"ALTER TABLE db1.o MODIFY SETTING compatibility = '20.1'",
                                  "ALTER TABLE db1.o MODIFY SETTING enable_global_with_statement = 0",
                                  "ALTER TABLE db1.o RESET SETTING implicit_table_at_top_level"}) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      EXPECT_EQ(RunTableRef(sql, si).code(), rewriter::RewriteCode::Success);
    }
  }
}

// ---------------------------------------------------------------------------
// System-table allowlist, step 1 (housegate spec 2026-09-26 §5 "The system
// database", rewriter-go v0.15.0 #58). Every tenant reaches ClickHouse as one
// shared user, so a caller-input reference to a system table outside the
// allowlist is refused with UnsupportedStatement "system table system.<t> is
// not accessible", in every position and both SI states. Allowed tables keep
// today's behaviour. The rows mirror rewriter-go's native_systemtables_test.go.

namespace {

std::string SystemTableMessage(const std::string &table) {
  return "system table system." + table + " is not accessible";
}

void ExpectSystemTableRefused(const std::string &sql, const std::string &table, bool si,
                              const std::string &context = "db1") {
  SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]") + " ctx=" + context);
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);
  AddTableRefOption(req, si)->set_upstream_logical_database_in_context(context);
  const auto resp = RunDirectRewrite(std::move(req));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
  EXPECT_EQ(resp.message(), SystemTableMessage(table));
  EXPECT_EQ(resp.sql_after_rewrite(), sql);
  EXPECT_EQ(resp.statement_type(), rewriter::STATEMENT_TYPE_UNSPECIFIED);
  EXPECT_TRUE(resp.table_rewrites().empty());
}

void ExpectAllowed(const std::string &sql, bool si, const std::string &want_sql = "") {
  SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
  const auto resp = RunTableRef(sql, si);
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
  if (!want_sql.empty()) EXPECT_EQ(resp.sql_after_rewrite(), want_sql);
}

void ExpectRefusal(const std::string &sql, bool si, rewriter::RewriteCode code, const std::string &message) {
  SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
  const auto resp = RunTableRef(sql, si);
  EXPECT_EQ(resp.code(), code) << resp.message();
  EXPECT_EQ(resp.message(), message);
}

const char *kSIUnmodelled =
  "storage-integrity is configured; statement class is not modelled by the rewriter and cannot be forwarded";

}  // namespace

TEST(TableRefSystemTables, AllowlistIsExact) {
  const std::vector<std::string> allowed = {
    // class (b)
    "aggregate_function_combinators", "azure_queue_settings", "build_options", "codecs", "collations",
    "contributors", "dashboards", "data_type_families", "database_engines", "formats", "functions",
    "jemalloc_bins", "jemalloc_stats", "keywords", "licenses", "merge_tree_settings", "numbers",
    "numbers_mt", "one", "primes", "privileges", "replicated_merge_tree_settings", "s3_queue_settings",
    "settings", "settings_changes", "table_engines", "table_functions", "time_zones", "tokenizers",
    "unicode", "warnings", "zeros", "zeros_mt",
    // clusters (b*, read by the Sentio driver)
    "clusters",
    // class (c), read by the Sentio driver and clients by physical name
    "tables", "columns", "data_skipping_indices", "projections", "parts", "parts_columns",
    "projection_parts", "projection_parts_columns", "databases", "completions", "mutations"};
  ASSERT_EQ(allowed.size(), 45u);
  for (const bool si : {false, true}) {
    for (const auto &table : allowed) {
      const std::string sql = "SELECT * FROM system." + table;
      ExpectAllowed(sql, si, sql);
    }
    // Case-sensitive, exact names (measured on ClickHouse 26.2: system.Tables
    // is UNKNOWN_TABLE), and every other name, a future one included.
    for (const std::string table : {"processes", "merges", "dictionaries", "query_log", "text_log", "errors",
           "part_log", "users", "grants", "asynchronous_metrics", "events", "detached_parts", "rocksdb",
           "iceberg_history", "database_replicas", "dropped_tables", "dropped_tables_parts", "detached_tables",
           "zookeeper", "session_log", "future_table_nobody_knows", "Tables", "TABLES", "PROCESSES"})
      ExpectSystemTableRefused("SELECT * FROM system." + table, table, si);
  }
}

TEST(TableRefSystemTables, AllowedTablesKeepTodaysBehaviour) {
  for (const bool si : {false, true}) {
    for (const std::string sql : {
           "SELECT * FROM information_schema.tables", "SELECT * FROM INFORMATION_SCHEMA.TABLES",
           "SELECT * FROM information_schema.columns WHERE table_schema = 'phys'",
           "SELECT name FROM system.tables WHERE database = 'phys' AND name LIKE 'db1.%'",
           "SELECT * FROM system.mutations WHERE database = 'phys' AND is_done = 0",
           "SELECT * FROM system.parts WHERE database = 'phys' AND active", "SELECT 1", "SELECT * FROM numbers(3)",
           "SHOW SETTINGS LIKE 'max%'", "SHOW FUNCTIONS", "SHOW ENGINES", "SHOW CLUSTERS", "SHOW CLUSTER 'default'",
           "SELECT * FROM db1.o WHERE a IN system.tables", "SELECT * FROM db1.o JOIN system.columns USING (a)",
           "INSERT INTO db1.o SELECT name FROM system.tables", "DESCRIBE “system”.“tables”",
           "DESCRIBE TABLE system.one", "SHOW COLUMNS FROM system.tables", "SHOW COLUMNS FROM tables FROM system"})
      ExpectAllowed(sql, si);
    // A string literal and a SYSTEM database are not the system database.
    ExpectAllowed("SELECT 'system.processes'", si);
    ExpectAllowed("SELECT * FROM db1.o WHERE a = 'system.processes'", si);
  }
  // The harmless SHOW forms the SI surface does not model keep their
  // inactive-surface pass-through; with the surface active the SI catch-all
  // answers, as in rewriter-go.
  for (const std::string sql : {"SHOW SETTING max_threads", "SHOW PRIVILEGES", "SHOW CHANGED SETTINGS LIKE 'max%'"}) {
    ExpectAllowed(sql, false);
    ExpectRefusal(sql, true, rewriter::RewriteCode::UnsupportedStatement, kSIUnmodelled);
  }
}

// The rewriter's own system.tables / system.columns synthesis is engine
// output, not caller input.
TEST(TableRefSystemTables, SynthesizedSQLKeepsWorking) {
  const std::string show_tables =
    "SELECT multiIf(startsWith(name, 'db1.'), substring(name, length('db1.') + 1), name) AS name FROM (SELECT name "
    "FROM system.tables WHERE database = 'phys' AND startsWith(name, 'db1.'))";
  ExpectAllowed("SHOW TABLES", false, show_tables);
  ExpectAllowed("SHOW TABLES", true, show_tables);
  ExpectAllowed("SHOW DATABASES", false);
  const auto describe = RunTableRef("DESCRIBE TABLE db1.t", true);
  EXPECT_EQ(describe.code(), rewriter::RewriteCode::Success) << describe.message();
  EXPECT_NE(describe.sql_after_rewrite().find("FROM system.columns"), std::string::npos) << describe.sql_after_rewrite();
}

TEST(TableRefSystemTables, RefusedEverywhere) {
  const std::vector<std::pair<std::string, std::string>> cases = {
    // quoting and escapes, decoded the way ClickHouse's ParserIdentifier does
    {"SELECT * FROM `system`.`processes`", "processes"},
    {"SELECT * FROM \"system\".\"processes\"", "processes"},
    {"SELECT * FROM system.`\\x70rocesses`", "processes"},
    {"SELECT * FROM `\\x73ystem`.processes", "processes"},
    {"SELECT * FROM `sys\\x74em`.`\\x70rocesses`", "processes"},
    {"SELECT * FROM `system\\N`.processes", "processes"},
    {"SELECT * FROM system.`proc\\Nesses`", "processes"},
    {"SELECT * FROM “system”.“processes”", "processes"},
    {"SELECT * FROM system /* c */ . processes", "processes"},
    // read positions
    {"SELECT * FROM db1.o JOIN system.processes USING (a)", "processes"},
    {"SELECT * FROM db1.o, system.processes", "processes"},
    {"SELECT * FROM system.processes AS p JOIN db1.o USING (a)", "processes"},
    {"SELECT * FROM db1.o WHERE a IN system.processes", "processes"},
    {"SELECT * FROM db1.o WHERE a IN (system.processes)", "processes"},
    {"SELECT * FROM db1.o WHERE a GLOBAL IN system.processes", "processes"},
    {"SELECT * FROM db1.o WHERE in(a, system.processes)", "processes"},
    {"SELECT * FROM db1.o WHERE a NOT IN system.processes", "processes"},
    {"SELECT * FROM db1.o WHERE a IN system.tables.processes", "tables.processes"},
    {"SELECT * FROM db1.o WHERE a IN (SELECT query FROM system.query_log)", "query_log"},
    {"SELECT (SELECT count() FROM system.merges)", "merges"},
    {"SELECT * FROM (SELECT * FROM system.processes)", "processes"},
    {"WITH x AS (SELECT * FROM system.processes) SELECT * FROM x", "processes"},
    {"SELECT * FROM db1.o UNION ALL SELECT * FROM system.processes", "processes"},
    {"SELECT * FROM db1.o WHERE EXISTS (SELECT 1 FROM system.processes)", "processes"},
    {"SELECT * FROM db1.o ARRAY JOIN (SELECT groupArray(1) FROM system.processes) AS x", "processes"},
    {"SELECT * FROM view(SELECT * FROM system.processes)", "processes"},
    {"INSERT INTO db1.o SELECT query FROM system.query_log", "query_log"},
    {"INSERT INTO db1.o (a) SELECT query FROM system.query_log", "query_log"},
    {"INSERT INTO db1.o VALUES ((SELECT 1 FROM system.processes))", "processes"},
    {"INSERT INTO db1.o VALUES (1), ((SELECT count() FROM system.merges))", "merges"},
    {"CREATE TABLE db1.n ENGINE = Memory AS SELECT * FROM system.processes", "processes"},
    {"CREATE VIEW db1.v AS SELECT query FROM system.query_log", "query_log"},
    {"CREATE MATERIALIZED VIEW db1.mv TO db1.o AS SELECT query FROM system.query_log", "query_log"},
    {"CREATE MATERIALIZED VIEW db1.mv ENGINE = Memory AS SELECT query FROM system.query_log", "query_log"},
    // write and DDL targets
    {"CREATE TABLE db1.n AS system.processes", "processes"},
    {"INSERT INTO system.query_log SELECT * FROM db1.o", "query_log"},
    {"DROP TABLE system.query_log", "query_log"},
    {"TRUNCATE TABLE system.query_log", "query_log"},
    {"RENAME TABLE system.query_log TO db1.x", "query_log"},
    {"RENAME TABLE “system”.“query_log” TO db1.x", "query_log"},
    {"CREATE MATERIALIZED VIEW db1.mv TO system.query_log AS SELECT * FROM db1.o", "query_log"},
    // column, mutation and ALTER-action expressions
    {"DELETE FROM db1.o WHERE a IN (SELECT 1 FROM system.processes)", "processes"},
    {"ALTER TABLE db1.o DELETE WHERE a IN (SELECT 1 FROM system.processes)", "processes"},
    {"ALTER TABLE db1.o UPDATE a = 1 WHERE a IN (SELECT 1 FROM system.processes)", "processes"},
    {"CREATE TABLE db1.n (a UInt64 DEFAULT (SELECT count() FROM system.processes)) ENGINE = Memory", "processes"},
    {"CREATE VIEW db1.v (a UInt8 DEFAULT (SELECT count() FROM system.processes)) AS SELECT 1 AS a", "processes"},
    {"CREATE VIEW db1.v (a UInt8, INDEX i a IN system.processes TYPE minmax) AS SELECT 1 AS a", "processes"},
    // DESCRIBE / EXISTS / SHOW CREATE / SHOW COLUMNS family
    {"DESCRIBE system.processes", "processes"},
    {"DESCRIBE TABLE `system`.processes", "processes"},
    {"DESC `system`.`processes`", "processes"},
    {"DESCRIBE “system”.“processes”", "processes"},
    {"DESCRIBE TABLE “system”.“processes” SETTINGS describe_include_subcolumns = 1", "processes"},
    {"EXISTS system.processes", "processes"},
    {"EXISTS TABLE `system`.processes", "processes"},
    {"EXISTS TABLE “system”.“processes”", "processes"},
    {"SHOW CREATE TABLE system.processes", "processes"},
    {"SHOW CREATE TABLE `system`.processes", "processes"},
    {"SHOW CREATE TABLE “system”.“processes”", "processes"},
    {"SHOW CREATE DICTIONARY system.d", "d"},
    {"SHOW COLUMNS FROM system.processes", "processes"},
    {"SHOW COLUMNS FROM `system`.`processes`", "processes"},
    {"SHOW COLUMNS FROM `system\\N`.processes", "processes"},
    {"SHOW COLUMNS FROM `system\\N`.`\\x70rocesses`", "processes"},
    {"SHOW COLUMNS FROM “system”.“processes”", "processes"},
    {"SHOW COLUMNS FROM processes FROM system", "processes"},
    {"SHOW COLUMNS FROM processes IN `system\\N`", "processes"},
    {"SHOW FULL COLUMNS FROM system.processes", "processes"},
    {"SHOW INDEX FROM system.processes", "processes"},
    {"SHOW KEYS FROM system.processes", "processes"},
    {"GRANT SELECT ON system.processes TO u", "processes"},
    {"GRANT SELECT ON system.* TO u", "*"},
    // table functions, table engines and string lookups naming system
    {"SELECT * FROM merge('system', '^processes$')", "^processes$"},
    {"SELECT * FROM merge('system', '^tables$')", "^tables$"},
    {"SELECT * FROM remote('127.0.0.1', system.processes)", "processes"},
    {"SELECT * FROM remote('127.0.0.1', 'system', 'processes')", "processes"},
    {"SELECT * FROM remoteSecure('127.0.0.1', system.processes)", "processes"},
    {"SELECT * FROM cluster('default', system.processes)", "processes"},
    {"SELECT * FROM clusterAllReplicas('default', system, processes)", "processes"},
    {"SELECT * FROM loop(system, processes)", "processes"},
    {"SELECT * FROM mergeTreeIndex('system', 'processes')", "processes"},
    {"CREATE TABLE db1.n (a UInt64) ENGINE = Merge('system', '^proc')", "^proc"},
    {"CREATE TABLE db1.n (a UInt64) ENGINE = Distributed(default, system, processes)", "processes"},
    {"CREATE TABLE db1.n (a UInt64) ENGINE = Buffer(system, processes, 1, 1, 1, 1, 1, 1, 1)", "processes"},
    {"SELECT joinGet('system.processes', 'a', 1)", "processes"},
    {"SELECT dictGet('system.d', 'a', toUInt64(1))", "d"},
    {"SELECT hasColumnInTable('system', 'processes', 'query')", "processes"},
  };
  for (const bool si : {false, true})
    for (const auto &[sql, table] : cases) ExpectSystemTableRefused(sql, table, si);
}

// Every AST member ClickHouse keeps outside `children` that Task I1 adopted
// (WINDOW definitions, APPLY lambdas, TTL GROUP BY … SET, RECOMPRESS CODEC,
// MODIFY REFRESH … DEPENDS ON) and ENGINE arguments are positions too.
TEST(TableRefSystemTables, OutOfChildrenMembersAreRead) {
  const std::vector<std::string> cases = {
    "SELECT a FROM db1.o WINDOW w AS (PARTITION BY (SELECT 1 FROM system.processes))",
    "CREATE TABLE db1.n (a UInt64, d DateTime, b UInt64) ENGINE = MergeTree ORDER BY a TTL d GROUP BY a SET b = "
    "max((SELECT 1 FROM system.processes))",
    "ALTER TABLE db1.o MODIFY TTL d GROUP BY a SET b = max((SELECT 1 FROM system.processes))",
    "CREATE TABLE db1.n (a UInt64, d DateTime) ENGINE = MergeTree ORDER BY a TTL d RECOMPRESS CODEC(ZSTD((SELECT 1 "
    "FROM system.processes)))",
    "ALTER TABLE db1.o MODIFY REFRESH EVERY 1 HOUR DEPENDS ON system.processes",
    "CREATE TABLE db1.n (a UInt64) ENGINE = MergeTree((SELECT 1 FROM system.processes)) ORDER BY a",
  };
  for (const bool si : {false, true})
    for (const auto &sql : cases) ExpectSystemTableRefused(sql, "processes", si);
  // An APPLY lambda holding a query is refused before any name is examined
  // (Task I3, as rewriter-go).
  for (const bool si : {false, true}) {
    const auto resp = RunTableRef("SELECT * APPLY(x -> (SELECT 1 FROM system.processes)) FROM db1.o", si);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
    EXPECT_EQ(resp.message(), si ? rewriter_handlers::kStorageIntegrityUnmodelledMessage
                                 : rewriter_handlers::kUnsupportedStatementMessage);
  }
}

// An unqualified name resolves in the session's logical database; when that
// is `system`, the reference is a system table.
TEST(TableRefSystemTables, UnqualifiedInSystemContext) {
  for (const bool si : {false, true}) {
    for (const std::string sql : {"SELECT * FROM processes", "SELECT * FROM db1.o WHERE a IN processes",
           "DESCRIBE processes", "SHOW COLUMNS FROM processes", "EXISTS TABLE processes",
           "SHOW CREATE TABLE processes", "INSERT INTO processes SELECT 1", "DROP TABLE processes"})
      ExpectSystemTableRefused(sql, "processes", si, "system");
    // A one-part quoted name is one table of the session database.
    ExpectSystemTableRefused("SELECT * FROM `system.processes`", "system.processes", si, "system");
    // An allowed table keeps today's refusal: `system` is not a mapped
    // logical database.
    rewriter::RewriteSQLRequest req;
    req.set_sql("SELECT * FROM tables");
    AddTableRefOption(req, si)->set_upstream_logical_database_in_context("system");
    const auto resp = RunDirectRewrite(std::move(req));
    EXPECT_EQ(resp.message(), UnresolvedUnqualifiedMessage("tables"));
  }
  // With another session database a one-part quoted name is not the system
  // database.
  for (const bool si : {false, true}) {
    const auto resp = RunTableRef("SELECT * FROM `system.processes`", si);
    EXPECT_NE(resp.message(), SystemTableMessage("processes")) << resp.message();
  }
}

TEST(TableRefSystemTables, RefusalReportsTheTable) {
  for (const bool si : {false, true}) {
    const auto resp = RunTableRef("SELECT * FROM db1.o JOIN system.processes USING (a)", si);
    ASSERT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
    bool found = false;
    for (const auto &a : resp.original_accessed_tables())
      if (a.original_database() == "system" && a.original_table() == "processes") found = true;
    EXPECT_TRUE(found);
    EXPECT_TRUE(resp.table_rewrites().empty());
    EXPECT_TRUE(resp.database_rewrites().empty());
  }
}

// SHOW forms that read a refused system table (the SHOW → system mapping
// measured on ClickHouse 26.2 / 25.8), after the form's SI namespace check
// and its body check.
TEST(TableRefSystemTables, ShowForms) {
  const std::vector<std::pair<std::string, std::string>> both = {
    {"SHOW PROCESSLIST", "processes"},
    {"SHOW PROCESSLIST FORMAT JSON", "processes"},
    {"SHOW MERGES", "merges"},
    {"SHOW MERGES LIKE 'x%'", "merges"},
    {"SHOW DICTIONARIES FROM db2", "dictionaries"},
    {"SHOW FULL DICTIONARIES FROM db2", "dictionaries"},
    {"SHOW DICTIONARIES FROM system", "dictionaries"},
    {"SHOW DICTIONARIES FROM nosuchdb LIKE '%'", "dictionaries"},
    {"SHOW USERS", "users"},
    {"SHOW ROLES", "roles"},
    {"SHOW GRANTS", "grants"},
    {"SHOW GRANTS FOR default", "grants"},
    {"SHOW ACCESS", "users"},
    {"SHOW PROFILES", "settings_profiles"},
    {"SHOW SETTINGS PROFILES", "settings_profiles"},
    {"SHOW ROW POLICIES", "row_policies"},
    {"SHOW POLICIES", "row_policies"},
    {"SHOW ROW POLICIES ON db1.o", "row_policies"},
    {"SHOW QUOTA", "quota_usage"},
    {"SHOW QUOTAS", "quotas"},
    {"SHOW FILESYSTEM CACHES", "filesystem_cache_settings"},
  };
  for (const bool si : {false, true}) {
    for (const auto &[sql, table] : both) {
      ExpectSystemTableRefused(sql, table, si);
      const auto resp = RunTableRef(sql, si);
      EXPECT_TRUE(resp.original_accessed_tables().empty()) << sql;
    }
  }
  // Without FROM, SHOW DICTIONARIES runs in the session database; with the
  // SI surface active and db1 owning an Active table, the SI namespace
  // message keeps precedence.
  for (const std::string sql : {"SHOW DICTIONARIES", "SHOW DICTIONARIES LIKE '%'", "SHOW DICTIONARIES FROM db1"}) {
    ExpectSystemTableRefused(sql, "dictionaries", false);
    ExpectRefusal(sql, true, rewriter::RewriteCode::UnsupportedStatement,
                  "storage-integrity logical database db1 is not directly addressable");
  }
  // SHOW kinds the SI surface does not model keep the SI catch-all under V2.
  for (const auto &[sql, table] : std::vector<std::pair<std::string, std::string>>{
         {"SHOW CURRENT ROLES", "current_roles"}, {"SHOW ENABLED ROLES", "enabled_roles"},
         {"SHOW CURRENT QUOTA", "quota_usage"}}) {
    ExpectSystemTableRefused(sql, table, false);
    ExpectRefusal(sql, true, rewriter::RewriteCode::UnsupportedStatement, kSIUnmodelled);
  }
  // SHOW CREATE of an access entity reads access storage: refused, as in
  // rewriter-go, in both SI states.
  for (const bool si : {false, true}) {
    for (const std::string sql : {"SHOW CREATE USER default", "SHOW CREATE ROLE r", "SHOW CREATE QUOTA q",
           "SHOW CREATE ROW POLICY p ON db1.o", "SHOW CREATE SETTINGS PROFILE p", "SHOW CREATE PROFILE p",
           "SHOW CREATE USERS", "SHOW CREATE QUOTA"})
      ExpectRefusal(sql, si, rewriter::RewriteCode::UnsupportedStatement, "statement is not supported");
    ExpectSystemTableRefused("SHOW CREATE ROW POLICY p ON system.processes", "processes", si);
    // DESCRIBE FILESYSTEM CACHE reads the cache registry
    // (system.filesystem_cache_settings); rewriter-go refuses it.
    ExpectRefusal("DESCRIBE FILESYSTEM CACHE 'x'", si, rewriter::RewriteCode::UnsupportedStatement,
                  "statement is not supported");
  }
}

// ClickHouse's SHOW COLUMNS / INDEX read a target of three or more parts as
// (first part, last part) (measured on 26.2 and 25.8). The scan checks that
// reading and the (first, second) one, and a multi-part target left is
// refused fail-closed.
TEST(TableRefSystemTables, MultiPartShowTargets) {
  const std::vector<std::pair<std::string, std::string>> refused = {
    {"SHOW COLUMNS FROM system.tables.processes", "processes"},
    {"SHOW FULL COLUMNS FROM system.tables.query_log", "query_log"},
    {"SHOW EXTENDED COLUMNS FROM system.tables.processes", "processes"},
    {"SHOW COLUMNS FROM system.one.processes", "processes"},
    {"SHOW COLUMNS FROM `system`.`tables`.`processes`", "processes"},
    {"SHOW COLUMNS FROM system.tables.x.processes", "processes"},
    {"SHOW INDEX FROM system.tables.processes", "processes"},
    {"SHOW INDEXES IN system.one.processes", "processes"},
    {"SHOW KEYS FROM system.columns.processes", "processes"},
    {"SHOW COLUMNS FROM system.processes.tables", "processes"},
  };
  for (const bool si : {false, true}) {
    for (const auto &[sql, table] : refused) ExpectSystemTableRefused(sql, table, si);
    for (const std::string sql : {"SHOW COLUMNS FROM system.one.tables", "SHOW COLUMNS FROM db2.x.y",
           "SHOW INDEX FROM default.x.y", "SHOW COLUMNS FROM a.b.c.d", "SHOW COLUMNS FROM db2 /* c */ . x . y"})
      ExpectRefusal(sql, si, rewriter::RewriteCode::UnsupportedStatement, "statement is not supported");
    ExpectAllowed("SHOW COLUMNS FROM db2.x", si, "SHOW COLUMNS FROM db2.x");
  }
  ExpectRefusal("SHOW COLUMNS FROM db1.x.y", false, rewriter::RewriteCode::UnsupportedStatement,
                "statement is not supported");
  ExpectRefusal("SHOW COLUMNS FROM db1.x.y", true, rewriter::RewriteCode::UnsupportedStatement,
                "storage-integrity logical database db1 is not directly addressable");
}

// Where the check sits: after T2, T7 and T3, before T5 / T6 / R5 / R2 with
// the SI surface inactive; with it active and an SI object named, after the
// SI pipeline, on its would-be Success.
TEST(TableRefSystemTables, Precedence) {
  const auto invalid = rewriter::RewriteCode::InvalidRewriteRequest;
  const auto unsupported = rewriter::RewriteCode::UnsupportedStatement;
  ExpectRefusal("SELECT * FROM {p:Identifier} JOIN system.processes USING (a)", false, invalid,
                "query parameters are not supported in a database or table position");
  ExpectRefusal("EXPLAIN SELECT * FROM system.processes", false, unsupported, "statement is not supported");
  ExpectRefusal("CHECK TABLE system.processes", false, unsupported, "statement is not supported");
  ExpectRefusal("EXPLAIN SELECT * FROM system.processes", true, unsupported, kSIUnmodelled);
  ExpectRefusal("SELECT * FROM system.processes JOIN phys.x USING (a)", false, invalid,
                "protected database phys is not addressable");
  ExpectSystemTableRefused("SELECT * FROM system.processes JOIN merge('db1', 'o') USING (a)", "processes", false);
  // merge('db1', …) names the SI logical database db1 through a pattern
  // carrier: an SI object, so under V2 the SI pipeline answers first.
  ExpectRefusal("SELECT * FROM system.processes JOIN merge('db1', 'o') USING (a)", true,
                rewriter::RewriteCode::RewriteError,
                "storage-integrity logical database db1 is not directly addressable through merge table function");
  for (const bool si : {false, true}) {
    ExpectSystemTableRefused("SELECT joinGet('db1.o', 'a', 1) FROM system.processes", "processes", si);
    ExpectSystemTableRefused("SELECT * FROM `other.x` JOIN system.processes USING (a)", "processes", si);
    ExpectSystemTableRefused("SELECT * FROM system.processes SETTINGS additional_result_filter = '1'", "processes",
                             si);
    ExpectSystemTableRefused("DELETE FROM db1.o WHERE a IN (SELECT 1 FROM system.processes) AND b IN db2.x",
                             "processes", si);
  }
  // R5 outranks a SHOW form's system-table refusal in both SI states.
  for (const bool si : {false, true}) {
    for (const std::string sql : {"SHOW SETTINGS PROFILES SETTINGS `\\Nimplicit_table_at_top_level` = 'x'",
           "SHOW PROCESSLIST SETTINGS implicit_table_at_top_level = 'x'",
           "SHOW MERGES SETTINGS implicit_table_at_top_level = 'x'",
           "SHOW DICTIONARIES FROM db2 SETTINGS implicit_table_at_top_level = 'x'"})
      ExpectRefusal(sql, si, unsupported, "table setting implicit_table_at_top_level is not accepted");
    for (const std::string sql : {"SHOW GRANTS SETTINGS additional_result_filter = '1'",
           "SHOW USERS SETTINGS additional_result_filter = '1'",
           "SHOW CREATE USER u SETTINGS additional_result_filter = '1'"})
      ExpectRefusal(sql, si, unsupported, "table setting additional_result_filter is not accepted");
  }
  // SI active: the SI pipeline answers first.
  ExpectRefusal("SELECT * FROM hg_safe.db1__t JOIN system.processes USING (a)", true,
                rewriter::RewriteCode::RewriteError,
                "storage-integrity physical table hg_safe.db1__t is not directly addressable");
  ExpectRefusal("SELECT * FROM hg_safe.db1__t JOIN system.processes USING (a)", false, invalid,
                "protected database hg_safe is not addressable");
  ExpectRefusal("CREATE VIEW db1.v AS SELECT * FROM db1.t JOIN system.processes USING (a)", true, unsupported,
                "storage-integrity table db1.t accepts writes only through the signed statement lane");
  ExpectSystemTableRefused("SELECT * FROM db1.t JOIN system.processes USING (a)", "processes", true);
  ExpectSystemTableRefused("CREATE VIEW db1.v AS SELECT * FROM db1.t JOIN system.processes USING (a)", "processes",
                           false);
  // The deferral: every refusal the rest of the SI pipeline issues first
  // outranks the system-table message (rewriter-go
  // TestSystemTables_DeferredPrecedenceUnderSI).
  const std::vector<std::tuple<std::string, rewriter::RewriteCode, std::string>> deferred = {
    {"SELECT joinGet('db1.o', 'a', 1) FROM db1.t JOIN system.processes USING (a)", invalid,
     "joinGet target \"db1.o\" does not resolve through the caller's databases"},
    {"SELECT * FROM db1.t JOIN system.processes USING (a) JOIN mysql('h:1', 'd', 't', 'u', 'p') USING (a)",
     unsupported, "table function mysql is not accepted"},
    {"SELECT * FROM db1.t JOIN system.processes USING (a) SETTINGS additional_result_filter = '1'", unsupported,
     "table setting additional_result_filter is not accepted"},
  };
  for (const auto &[sql, code, message] : deferred) {
    ExpectSystemTableRefused(sql, "processes", false);
    ExpectRefusal(sql, true, code, message);
  }
  // The deferred refusal keeps the SI pipeline's accessed tables and adds the
  // system table.
  const auto resp = RunTableRef("SELECT * FROM db1.t JOIN system.processes USING (a)", true);
  bool system_found = false, si_found = false;
  for (const auto &a : resp.original_accessed_tables()) {
    if (a.original_database() == "system" && a.original_table() == "processes") system_found = true;
    if (a.is_storage_integrity()) si_found = true;
  }
  EXPECT_TRUE(system_found);
  EXPECT_TRUE(si_found);
  EXPECT_TRUE(resp.table_rewrites().empty());
}

// Names are compared after ClickHouse's identifier decoding, for every quoting
// ClickHouse's lexer accepts; a name that still carries a backslash is
// decoded again (rewriter-go ClickHouseIdentifierValue: it errs toward
// refusing). A ‘…’ lexeme is a string and `SYSTEM` is not the system
// database.
TEST(TableRefSystemTables, EscapesAndQuoting) {
  const std::vector<std::pair<std::string, std::string>> refused = {
    {"SELECT * FROM `system\\\\N`.processes", "processes"},
    {"SELECT dictGet('system\\\\N.d', 'a', 1)", "d"},
    {"SELECT dictGet('system\\N.d', 'a', 1)", "d"},
    {"SELECT hasColumnInTable('system\\\\N', 'processes', 'x')", "processes"},
    {"SELECT * FROM db1.o WHERE a IN `system\\\\N`.processes", "processes"},
    {"SELECT dictGet('`system`.d', 'a', 1)", "d"},
    {"SELECT hasColumnInTable('h', 'system', 'processes', 'x')", "processes"},
    {"SELECT hasColumnInTable('h', 'u', 'p', 'system', 'processes', 'x')", "processes"},
    // A three-part command-text run is read as (first, last) before (first,
    // second): both are refused here, and the (first, last) one names it.
    {"SHOW COLUMNS FROM system.merges.processes", "processes"},
    // C++ also reads the DESCRIBE target from the AST, whose name ClickHouse
    // already decoded once, so the second decode refuses it; rewriter-go
    // reads DESCRIBE only as command text, decoded once, and admits it.
    {"DESCRIBE `system\\\\N`.processes", "processes"},
  };
  for (const bool si : {false, true}) {
    for (const auto &[sql, table] : refused) ExpectSystemTableRefused(sql, table, si);
    for (const std::string sql : {"SELECT * FROM SYSTEM.processes", "SELECT * FROM `sys``tem`.processes",
           "SELECT hasColumnInTable('SYSTEM', 'processes', 'x')", "SELECT system.processes FROM db1.o",
           "SELECT * FROM db1.o AS system WHERE system.processes = 1"}) {
      SCOPED_TRACE(sql);
      EXPECT_NE(RunTableRef(sql, si).message(), SystemTableMessage("processes"));
    }
  }
}

// Which refused table a statement with several is named after: rewriter-go's
// collector order (write targets, read sources in walker order, opaque
// texts, string lookups), not strict document order.
TEST(TableRefSystemTables, CollectorOrder) {
  const std::vector<std::pair<std::string, std::string>> cases = {
    {"SELECT dictGet('system.aaa', 'x', 1) FROM system.processes", "processes"},
    {"SELECT hasColumnInTable('system', 'aaa', 'x') FROM system.processes", "processes"},
    {"SELECT joinGet('system.aaa', 'x', 1) FROM db1.o WHERE a IN (SELECT 1 FROM system.merges)", "merges"},
    {"INSERT INTO system.query_log SELECT * FROM system.processes", "query_log"},
    {"SELECT * FROM system.merges WHERE a IN (SELECT 1 FROM system.processes)", "merges"},
    {"WITH x AS (SELECT * FROM system.processes) SELECT * FROM system.merges, x", "processes"},
    {"SELECT (SELECT 1 FROM system.processes) FROM system.merges", "processes"},
    {"SELECT * FROM system.merges JOIN system.processes USING (a)", "merges"},
    {"SELECT * FROM system.tables JOIN system.processes USING (a)", "processes"},
    {"RENAME TABLE system.query_log TO system.x", "query_log"},
    {"ALTER TABLE db1.o UPDATE a = (SELECT 1 FROM system.merges) WHERE a IN (SELECT 1 FROM system.processes)",
     "merges"},
  };
  for (const bool si : {false, true})
    for (const auto &[sql, table] : cases) ExpectSystemTableRefused(sql, table, si);
}

// ============================================================================
// Interstitial I3: name binding of an unqualified IN operand (rewriter-go
// v0.15.0 readSourceScope; spec 2026-09-26 §5 "Names that bind an unqualified
// IN operand", "Stored view bodies", "No WITH binding in an R2 position").
// ============================================================================

namespace {

// An alias spelling: how the SQL writes it and how the engine formats it.
struct AliasInName { std::string decl, gen; };
const std::vector<AliasInName> kAliasInNames = {
  {"\"other.secret\"", "`other.secret`"}, {"`db2.x`", "`db2.x`"}, {"t", "t"}};
const std::vector<std::string> kAliasInPredicates = {
  "a IN {R}", "a IN ({R})", "a NOT IN {R}", "a GLOBAL IN {R}", "(a, a) IN {R}", "in(a, {R})", "notIn(a, {R})",
  "globalIn(a, {R})"};
constexpr const char *kAliasInControl = "zz_alias_control";

struct AliasInContext { std::string name, logical; const char *physical; };
const std::vector<AliasInContext> kAliasInContexts = {
  {"mapped", "db1", "phys"}, {"unmapped", "db9", "phys"}, {"empty", "", "phys"},
  {"mapped without physical context", "db1", nullptr}};

std::string replaceAll(std::string s, const std::string &from, const std::string &to) {
  for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
  return s;
}

std::vector<std::string> accessedKeys(const rewriter::RewriteSQLResponse &resp) {
  std::vector<std::string> out;
  for (const auto &a : resp.original_accessed_tables())
    out.push_back(a.original_database() + "." + a.original_table() + "@" + a.logical_database() + "/"
                  + a.physical_database());
  return out;
}

// rewriter-go runAliasInMetamorphic: declaring {N} (as a table alias, a
// projection alias of an enclosing SELECT, or a WITH alias that does not
// bind) changes nothing about how an IN operand named N is handled — the
// answer equals the answer with the declaration renamed.
void ExpectAliasInMetamorphic(const AliasInContext &ctx, bool si, const std::string &decl,
                              const AliasInName &name, const std::string &pred) {
  const std::string p = replaceAll(pred, "{R}", name.decl);
  const std::string sql = replaceAll(replaceAll(decl, "{P}", p), "{N}", name.decl);
  const std::string control = replaceAll(replaceAll(decl, "{P}", p), "{N}", kAliasInControl);
  SCOPED_TRACE(ctx.name + (si ? "/si v2/" : "/si off/") + sql);
  const auto got = RunTableRefContext(sql, si, ctx.logical, ctx.physical);
  const auto want = RunTableRefContext(control, si, ctx.logical, ctx.physical);
  ASSERT_EQ(got.code(), want.code()) << got.message() << " | want " << want.message();
  EXPECT_EQ(got.message(), want.message());
  if (want.code() == rewriter::RewriteCode::Success)
    EXPECT_EQ(got.sql_after_rewrite(), replaceAll(want.sql_after_rewrite(), kAliasInControl, name.gen));
  else
    EXPECT_EQ(got.sql_after_rewrite(), sql);
  EXPECT_EQ(accessedKeys(got), accessedKeys(want));
  std::map<std::string, std::string> g(got.table_rewrites().begin(), got.table_rewrites().end());
  std::map<std::string, std::string> w(want.table_rewrites().begin(), want.table_rewrites().end());
  EXPECT_EQ(g, w);
}

// A name that binds: the operand stays as written and is neither rewritten
// nor reported.
void ExpectOperandBinds(const std::string &sql, bool si, const std::string &operand,
                        const std::string &logical = "db1", const char *physical = "phys") {
  SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]") + " ctx=" + logical);
  const auto resp = RunTableRefContext(sql, si, logical, physical);
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
  for (const auto &a : resp.original_accessed_tables())
    EXPECT_FALSE(a.original_database().empty() && a.original_table() == operand) << resp.sql_after_rewrite();
  EXPECT_EQ(resp.table_rewrites().count(operand), 0u) << resp.sql_after_rewrite();
  EXPECT_EQ(resp.sql_after_rewrite().find("phys.`db1." + operand + "`"), std::string::npos) << resp.sql_after_rewrite();
  EXPECT_EQ(resp.sql_after_rewrite().find("hg_safe.db1__" + operand), std::string::npos) << resp.sql_after_rewrite();
}

// A name that does not bind: the operand is the table of the session's
// logical database, rewritten and reported.
void ExpectOperandIsTable(const std::string &sql, bool si, const std::string &operand) {
  SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
  const auto resp = RunTableRef(sql, si);
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
  const std::string read = si && operand == "t" ? "(SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t)"
                                                : "phys.`db1." + operand + "`";
  EXPECT_NE(resp.sql_after_rewrite().find(read), std::string::npos) << resp.sql_after_rewrite();
  bool reported = false;
  for (const auto &a : resp.original_accessed_tables())
    reported = reported || (a.original_database().empty() && a.original_table() == operand);
  EXPECT_TRUE(reported) << resp.sql_after_rewrite();
}

} // namespace

TEST(TableRefBinding, TableAliasIsNotAnInOperand) {
  const std::vector<std::string> decls = {
    "SELECT * FROM db1.o AS {N} WHERE a IN (SELECT a FROM db1.p WHERE {P})",
    "SELECT * FROM db1.o AS {N} WHERE a IN (SELECT a FROM db1.p WHERE a IN (SELECT a FROM db1.p WHERE {P}))",
    "SELECT * FROM db1.o AS {N} WHERE {P}",
    "SELECT {P} FROM db1.o AS {N}",
    "SELECT * FROM db1.o WHERE a IN (SELECT a FROM db1.p AS {N} WHERE {P})",
    "SELECT * FROM db1.o JOIN db1.p AS {N} USING (a) WHERE a IN (SELECT a FROM db1.p WHERE {P})",
    "SELECT * FROM (SELECT a FROM db1.o) AS {N} WHERE a IN (SELECT a FROM db1.p WHERE {P})",
    "SELECT * FROM numbers(3) AS {N} WHERE number IN (SELECT a FROM db1.p WHERE {P})",
    "SELECT (SELECT count() FROM db1.p WHERE {P}) FROM db1.o AS {N}",
    "SELECT * FROM db1.o AS {N} JOIN (SELECT a FROM db1.p WHERE {P}) AS j USING (a)",
    "SELECT a FROM db1.o AS {N} WHERE a IN (SELECT a FROM db1.p WHERE {P} UNION ALL SELECT 1)",
    "SELECT * FROM db1.o AS {N} WHERE a IN (WITH w AS (SELECT a FROM db1.p WHERE {P}) SELECT a FROM w)",
    "SELECT a, 1 AS {N} FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE {P})",
    "INSERT INTO db1.q SELECT * FROM db1.o AS {N} WHERE a IN (SELECT a FROM db1.p WHERE {P})",
    "CREATE VIEW db1.v AS SELECT * FROM db1.o AS {N} WHERE a IN (SELECT a FROM db1.p WHERE {P})",
    "CREATE TABLE db1.n ENGINE = Memory AS SELECT * FROM db1.o AS {N} WHERE a IN (SELECT a FROM db1.p WHERE {P})",
  };
  for (const auto &ctx : kAliasInContexts) for (const bool si : {false, true})
    for (const auto &decl : decls) for (const auto &name : kAliasInNames) for (const auto &pred : kAliasInPredicates)
      ExpectAliasInMetamorphic(ctx, si, decl, name, pred);
}

// A WITH alias whose value is an identifier (bare, quoted, the quoted twin of
// the Active table, parenthesised, qualified, dotted, unresolvable), or a
// chain of aliases, binds nothing: ClickHouse reads a table through it.
TEST(TableRefBinding, IdentifierWithAliasIsNotAnInBinding) {
  const std::vector<std::pair<std::string, std::string>> decls = {
    {"same", "WITH {V} AS {N} SELECT * FROM db1.o WHERE {P}"},
    {"enclosing", "WITH {V} AS {N} SELECT * FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE {P})"},
    {"two up", "WITH {V} AS {N} SELECT * FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE a IN (SELECT a FROM db1.p WHERE {P}))"},
    {"subquery", "SELECT * FROM db1.o WHERE a IN (WITH {V} AS {N} SELECT a FROM db1.p WHERE {P})"},
    {"table alias", "WITH {V} AS {N} SELECT * FROM db1.o AS {N} WHERE {P}"},
    {"chain", "WITH {V} AS w, w AS {N} SELECT * FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE {P})"},
  };
  const std::vector<std::string> values = {"secret", "\"other.secret\"", "`db1.t`", "(secret)", "phys.secret", "db2.x",
                                           "nosuch"};
  for (const auto &ctx : kAliasInContexts) for (const bool si : {false, true})
    for (const auto &[kind, decl] : decls) {
      const auto vs = kind == "chain" ? std::vector<std::string>{"secret", "1"} : values;
      for (const auto &v : vs) for (const auto &name : kAliasInNames) for (const auto &pred : kAliasInPredicates)
        ExpectAliasInMetamorphic(ctx, si, replaceAll(decl, "{V}", v), name, pred);
    }
}

// A no-parens keyword (an identifier to ClickHouse) binds nothing, in any
// case and parenthesised.
TEST(TableRefBinding, KeywordWithAliasIsNotAnInBinding) {
  const std::vector<std::string> decls = {
    "WITH {V} AS {N} SELECT * FROM db1.o WHERE {P}",
    "WITH {V} AS {N} SELECT * FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE {P})",
    "WITH {V} AS {N} SELECT * FROM db1.o AS {N} WHERE {P}",
  };
  const std::vector<std::string> keywords = {"CURDATE", "CURRENT_CATALOG", "CURRENT_DATE", "CURRENT_DATETIME",
    "CURRENT_ROLE", "CURRENT_SCHEMA", "CURRENT_TIME", "CURRENT_USER", "GETDATE", "LOCALTIME", "LOCALTIMESTAMP", "NOW",
    "PI", "SESSION_USER", "SYSDATE", "SYSDATETIME", "SYSTEM_USER", "SYSTIMESTAMP", "UTC_DATE", "UTC_TIME",
    "UTC_TIMESTAMP"};
  for (const auto &ctx : {kAliasInContexts[0], kAliasInContexts[2]}) for (const bool si : {false, true})
    for (const auto &decl : decls) for (const auto &kw : keywords) {
      std::string lower = kw;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
      for (const auto &v : {kw, lower, "(" + kw + ")", "((" + lower + "))"})
        for (const auto &name : kAliasInNames) for (const std::string pred : {"a IN {R}", "a NOT IN ({R})", "globalIn(a, {R})"})
          ExpectAliasInMetamorphic(ctx, si, replaceAll(decl, "{V}", v), name, pred);
    }
}

// In a stored view body a WITH alias binds only a single folded literal and
// no projection alias binds. `((1, 2))` is left out: ClickHouse 26.3's parser
// (the one the engine links) folds it into the flat tuple literal `(1, 2)`,
// which the rewrite re-emits and which binds on the server (PARITY ledger;
// rewriter-go regenerates the doubled parentheses, which do not).
TEST(TableRefBinding, StoredViewAliasIsNotAnInBinding) {
  const std::vector<std::pair<std::string, std::string>> decls = {
    {"view", "CREATE VIEW db1.v AS WITH {V} AS {N} SELECT a FROM db1.o WHERE {P}"},
    {"view", "CREATE VIEW db1.v AS WITH {V} AS {N} SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE {P})"},
    {"view", "CREATE OR REPLACE VIEW db1.v AS SELECT a FROM db1.o WHERE a IN (WITH {V} AS {N} SELECT a FROM db1.p WHERE {P})"},
    {"view", "CREATE VIEW db1.v AS SELECT a FROM view(WITH {V} AS {N} SELECT a FROM db1.o WHERE {P})"},
    {"view", "CREATE MATERIALIZED VIEW db1.mv TO db1.q AS WITH {V} AS {N} SELECT a FROM db1.o WHERE {P}"},
    {"view", "CREATE MATERIALIZED VIEW db1.mv ENGINE = Memory AS WITH {V} AS {N} SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE {P})"},
    {"projection", "CREATE VIEW db1.v AS SELECT a, {V} AS {N} FROM db1.o WHERE {P}"},
    {"view", "CREATE VIEW db1.v AS WITH {V} AS {N} SELECT a FROM db1.o WHERE {P} UNION ALL SELECT a FROM db1.o WHERE 0"},
    {"view", "CREATE VIEW db1.v AS WITH {V} AS {N} SELECT a FROM db1.o WHERE 0 UNION DISTINCT SELECT a FROM db1.o WHERE {P}"},
    {"view", "CREATE VIEW db1.v AS WITH {V} AS {N} SELECT a FROM db1.o WHERE {P} INTERSECT SELECT a FROM db1.o"},
    {"view", "CREATE VIEW db1.v AS WITH {V} AS {N} SELECT a FROM db1.o EXCEPT SELECT a FROM db1.o WHERE {P}"},
    {"view", "CREATE VIEW db1.v AS (WITH {V} AS {N} SELECT a FROM db1.o WHERE {P} UNION ALL SELECT a FROM db1.o WHERE 0)"},
    {"view", "CREATE VIEW db1.v AS SELECT a FROM db1.o WHERE 0 UNION ALL (WITH {V} AS {N} SELECT a FROM db1.o WHERE {P} UNION ALL SELECT a FROM db1.o WHERE 0)"},
    {"view", "CREATE VIEW db1.v AS SELECT a FROM db1.o WHERE a IN (WITH {V} AS {N} SELECT a FROM db1.p WHERE {P} UNION ALL SELECT 1)"},
    {"view", "CREATE MATERIALIZED VIEW db1.mv ENGINE = Memory POPULATE AS WITH {V} AS {N} SELECT a FROM db1.o WHERE {P} UNION ALL SELECT a FROM db1.o WHERE 0"},
    {"view", "CREATE MATERIALIZED VIEW db1.mv TO db1.q AS WITH {V} AS {N} SELECT a FROM db1.o WHERE 0 UNION ALL SELECT a FROM db1.o WHERE {P}"},
  };
  const std::vector<std::string> values = {"toUInt64(1)", "(toUInt64(1))", "(SELECT 1)", "1 + 1", "CAST(1 AS UInt64)",
                                           "[(1, 2)]", "tuple(1)"};
  for (const auto &ctx : {kAliasInContexts[0], kAliasInContexts[2]}) for (const bool si : {false, true})
    for (const auto &[kind, decl] : decls) {
      const auto vs = kind == "projection" ? std::vector<std::string>{"1", "'x'", "toUInt64(1)", "(SELECT 1)"} : values;
      for (const auto &v : vs) for (const auto &name : kAliasInNames)
        for (const std::string pred : {"a IN {R}", "a NOT IN ({R})", "in(a, {R})"})
          ExpectAliasInMetamorphic(ctx, si, replaceAll(decl, "{V}", v), name, pred);
    }
}

// Names ClickHouse binds before any table (rewriter-go
// TestTableRef_CorrelatedAliasInOperandPins, KeywordPins, StoredViewAliasPins):
// a read-query CTE, a WITH alias whose value is provably not a table reference
// (in every nested scope, even under a same-named table alias), and a
// projection alias in its own SELECT; a stored view body binds a CTE and a
// folded literal. The operand stays as written, unreported, in every context.
TEST(TableRefBinding, NamesThatBindAreLeftUnreported) {
  const std::vector<std::pair<std::string, std::string>> plain = {
    {"WITH (1, 2) AS s SELECT * FROM db1.o WHERE a IN s", "s"},
    {"WITH (1, 2) AS s SELECT * FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE a IN s)", "s"},
    {"WITH (SELECT 1) AS s SELECT * FROM db1.o WHERE a IN s", "s"},
    {"WITH [1, 2] AS s SELECT * FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE a GLOBAL IN s)", "s"},
    {"WITH ((-1), 'x') AS s SELECT * FROM db1.o WHERE (a, b) IN s", "s"},
    // A grouped literal tuple keeps its grouping (MeaningChangingLiteralFoldsAreRefused).
    {"WITH ((-1, 'x')) AS s SELECT * FROM db1.o WHERE (a, b) IN s", "s"},
    {"WITH identity(secret) AS s SELECT * FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE notIn(a, s))", "s"},
    {"WITH NULL AS s SELECT * FROM db1.o AS s WHERE a IN s", "s"},
    {"WITH c AS (SELECT 1 AS a) SELECT * FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE a IN c)", "c"},
    {"WITH 1 AS c SELECT * FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE a IN c)", "c"},
    {"WITH (SELECT 1) AS c SELECT * FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE in(a, c))", "c"},
    {"WITH c AS (SELECT 1 AS a) SELECT * FROM db1.o WHERE a IN (SELECT a FROM db1.p AS c WHERE a IN c)", "c"},
    {"WITH 1 AS c SELECT * FROM db1.o AS c WHERE a IN c", "c"},
    {"SELECT a, 1 AS c FROM db1.o WHERE a IN c", "c"},
    {"SELECT a, 1 AS c FROM db1.o AS c WHERE a GLOBAL IN (c)", "c"},
    {"WITH NOW() AS s SELECT a FROM db1.o WHERE a IN s", "s"},
    {"WITH now() AS s SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE a IN s)", "s"},
    {"WITH PI() AS s SELECT a FROM db1.o WHERE in(a, s)", "s"},
    {"WITH (timezone()) AS s SELECT a FROM db1.o AS s WHERE a IN s", "s"},
    {"SELECT a FROM db1.o WHERE a IN (WITH inf AS s SELECT a FROM db1.p WHERE a IN s)", "s"},
    {"SELECT a FROM db1.o WHERE a IN (WITH -nan AS s SELECT a FROM db1.p WHERE a IN s)", "s"},
    {"SELECT a FROM db1.o WHERE a IN (WITH 0b101 AS s SELECT a FROM db1.p WHERE a IN s)", "s"},
    {"WITH 1 AS \"db1.t\" SELECT a FROM db1.o WHERE a IN \"db1.t\"", "db1.t"},
    {"WITH 1 AS t SELECT a FROM db1.o WHERE a IN t", "t"},
    // A top-level view(), INSERT … SELECT and CTAS bodies follow the plain rule.
    {"SELECT a FROM view(WITH toUInt64(1) AS s SELECT a FROM db1.o WHERE a IN s)", "s"},
    {"INSERT INTO db1.q WITH (SELECT 1) AS s SELECT a FROM db1.o WHERE a IN s", "s"},
    {"CREATE TABLE db1.n ENGINE = Memory AS SELECT a, 1 AS s FROM db1.o WHERE a IN s", "s"},
    {"INSERT INTO db1.q WITH 1 AS t SELECT a FROM db1.o WHERE a IN t", "t"},
    // Stored view bodies: a folded literal and a read-query CTE.
    {"CREATE VIEW db1.v AS WITH 1 AS s SELECT a FROM db1.o WHERE a IN s", "s"},
    {"CREATE VIEW db1.v AS WITH (1, 2) AS s SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE a IN s)", "s"},
    {"CREATE VIEW db1.v AS WITH [-1, 2] AS s SELECT a FROM db1.o WHERE in(a, s)", "s"},
    {"CREATE VIEW db1.v AS WITH (-1) AS s SELECT a FROM db1.o WHERE a IN s", "s"},
    {"CREATE MATERIALIZED VIEW db1.mv TO db1.q AS WITH 'x' AS s SELECT a FROM db1.o WHERE a IN s", "s"},
    {"CREATE VIEW db1.v AS WITH NULL AS s SELECT a FROM db1.o AS s WHERE a IN s", "s"},
    {"CREATE VIEW db1.v AS WITH c AS (SELECT 1 AS a) SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE a IN c)", "c"},
    {"CREATE VIEW db1.v AS WITH 1 AS t SELECT a FROM db1.o WHERE a IN t", "t"},
  };
  for (const auto *v : {"0x10", "-0x10", "0b101", "inf", "-INF", "NaN", "x'41'", "$$abc$$", "(0x10, inf)"})
    for (const bool si : {false, true})
      ExpectOperandBinds(std::string("CREATE VIEW db1.v AS WITH ") + v
                           + " AS s SELECT a FROM db1.o WHERE a IN s UNION ALL SELECT 1", si, "s");
  for (const bool si : {false, true}) for (const auto &[sql, operand] : plain)
    for (const std::string logical : {"db1", "", "db9"}) ExpectOperandBinds(sql, si, operand, logical);
  // Exact output for two of them.
  EXPECT_EQ(RunTableRef("WITH 1 AS c SELECT * FROM db1.o AS c WHERE a IN c", true).sql_after_rewrite(),
            "WITH 1 AS c SELECT * FROM phys.`db1.o` AS c WHERE a IN (c)");
  EXPECT_EQ(RunTableRef("SELECT a, 1 AS c FROM db1.o WHERE a IN c", true).sql_after_rewrite(),
            "SELECT a, 1 AS c FROM phys.`db1.o` AS `db1.o` WHERE a IN (c)");
}

// Names that do not bind: the operand is a table, rewritten and reported
// (rewriter-go F1, N1, N2, N4, N6 pins and the stored-view finding).
TEST(TableRefBinding, NamesThatDoNotBindAreTables) {
  const std::vector<std::pair<std::string, std::string>> cases = {
    {"SELECT * FROM db1.o AS \"other.secret\" WHERE a IN (SELECT a FROM db1.p WHERE a IN \"other.secret\")", "other.secret"},
    {"SELECT * FROM db1.o AS `db2.x` WHERE a IN (SELECT a FROM db1.p WHERE in(a, `db2.x`))", "db2.x"},
    {"SELECT * FROM db1.o AS t WHERE a IN (SELECT a FROM db1.p WHERE a IN t)", "t"},
    {"SELECT * FROM db1.o AS \"other.secret\" WHERE a IN \"other.secret\"", "other.secret"},
    {"SELECT a, 1 AS t FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE a IN t)", "t"},
    {"WITH nosuch AS \"other.secret\" SELECT a FROM db1.o WHERE a IN \"other.secret\"", "other.secret"},
    {"WITH `db1.t` AS z SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE a IN z)", "z"},
    {"WITH 1 AS w, w AS t SELECT a FROM db1.o WHERE a IN t", "t"},
    {"WITH 1 AS t SELECT a FROM db1.o WHERE a IN (WITH secret AS t SELECT a FROM db1.p WHERE in(a, t))", "t"},
    {"WITH NOW AS \"db1.t\" SELECT a FROM db1.o WHERE a IN \"db1.t\"", "db1.t"},
    {"WITH (current_date) AS \"other.secret\" SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE in(a, \"other.secret\"))",
     "other.secret"},
    {"SELECT a FROM db1.o WHERE a IN NOW", "NOW"},
    // (`IN (current_user)` reads a table too, but a bare current_user is a
    // deployment-identity name, refused last: MetadataAccess.)
    {"SELECT a FROM db1.o WHERE a IN (current_date)", "current_date"},
    {"SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE globalIn(a, CURRENT_DATE))", "CURRENT_DATE"},
    {"SELECT a FROM db1.o WHERE a IN EXISTS", "EXISTS"},
    {"SELECT a FROM db1.o WHERE a NOT IN (interval)", "interval"},
    {"WITH -NOW AS z SELECT a FROM db1.o WHERE a IN z", "z"},
    {"WITH CAST(NOW AS String) AS z SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.p WHERE in(a, z))", "z"},
    {"CREATE VIEW db1.vv AS WITH (SELECT 1) AS `db2.my-t` SELECT a FROM db1.o WHERE a IN `db2.my-t`", "db2.my-t"},
    {"CREATE MATERIALIZED VIEW db1.mv TO db1.q AS WITH toUInt64(1) AS x SELECT a FROM db1.o WHERE a IN x", "x"},
    {"CREATE VIEW db1.v AS SELECT a, 1 AS c FROM db1.o WHERE a IN c", "c"},
    {"CREATE VIEW db1.vv AS WITH (SELECT 1) AS `db2.my-t` SELECT a FROM db1.o WHERE a IN `db2.my-t` UNION ALL SELECT a FROM db1.o WHERE 0",
     "db2.my-t"},
    {"CREATE VIEW db1.vv AS WITH toUInt64(1) AS \"db1.t\" SELECT a FROM db1.o WHERE a IN \"db1.t\" UNION ALL SELECT a FROM db1.o WHERE 0",
     "db1.t"},
    // A view() inside a stored view body follows the stored rule.
    {"CREATE VIEW db1.v AS SELECT a FROM view(WITH toUInt64(1) AS s SELECT a FROM db1.o WHERE a IN s)", "s"},
    // A binding declared outside view() does not reach its body.
    {"WITH 1 AS s SELECT a FROM view(SELECT a FROM db1.o WHERE a IN s)", "s"},
    // Only the leading SELECT's WITH reaches the later arms of a set operation.
    {"SELECT a FROM db1.o WHERE 0 UNION ALL (WITH 1 AS s SELECT a FROM db1.o) UNION ALL SELECT a FROM db1.o WHERE a IN s", "s"},
  };
  for (const bool si : {false, true}) for (const auto &[sql, operand] : cases) ExpectOperandIsTable(sql, si, operand);
}

// Task I3 fix round 1 (review C1): a WITH alias whose value is a function
// Polyglot types as its own node — `if` / `ifNull` / `coalesce` / `cast`, and
// `multiIf` (stricter than Go, defence in depth) — with an identifier argument
// does not bind: ClickHouse's old analyzer folds `if(1, x, …)` to the
// identifier and reads the table. The operand is a table, in every body shape
// and the `IF` spelling; a value with no name reference (`if(1, 2, 3)`) still
// binds. Measured on 26.2 / 25.8: Go rewrites the operand too (the leak was
// C++-only before this fix).
TEST(TableRefBinding, FoldingFunctionWithAliasIsNotABinding) {
  const std::vector<std::string> fold_values = {
    "if(1, {I}, 0)", "IF(1, {I}, 0)", "if(0, 0, {I})", "ifNull({I}, 0)", "coalesce({I}, 0)",
    "cast({I} AS String)", "multiIf(1, {I}, 0)", "if(1, {I}, 0) + 0"};
  const std::vector<std::pair<std::string, std::string>> bodies = {
    {"WITH {V} AS z SELECT a FROM db1.o WHERE a IN z", "same scope"},
    {"WITH {V} AS z SELECT a FROM db1.o WHERE a IN (SELECT a FROM db1.o WHERE a IN z)", "nested"},
    {"INSERT INTO db1.q WITH {V} AS z SELECT a FROM db1.o WHERE a IN z", "insert select"},
    {"CREATE TABLE db1.n ENGINE = Memory AS WITH {V} AS z SELECT a FROM db1.o WHERE a IN z", "ctas"},
    {"SELECT a FROM view(WITH {V} AS z SELECT a FROM db1.o WHERE a IN z)", "view()"},
  };
  for (const bool si : {false, true}) {
    for (const std::string &fv : fold_values) {
      for (const std::string &ident : {"`db2.x`", "`db1.t`"}) {
        std::string value = fv;
        value.replace(value.find("{I}"), 3, ident);
        for (const auto &[tmpl, name] : bodies) {
          std::string sql = tmpl;
          sql.replace(sql.find("{V}"), 3, value);
          SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
          // The alias z is a table in the session's logical database, not bound.
          ExpectOperandIsTable(sql, si, "z");
        }
      }
    }
    // A folding function with no name reference still binds (matches Go).
    for (const std::string sql : {"WITH if(1, 2, 3) AS z SELECT a FROM db1.o WHERE a IN z",
                                  "WITH coalesce(1, 2) AS z SELECT a FROM db1.o WHERE a IN z",
                                  "WITH multiIf(1, 2, 3) AS z SELECT a FROM db1.o WHERE a IN z"})
      ExpectOperandBinds(sql, si, "z");
  }
}

// The leading SELECT's WITH reaches every later arm of a set operation (UNION,
// INTERSECT, EXCEPT; ClickHouse's enable_global_with_statement, measured on
// 26.2 and 25.8 under both analyzers), for a CTE name as a FROM table too. The
// same statement in a `system` session: the CTE is not a system table (Task I2
// review M1's over-refusal).
TEST(TableRefBinding, SetOperationArmsSeeTheLeadingWith) {
  for (const bool si : {false, true}) {
    for (const std::string sql : {
           "WITH 1 AS s SELECT a FROM db1.o WHERE 0 UNION ALL SELECT a FROM db1.o WHERE a IN s",
           "WITH 1 AS s SELECT a FROM db1.o INTERSECT SELECT a FROM db1.o WHERE a IN s",
           "WITH 1 AS s SELECT a FROM db1.o EXCEPT SELECT a FROM db1.o WHERE a IN s",
           "WITH 1 AS s SELECT a FROM db1.o UNION ALL SELECT a FROM db1.o INTERSECT SELECT a FROM db1.o WHERE a IN s",
           "WITH 1 AS s SELECT a FROM db1.o UNION ALL (SELECT a FROM db1.o WHERE a IN s)",
           "SELECT a FROM db1.o WHERE a IN (WITH 1 AS s SELECT 1 UNION ALL SELECT a FROM db1.p WHERE a IN s)",
         })
      ExpectOperandBinds(sql, si, "s");
    for (const std::string sql : {
           "WITH s AS (SELECT 1 AS a) SELECT a FROM s UNION ALL SELECT a FROM s",
           "WITH s AS (SELECT 1 AS a) SELECT a FROM s INTERSECT SELECT a FROM s",
         }) {
      SCOPED_TRACE(sql);
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
      EXPECT_EQ(resp.original_accessed_tables_size(), 0);
    }
    // A `system` session: the CTE in the later arm is not system.processes.
    const auto sys = RunTableRefContext(
      "WITH processes AS (SELECT 1) SELECT * FROM processes UNION ALL SELECT * FROM processes", si, "system", "phys");
    EXPECT_EQ(sys.code(), rewriter::RewriteCode::Success) << sys.message();
  }
}

// No WITH binding in an R2 position: a CTE, a WITH alias and a projection
// alias declared inside a mutation, a column / constraint / TTL expression or
// an ALTER action bind nothing, so the name is a table and R2 refuses the
// statement (under V2 an Active `t` is named by the SI refusal first).
TEST(TableRefBinding, R2PositionsBindNothing) {
  for (const bool si : {false, true}) {
    for (const std::string sql : {
           "DELETE FROM db1.o WHERE a IN (WITH 1 AS s SELECT a FROM db1.p WHERE a IN s)",
           "DELETE FROM db1.o WHERE a IN (WITH s AS (SELECT 1) SELECT * FROM s)",
           "UPDATE db1.o SET b = 1 WHERE a IN (SELECT 1 AS s WHERE 1 IN s)",
           "ALTER TABLE db1.o DELETE WHERE a IN (WITH (1, 2) AS s SELECT a FROM db1.p WHERE a IN s)",
           "ALTER TABLE db1.o UPDATE b = 1 WHERE a IN (WITH s AS (SELECT 1) SELECT * FROM s)",
           "CREATE TABLE db1.n (a UInt8, CONSTRAINT c CHECK a IN (WITH 1 AS s SELECT 1 WHERE 1 IN s)) ENGINE = Memory",
           "CREATE TABLE db1.n (a UInt8 DEFAULT (WITH s AS (SELECT 1) SELECT * FROM s)) ENGINE = Memory",
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.message();
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
  }
}

// Task I2 review M4: a TimeSeries table's DATA / TAGS / METRICS target
// clauses (a named table, INNER UUID, an inner ENGINE) are refused with T7 in
// both SI states before any name is examined, as rewriter-go refuses them; a
// bare TimeSeries engine keeps T5.
TEST(TableRefFidelity, TimeSeriesTargetsAreRefused) {
  for (const bool si : {false, true}) {
    for (const std::string sql : {
           "CREATE TABLE db1.x ENGINE = TimeSeries DATA system.query_log",
           "CREATE TABLE db1.x ENGINE = TimeSeries DATA db1.d TAGS db1.tg METRICS db1.m",
           "CREATE TABLE db1.x ENGINE = TimeSeries DATA hg_safe.db1__t",
           "CREATE TABLE db1.x ENGINE = TimeSeries TAGS ENGINE = MergeTree ORDER BY id",
           "CREATE TABLE db1.x ENGINE = TimeSeries DATA INNER UUID '01234567-89ab-cdef-0123-456789abcdef'",
           "CREATE TABLE db1.x ENGINE = TimeSeries METRICS db1.t",
           // ClickHouse 26.8 names DATA as SAMPLES and adds RECENT SAMPLES and
           // INNER COLUMNS (an inner table's own column declarations).
           "CREATE TABLE db1.x ENGINE = TimeSeries SAMPLES db1.d",
           "CREATE TABLE db1.x ENGINE = TimeSeries RECENT SAMPLES db1.d",
           "CREATE TABLE db1.x ENGINE = TimeSeries SAMPLES INNER COLUMNS (id UUID DEFAULT (SELECT max(id) FROM db2.x)) SAMPLES ENGINE = MergeTree ORDER BY id",
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement);
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
      EXPECT_EQ(resp.original_accessed_tables_size(), 0);
    }
    const auto bare = RunTableRef("CREATE TABLE db1.x ENGINE = TimeSeries", si);
    EXPECT_EQ(bare.code(), rewriter::RewriteCode::UnsupportedStatement);
    EXPECT_EQ(bare.message(), "table engine TimeSeries is not accepted");
  }
}

// The synthetic SHOW TABLES / SHOW DATABASES listings and the SI DESCRIBE
// metadata SELECT model only their plain forms: a clause they would drop
// (LIKE / ILIKE, an empty pattern included, WHERE, LIMIT, FORMAT, INTO
// OUTFILE, SETTINGS) and SHOW FULL / TEMPORARY TABLES are refused with T7, as
// rewriter-go refuses them, instead of being silently answered as the plain
// form. SHOW DATABASES keeps its LIKE / ILIKE filter (an empty one too).
TEST(TableRefShow, SyntheticOutputsRefuseDroppedClauses) {
  for (const bool si : {false, true}) {
    for (const std::string sql : {
           "SHOW TABLES FROM db1 LIKE 'x%'", "SHOW TABLES LIKE ''", "SHOW TABLES FROM db1 NOT ILIKE 'x%'",
           "SHOW TABLES FROM db1 WHERE 1", "SHOW TABLES FROM db1 LIMIT 1", "SHOW TABLES FROM db1 FORMAT JSON",
           "SHOW TABLES FROM db1 INTO OUTFILE '/tmp/x'", "SHOW TABLES FROM db1 SETTINGS max_threads = 1",
           "SHOW FULL TABLES", "SHOW TEMPORARY TABLES", "SHOW FULL TEMPORARY TABLES FROM db1", "SHOW TEMPORARY TABLES IN db1",
           "SHOW DATABASES LIMIT 1", "SHOW DATABASES FORMAT JSON", "SHOW DATABASES INTO OUTFILE '/tmp/x'",
           "SHOW DATABASES SETTINGS max_threads = 1", "SHOW DATABASES LIKE 'd%' LIMIT 1",
           "SHOW DATABASES LIKE 'd%' FORMAT JSON",
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
      EXPECT_EQ(resp.database_rewrites_size(), 0);
    }
    // The resolution refusals keep precedence (rewriter-go's order).
    EXPECT_EQ(RunTableRef("SHOW TABLES FROM db9 LIKE 'x'", si).message(),
              "SHOW TABLES target logical database 'db9' is not in database_map and not a known physical database; "
              "user does not have this database");
    EXPECT_EQ(RunTableRef("SHOW TABLES FROM hg_safe LIKE 'x'", si).message(),
              si ? "storage-integrity physical database hg_safe is not directly addressable"
                 : "protected database hg_safe is not addressable");
    // R5 outranks the refusal in both SI states.
    EXPECT_EQ(RunTableRef("SHOW TABLES FROM db1 LIKE 'x%' SETTINGS enable_global_with_statement = 0", si).message(),
              "table setting enable_global_with_statement is not accepted");
    EXPECT_EQ(RunTableRef("SHOW DATABASES SETTINGS compatibility = '20.1'", si).message(),
              "table setting compatibility is not accepted");
    // The plain forms and SHOW DATABASES' LIKE filter keep working.
    for (const std::string sql : {"SHOW TABLES", "SHOW TABLES FROM db1", "SHOW TABLES IN db1", "SHOW DATABASES"}) {
      SCOPED_TRACE(sql);
      EXPECT_EQ(RunTableRef(sql, si).code(), rewriter::RewriteCode::Success);
    }
    EXPECT_EQ(RunTableRef("SHOW DATABASES LIKE 'd%'", si).sql_after_rewrite(),
              "SELECT name FROM (SELECT 'db1' AS name) WHERE name LIKE 'd%' ORDER BY name");
    EXPECT_EQ(RunTableRef("SHOW DATABASES NOT ILIKE 'd%'", si).sql_after_rewrite(),
              "SELECT name FROM (SELECT 'db1' AS name) WHERE name NOT ILIKE 'd%' ORDER BY name");
    EXPECT_EQ(RunTableRef("SHOW DATABASES LIKE ''", si).sql_after_rewrite(),
              "SELECT name FROM (SELECT 'db1' AS name) WHERE name LIKE '' ORDER BY name");
  }
  // The SI DESCRIBE metadata SELECT: FORMAT / INTO OUTFILE / SETTINGS refused.
  for (const std::string sql : {"DESCRIBE TABLE db1.t FORMAT JSON", "DESCRIBE TABLE t INTO OUTFILE '/tmp/x'",
                                "DESCRIBE TABLE db1.t SETTINGS max_threads = 1"}) {
    SCOPED_TRACE(sql);
    const auto resp = RunTableRef(sql, true);
    EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
    EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
  }
  EXPECT_EQ(RunTableRef("DESCRIBE TABLE db1.t", true).code(), rewriter::RewriteCode::Success);
  // PARITY (rewriter-go refuses these on its own regeneration gate): an
  // ordinary DESCRIBE keeps its clause verbatim, so nothing is dropped.
  EXPECT_EQ(RunTableRef("DESCRIBE TABLE db1.o FORMAT JSON", true).sql_after_rewrite(),
            "DESCRIBE TABLE db1.o FORMAT JSON");
}

// A column declaration's own SETTINGS (…) clause in a CREATE column list or an
// ALTER … ADD COLUMN is refused on a would-be Success, in both SI states, as
// rewriter-go refuses it (Task I3 / I2 review, out of scope note); an earlier
// refusal keeps its message, and ALTER … MODIFY COLUMN … SETTINGS and a
// table-level SETTINGS stay admitted by both engines.
TEST(TableRefSettings, ColumnDeclarationSettingsAreRefused) {
  for (const bool si : {false, true}) {
    for (const std::string sql : {
           "CREATE TABLE db1.x (a Int8 SETTINGS (max_compress_block_size = 1)) ENGINE = MergeTree ORDER BY a",
           "CREATE TABLE db1.x (a Int8 CODEC(ZSTD) SETTINGS (max_compress_block_size = 1, min_compress_block_size = 2)) "
           "ENGINE = MergeTree ORDER BY a",
           "CREATE VIEW db1.v (a Int8 SETTINGS (max_compress_block_size = 1)) AS SELECT 1 AS a",
           "ALTER TABLE db1.o ADD COLUMN b Int8 SETTINGS (max_compress_block_size = 1)",
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
    for (const std::string sql : {
           "ALTER TABLE db1.o MODIFY COLUMN a Int8 SETTINGS (max_compress_block_size = 1)",
           "ALTER TABLE db1.o MODIFY COLUMN a MODIFY SETTING max_compress_block_size = 1",
           "CREATE TABLE db1.x (a Int8) ENGINE = MergeTree ORDER BY a SETTINGS index_granularity = 8192",
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      EXPECT_EQ(RunTableRef(sql, si).code(), rewriter::RewriteCode::Success) << RunTableRef(sql, si).message();
    }
    // An earlier refusal keeps its message.
    EXPECT_EQ(RunTableRef("CREATE TABLE hg_safe.x (a Int8 SETTINGS (max_compress_block_size = 1)) ENGINE = Memory", si)
                .message(),
              si ? "storage-integrity physical table hg_safe.x is not directly addressable"
                 : "protected database hg_safe is not addressable");
  }
}

// Task I3 fix round 1 (controller verification): C++ refuses a nested
// (EXPLAIN …) subquery in every expression position, and a read in a CODEC
// argument, a data-type parameter or a storage-setting value — by a local
// SyntaxError, viewExplain-not-recognised, protected-database or
// statement-not-supported refusal. rewriter-go admits some of these (it does
// not walk CODEC args / data-type params; a Go-side gap, ledgered); C++ is
// fail-closed everywhere, so no fix is needed, only this pin.
TEST(TableRefFidelity, ExplainAndTypeArgReadsAreRefused) {
  for (const bool si : {false, true}) {
    for (const std::string sql : {
           "SELECT a FROM db1.o WHERE a IN (EXPLAIN SELECT 1 FROM db2.x)",
           "SELECT (EXPLAIN SELECT 1 FROM db2.x) FROM db1.o",
           "SELECT * FROM (EXPLAIN SELECT 1 FROM db2.x)",
           "SELECT a FROM db1.o WHERE EXISTS (EXPLAIN SELECT 1 FROM db2.x)",
           "SELECT a FROM db1.o WHERE a IN (EXPLAIN SELECT 1 FROM hg_unsafe.x)",
           "INSERT INTO db1.q SELECT a FROM db1.o WHERE a IN (EXPLAIN SELECT 1 FROM db2.x)",
           "CREATE TABLE db1.n ENGINE=Memory AS SELECT a FROM db1.o WHERE a IN (EXPLAIN SELECT 1 FROM db2.x)",
           "CREATE VIEW db1.v AS SELECT a FROM db1.o WHERE a IN (EXPLAIN SELECT 1 FROM db2.x)",
           "ALTER TABLE db1.o DELETE WHERE a IN (EXPLAIN SELECT 1 FROM db2.x)",
           "CREATE TABLE db1.n (a UInt8 CODEC(ZSTD((SELECT 1 FROM db2.x)))) ENGINE=MergeTree ORDER BY a",
           "CREATE TABLE db1.n (a UInt8 CODEC(ZSTD((SELECT 1 FROM hg_unsafe.x)))) ENGINE=MergeTree ORDER BY a",
           "CREATE TABLE db1.n (a FixedString((SELECT 1 FROM db2.x))) ENGINE=Memory ORDER BY tuple()",
           "CREATE TABLE db1.n (a UInt8) ENGINE=MergeTree ORDER BY a SETTINGS index_granularity = (SELECT 1 FROM db2.x)",
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      EXPECT_NE(RunTableRef(sql, si).code(), rewriter::RewriteCode::Success) << RunTableRef(sql, si).sql_after_rewrite();
    }
  }
}

#if !REWRITER_RELEASE_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif

// Task I3 fix round 2 (re-review I-1): the embedded 26.3 parser folds a
// literal tuple wrapped in an extra pair of grouping parentheses, `((1, 2))`,
// into the flat `(1, 2)`. ClickHouse 25.8 and 26.2 read the source as a
// one-element tuple of a tuple (measured: `SELECT ((1, 2))` gives `((1,2))` of
// type Tuple(Tuple(UInt8, UInt8)); `1 IN ((1, 2))` is error 43; `((1, 2)).1`
// is `(1,2)`), so re-emitting the flat form would change the meaning there.
// Since the Sentio driver report (gap 2) such a group is preserved: the output
// spells `((1, 2))` as the source did (adoptLiteralFolds), with at most the
// formatter's own outer priority parentheses added or dropped and its usual
// literal spelling (`+2` -> `2`, `x'30'` -> `'0'`); measured on 25.8, 26.2
// and 26.3, input and output read the same. What cannot be preserved stays
// refused (`statement is not supported`) on a would-be Success, in both SI
// states: a float literal below the smallest normal double (`1e-400`: 25.8
// rejects it, error 62, the formatter re-emits `0.`), a group in a clause the
// marked parse cannot take (`DROP PARTITION ((1, 2))`), a group whose marked
// tree differs from the source's (a fold beside other tuple elements, where
// the parser merges the literals into one), and a source that spells the
// marker itself.
TEST(TableRefFidelity, MeaningChangingLiteralFoldsAreRefused) {
  for (const bool si : {false, true}) {
    for (const std::string sql : {
           "ALTER TABLE db1.o DROP PARTITION ((1, 2))",
           "SELECT 1e-400",
           "SELECT a FROM db1.o WHERE a > -1e-0400",
           "SELECT (((1, 2)), 3)",
           "SELECT (((1,2), ((3,4))))",
           "SELECT ((1, 2)), __housegate_grouped_literal_tuple",
           "SELECT ((1, 2)) FROM db1.o WHERE a > 1e-400",
           // Fix round 1 (review I-1): a group of literal-tuple shape whose
           // nested brackets are not literal tuples for 25.8 / 26.2. The
           // embedded parser folds them into a literal anyway, so a preserved
           // grouping would print `((1, 2))` where 25.8 / 26.2 read `(1, 2)`
           // in the source (measured): refused, as v0.16.0 refused them.
           "SELECT toString(((1, (2))))",
           "SELECT 1 IN ((1, (2)))",
           "SELECT a FROM db1.o WHERE a IN ((1, (2)))",
           "INSERT INTO db1.o (a, b, s) SELECT 9, 9, toString(((1, (2))))",
           "SELECT toTypeName(((1, (2, 3), (4))))",
           "SELECT ((1, (2)))", "SELECT (((1), 2))", "SELECT ((1, ((2))))", "SELECT ((1, (-2)))",
           "SELECT ((+(1), 2))", "SELECT ((1, ('a')))", "SELECT ((1, (NULL)))", "SELECT ((1, (inf)))",
           "SELECT ((1, (2, 3,)))", "SELECT (((1, 2), (3)))", "SELECT ((1, (2, 3), (4)))",
           "SELECT ((1, ((2, 3))))", "SELECT ((1, -(2, 3)))", "SELECT ((1, ()))",
           // Fix round 2 (re-review R1-I-1): a sign before anything but a
           // number or inf / infinity / nan makes the tuple an expression on
           // 25.8 / 26.2, while the embedded parser drops a `+` and folds it.
           "SELECT toString(((1, +'a')))",
           "SELECT a FROM db1.o WHERE s IN ((+'a', +'b'))",
           "INSERT INTO db1.o (a, b, s) SELECT 9, 9, toString(((1, +'a')))",
           "CREATE TABLE db1.n (a UInt8, q String DEFAULT toString(((1, +'a')))) ENGINE = MergeTree ORDER BY a",
           "CREATE VIEW db1.n AS SELECT a FROM db1.o WHERE s IN ((+'a', +'b'))",
           "SELECT ((+NULL, 1))", "SELECT ((1, +x'31'))", "SELECT ((1, +b'1'))", "SELECT ((1, +$$a$$))",
           "SELECT ((1, +$t$a$t$))", "SELECT ((1, +true))", "SELECT ((1, +FALSE))", "SELECT ((1, +null))",
           "SELECT ((1, + 'a'))", "SELECT ((1, +/*c*/'a'))", "SELECT ((1, -'a'))", "SELECT ((1, -NULL))",
           "SELECT ((1, (1, +'a')))", "SELECT (((1, +'a'), 1))",
           // Fix round 2 (R1-M-1): `NOT (` is a call to 25.8 / 26.2 and an
           // operator over a grouping to 26.3; a fold right after it is refused.
           "SELECT NOT ((1, 2)) = ((1, 2))", "SELECT NOT ((1, 2)) != ((3, 4))", "SELECT NOT ((1, 2)) IN ((1, 2))",
           "SELECT NOT ((0, 1)) IN ((0, 1))", "SELECT NOT ((1, 2))", "SELECT not /*c*/ ((1, 2))",
           // I-2b: a non-zero float below the smallest normal double. 25.8
           // rejects it (error 62); the formatter re-emits a value it accepts
           // (`2.2250738585072013e-308` -> `2.2250738585072014e-308`).
           "SELECT 2.2250738585072013e-308",
           "SELECT 2.2250738585072012e-308",
           "SELECT 0x1.fffffffffffffp-1023",
           "SELECT -1e-310, 4.9e-324",
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
    const std::string t = si ? "(SELECT * EXCEPT (_hg_row_id) FROM hg_safe.db1__t) AS `db1.t`"
                             : "phys.`db1.t` AS `db1.t`";
    for (const auto &[sql, want] : std::vector<std::pair<std::string, std::string>>{
           {"SELECT ((1, 2))", "SELECT ((1, 2))"},
           {"SELECT (((1, 2)))", "SELECT ((1, 2))"},
           {"SELECT ((((1, 2))))", "SELECT ((1, 2))"},
           {"SELECT toTypeName(((1, 2)))", "SELECT toTypeName(((1, 2)))"},
           {"SELECT ((1, 2)).1 AS v FROM db1.o", "SELECT (((1, 2))).1 AS v FROM phys.`db1.o` AS `db1.o`"},
           {"SELECT a FROM db1.o WHERE a IN ((1, 2))", "SELECT a FROM phys.`db1.o` AS `db1.o` WHERE a IN ((1, 2))"},
           {"SELECT ((1, 2)) FROM db1.t", "SELECT ((1, 2)) FROM " + t},
           {"SELECT a FROM db1.t WHERE (a, b) = ((1, 2))", "SELECT a FROM " + t + " WHERE (a, b) = ((1, 2))"},
           {"SELECT untuple(((1, 2)))", "SELECT untuple(((1, 2)))"},
           {"SELECT tupleElement(((1, 2)), 1)", "SELECT tupleElement(((1, 2)), 1)"},
           {"SELECT ((1, 2)) FORMAT JSONEachRow", "SELECT ((1, 2)) FORMAT JSONEachRow"},
           {"SELECT toString(((1, 2))), -((1, 2)), ((1, 2)) + ((3, 4))",
            "SELECT toString(((1, 2))), -((1, 2)), ((1, 2)) + ((3, 4))"},
           {"INSERT INTO db1.o SELECT ((1, 2)).1 FROM db1.o",
            "INSERT INTO phys.`db1.o` SELECT (((1, 2))).1 FROM phys.`db1.o` AS `db1.o`"},
           {"SELECT ( /*c*/ (1, 2) /*d*/ )", "SELECT ((1, 2))"},
           {"SELECT (((1, 2), (3, 4)))", "SELECT (((1, 2), (3, 4)))"},
           {"SELECT [((1, 2))], map(1, ((1, 2)))", "SELECT [((1, 2))], map(1, ((1, 2)))"},
           {"SELECT ((1, 2) AS x) FROM db1.o", "SELECT ((1, 2) AS x) FROM phys.`db1.o` AS `db1.o`"},
           {"SELECT (((1, 2)) AS x)", "SELECT ((1, 2)) AS x"},
           {"SELECT ((-1, +2)), ((x'30', $$a$$)), ((NULL, true, inf, nan))",
            "SELECT ((-1, 2)), (('0', 'a')), ((NULL, true, inf, nan))"},
           {"SELECT ((1, (2, 3)))", "SELECT ((1, (2, 3)))"},
           {"SELECT ((1, (-2, +3), ('a', NULL), (inf, nan), (2, (3, 4)), ((2, 3), (4, 5))))",
            "SELECT ((1, (-2, 3), ('a', NULL), (inf, nan), (2, (3, 4)), ((2, 3), (4, 5))))"},
           // Review M-1: a GROUPING SETS set's own parenthesis is syntax, so
           // `(((1, 2)))` there is the set `((1, 2))` and is re-emitted as is.
           {"SELECT a FROM db1.o GROUP BY GROUPING SETS (((1, 2)))",
            "SELECT a FROM phys.`db1.o` AS `db1.o` GROUP BY GROUPING SETS (((1, 2)))"},
           {"SELECT a FROM db1.o GROUP BY GROUPING SETS (((1, 2)), (a))",
            "SELECT a FROM phys.`db1.o` AS `db1.o` GROUP BY GROUPING SETS (((1, 2)), (a))"},
           {"SELECT a FROM db1.o GROUP BY GROUPING SETS ((((1, 2))), (a))",
            "SELECT a FROM phys.`db1.o` AS `db1.o` GROUP BY GROUPING SETS ((((1, 2))), (a))"},
           {"WITH ((-1, 'x')) AS s SELECT * FROM db1.o WHERE (a, b) IN s",
            "WITH ((-1, 'x')) AS s SELECT * FROM phys.`db1.o` AS `db1.o` WHERE (a, b) IN (s)"},
           {"CREATE TABLE db1.n (a Tuple(Tuple(UInt8, UInt8)) DEFAULT ((1, 2))) ENGINE = MergeTree ORDER BY tuple()",
            "CREATE TABLE phys.`db1.n` (`a` Tuple(Tuple(UInt8, UInt8)) DEFAULT ((1, 2))) ENGINE = MergeTree ORDER BY tuple()"},
           {"CREATE VIEW db1.v AS SELECT ((1, 2)) AS x", "CREATE VIEW phys.`db1.v` AS SELECT ((1, 2)) AS x"},
           {"ALTER TABLE db1.o DELETE WHERE a IN ((1, 2))", "ALTER TABLE phys.`db1.o` (DELETE WHERE a IN ((1, 2)))"},
           {"ALTER TABLE db1.o UPDATE b = 1 WHERE a IN ((1, 2))",
            "ALTER TABLE phys.`db1.o` (UPDATE  b = 1 WHERE a IN ((1, 2)))"},
           {"DELETE FROM db1.o WHERE a IN ((1, 2))", "DELETE FROM phys.`db1.o` WHERE a IN ((1, 2))"},
           {"SELECT * FROM db1.t WHERE a IN ((1, 2) AS z)", "SELECT * FROM " + t + " WHERE a IN ((1, 2) AS z)"},
           {"SELECT * FROM db1.o WHERE (a, b) IN ((1,2) AS z)",
            "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE (a, b) IN ((1, 2) AS z)"},
           {"SELECT * FROM db1.o WHERE (a, b) IN ((1, 2) AS z) AND c IN ((3, 4))",
            "SELECT * FROM phys.`db1.o` AS `db1.o` WHERE ((a, b) IN ((1, 2) AS z)) AND (c IN ((3, 4)))"},
           {"SELECT any (((1, 2)))", "SELECT any(((1, 2)))"},
           {"SELECT array (((1, 2))) FROM db1.o", "SELECT array(((1, 2))) FROM phys.`db1.o` AS `db1.o`"},
           {"SELECT ((1, 2))[1], ((1, 2)) = ((1, 2))", "SELECT ((1, 2))[1], ((1, 2)) = ((1, 2))"},
           // Fix round 2: a sign before a number or inf / infinity / nan keeps
           // the tuple literal (measured on 25.8 / 26.2 / 26.3).
           {"SELECT ((1, +1, -0x10, +1e3, +inf, -Infinity, +NaN, - 1, +/*c*/2))",
            "SELECT ((1, 1, -16, 1000., inf, -inf, nan, -1, 2))"},
           {"SELECT CAST(((1, 2)) AS String)", "SELECT CAST(((1, 2)), 'String')"},
           {"WITH ((1, 2)) AS s SELECT s", "WITH ((1, 2)) AS s SELECT s"},
           {"INSERT INTO db1.o (a) SELECT ((1, 2)) FROM db1.p",
            "INSERT INTO phys.`db1.o` (a) SELECT ((1, 2)) FROM phys.`db1.p` AS `db1.p`"},
           {"SELECT ((1, 2)) FROM db1.o UNION ALL SELECT ((3, 4)) FROM db1.p",
            "SELECT ((1, 2)) FROM phys.`db1.o` AS `db1.o` UNION ALL SELECT ((3, 4)) FROM phys.`db1.p` AS `db1.p`"},
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.sql_after_rewrite(), want);
    }
    for (const std::string sql : {
           "SELECT (1, 2)",
           "SELECT ((1, 2), (3, 4))",
           "SELECT tuple((1, 2)), toString((1, 2))",
           "SELECT ((1 + 1, 2)) FROM db1.o",
           "SELECT ((1 - 1, 2)), ((- -1, 2)), ((+-1, 2))",
           "SELECT ((a, 2)) FROM db1.o",
           "SELECT (([1, 2])), ((1)), (('a'))",
           "SELECT ((1,)), ((1, 2,))",
           "SELECT ((1, [2]))",
           "SELECT '((1, 2))' /* ((1, 2)) */ FROM db1.o",
           "INSERT INTO db1.o VALUES ((1, 2))",
           "INSERT INTO db1.o (a) VALUES (((1, 2)))",
           "SELECT 0e-400, 1e-30, 2.3e-308, -1e-307",
           "SELECT __housegate_grouped_literal_tuple((1, 2))",
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
    }
    // Keyword-named single-argument calls: the inner group is preserved and
    // the call re-emitted as written. (`user(…)` and `database(…)` are
    // deployment-identity calls, refused in dynamic mode: MetadataAccess.)
    for (const std::string name : {"if", "and", "or", "array", "left", "right", "replace", "date", "any", "file",
                                   "view", "values", "default", "key", "index", "format",
                                   "first", "last", "mod", "div", "in", "like", "interval", "case", "timestamp",
                                   "grouping"}) {
      const std::string sql = "SELECT " + name + "(((1, 2)))";
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
  }
}

// A GRANT/REVOKE target whose database or table name contains `*` names one
// object to ClickHouse, but a privilege delta carrying that name could be
// read as a wildcard downstream; a prefix wildcard grant (`db1.o*`) has no
// delta shape at all. Both are refused exactly as rewriter-go v0.16.0 refuses
// them (measured message for message), in both SI states. `db1.*` (database
// scope), `*` (current-database scope) and ordinary tables stay admitted.
TEST(TableRefGrant, StarNamedAndWildcardTargetsAreRefused) {
  const auto star = [](const char *kind) {
    return std::string(kind)
      + " target names a database or table whose name contains '*', which a privilege delta cannot represent";
  };
  for (const bool si : {false, true}) {
    for (const auto &[sql, msg] : std::vector<std::pair<std::string, std::string>>{
           {"GRANT SELECT ON `db1.*` TO u", star("GRANT")},
           {"GRANT SELECT ON db1.`*` TO u", star("GRANT")},
           {"GRANT SELECT ON db1.\"*\" TO u", star("GRANT")},
           {"GRANT SELECT ON `*` TO u", star("GRANT")},
           {"GRANT SELECT ON \"*\" TO u", star("GRANT")},
           {"GRANT SELECT ON db1.`a*b` TO u", star("GRANT")},
           {"GRANT SELECT ON `a*`.o TO u", star("GRANT")},
           {"GRANT SELECT ON `*`.o TO u", star("GRANT")},
           {"GRANT SELECT ON `*`.* TO u", star("GRANT")},
           {"GRANT SELECT ON db1.\xE2\x80\x9C*\xE2\x80\x9D TO u", star("GRANT")},
           {"GRANT SELECT(a) ON db1.`*` TO u", star("GRANT")},
           {"GRANT SELECT ON db1.`*` TO u WITH GRANT OPTION", star("GRANT")},
           {"GRANT ON CLUSTER c SELECT ON db1.`*` TO u", star("GRANT")},
           {"GRANT SELECT, INSERT ON db1.`*` TO u, v", star("GRANT")},
           {"GRANT SELECT ON db2.`*` TO u", star("GRANT")},
           {"GRANT SELECT ON other.`*` TO u", star("GRANT")},
           {"/* c */ GRANT SELECT ON db1.`*` TO u -- c", star("GRANT")},
           {"REVOKE SELECT ON `db1.*` FROM u", star("REVOKE")},
           {"REVOKE SELECT ON db1.`*` FROM u", star("REVOKE")},
           {"REVOKE SELECT ON `*` FROM u", star("REVOKE")},
           {"REVOKE GRANT OPTION FOR SELECT ON db1.`*` FROM u", star("REVOKE")},
           {"GRANT SELECT ON db1.o* TO u", "GRANT form is not supported"},
           {"GRANT SELECT ON db1* TO u", "GRANT form is not supported"},
           {"GRANT SELECT ON db1*.* TO u", "GRANT form is not supported"},
           {"GRANT SELECT ON db1.`o`* TO u", "GRANT form is not supported"},
           {"REVOKE SELECT ON db1.o* FROM u", "REVOKE form is not supported"},
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
      EXPECT_EQ(resp.message(), msg);
      EXPECT_EQ(resp.privileges_deltas_size(), 0);
    }
    // Spellings rewriter-go cannot structure (`GRANT form is not supported`):
    // ClickHouse decodes the escape to `*`, so this engine names the star.
    // Both refuse; only the message differs.
    for (const std::string sql : {
           "GRANT SELECT ON db1.`\\x2a` TO u", "GRANT SELECT ON `\\x2a`.o TO u",
           "GRANT SELECT ON db1.`\\*` TO u", "GRANT SELECT ON db1.p, INSERT ON `db1.*` TO u",
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
      EXPECT_EQ(resp.message(), star("GRANT"));
      EXPECT_EQ(resp.privileges_deltas_size(), 0);
    }
    // Wildcard-free scopes stay admitted; *.* keeps its own refusal.
    for (const std::string sql : {
           "GRANT SELECT ON db1.* TO u", "GRANT SELECT ON `db1`.* TO u", "GRANT SELECT ON * TO u",
           "GRANT SELECT ON db1.o TO u", "REVOKE SELECT ON db1.* FROM u",
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_EQ(resp.privileges_deltas_size(), 1);
    }
    const auto global = RunTableRef("GRANT SELECT ON *.* TO u", si);
    EXPECT_EQ(global.message(), "GRANT ON *.* (global scope) is not supported");
  }
}

// ---------------------------------------------------------------------------
// Sentio indexer driver compatibility (report driver-sql-engine-compat.md,
// 2026-10-02).
//
// Gap 1: the storage-integrity compatibility scan (prepareStorageIntegritySQL)
// compared the upper-cased decoded text of every name token with its clause
// keywords, so a quoted column named `from` or `join` opened a FROM list. In a
// CREATE TABLE column list the scan then read `, `ts` DateTime64(6, 'UTC')` as
// a table `ts` with implicit alias `DateTime64` and a column-alias list, cut
// the type arguments out of the parse text and, unable to put them back,
// refused the statement with the SI wrapper message. A quoted or escaped name
// is never a keyword for ClickHouse; only a bare word is.
TEST(DriverCompat, QuotedKeywordNameIsNotAFromListBoundary) {
  for (const std::string name : {"`from`", "`join`", "`From`", "`JOIN`", "\"from\"", "`fr\\x6fm`", "`\\x66rom`"}) {
    for (const std::string type : {"DateTime64(6, 'UTC')", "DateTime64(6)", "Nullable(String)", "Decimal(76, 30)"}) {
      const std::string sql =
        "CREATE TABLE db1.e (" + name + " String, `ts` " + type + ") ENGINE = MergeTree() ORDER BY ts";
      SCOPED_TRACE(sql);
      const auto off = RunTableRef(sql, /*si=*/false);
      const auto on = RunTableRef(sql, /*si=*/true);
      ASSERT_EQ(off.code(), rewriter::RewriteCode::Success) << off.message();
      EXPECT_EQ(on.code(), rewriter::RewriteCode::Success) << on.message();
      EXPECT_EQ(on.sql_after_rewrite(), off.sql_after_rewrite());
    }
  }
  // Report shape 61: the driver's timeseries event table (ERC-20 Transfer).
  const std::string event =
    "CREATE TABLE db1.event_Transfer (`amount` Decimal(76, 30), `data` JSON, `distinctId` String, `from` String, "
    "`meta.address` String COMMENT '@SeriesLabel', `meta.block_number` Int64 COMMENT '@SlotNumber', "
    "`meta.chain` String COMMENT '@ChainID', `timestamp` DateTime64(6, 'UTC') COMMENT '@Timestamp', "
    "INDEX `idx_data_x` CAST(`data.x`, 'String') TYPE bloom_filter GRANULARITY 1) ENGINE = MergeTree() "
    "PARTITION BY `meta.chain` ORDER BY (`timestamp`)";
  const auto off = RunTableRef(event, false);
  const auto on = RunTableRef(event, true);
  ASSERT_EQ(off.code(), rewriter::RewriteCode::Success) << off.message();
  EXPECT_EQ(on.code(), rewriter::RewriteCode::Success) << on.message();
  EXPECT_EQ(on.sql_after_rewrite(), off.sql_after_rewrite());
}

// The audit of the same scan: every keyword it matches (WITH OFFSET, AS, the
// FROM-list ends WHERE / ORDER / …, RENAME, SELECT) is likewise a bare word
// only. A quoted alias spelling one is an ordinary name.
TEST(DriverCompat, QuotedKeywordNameIsNotAnSICompatibilityKeyword) {
  // `with` `offset` is a column with a quoted implicit alias, not WITH OFFSET
  // bound to the preceding comma's "table".
  for (const std::string sql : {"SELECT a, `with` `offset` FROM db1.o", "SELECT a, \"with\" \"offset\" FROM db1.o",
                                "SELECT a, `with` `offset`, b FROM db1.o WHERE a = 1"}) {
    SCOPED_TRACE(sql);
    const auto off = RunTableRef(sql, false);
    const auto on = RunTableRef(sql, true);
    ASSERT_EQ(off.code(), rewriter::RewriteCode::Success) << off.message();
    EXPECT_EQ(on.code(), rewriter::RewriteCode::Success) << on.message();
    EXPECT_EQ(on.sql_after_rewrite(), off.sql_after_rewrite());
  }
  // A quoted alias spelling a FROM-list end (`where`, `order`, …) does not end
  // the FROM list: the next table's alias column list is still kept.
  for (const std::string alias : {"w", "`where`", "`order`", "`limit`", "`settings`", "\"group\""}) {
    const std::string sql = "SELECT * FROM db1.o AS " + alias + ", db1.p AS x (c1, c2)";
    SCOPED_TRACE(sql);
    const auto on = RunTableRef(sql, true);
    EXPECT_EQ(on.code(), rewriter::RewriteCode::Success) << on.message();
    EXPECT_NE(on.sql_after_rewrite().find("AS x(c1, c2)"), std::string::npos) << on.sql_after_rewrite();
  }
  // ... and the star-RENAME check still sees the Active table behind it.
  for (const std::string alias : {"w", "`where`", "`order`"}) {
    const std::string sql = "SELECT * RENAME (_hg_row_id AS x) FROM db1.o AS " + alias + ", db1.t";
    SCOPED_TRACE(sql);
    const auto on = RunTableRef(sql, true);
    EXPECT_EQ(on.code(), rewriter::RewriteCode::RewriteError) << on.message();
    EXPECT_EQ(on.message(), "reserved column _hg_row_id is not addressable");
  }
}

// Gap 2: the embedded 26.3 parser folds a literal tuple in an extra pair of
// grouping parentheses, `x IN ((1, 0, '0'))`, into the flat `(1, 0, '0')`,
// which ClickHouse 25.8 / 26.2 read differently (a set of three scalars rather
// than a set of one tuple), so rule N1 refused it. The driver's IN filter on a
// Tuple-encoded BigInt entity field builds exactly that operand. The output
// now keeps the source's grouping (adoptLiteralFolds), so every server reads it
// as it reads the source; it is otherwise the statement's ordinary rewrite,
// the one its `tuple((…))` spelling gets.
TEST(DriverCompat, SingleTupleInOperandKeepsItsGrouping) {
  for (const bool si : {false, true}) {
    for (const auto &[sql, kept] : std::vector<std::pair<std::string, std::string>>{
           {"SELECT id FROM db1.o WHERE x IN ((1,0,'0'))", "x IN ((1, 0, '0'))"},
           {"SELECT id FROM db1.o WHERE x NOT IN ((1,1,'1'))", "x NOT IN ((1, 1, '1'))"},
           {"SELECT id FROM db1.o WHERE x GLOBAL IN ((1, 0))", "x GLOBAL IN ((1, 0))"},
           {"SELECT id FROM db1.o WHERE x GLOBAL NOT IN ((1, 0))", "x GLOBAL NOT IN ((1, 0))"},
           {"SELECT id FROM db1.o WHERE (a, b) IN ((1, 2))", "(a, b) IN ((1, 2))"},
           {"SELECT id FROM db1.o WHERE 1 IN ((1, 2))", "1 IN ((1, 2))"},
           {"SELECT id FROM db1.o WHERE (`x` NOT IN ((1,1,'1')) OR x = (false,0,0)) ORDER BY id LIMIT 10",
            "x NOT IN ((1, 1, '1'))"},
           {"SELECT id FROM db1.o WHERE x IN ((1,0,'0')) AND y IN ((2, 3))", "y IN ((2, 3))"},
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      EXPECT_NE(resp.sql_after_rewrite().find(kept), std::string::npos) << resp.sql_after_rewrite();
      // The same statement spelled with tuple((…)) is rewritten identically
      // but for that spelling.
      std::string spelled = sql;
      for (size_t at = spelled.find("(("); at != std::string::npos; at = spelled.find("((", at + 7))
        spelled.replace(at, 1, "tuple(");
      const auto twin = RunTableRef(spelled, si);
      ASSERT_EQ(twin.code(), rewriter::RewriteCode::Success) << twin.message();
      std::string want = twin.sql_after_rewrite();
      for (size_t at = want.find("tuple(("); at != std::string::npos; at = want.find("tuple((", at))
        want.erase(at, 5);
      EXPECT_EQ(resp.sql_after_rewrite(), want);
    }
  }
}

namespace {

// The request HouseGate sends for a Sentio driver session (report "The
// request"): the ClientHello database is the physical database, so the
// session carries no logical context; delim is empty (the sentio-node chart
// sets none). `si` adds contract V2 with one Active table of another tenant.
rewriter::RewriteSQLRequest DriverSessionRequest(const std::string &sql, bool si) {
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
  (*dyn->mutable_database_map())["hw1kzqe3_0"] = "devnet2";
  dyn->add_known_physical_databases("devnet2");
  for (const char *p : {"devnet2", "hg_safe", "hg_unsafe", "hg_promote"}) dyn->add_protected_databases(p);
  dyn->set_upstream_logical_database_in_context("");
  dyn->set_upstream_physical_database_in_context("devnet2");
  dyn->set_delim("");
  if (si) {
    auto *args = dyn->mutable_storage_integrity();
    args->set_contract_version(rewriter::STORAGE_INTEGRITY_CONTRACT_V2);
    args->set_read_mode(rewriter::StorageIntegrityArgs::READ_MODE_SAFE);
    args->set_reserved_row_id_column("_hg_row_id");
    for (const char *r : {"hg_safe", "hg_unsafe", "hg_promote"}) args->add_reserved_databases(r);
    auto &t = (*args->mutable_tables())["si_db.events"];
    t.set_safe_table("hg_safe.si_db__events");
    t.set_unsafe_table("hg_unsafe.si_db__events");
  }
  return req;
}

struct DriverStatement {
  std::string expect;  // ok | filter | na
  std::string source;  // A<n> | R<n> | X (testdata header)
  std::string sql;
};

std::vector<DriverStatement> LoadDriverStatements() {
  std::ifstream in(std::string(REWRITER_TEST_DATA_DIR) + "/driver_compat_statements.tsv");
  std::vector<DriverStatement> out;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t a = line.find('\t');
    const size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
    if (b == std::string::npos) {
      ADD_FAILURE() << "malformed line: " << line;
      continue;
    }
    out.push_back({line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1)});
  }
  return out;
}

}  // namespace

// Every statement of the driver corpus (tests/testdata/driver_compat_statements.tsv)
// through Rewrite with the driver session's request, SI off and on. All must
// answer Success except the external-table filter, a known deferred
// limitation (the session has no logical context, and the external table's
// blocks arrive after the Query packet), and the four statements the driver
// cannot reach today, each pinned to its current answer.
TEST(DriverCompat, EveryDriverStatementIsAccepted) {
  const auto statements = LoadDriverStatements();
  ASSERT_EQ(statements.size(), 1083u);
  std::map<std::string, size_t> by_expect;
  for (const auto &st : statements) ++by_expect[st.expect];
  EXPECT_EQ(by_expect["ok"], 1035u);
  EXPECT_EQ(by_expect["filter"], 44u);
  EXPECT_EQ(by_expect["na"], 4u);

  const std::regex filter_table("FROM `(filter_[0-9a-f]{16})`");
  const std::regex single_tuple_in("IN \\(\\([^()]*\\)\\)");
  size_t grouped_in = 0;
  for (const bool si : {false, true}) {
    for (const auto &st : statements) {
      SCOPED_TRACE(st.source + (si ? " [V2] " : " [inactive] ") + st.sql.substr(0, 200));
      const auto resp = RunDirectRewrite(DriverSessionRequest(st.sql, si));
      if (st.expect == "filter") {
        std::smatch m;
        ASSERT_TRUE(std::regex_search(st.sql, m, filter_table));
        EXPECT_EQ(resp.code(), rewriter::RewriteCode::InvalidRewriteRequest) << resp.sql_after_rewrite();
        EXPECT_EQ(resp.message(), UnresolvedUnqualifiedMessage(m[1].str()));
        continue;
      }
      if (st.source == "A971") {
        EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
        EXPECT_EQ(resp.message(), "table engine ReplicatedMergeTree is not accepted");
        continue;
      }
      if (st.source == "A972") {
        EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
        EXPECT_EQ(resp.message(), si ? kSIUnmodelled : "statement is not supported");
        continue;
      }
      ASSERT_TRUE(st.expect == "ok" || st.source == "A973" || st.source == "A974") << st.expect;
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
      if (resp.code() != rewriter::RewriteCode::Success) continue;
      // The output is a statement the embedded parser reads back.
      const std::string &out = resp.sql_after_rewrite();
      try {
        DB::ParserQuery parser(out.data() + out.size(), false);
        DB::parseQuery(parser, out.data(), out.data() + out.size(), /*description=*/"", 0, 0, 0);
      } catch (const DB::Exception &e) {
        ADD_FAILURE() << "output does not parse: " << e.displayText() << " | " << out;
      }
      // Gap 2: a single-tuple IN operand keeps its grouping parentheses.
      if (std::regex_search(st.sql, single_tuple_in)) {
        ++grouped_in;
        EXPECT_TRUE(std::regex_search(out, single_tuple_in)) << out;
      }
    }
  }
  EXPECT_EQ(grouped_in, 8u);  // 4 statements, both SI states
}

// Fix round 2 (re-review R1-N-1): a statement may preserve at most
// kMaxLiteralFolds grouped literal tuples; one more refuses it.
TEST(TableRefFidelity, LiteralFoldCountIsCapped) {
  const auto statement = [](size_t folds) {
    std::string sql = "SELECT [";
    for (size_t i = 0; i < folds; ++i) sql += i ? ", ((1, 2))" : "((1, 2))";
    return sql + "]";
  };
  for (const bool si : {false, true}) {
    const auto at_cap = RunTableRef(statement(rewriter_handlers::kMaxLiteralFolds), si);
    EXPECT_EQ(at_cap.code(), rewriter::RewriteCode::Success) << at_cap.message();
    const auto over = RunTableRef(statement(rewriter_handlers::kMaxLiteralFolds + 1), si);
    EXPECT_EQ(over.code(), rewriter::RewriteCode::UnsupportedStatement);
    EXPECT_EQ(over.message(), rewriter_handlers::kUnsupportedStatementMessage);
  }
}

// Re-review 1 (pre-existing): `NOT (expr) <operator>` is `not(expr) <op>` to
// ClickHouse 25.8 / 26.2 (a call) and `NOT (expr <op> …)` to the embedded
// 26.3 parser, which re-emits that grouping. Measured on 25.8, 26.2 and 26.3
// (a in 1..3): `WHERE NOT (a) = 1` returns no row on 25.8 / 26.2 and the
// output `NOT (a = 1)` rows 2 and 3; `NOT (p).1` is error 43 there and a value
// for the output. Refused in both SI states; NOT (…) that ends its
// expression, and plain `NOT a = 1`, keep their meaning and are admitted.
TEST(TableRefFidelity, NotCallPrecedenceDivergenceIsRefused) {
  for (const bool si : {false, true}) {
    for (const std::string sql : {
           "SELECT a FROM db1.o WHERE NOT (a) = 1",
           "SELECT a FROM db1.o WHERE NOT (a) != 1",
           "SELECT a FROM db1.o WHERE NOT (a) < 2",
           "SELECT a FROM db1.o WHERE NOT (a) IN (1)",
           "SELECT a FROM db1.o WHERE NOT (a) NOT IN (1)",
           "SELECT a FROM db1.o WHERE NOT (a) GLOBAL IN (1)",
           "SELECT a FROM db1.o WHERE NOT (a) IN (SELECT 1)",
           "SELECT a FROM db1.o WHERE NOT (a) IS NULL",
           "SELECT a FROM db1.o WHERE NOT (a) IS NOT NULL",
           "SELECT a FROM db1.o WHERE NOT (a) BETWEEN 1 AND 2",
           "SELECT a FROM db1.o WHERE NOT (s) LIKE '%'",
           "SELECT a FROM db1.o WHERE NOT (a) IS NOT DISTINCT FROM 1",
           "SELECT a FROM db1.o WHERE NOT (a) = 1 AND b = 1",
           "SELECT a FROM db1.o WHERE b = 1 AND NOT (a) = 1",
           "SELECT a FROM db1.o WHERE NOT (a) = 1 OR NOT (b) = 1",
           "SELECT a FROM db1.o WHERE NOT NOT (a) = 1",
           "SELECT a FROM db1.o WHERE NOT (a) == 1",
           "SELECT a FROM db1.o WHERE NOT(a) = 1",
           "SELECT a FROM db1.o WHERE not (a) = 1",
           "SELECT a FROM db1.o WHERE NOT /*c*/ (a) = 1",
           "SELECT a FROM db1.o WHERE NOT ((a)) = 1",
           "SELECT a FROM db1.o WHERE NOT (a, b) = (1, 1)",
           "SELECT a FROM db1.o WHERE NOT (SELECT 1) = 1",
           "SELECT NOT (p).1 FROM db1.o",
           "SELECT NOT (arr)[1] FROM db1.o",
           "SELECT NOT (a) || 'x' FROM db1.o",
           "SELECT NOT (a) % 2 FROM db1.o",
           "SELECT NOT (a) - 1 FROM db1.o",
           "SELECT NOT (a) = 1 ? 1 : 0 FROM db1.o",
           "SELECT NOT (a)::UInt8 FROM db1.o",
           "SELECT NOT (a) x FROM db1.o",
           "SELECT a FROM db1.t WHERE NOT (a) = 1",
           "INSERT INTO db1.o SELECT a FROM db1.p WHERE NOT (a) = 1",
           "CREATE VIEW db1.v AS SELECT a FROM db1.o WHERE NOT (a) = 1",
           "ALTER TABLE db1.o DELETE WHERE NOT (a) = 1",
           "DELETE FROM db1.o WHERE NOT (a) = 1",
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      const auto resp = RunTableRef(sql, si);
      EXPECT_EQ(resp.code(), rewriter::RewriteCode::UnsupportedStatement) << resp.sql_after_rewrite();
      EXPECT_EQ(resp.message(), rewriter_handlers::kUnsupportedStatementMessage);
      EXPECT_EQ(resp.sql_after_rewrite(), sql);
    }
    for (const std::string sql : {
           "SELECT a FROM db1.o WHERE NOT (a)",
           "SELECT a FROM db1.o WHERE NOT (a = 1)",
           "SELECT a FROM db1.o WHERE NOT a = 1",
           "SELECT a FROM db1.o WHERE NOT (a) AND b",
           "SELECT a FROM db1.o WHERE NOT (a) OR b = 2",
           "SELECT a FROM db1.o WHERE b AND NOT (a)",
           "SELECT a FROM db1.o WHERE NOT (a) SETTINGS max_threads = 1",
           "SELECT a FROM db1.o WHERE NOT EXISTS (SELECT 1)",
           "SELECT a FROM db1.o WHERE (NOT (a)) = 1",
           "SELECT NOT (a), NOT (a) AS x, [NOT (a)], tuple(NOT (a), 1) FROM db1.o",
           "SELECT NOT (a) ? 5 : 6, b ? NOT (a) : 0 FROM db1.o",
           "SELECT CASE WHEN NOT (a) THEN 1 ELSE NOT (b) END FROM db1.o",
           "SELECT a FROM db1.o ORDER BY NOT (a) DESC, a",
           "SELECT -(a) = -1, -(p).1, -(arr)[1], -(a) * 2 FROM db1.o",
           "SELECT a FROM db1.o WHERE a NOT IN (1) AND s NOT LIKE 'x'",
         }) {
      SCOPED_TRACE(sql + (si ? " [V2]" : " [inactive]"));
      EXPECT_EQ(RunTableRef(sql, si).code(), rewriter::RewriteCode::Success) << RunTableRef(sql, si).message();
    }
  }
}
