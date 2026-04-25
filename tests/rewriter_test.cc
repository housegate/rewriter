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
    auto *tbl_args = tbl_option->mutable_table_name_args()->mutable_static_args();
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

struct DynamicArgs {
  std::map<std::string, std::string> database_map;
  std::vector<std::string> known_physical_databases;
  std::string upstream_logical_database_in_context;
  std::string delim;  // "" → server defaults to "_"
  std::vector<std::string> extra_arguments;
  // std::nullopt → leave the optional field unset (proxy hasn't picked
  // a physical context yet). Empty string → set to "" (rare but valid).
  std::optional<std::string> upstream_physical_database_in_context;
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
  // No FROM clause; logical comes from upstream context. prefix = "coinbase_".
  auto r = RunDynamic("SHOW TABLES",
                      {.database_map = {{"coinbase", "sentio_coinbase"}},
                       .upstream_logical_database_in_context = "coinbase"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_SHOW_TABLES);
  // Filter targets the physical DB.
  EXPECT_NE(r.sql_after_rewrite.find("database = 'sentio_coinbase'"), std::string::npos);
  // Prefix = logical + default delim "_" (no extra_args).
  EXPECT_NE(r.sql_after_rewrite.find("startsWith(name, 'coinbase_')"), std::string::npos);
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
  EXPECT_NE(r.sql_after_rewrite.find("startsWith(name, 'mydb_')"), std::string::npos);
}

TEST(ShowTables, WithExtraArgs_PrefixIncludesThem) {
  // prefix = logical + delim + extras.join(delim) + delim
  auto r = RunDynamic("SHOW TABLES",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1",
                       .extra_arguments = {"v2", "shard0"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("startsWith(name, 'tenant1_v2_shard0_')"),
            std::string::npos);
}

TEST(ShowTables, CustomDelim) {
  auto r = RunDynamic("SHOW TABLES",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1",
                       .delim = "-",
                       .extra_arguments = {"v2"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("startsWith(name, 'tenant1-v2-')"),
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
  // database_map -> one existence-check subquery per logical, alphabetical
  // by logical name, joined with UNION ALL, wrapped with ORDER BY name.
  auto r = RunDynamic(
    "SHOW DATABASES",
    {.database_map = {{"tenant2", "testnet2"}, {"tenant1", "testnet1"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_SHOW_DATABASES);

  const auto &sql = r.sql_after_rewrite;
  // Per-tenant subquery shape.
  EXPECT_NE(sql.find("SELECT 'tenant1' AS name"), std::string::npos);
  EXPECT_NE(sql.find("SELECT 'tenant2' AS name"), std::string::npos);
  EXPECT_NE(sql.find("database = 'testnet1'"), std::string::npos);
  EXPECT_NE(sql.find("database = 'testnet2'"), std::string::npos);
  EXPECT_NE(sql.find("startsWith(name, 'tenant1_')"), std::string::npos);
  EXPECT_NE(sql.find("startsWith(name, 'tenant2_')"), std::string::npos);
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

TEST(ShowDatabases, DatabaseMap_WithExtraArgs) {
  // extra_arguments thread through buildDynamicTablePrefix.
  auto r = RunDynamic(
    "SHOW DATABASES",
    {.database_map = {{"tenant1", "testnet"}},
     .extra_arguments = {"v2", "shard0"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("startsWith(name, 'tenant1_v2_shard0_')"),
            std::string::npos);
}

TEST(ShowDatabases, DatabaseMap_CustomDelim) {
  auto r = RunDynamic(
    "SHOW DATABASES",
    {.database_map = {{"tenant1", "testnet"}},
     .delim = "-",
     .extra_arguments = {"v2"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("startsWith(name, 'tenant1-v2-')"),
            std::string::npos);
}

TEST(ShowDatabases, KnownPhysicalOnly) {
  // No database_map; only known_physical_databases. The output is a single
  // subquery against system.databases filtered by the IN clause; nothing
  // lands in database_rewrites because logical == physical.
  auto r = RunDynamic(
    "SHOW DATABASES",
    {.known_physical_databases = {"realdb2", "realdb1"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_SHOW_DATABASES);

  const auto &sql = r.sql_after_rewrite;
  EXPECT_NE(sql.find("SELECT name FROM system.databases"), std::string::npos);
  // Names sorted alphabetically inside the IN list.
  EXPECT_NE(sql.find("name IN ('realdb1', 'realdb2')"), std::string::npos);
  EXPECT_EQ(sql.find("system.tables"), std::string::npos);
  EXPECT_EQ(sql.find("UNION ALL"), std::string::npos);
  EXPECT_NE(sql.find("ORDER BY name"), std::string::npos);

  EXPECT_TRUE(r.database_rewrites.empty());
  EXPECT_TRUE(r.table_rewrites.empty());
}

TEST(ShowDatabases, Mixed_DatabaseMapAndKnownPhysical) {
  auto r = RunDynamic(
    "SHOW DATABASES",
    {.database_map = {{"tenant1", "testnet1"}},
     .known_physical_databases = {"realdb1"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  const auto &sql = r.sql_after_rewrite;
  // Both subquery groups present, joined by UNION ALL.
  EXPECT_NE(sql.find("SELECT 'tenant1' AS name"), std::string::npos);
  EXPECT_NE(sql.find("system.databases"), std::string::npos);
  EXPECT_NE(sql.find("UNION ALL"), std::string::npos);
  // Only the database_map entry is recorded.
  EXPECT_EQ(r.database_rewrites["tenant1"], "testnet1");
  EXPECT_EQ(r.database_rewrites.size(), 1u);
}

TEST(ShowDatabases, Like_Pattern) {
  auto r = RunDynamic(
    "SHOW DATABASES LIKE 'tenant%'",
    {.database_map = {{"tenant1", "testnet1"}, {"sysdb", "physsys"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("WHERE name LIKE 'tenant%'"),
            std::string::npos);
}

TEST(ShowDatabases, NotLike_Pattern) {
  auto r = RunDynamic(
    "SHOW DATABASES NOT LIKE 'sys%'",
    {.database_map = {{"tenant1", "testnet1"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("WHERE name NOT LIKE 'sys%'"),
            std::string::npos);
}

TEST(ShowDatabases, Ilike_Pattern) {
  auto r = RunDynamic(
    "SHOW DATABASES ILIKE 'TENANT%'",
    {.database_map = {{"tenant1", "testnet1"}}});
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
    resp.original_accessed_table_names_size(),
  };
}

// --- CREATE TABLE ---

TEST(WriteOp, CreateTable_Simple) {
  auto r = RunWriteOp("CREATE TABLE t (a Int32) ENGINE=Memory");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_TABLE);
  EXPECT_EQ(r.original_accessed_count, 0);
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
  EXPECT_EQ(r.original_accessed_count, 0);
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
  EXPECT_EQ(r.original_accessed_count, 0);
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
  EXPECT_EQ(r.original_accessed_count, 0);
}

TEST(WriteOp, Insert_Select_ReturnsTargetOnly) {
  auto r = RunWriteOp("INSERT INTO db.t SELECT * FROM src");
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_INSERT);
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
  // provides physical. No extra_args -> final name = tenant1_t.
  auto r = RunDynamic("INSERT INTO tenant1.t VALUES (1)",
                      {.database_map = {{"tenant1", "testnet"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_INSERT);
  EXPECT_NE(r.sql_after_rewrite.find("testnet"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("tenant1_t"), std::string::npos);
  EXPECT_EQ(r.table_rewrites["tenant1.t"], "testnet.tenant1_t");
}

TEST(TableRewrite, Insert_UnqualifiedDB_FromUpstreamContext) {
  // `INSERT INTO t` — no SQL qualifier, upstream context fills in.
  auto r = RunDynamic("INSERT INTO t VALUES (1)",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("testnet"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("tenant1_t"), std::string::npos);
  EXPECT_EQ(r.table_rewrites["t"], "testnet.tenant1_t");
}

TEST(TableRewrite, Insert_WithExtraArgs) {
  // extra_args = ["v2", "shard1"] -> name = tenant1_v2_shard1_t.
  auto r = RunDynamic("INSERT INTO t VALUES (1)",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1",
                       .extra_arguments = {"v2", "shard1"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("tenant1_v2_shard1_t"), std::string::npos);
}

TEST(TableRewrite, Insert_CustomDelim) {
  auto r = RunDynamic("INSERT INTO t VALUES (1)",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1",
                       .delim = "-",
                       .extra_arguments = {"v2"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("tenant1-v2-t"), std::string::npos);
}

TEST(TableRewrite, Insert_EmptyExtraArgs_NoDoubleDelim) {
  // Empty extra_args must NOT produce "tenant1__t".
  auto r = RunDynamic("INSERT INTO t VALUES (1)",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.sql_after_rewrite.find("tenant1__t"), std::string::npos)
    << r.sql_after_rewrite;
  EXPECT_NE(r.sql_after_rewrite.find("tenant1_t"), std::string::npos);
}

TEST(TableRewrite, Insert_KnownPhysical_Passthrough) {
  // Logical db is already a known physical -> logical == physical; table
  // name is still prefixed.
  auto r = RunDynamic("INSERT INTO testnet.t VALUES (1)",
                      {.known_physical_databases = {"testnet"}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.testnet_t"), std::string::npos)
    << r.sql_after_rewrite;
  EXPECT_EQ(r.table_rewrites["testnet.t"], "testnet.testnet_t");
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
  EXPECT_NE(r.sql_after_rewrite.find("testnet.tenant1_t"), std::string::npos);
  // FORMAT clause must round-trip — the server uses it to pick the decoder.
  EXPECT_NE(r.sql_after_rewrite.find("FORMAT Native"), std::string::npos);
  EXPECT_EQ(r.table_rewrites["t"], "testnet.tenant1_t");
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
  EXPECT_NE(r.sql_after_rewrite.find("testnet.tenant1_t"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("FORMAT Native"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("a"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("b"), std::string::npos);
  EXPECT_NE(r.sql_after_rewrite.find("c"), std::string::npos);
  EXPECT_EQ(r.table_rewrites["t"], "testnet.tenant1_t");
}

// --- CREATE TABLE ---

TEST(TableRewrite, CreateTable_Basic) {
  auto r = RunDynamic("CREATE TABLE t (a Int32) ENGINE=Memory",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_CREATE_TABLE);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.tenant1_t"), std::string::npos);
  EXPECT_EQ(r.table_rewrites["t"], "testnet.tenant1_t");
}

TEST(TableRewrite, CreateTable_AsSource_AlsoRewritten) {
  // CREATE TABLE dst AS src — both sides rewrite using the same context.
  auto r = RunDynamic("CREATE TABLE dst ENGINE=Memory AS src",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.table_rewrites["dst"], "testnet.tenant1_dst");
  EXPECT_EQ(r.table_rewrites["src"], "testnet.tenant1_src");
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

// --- DROP / ALTER / UPDATE / DELETE ---

TEST(TableRewrite, DropTable_Basic) {
  auto r = RunDynamic("DROP TABLE t",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DROP_TABLE);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.tenant1_t"), std::string::npos);
  EXPECT_EQ(r.table_rewrites["t"], "testnet.tenant1_t");
}

TEST(TableRewrite, AlterTable_Basic) {
  auto r = RunDynamic("ALTER TABLE t ADD COLUMN c Int32",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_ALTER_TABLE);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.tenant1_t"), std::string::npos);
}

TEST(TableRewrite, Update_Basic) {
  auto r = RunDynamic("UPDATE t SET a = 1 WHERE id = 0",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_UPDATE);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.tenant1_t"), std::string::npos);
}

TEST(TableRewrite, Delete_Basic) {
  auto r = RunDynamic("DELETE FROM t WHERE id = 0",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_DELETE);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.tenant1_t"), std::string::npos);
}

// --- RENAME ---

TEST(TableRewrite, Rename_BothSidesRewritten) {
  auto r = RunDynamic("RENAME TABLE a TO b",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_RENAME_TABLE);
  EXPECT_EQ(r.table_rewrites["a"], "testnet.tenant1_a");
  EXPECT_EQ(r.table_rewrites["b"], "testnet.tenant1_b");
}

TEST(TableRewrite, Rename_MultiplePairs) {
  auto r = RunDynamic("RENAME TABLE a TO b, c TO d",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  ASSERT_EQ(r.table_rewrites.size(), 4u);
  EXPECT_EQ(r.table_rewrites["a"], "testnet.tenant1_a");
  EXPECT_EQ(r.table_rewrites["b"], "testnet.tenant1_b");
  EXPECT_EQ(r.table_rewrites["c"], "testnet.tenant1_c");
  EXPECT_EQ(r.table_rewrites["d"], "testnet.tenant1_d");
}

TEST(TableRewrite, Rename_QualifiedSidesUseTheirOwnLogical) {
  // Each side can carry its own qualified logical DB.
  auto r = RunDynamic("RENAME TABLE t1.a TO t2.b",
                      {.database_map = {{"t1", "phys1"}, {"t2", "phys2"}}});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.table_rewrites["t1.a"], "phys1.t1_a");
  EXPECT_EQ(r.table_rewrites["t2.b"], "phys2.t2_b");
}

// --- EXISTS TABLE ---

TEST(TableRewrite, ExistsTable_Basic) {
  auto r = RunDynamic("EXISTS TABLE t",
                      {.database_map = {{"tenant1", "testnet"}},
                       .upstream_logical_database_in_context = "tenant1"});
  EXPECT_EQ(r.code, rewriter::RewriteCode::Success);
  EXPECT_EQ(r.statement_type, rewriter::STATEMENT_TYPE_EXISTS_TABLE);
  EXPECT_NE(r.sql_after_rewrite.find("testnet.tenant1_t"), std::string::npos);
  EXPECT_EQ(r.table_rewrites["t"], "testnet.tenant1_t");
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
  EXPECT_NE(r.sql_after_rewrite.find("testnet.tenant1_t"), std::string::npos);
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

#if !REWRITER_RELEASE_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif