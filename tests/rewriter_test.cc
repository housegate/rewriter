#include "protos/rewriter.grpc.pb.h"
#include "rewriter-server.h"
#include <gtest/gtest.h>
#include <iostream>
#include <map>
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
  std::vector<std::string> names(
    resp.original_accessed_table_names().begin(), resp.original_accessed_table_names().end());
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
    auto *tbl_args = tbl_option->mutable_table_name_args();
    for (const auto &kv : table_map) { (*tbl_args->mutable_table_map())[kv.first] = kv.second; }
  }

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  if (!status.ok() || resp.code() != rewriter::RewriteCode::Success) {
    return {"", {}}; // or throw
  }
  std::vector<std::string> names(
    resp.original_accessed_table_names().begin(), resp.original_accessed_table_names().end());
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
    auto *tbl_args = tbl_option->mutable_table_name_args();
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
    auto *args = opt->mutable_table_name_args();
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
    auto *args = opt->mutable_table_name_args();
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
// USE Statement Tests
// ============================================================================

// Helper: run USE rewrite through the full gRPC Rewrite path
static std::pair<std::string, rewriter::StatementType> RunUseRewrite(
  const std::string &sql, const std::map<std::string, std::string> &db_map) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);

  if (!db_map.empty()) {
    auto *opt = req.add_options();
    opt->set_op(rewriter::RewriteOp::DatabaseNameRewrite);
    auto *args = opt->mutable_database_name_args();
    for (const auto &kv : db_map) {
      (*args->mutable_database_map())[kv.first] = kv.second;
    }
  }

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  if (!status.ok() || resp.code() != rewriter::RewriteCode::Success) {
    return {"", rewriter::STATEMENT_TYPE_UNSPECIFIED};
  }
  return {resp.sql_after_rewrite(), resp.statement_type()};
}

TEST(UseStatement, SimpleUse_NoRewrite) {
  auto [sql, stmt_type] = RunUseRewrite("USE mydb", {});
  EXPECT_FALSE(sql.empty());
  EXPECT_EQ(stmt_type, rewriter::STATEMENT_TYPE_USE);
  // Should contain the database name
  EXPECT_NE(sql.find("mydb"), std::string::npos);
}

TEST(UseStatement, UseWithMapping) {
  std::map<std::string, std::string> db_map{{"coinbase", "sentio_coinbase"}};
  auto [sql, stmt_type] = RunUseRewrite("USE coinbase", db_map);
  EXPECT_EQ(stmt_type, rewriter::STATEMENT_TYPE_USE);
  EXPECT_NE(sql.find("sentio_coinbase"), std::string::npos);
  // Original name should not appear
  EXPECT_EQ(sql.find("coinbase"), sql.find("sentio_coinbase") + std::string("sentio_").size());
}

TEST(UseStatement, UseNoMatch) {
  // database not in map → original SQL returned
  std::map<std::string, std::string> db_map{{"other", "sentio_other"}};
  auto [sql, stmt_type] = RunUseRewrite("USE mydb", db_map);
  EXPECT_EQ(stmt_type, rewriter::STATEMENT_TYPE_USE);
  EXPECT_NE(sql.find("mydb"), std::string::npos);
}

TEST(UseStatement, UseWithBackticks) {
  std::map<std::string, std::string> db_map{{"my-db", "sentio_mydb"}};
  auto [sql, stmt_type] = RunUseRewrite("USE `my-db`", db_map);
  EXPECT_EQ(stmt_type, rewriter::STATEMENT_TYPE_USE);
  EXPECT_NE(sql.find("sentio_mydb"), std::string::npos);
}

// ============================================================================
// SHOW TABLES Tests
// ============================================================================

// Helper: run SHOW TABLES rewrite.
// `database` maps to physical_database, `processor_id` maps to logical_database.
// The physical_database == db branch fires, using physical as the queried DB
// and logical as the name prefix — equivalent to the legacy (database,
// processor_id) pair.
static std::pair<std::string, rewriter::StatementType> RunShowTablesRewrite(
  const std::string &sql, const std::string &database, const std::string &processor_id) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);

  if (!database.empty() && !processor_id.empty()) {
    auto *opt = req.add_options();
    opt->set_op(rewriter::RewriteOp::ShowTablesRewrite);
    auto *args = opt->mutable_show_tables_args();
    args->set_physical_database(database);
    args->set_logical_database(processor_id);
  }

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  if (!status.ok() || resp.code() != rewriter::RewriteCode::Success) {
    return {"", rewriter::STATEMENT_TYPE_UNSPECIFIED};
  }
  return {resp.sql_after_rewrite(), resp.statement_type()};
}

TEST(ShowTables, SimpleShowTables_NoRewrite) {
  auto [sql, stmt_type] = RunShowTablesRewrite("SHOW TABLES", "", "");
  EXPECT_FALSE(sql.empty());
  EXPECT_EQ(stmt_type, rewriter::STATEMENT_TYPE_SHOW_TABLES);
}

TEST(ShowTables, ShowTablesWithProcessor) {
  auto [sql, stmt_type] = RunShowTablesRewrite("SHOW TABLES", "sentio_coinbase", "coinbase");
  EXPECT_EQ(stmt_type, rewriter::STATEMENT_TYPE_SHOW_TABLES);
  // Rewritten to a SELECT over system.tables with startsWith-based prefix filter.
  EXPECT_NE(sql.find("system.tables"), std::string::npos);
  EXPECT_NE(sql.find("sentio_coinbase"), std::string::npos);
  EXPECT_NE(sql.find("startsWith(name, 'coinbase')"), std::string::npos);
  // Outer multiIf strips the prefix; inner subquery does the filtering so the
  // WHERE clause isn't shadowed by the aliased name column.
  EXPECT_NE(sql.find("multiIf"), std::string::npos);
  EXPECT_NE(sql.find("FROM (SELECT name FROM system.tables"), std::string::npos);
  std::cout << "\n=== ShowTablesWithProcessor Result ===\n" << sql << "\n";
}

TEST(ShowTables, ShowTablesWithSpecialCharProcessor) {
  // Processor ID with special characters — only single-quote escaping applies
  // now (LIKE escaping was removed when we switched to startsWith).
  auto [sql, stmt_type] = RunShowTablesRewrite("SHOW TABLES", "sentio_special", "proc_test%\\");
  EXPECT_EQ(stmt_type, rewriter::STATEMENT_TYPE_SHOW_TABLES);
  // startsWith uses exact-string match, so the processor_id appears verbatim
  // inside a single-quoted literal (no LIKE-metachar escaping).
  EXPECT_NE(sql.find("startsWith(name, 'proc_test%\\')"), std::string::npos);
  std::cout << "\n=== ShowTablesWithSpecialCharProcessor Result ===\n" << sql << "\n";
}

TEST(ShowTables, ShowTablesFromDB_NoRewrite) {
  auto [sql, stmt_type] = RunShowTablesRewrite("SHOW TABLES FROM mydb", "", "");
  EXPECT_EQ(stmt_type, rewriter::STATEMENT_TYPE_SHOW_TABLES);
  // Without rewrite option, original SQL is returned
  EXPECT_NE(sql.find("mydb"), std::string::npos);
}

TEST(ShowTables, ShowDatabases) {
  auto [sql, stmt_type] = RunShowTablesRewrite("SHOW DATABASES", "", "");
  EXPECT_EQ(stmt_type, rewriter::STATEMENT_TYPE_SHOW_TABLES);
  EXPECT_NE(sql.find("DATABASES"), std::string::npos);
}

TEST(ShowTables, ShowTablesLike) {
  auto [sql, stmt_type] = RunShowTablesRewrite("SHOW TABLES LIKE 'transfer%'", "sentio_coinbase", "coinbase");
  EXPECT_EQ(stmt_type, rewriter::STATEMENT_TYPE_SHOW_TABLES);
  // Input LIKE is intentionally ignored — we always filter by processor prefix.
  EXPECT_NE(sql.find("system.tables"), std::string::npos);
  EXPECT_NE(sql.find("startsWith(name, 'coinbase')"), std::string::npos);
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
  std::string write_target_table;
  std::string sql_after_rewrite;
  std::string message;
  int original_accessed_count;
};

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
    resp.write_stmt_value().target_table(),
    resp.sql_after_rewrite(),
    resp.message(),
    resp.original_accessed_table_names_size(),
  };
}

// --- CREATE TABLE ---

TEST(WriteOp, CreateTable_Simple) {
  auto r = RunWriteOp("CREATE TABLE t (a Int32) ENGINE=Memory");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_TABLE);
  EXPECT_EQ(r.write_target_table, "t");
  EXPECT_EQ(r.original_accessed_count, 0);
}

TEST(WriteOp, CreateTable_WithDatabase) {
  auto r = RunWriteOp("CREATE TABLE db.t (a Int32) ENGINE=Memory");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_TABLE);
  EXPECT_EQ(r.write_target_table, "db.t");
}

TEST(WriteOp, CreateTable_AsSelect_ReturnsTargetOnly) {
  auto r = RunWriteOp("CREATE TABLE t ENGINE=Memory AS SELECT * FROM src");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_TABLE);
  EXPECT_EQ(r.write_target_table, "t");
  EXPECT_EQ(r.original_accessed_count, 0);
}

TEST(WriteOp, CreateView_Rejected) {
  auto r = RunWriteOp("CREATE VIEW v AS SELECT 1");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
  EXPECT_TRUE(r.write_target_table.empty());
}

TEST(WriteOp, CreateMaterializedView_Rejected) {
  auto r = RunWriteOp("CREATE MATERIALIZED VIEW mv ENGINE=Memory AS SELECT 1 AS a");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

TEST(WriteOp, CreateDatabase_Rejected) {
  auto r = RunWriteOp("CREATE DATABASE d");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

// --- DROP TABLE ---

TEST(WriteOp, DropTable_Simple) {
  auto r = RunWriteOp("DROP TABLE t");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DROP_TABLE);
  EXPECT_EQ(r.write_target_table, "t");
  EXPECT_EQ(r.original_accessed_count, 0);
}

TEST(WriteOp, DropTable_WithDatabase) {
  auto r = RunWriteOp("DROP TABLE db.t");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DROP_TABLE);
  EXPECT_EQ(r.write_target_table, "db.t");
}

TEST(WriteOp, DropTable_IfExists) {
  auto r = RunWriteOp("DROP TABLE IF EXISTS t");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DROP_TABLE);
  EXPECT_EQ(r.write_target_table, "t");
}

TEST(WriteOp, DropTable_MultiTable_Rejected) {
  auto r = RunWriteOp("DROP TABLE t1, t2");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

TEST(WriteOp, DropDatabase_Rejected) {
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

TEST(WriteOp, Truncate_Rejected) {
  auto r = RunWriteOp("TRUNCATE TABLE t");
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

// --- ALTER TABLE ---

TEST(WriteOp, AlterTable_AddColumn) {
  auto r = RunWriteOp("ALTER TABLE t ADD COLUMN c Int32");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_ALTER_TABLE);
  EXPECT_EQ(r.write_target_table, "t");
}

TEST(WriteOp, AlterTable_Update) {
  // ALTER TABLE ... UPDATE is a mutation, not a SQL-standard UPDATE.
  auto r = RunWriteOp("ALTER TABLE t UPDATE a = 1 WHERE id = 0");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_ALTER_TABLE);
  EXPECT_EQ(r.write_target_table, "t");
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
  EXPECT_EQ(r.write_target_table, "t");
  EXPECT_EQ(r.original_accessed_count, 0);
}

TEST(WriteOp, Insert_Select_ReturnsTargetOnly) {
  auto r = RunWriteOp("INSERT INTO db.t SELECT * FROM src");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_INSERT);
  EXPECT_EQ(r.write_target_table, "db.t");
  // The SELECT source (src) is NOT reported.
  EXPECT_EQ(r.original_accessed_count, 0);
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
  EXPECT_EQ(r.write_target_table, "t");
}

TEST(WriteOp, Update_WithDatabase) {
  auto r = RunWriteOp("UPDATE db.t SET a = 1 WHERE id = 0");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_UPDATE);
  EXPECT_EQ(r.write_target_table, "db.t");
}

TEST(WriteOp, Delete_Simple) {
  auto r = RunWriteOp("DELETE FROM t WHERE id = 0");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DELETE);
  EXPECT_EQ(r.write_target_table, "t");
}

TEST(WriteOp, Delete_WithDatabase) {
  auto r = RunWriteOp("DELETE FROM db.t WHERE id = 0");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DELETE);
  EXPECT_EQ(r.write_target_table, "db.t");
}

// --- Other write-ish statements rejected by getQueryKind() catch-all ---

TEST(WriteOp, Rename_Accepted) {
  auto r = RunWriteOp("RENAME TABLE a TO b");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_RENAME_TABLE);
  // No single target_table for RENAME — callers consult table_rewrites.
  EXPECT_TRUE(r.write_target_table.empty());
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
// TableNameRewrite coverage for non-SELECT statements
// ============================================================================

namespace {

// What a caller typically wants to assert about a single-statement response
// after a TableNameRewrite option was applied.
struct RewriteProbe {
  rewriter::RewriteCode code;
  rewriter::StatementType statement_type;
  std::string sql_after_rewrite;
  std::string message;
  std::string write_target_table;
  std::map<std::string, std::string> table_rewrites;
};

struct NameMaps {
  std::map<std::string, std::string> table_map;
  std::map<std::string, std::pair<std::string, std::string>> remote_map; // key -> {addr, db.table}
  std::map<std::string, std::pair<std::string, std::string>> with_db_map; // key -> {db, table}
};

RewriteProbe RunWithMaps(const std::string &sql, const NameMaps &maps) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);

  if (!maps.table_map.empty() || !maps.remote_map.empty() || !maps.with_db_map.empty()) {
    auto *opt = req.add_options();
    opt->set_op(rewriter::RewriteOp::TableNameRewrite);
    auto *args = opt->mutable_table_name_args();
    for (const auto &kv : maps.table_map) {
      (*args->mutable_table_map())[kv.first] = kv.second;
    }
    for (const auto &kv : maps.remote_map) {
      auto &r = (*args->mutable_remote_table_map())[kv.first];
      r.set_addr(kv.second.first);
      // Split "db.table" once; if no dot, all goes to table.
      const auto dot = kv.second.second.find('.');
      if (dot == std::string::npos) {
        r.set_table(kv.second.second);
      } else {
        r.set_database(kv.second.second.substr(0, dot));
        r.set_table(kv.second.second.substr(dot + 1));
      }
    }
    for (const auto &kv : maps.with_db_map) {
      auto &t = (*args->mutable_table_with_database_map())[kv.first];
      t.set_database(kv.second.first);
      t.set_table(kv.second.second);
    }
  }

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  EXPECT_TRUE(status.ok()) << "gRPC status not OK for: " << sql;

  RewriteProbe out;
  out.code = resp.code();
  out.statement_type = resp.statement_type();
  out.sql_after_rewrite = resp.sql_after_rewrite();
  out.message = resp.message();
  out.write_target_table = resp.write_stmt_value().target_table();
  for (const auto &kv : resp.table_rewrites()) {
    out.table_rewrites[kv.first] = kv.second;
  }
  return out;
}

} // namespace

// --- INSERT ---

TEST(TableRewrite, Insert_TableMap_Hit) {
  auto r = RunWithMaps("INSERT INTO t VALUES (1)",
                       {.table_map = {{"t", "t_v2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_INSERT);
  EXPECT_EQ(r.write_target_table, "t_v2");
  ASSERT_EQ(r.table_rewrites.size(), 1u);
  EXPECT_EQ(r.table_rewrites["t"], "t_v2");
}

TEST(TableRewrite, Insert_TableMap_HitQualified) {
  auto r = RunWithMaps("INSERT INTO mydb.t VALUES (1)",
                       {.table_map = {{"mydb.t", "t_v2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  // DB preserved, short name swapped. Full target is mydb.t_v2.
  EXPECT_EQ(r.write_target_table, "mydb.t_v2");
  EXPECT_EQ(r.table_rewrites["mydb.t"], "mydb.t_v2");
}

TEST(TableRewrite, Insert_TableMap_Miss) {
  auto r = RunWithMaps("INSERT INTO t VALUES (1)",
                       {.table_map = {{"other", "other_v2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.write_target_table, "t");
  EXPECT_TRUE(r.table_rewrites.empty());
}

TEST(TableRewrite, Insert_WithDatabaseMap_Hit) {
  auto r = RunWithMaps("INSERT INTO t VALUES (1)",
                       {.with_db_map = {{"t", {"physdb", "t_v2"}}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.write_target_table, "physdb.t_v2");
  EXPECT_EQ(r.table_rewrites["t"], "physdb.t_v2");
}

TEST(TableRewrite, Insert_RemoteMap_Rejected) {
  auto r = RunWithMaps(
    "INSERT INTO t VALUES (1)",
    {.remote_map = {{"t", {"host:9000", "db.t_v2"}}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
  EXPECT_NE(r.message.find("remote_table_map"), std::string::npos);
}

// --- CREATE TABLE ---

TEST(TableRewrite, CreateTable_TableMap_Hit) {
  auto r = RunWithMaps("CREATE TABLE t (a Int32) ENGINE=Memory",
                       {.table_map = {{"t", "t_v2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.write_target_table, "t_v2");
  EXPECT_EQ(r.table_rewrites["t"], "t_v2");
}

TEST(TableRewrite, CreateTable_AsSource_Rewritten) {
  auto r = RunWithMaps("CREATE TABLE dst ENGINE=Memory AS src",
                       {.table_map = {{"dst", "dst_v2"}, {"src", "src_v2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.write_target_table, "dst_v2");
  EXPECT_EQ(r.table_rewrites["dst"], "dst_v2");
  EXPECT_EQ(r.table_rewrites["src"], "src_v2");
}

TEST(TableRewrite, CreateTable_RemoteMap_Rejected) {
  auto r = RunWithMaps(
    "CREATE TABLE t (a Int32) ENGINE=Memory",
    {.remote_map = {{"t", {"host:9000", "db.t_v2"}}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
}

// --- DROP / ALTER / UPDATE / DELETE ---

TEST(TableRewrite, DropTable_TableMap_Hit) {
  auto r = RunWithMaps("DROP TABLE t", {.table_map = {{"t", "t_v2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.write_target_table, "t_v2");
  EXPECT_EQ(r.table_rewrites["t"], "t_v2");
}

TEST(TableRewrite, AlterTable_TableMap_Hit) {
  auto r = RunWithMaps("ALTER TABLE t ADD COLUMN c Int32",
                       {.table_map = {{"t", "t_v2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.write_target_table, "t_v2");
  EXPECT_EQ(r.table_rewrites["t"], "t_v2");
}

TEST(TableRewrite, Update_TableMap_Hit) {
  auto r = RunWithMaps("UPDATE t SET a = 1 WHERE id = 0",
                       {.table_map = {{"t", "t_v2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.write_target_table, "t_v2");
  EXPECT_EQ(r.table_rewrites["t"], "t_v2");
}

TEST(TableRewrite, Delete_TableMap_Hit) {
  auto r = RunWithMaps("DELETE FROM t WHERE id = 0",
                       {.table_map = {{"t", "t_v2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.write_target_table, "t_v2");
  EXPECT_EQ(r.table_rewrites["t"], "t_v2");
}

// --- RENAME ---

TEST(TableRewrite, Rename_BothSidesRewritten) {
  auto r = RunWithMaps("RENAME TABLE a TO b",
                       {.table_map = {{"a", "a_v2"}, {"b", "b_v2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_RENAME_TABLE);
  EXPECT_TRUE(r.write_target_table.empty());
  ASSERT_EQ(r.table_rewrites.size(), 2u);
  EXPECT_EQ(r.table_rewrites["a"], "a_v2");
  EXPECT_EQ(r.table_rewrites["b"], "b_v2");
}

TEST(TableRewrite, Rename_MultiplePairs) {
  auto r = RunWithMaps("RENAME TABLE a TO b, c TO d",
                       {.table_map = {{"a", "a_v2"}, {"c", "c_v2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_RENAME_TABLE);
  EXPECT_EQ(r.table_rewrites["a"], "a_v2");
  EXPECT_EQ(r.table_rewrites["c"], "c_v2");
  // b, d unmapped — not in table_rewrites.
  EXPECT_EQ(r.table_rewrites.count("b"), 0u);
  EXPECT_EQ(r.table_rewrites.count("d"), 0u);
}

TEST(TableRewrite, Rename_RemoteMap_Rejected) {
  auto r = RunWithMaps(
    "RENAME TABLE a TO b",
    {.remote_map = {{"a", {"host:9000", "db.a_v2"}}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
}

// --- EXISTS TABLE ---

TEST(TableRewrite, ExistsTable_Accepted) {
  auto r = RunWithMaps("EXISTS TABLE t", {});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_EXISTS_TABLE);
}

TEST(TableRewrite, ExistsTable_TableMap_Hit) {
  auto r = RunWithMaps("EXISTS TABLE t", {.table_map = {{"t", "t_v2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_EXISTS_TABLE);
  EXPECT_EQ(r.table_rewrites["t"], "t_v2");
}

TEST(TableRewrite, ExistsTable_RemoteMap_Rejected) {
  auto r = RunWithMaps(
    "EXISTS TABLE t", {.remote_map = {{"t", {"host:9000", "db.t_v2"}}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::InvalidRewriteRequest);
}

TEST(TableRewrite, ExistsDatabase_Rejected) {
  auto r = RunWithMaps("EXISTS DATABASE d", {});
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

// --- SHOW CREATE TABLE ---

TEST(TableRewrite, ShowCreateTable_Accepted) {
  auto r = RunWithMaps("SHOW CREATE TABLE t", {});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_SHOW_CREATE_TABLE);
}

TEST(TableRewrite, ShowCreateTable_TableMap_Hit) {
  auto r = RunWithMaps("SHOW CREATE TABLE t", {.table_map = {{"t", "t_v2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_SHOW_CREATE_TABLE);
  EXPECT_EQ(r.table_rewrites["t"], "t_v2");
}

TEST(TableRewrite, ShowCreateDatabase_Rejected) {
  auto r = RunWithMaps("SHOW CREATE DATABASE d", {});
  EXPECT_EQ(r.code, rewriter::RewriteCode::UnsupportedStatement);
}

// --- SELECT still populates table_rewrites ---

TEST(TableRewrite, Select_PopulatesTableRewrites) {
  auto r = RunWithMaps("SELECT * FROM t", {.table_map = {{"t", "t_v2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_SELECT);
  EXPECT_EQ(r.table_rewrites["t"], "t_v2");
}

#if !REWRITER_RELEASE_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif