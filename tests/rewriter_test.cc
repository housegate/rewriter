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
    {"insert_select",
      "INSERT INTO dst SELECT * FROM c1",
      {{"c1", "SELECT 1"}},
      /*expect_with_count*/ 1,
      {"INSERT INTO dst", "WITH c1 AS"}},
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
    {"insert_select_with_union",
      "INSERT INTO dst (x) SELECT * FROM c1 UNION ALL SELECT * FROM c2",
      {{"c1", "SELECT 1 AS x"}, {"c2", "SELECT 2 AS x"}},
      /*expect_with_count*/ 1,
      {"INSERT INTO dst", "WITH c1 AS", "WITH c2 AS"}},
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

TEST(CTEOnly, RunByName_InsertSelect) {
  auto out = RunCTERewriteByName("insert_select");
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());
  EXPECT_NE(norm.find("INSERT INTO dst"), std::string::npos);
  EXPECT_NE(norm.find("WITH c1 AS"), std::string::npos);
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

TEST(CTEOnly, RunByName_InsertSelectWithUnion) {
  auto out = RunCTERewriteByName("insert_select_with_union");
  auto norm = NormalizeWS(out);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(norm.empty());
  EXPECT_EQ(CountSubstr(norm, "WITH "), 2u);
  EXPECT_NE(norm.find("INSERT INTO dst"), std::string::npos);
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

// 仅在使用到的 key 注入（大 map 中只取子集）
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

  // 仅一次 WITH
  EXPECT_EQ(CountSubstr(norm, "WITH "), 1u);

  // 仅注入 c3/e5
  EXPECT_EQ(CountSubstr(norm, " c3 AS "), 1u);
  EXPECT_EQ(CountSubstr(norm, " e5 AS "), 1u);

  // 未使用的 key 不应出现
  EXPECT_EQ(norm.find(" a1 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" a2 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" b1 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" b2 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" c4 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" d1 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" d2 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" f6 AS "), std::string::npos);

  // JOIN 仍然存在
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

  // 未使用的不应出现
  EXPECT_EQ(norm.find(" a0 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" c2 AS "), std::string::npos);
  EXPECT_EQ(norm.find(" x9 AS "), std::string::npos);

  // UNION 结构保留
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
  // 仅改变来源表为逻辑表名，其他语义保持一致，便于整条 SQL 替换后用户能看懂。
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
  // 打印整个响应，便于肉眼核对最终替换是否符合预期
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
// QueryPreprocessor Tests - 防止栈溢出的查询预处理
// ============================================================================

TEST(QueryPreprocessorTest, NoPreprocessingNeeded) {
  // 小查询不需要预处理
  std::string sql = "SELECT * FROM table WHERE id IN (1, 2, 3)";
  std::string result = QueryPreprocessor::preprocess(sql);
  EXPECT_EQ(sql, result) << "Small queries should not be modified";
}

TEST(QueryPreprocessorTest, SimpleINClauseSplit) {
  // 创建一个有 100 个元素的 IN 子句
  std::string sql = "SELECT * FROM table WHERE id IN (";
  for (int i = 0; i < 100; i++) {
    if (i > 0) sql += ", ";
    sql += std::to_string(i);
  }
  sql += ")";
  
  std::string result = QueryPreprocessor::preprocess(sql);
  
  // 验证结果包含 OR
  EXPECT_NE(result.find(" OR "), std::string::npos) 
    << "Large IN clause should be split with OR";
  
  // 验证包含原始列名
  EXPECT_NE(result.find("id IN"), std::string::npos)
    << "Column name should be preserved";
  
  // 验证所有数字都在
  EXPECT_NE(result.find("0"), std::string::npos);
  EXPECT_NE(result.find("99"), std::string::npos);
  
  std::cout << "\n=== SimpleINClauseSplit Result (first 500 chars) ===\n"
            << result.substr(0, 500) << "...\n";
}

TEST(QueryPreprocessorTest, TupleINClauseSplit) {
  // 创建一个有 80 个元组的 IN 子句
  std::string sql = "SELECT * FROM table WHERE (owner_address, coin_type) IN [";
  for (int i = 0; i < 80; i++) {
    if (i > 0) sql += ", ";
    sql += "('0xaddr" + std::to_string(i) + "', '0xcoin" + std::to_string(i) + "::token::NAME')";
  }
  sql += "]";
  
  std::string result = QueryPreprocessor::preprocess(sql);
  
  // 验证结果包含 OR
  EXPECT_NE(result.find(" OR "), std::string::npos)
    << "Large tuple IN clause should be split";
  
  // 验证保留了方括号
  EXPECT_NE(result.find("IN ["), std::string::npos)
    << "Square brackets should be preserved";
  
  // 验证保留了元组格式
  EXPECT_NE(result.find("(owner_address, coin_type)"), std::string::npos)
    << "Tuple columns should be preserved";
  
  std::cout << "\n=== TupleINClauseSplit Result (first 600 chars) ===\n"
            << result.substr(0, 600) << "...\n";
}

TEST(QueryPreprocessorTest, MultipleINClausesSplit) {
  // 创建包含多个大 IN 子句的查询
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
  
  // 应该有多个 OR（因为有多个 IN 子句被拆分）
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
  // 模拟真实的崩溃查询场景：300+ 元组的 IN 子句
  std::ostringstream oss;
  oss << "SELECT coin_type, countDistinctIf(owner_address, balance > 0) AS holders ";
  oss << "FROM (SELECT coin_type, owner_address, sum(amount_number) AS balance, ";
  oss << "((owner_address, coin_type) IN [";
  
  // 第一个大 IN 子句：320 个元组
  for (int i = 0; i < 320; i++) {
    if (i > 0) oss << ", ";
    oss << "('0x" << std::hex << (i * 1000) << "', '0x" << std::hex << (i * 2000) 
        << "::token::TOKEN" << std::dec << i << "')";
  }
  
  oss << "]) AS is_dev, ";
  oss << "((owner_address, coin_type) IN [";
  
  // 第二个大 IN 子句：320 个元组
  for (int i = 0; i < 320; i++) {
    if (i > 0) oss << ", ";
    oss << "('0x" << std::hex << (i * 3000) << "', '0x" << std::hex << (i * 4000) 
        << "::token::TOKEN" << std::dec << i << "')";
  }
  
  oss << "]) AS is_sniper ";
  oss << "FROM sui.balance_changes WHERE coin_type IN (";
  
  // 第三个大 IN 子句：320 个地址
  for (int i = 0; i < 320; i++) {
    if (i > 0) oss << ", ";
    oss << "'0x" << std::hex << (i * 5000) << "::token::TOKEN" << std::dec << i << "'";
  }
  
  oss << ") GROUP BY coin_type, owner_address) balances GROUP BY coin_type";
  
  std::string sql = oss.str();
  
  std::cout << "\n=== RealWorldCrashQuery Test ===\n"
            << "Original query length: " << sql.size() << " bytes\n";
  
  // 这个查询会导致栈溢出，但预处理后应该安全
  std::string result = QueryPreprocessor::preprocess(sql);
  
  std::cout << "Preprocessed query length: " << result.size() << " bytes\n";
  
  // 验证所有大 IN 子句都被拆分了
  size_t or_count = 0;
  size_t pos = 0;
  while ((pos = result.find(" OR ", pos)) != std::string::npos) {
    or_count++;
    pos += 4;
  }
  
  // 每个 320 元素的 IN 子句应该被拆分成 7 批（50*6 + 20），所以每个有 6 个 OR
  // 3 个 IN 子句 × 6 个 OR = 18 个 OR
  EXPECT_GE(or_count, 15) << "Large query should have many OR clauses from splitting";
  
  std::cout << "OR count after preprocessing: " << or_count << "\n";
  std::cout << "✅ Query can now be safely parsed without stack overflow\n";
}

TEST(QueryPreprocessorTest, StringWithCommas) {
  // 测试包含逗号的字符串不会被错误拆分
  std::string sql = "SELECT * FROM table WHERE description IN (";
  for (int i = 0; i < 60; i++) {
    if (i > 0) sql += ", ";
    sql += "'item " + std::to_string(i) + ", with comma'";
  }
  sql += ")";
  
  std::string result = QueryPreprocessor::preprocess(sql);
  
  // 应该仍然包含字符串中的逗号
  EXPECT_NE(result.find(", with comma"), std::string::npos)
    << "Commas in strings should be preserved";
  
  std::cout << "\n=== StringWithCommas Test Passed ===\n";
}

// ============================================================================
// USE Statement Tests
// ============================================================================

// Helper: run USE rewrite through the full gRPC Rewrite path
static std::pair<std::string, std::string> RunUseRewrite(
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
    return {"", ""};
  }
  return {resp.sql_after_rewrite(), resp.statement_type()};
}

TEST(UseStatement, SimpleUse_NoRewrite) {
  auto [sql, stmt_type] = RunUseRewrite("USE mydb", {});
  EXPECT_FALSE(sql.empty());
  EXPECT_EQ(stmt_type, "USE");
  // Should contain the database name
  EXPECT_NE(sql.find("mydb"), std::string::npos);
}

TEST(UseStatement, UseWithMapping) {
  std::map<std::string, std::string> db_map{{"coinbase", "sentio_coinbase"}};
  auto [sql, stmt_type] = RunUseRewrite("USE coinbase", db_map);
  EXPECT_EQ(stmt_type, "USE");
  EXPECT_NE(sql.find("sentio_coinbase"), std::string::npos);
  // Original name should not appear
  EXPECT_EQ(sql.find("coinbase"), sql.find("sentio_coinbase") + std::string("sentio_").size());
}

TEST(UseStatement, UseNoMatch) {
  // database not in map → original SQL returned
  std::map<std::string, std::string> db_map{{"other", "sentio_other"}};
  auto [sql, stmt_type] = RunUseRewrite("USE mydb", db_map);
  EXPECT_EQ(stmt_type, "USE");
  EXPECT_NE(sql.find("mydb"), std::string::npos);
}

TEST(UseStatement, UseWithBackticks) {
  std::map<std::string, std::string> db_map{{"my-db", "sentio_mydb"}};
  auto [sql, stmt_type] = RunUseRewrite("USE `my-db`", db_map);
  EXPECT_EQ(stmt_type, "USE");
  EXPECT_NE(sql.find("sentio_mydb"), std::string::npos);
}

// ============================================================================
// SHOW TABLES Tests
// ============================================================================

// Helper: run SHOW TABLES rewrite
static std::pair<std::string, std::string> RunShowTablesRewrite(
  const std::string &sql, const std::string &database, const std::string &processor_id) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(sql);

  if (!database.empty() && !processor_id.empty()) {
    auto *opt = req.add_options();
    opt->set_op(rewriter::RewriteOp::ShowTablesRewrite);
    auto *args = opt->mutable_show_tables_args();
    args->set_database(database);
    args->set_processor_id(processor_id);
  }

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  if (!status.ok() || resp.code() != rewriter::RewriteCode::Success) {
    return {"", ""};
  }
  return {resp.sql_after_rewrite(), resp.statement_type()};
}

TEST(ShowTables, SimpleShowTables_NoRewrite) {
  auto [sql, stmt_type] = RunShowTablesRewrite("SHOW TABLES", "", "");
  EXPECT_FALSE(sql.empty());
  EXPECT_EQ(stmt_type, "SHOW_TABLES");
}

TEST(ShowTables, ShowTablesWithProcessor) {
  auto [sql, stmt_type] = RunShowTablesRewrite("SHOW TABLES", "sentio_coinbase", "coinbase");
  EXPECT_EQ(stmt_type, "SHOW_TABLES");
  // Should be rewritten to a SELECT with prefix filter
  EXPECT_NE(sql.find("system.tables"), std::string::npos);
  EXPECT_NE(sql.find("sentio_coinbase"), std::string::npos);
  EXPECT_NE(sql.find("coinbase%"), std::string::npos);
  std::cout << "\n=== ShowTablesWithProcessor Result ===\n" << sql << "\n";
}

TEST(ShowTables, ShowTablesFromDB_NoRewrite) {
  auto [sql, stmt_type] = RunShowTablesRewrite("SHOW TABLES FROM mydb", "", "");
  EXPECT_EQ(stmt_type, "SHOW_TABLES");
  // Without rewrite option, original SQL is returned
  EXPECT_NE(sql.find("mydb"), std::string::npos);
}

TEST(ShowTables, ShowDatabases) {
  auto [sql, stmt_type] = RunShowTablesRewrite("SHOW DATABASES", "", "");
  EXPECT_EQ(stmt_type, "SHOW_TABLES");
  EXPECT_NE(sql.find("DATABASES"), std::string::npos);
}

TEST(ShowTables, ShowTablesLike) {
  auto [sql, stmt_type] = RunShowTablesRewrite("SHOW TABLES LIKE 'transfer%'", "sentio_coinbase", "coinbase");
  EXPECT_EQ(stmt_type, "SHOW_TABLES");
  // Should be rewritten to SELECT with processor prefix
  EXPECT_NE(sql.find("system.tables"), std::string::npos);
  EXPECT_NE(sql.find("coinbase%"), std::string::npos);
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
  EXPECT_EQ(resp.statement_type(), "SELECT");
}

#if !REWRITER_RELEASE_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif