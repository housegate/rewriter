#include "protos/rewriter.grpc.pb.h"
#include "rewriter-server.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <iostream>
#include <map>
#include <regex>
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
// QueryPreprocessor Tests - guard against stack overflow in the query preprocessor.
// ============================================================================

TEST(QueryPreprocessorTest, NoPreprocessingNeeded) {
  // Small queries don't need preprocessing.
  std::string sql = "SELECT * FROM table WHERE id IN (1, 2, 3)";
  std::string result = QueryPreprocessor::preprocess(sql);
  EXPECT_EQ(sql, result) << "Small queries should not be modified";
}

TEST(QueryPreprocessorTest, SimpleINClauseSplit) {
  // Build an IN clause with 100 elements.
  std::string sql = "SELECT * FROM table WHERE id IN (";
  for (int i = 0; i < 100; i++) {
    if (i > 0) sql += ", ";
    sql += std::to_string(i);
  }
  sql += ")";
  
  std::string result = QueryPreprocessor::preprocess(sql);
  
  // Result should contain OR.
  EXPECT_NE(result.find(" OR "), std::string::npos)
    << "Large IN clause should be split with OR";

  // Original column name should be present.
  EXPECT_NE(result.find("id IN"), std::string::npos)
    << "Column name should be preserved";

  // All of the numeric literals should be present.
  EXPECT_NE(result.find("0"), std::string::npos);
  EXPECT_NE(result.find("99"), std::string::npos);
  
  std::cout << "\n=== SimpleINClauseSplit Result (first 500 chars) ===\n"
            << result.substr(0, 500) << "...\n";
}

TEST(QueryPreprocessorTest, TupleINClauseSplit) {
  // Build an IN clause with 80 tuples.
  std::string sql = "SELECT * FROM table WHERE (owner_address, coin_type) IN [";
  for (int i = 0; i < 80; i++) {
    if (i > 0) sql += ", ";
    sql += "('0xaddr" + std::to_string(i) + "', '0xcoin" + std::to_string(i) + "::token::NAME')";
  }
  sql += "]";
  
  std::string result = QueryPreprocessor::preprocess(sql);
  
  // Result should contain OR.
  EXPECT_NE(result.find(" OR "), std::string::npos)
    << "Large tuple IN clause should be split";

  // Square brackets should be preserved.
  EXPECT_NE(result.find("IN ["), std::string::npos)
    << "Square brackets should be preserved";

  // Tuple shape on the LHS should be preserved.
  EXPECT_NE(result.find("(owner_address, coin_type)"), std::string::npos)
    << "Tuple columns should be preserved";
  
  std::cout << "\n=== TupleINClauseSplit Result (first 600 chars) ===\n"
            << result.substr(0, 600) << "...\n";
}

TEST(QueryPreprocessorTest, MultipleINClausesSplit) {
  // Build a query containing several large IN clauses.
  std::string sql = "SELECT * FROM table WHERE id IN (";
  for (int i = 0; i < 70; i++) {
    if (i > 0) sql += ", ";
    sql += std::to_string(i);
  }
  sql += ") AND status IN (";
  for (int i = 0; i < 60; i++) {
    if (i > 0) sql += ", ";
    sql += "'status" + std::to_string(i) + "'";
  }
  sql += ")";
  
  std::string result = QueryPreprocessor::preprocess(sql);
  
  // Multiple ORs expected (multiple IN clauses got split).
  size_t or_count = 0;
  size_t pos = 0;
  while ((pos = result.find(" OR ", pos)) != std::string::npos) {
    or_count++;
    pos += 4;
  }
  
  EXPECT_GT(or_count, 1) << "Multiple IN clauses should be split";
  
  std::cout << "\n=== MultipleINClausesSplit Result ===\n"
            << "Original length: " << sql.size() << " bytes\n"
            << "Result length: " << result.size() << " bytes\n"
            << "OR count: " << or_count << "\n";
}

TEST(QueryPreprocessorTest, RealWorldCrashQuery) {
  // Simulate a real-world crashing query: 300+ tuple IN clauses.
  std::ostringstream oss;
  oss << "SELECT coin_type, countDistinctIf(owner_address, balance > 0) AS holders ";
  oss << "FROM (SELECT coin_type, owner_address, sum(amount_number) AS balance, ";
  oss << "((owner_address, coin_type) IN [";
  
  // First large IN clause: 320 tuples.
  for (int i = 0; i < 320; i++) {
    if (i > 0) oss << ", ";
    oss << "('0x" << std::hex << (i * 1000) << "', '0x" << std::hex << (i * 2000) 
        << "::token::TOKEN" << std::dec << i << "')";
  }
  
  oss << "]) AS is_dev, ";
  oss << "((owner_address, coin_type) IN [";
  
  // Second large IN clause: 320 tuples.
  for (int i = 0; i < 320; i++) {
    if (i > 0) oss << ", ";
    oss << "('0x" << std::hex << (i * 3000) << "', '0x" << std::hex << (i * 4000) 
        << "::token::TOKEN" << std::dec << i << "')";
  }
  
  oss << "]) AS is_sniper ";
  oss << "FROM sui.balance_changes WHERE coin_type IN (";
  
  // Third large IN clause: 320 addresses.
  for (int i = 0; i < 320; i++) {
    if (i > 0) oss << ", ";
    oss << "'0x" << std::hex << (i * 5000) << "::token::TOKEN" << std::dec << i << "'";
  }
  
  oss << ") GROUP BY coin_type, owner_address) balances GROUP BY coin_type";
  
  std::string sql = oss.str();
  
  std::cout << "\n=== RealWorldCrashQuery Test ===\n"
            << "Original query length: " << sql.size() << " bytes\n";
  
  // This query would otherwise overflow the parser stack, but the preprocessor splits it first.
  std::string result = QueryPreprocessor::preprocess(sql);
  
  std::cout << "Preprocessed query length: " << result.size() << " bytes\n";
  
  // Every large IN clause should be split.
  size_t or_count = 0;
  size_t pos = 0;
  while ((pos = result.find(" OR ", pos)) != std::string::npos) {
    or_count++;
    pos += 4;
  }
  
  // Each 320-element IN clause splits into 7 batches (50*6 + 20), giving 6 ORs each.
  // 3 IN clauses x 6 ORs = 18 ORs.
  EXPECT_GE(or_count, 15) << "Large query should have many OR clauses from splitting";
  
  std::cout << "OR count after preprocessing: " << or_count << "\n";
  std::cout << "✅ Query can now be safely parsed without stack overflow\n";
}

TEST(QueryPreprocessorTest, StringWithCommas) {
  // Commas inside quoted strings should NOT be treated as delimiters.
  std::string sql = "SELECT * FROM table WHERE description IN (";
  for (int i = 0; i < 60; i++) {
    if (i > 0) sql += ", ";
    sql += "'item " + std::to_string(i) + ", with comma'";
  }
  sql += ")";
  
  std::string result = QueryPreprocessor::preprocess(sql);
  
  // Commas inside string literals should survive the split.
  EXPECT_NE(result.find(", with comma"), std::string::npos)
    << "Commas in strings should be preserved";
  
  std::cout << "\n=== StringWithCommas Test Passed ===\n";
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
  // SHOW DATABASES handler — it still passes through with the existing
  // STATEMENT_TYPE_SHOW_TABLES classification.
  auto r = RunDynamic("SHOW DICTIONARIES",
                      {.database_map = {{"tenant1", "testnet1"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_SHOW_TABLES);
  EXPECT_NE(r.sql_after_rewrite.find("DICTIONARIES"), std::string::npos);
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

TEST(WriteOp, CreateTable_AsSelect_ReturnsTargetOnly) {
  auto r = RunWriteOp("CREATE TABLE t ENGINE=Memory AS SELECT * FROM src");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_TABLE);
  // Only the CREATE TABLE target — writes.cc does not walk the AS SELECT
  // subquery into rewriteOneTarget, so `src` is not recorded here.
  EXPECT_EQ(r.original_accessed_count, 1);
}

TEST(WriteOp, CreateView_Rejected) {
  auto r = RunWriteOp("CREATE VIEW v AS SELECT 1");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

TEST(WriteOp, CreateMaterializedView_Rejected) {
  auto r = RunWriteOp("CREATE MATERIALIZED VIEW mv ENGINE=Memory AS SELECT 1 AS a");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
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

TEST(WriteOp, DropView_Rejected) {
  auto r = RunWriteOp("DROP VIEW v");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
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

TEST(WriteOp, Insert_Select_ReturnsTargetOnly) {
  auto r = RunWriteOp("INSERT INTO db.t SELECT * FROM src");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_INSERT);
  // Only the INSERT target (db.t). writes.cc doesn't walk the SELECT subquery
  // into rewriteOneTarget, so `src` doesn't get its own AccessedTable entry.
  EXPECT_EQ(r.original_accessed_count, 1);
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

#if !REWRITER_RELEASE_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif