#include <gtest/gtest.h>
#include "protos/rewriter.grpc.pb.h"
#include "rewriter-server.h"
#include <string>
#include <map>
#include <vector>
#include <utility>
#include <iostream>

// ------------------------ Debug dump helpers ------------------------
static std::string DumpMap(const std::map<std::string, std::string> &m) {
  std::string s;
  s += "{\n";
  for (const auto &kv : m) {
    s += "  \"" + kv.first + "\": \"" + kv.second + "\",\n";
  }
  s += "}";
  return s;
}

static std::string DumpContext(const std::string &origin_sql,
  const std::string &rewritten_sql,
  const std::map<std::string, std::string> &cte_map,
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
    {
      "simple_select",
      "SELECT * FROM c1",
      {{"c1", "SELECT 1"}},
      /*expect_with_count*/1,
      {"WITH c1 AS", "SELECT 1"}
    },
    {
      "union_inject_once",
      "SELECT * FROM cte1 UNION ALL SELECT * FROM t2",
      {{"cte1", "SELECT * FROM t1"}},
      /*expect_with_count*/1,
      {"WITH cte1 AS", "t2"}
    },
    {
      "existing_with_merge",
      "WITH a AS (SELECT 0) SELECT * FROM a",
      {{"a", "SELECT 1"}, {"b", "SELECT 2"}},
      /*expect_with_count*/-1,
      {" a AS ", " b AS "}
    },
    {
      "insert_select",
      "INSERT INTO dst SELECT * FROM c1",
      {{"c1", "SELECT 1"}},
      /*expect_with_count*/1,
      {"INSERT INTO dst", "WITH c1 AS"}
    },
    {
      "parse_failure_skips_bad",
      "SELECT * FROM good_cte",
      {{"bad", "SELECT * FROM"}, {"good_cte", "SELECT 1"}},
      /*expect_with_count*/-1,
      {"WITH good_cte AS"}
    },
    {
      "join_and_alias",
      "SELECT t.id FROM c1 AS t JOIN c2 USING (id)",
      {{"c1", "SELECT id FROM s1"}, {"c2", "SELECT id FROM s2"}},
      /*expect_with_count*/1,
      {"WITH c1 AS", "WITH c2 AS"}
      // we will not assert both prefixes literally; see specific test below
    },
    {
      "subquery_scope",
      "SELECT 1 FROM (SELECT * FROM c1) s",
      {{"c1", "SELECT 1"}},
      /*expect_with_count*/1,
      {"WITH c1 AS"}
    },
    {
      "none_used_still_inject",
      "SELECT 1",
      {{"a", "SELECT 1"}},
      /*expect_with_count*/1,
      {"WITH a AS"}
    },
    {
      "db_qualified",
      "SELECT * FROM db.c1",
      {{"c1", "SELECT 42"}},
      /*expect_with_count*/1,
      {"WITH c1 AS"}
    },
    {
      "multiple_refs_once",
      "SELECT * FROM c1 UNION ALL SELECT * FROM c1",
      {{"c1", "SELECT 1"}},
      /*expect_with_count*/1,
      {"WITH c1 AS"}
    },
    {
      "cte_value_with_union",
      "SELECT * FROM c1",
      {{"c1", "SELECT 1 UNION ALL SELECT 2"}},
      /*expect_with_count*/1,
      {"WITH c1 AS"}
    },
    {
      "nested_union_tree",
      "(SELECT * FROM c1) UNION ALL (SELECT * FROM c2)",
      {{"c1", "SELECT 1"}, {"c2", "SELECT 2"}},
      /*expect_with_count*/1,
      {"WITH c1 AS", "WITH c2 AS"}
    },
    {
      "insert_select_with_union",
      "INSERT INTO dst (x) SELECT * FROM c1 UNION ALL SELECT * FROM c2",
      {{"c1", "SELECT 1 AS x"}, {"c2", "SELECT 2 AS x"}},
      /*expect_with_count*/1,
      {"INSERT INTO dst", "WITH c1 AS", "WITH c2 AS"}
    },
    {
      "huge_cte_map_unused_many",
      "SELECT 1",
      {{"a", "SELECT 1"}, {"b", "SELECT 2"}, {"c", "SELECT 3"}, {"d", "SELECT 4"},
       {"e", "SELECT 5"}},
      /*expect_with_count*/1,
      {"WITH a AS", "WITH b AS", "WITH c AS"}
    },
  };
  return kCases;
}

inline const CTECase *Find(const std::string &name) {
  for (const auto &c : Cases()) if (name == c.name) return &c;
  return nullptr;
}
} // namespace TestData

// ------------------------ Helpers: run CTE rewrite ------------------------
// Core helper: run ONLY CTE rewrite and return rewritten SQL
static std::string RunCTERewrite(const std::string &origin_sql,
  const std::map<std::string, std::string> &cte_map) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(origin_sql);

  if (!cte_map.empty()) {
    auto *cte_option = req.add_options();
    cte_option->set_op(rewriter::RewriteOp::CTERewrite);
    auto *cte_args = cte_option->mutable_cte_args();
    for (const auto &kv : cte_map) {
      (*cte_args->mutable_cte_map())[kv.first] = kv.second;
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
  std::map<std::string, std::string> m;
  for (const auto &kv : c->cte_kv) m.emplace(kv.first, kv.second);
  return RunCTERewrite(c->origin_sql, m);
}

// ------------------------ Helpers: run CTE + TableName rewrite ------------------------
static std::string RunCTEAndTableRewrite(const std::string &origin_sql,
  const std::map<std::string, std::string> &cte_map,
  const std::map<std::string, std::string> &table_map) {
  RewriterServiceImpl service;
  rewriter::RewriteSQLRequest req;
  req.set_sql(origin_sql);

  if (!cte_map.empty()) {
    auto *cte_option = req.add_options();
    cte_option->set_op(rewriter::RewriteOp::CTERewrite);
    auto *cte_args = cte_option->mutable_cte_args();
    for (const auto &kv : cte_map) {
      (*cte_args->mutable_cte_map())[kv.first] = kv.second;
    }
  }
  if (!table_map.empty()) {
    auto *tbl_option = req.add_options();
    tbl_option->set_op(rewriter::RewriteOp::TableNameRewrite);
    auto *tbl_args = tbl_option->mutable_table_name_args();
    for (const auto &kv : table_map) {
      (*tbl_args->mutable_table_map())[kv.first] = kv.second;
    }
  }

  rewriter::RewriteSQLResponse resp;
  grpc::ServerContext ctx;
  auto status = service.Rewrite(&ctx, &req, &resp);
  if (!status.ok() || resp.code() != rewriter::RewriteCode::Success) {
    return "";
  }
  auto out = resp.sql_after_rewrite();
  g_last_debug = DumpContext(origin_sql, out, cte_map, table_map);
  std::cout << "\n****************** REWRITE DEBUG START (CTE+TABLE) ******************\n";
  std::cout << g_last_debug;
  std::cout << "****************** REWRITE DEBUG END (CTE+TABLE) ********************\n" <<
    std::endl;
  return out;
}

// ------------------------ CTE-only tests using helpers ------------------------

TEST(CTEOnly, RunByName_Simple) {
  auto out = RunCTERewriteByName("simple_select");
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());
  EXPECT_NE(out.find("WITH c1 AS"), std::string::npos);
  EXPECT_EQ(CountSubstr(out, "WITH "), 1u);
}

TEST(CTEOnly, RunByName_ExistingWithMerge) {
  auto out = RunCTERewriteByName("existing_with_merge");
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(CountSubstr(out, " a AS "), 1u); // ensure no duplicate a
  EXPECT_NE(out.find(" b AS "), std::string::npos);
}

TEST(CTEOnly, RunByName_UnionOnce) {
  auto out = RunCTERewriteByName("union_inject_once");
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(CountSubstr(out, "WITH "), 1u);
}

TEST(CTEOnly, RunByName_InsertSelect) {
  auto out = RunCTERewriteByName("insert_select");
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());
  EXPECT_NE(out.find("INSERT INTO dst"), std::string::npos);
  EXPECT_NE(out.find("WITH c1 AS"), std::string::npos);
}

TEST(CTEOnly, RunByName_ParseFailureSkipsBad) {
  auto out = RunCTERewriteByName("parse_failure_skips_bad");
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());
  EXPECT_NE(out.find("WITH good_cte AS"), std::string::npos);
  EXPECT_EQ(out.find("WITH bad AS"), std::string::npos);
}

TEST(CTEOnly, RunByName_JoinAndAlias) {
  auto out = RunCTERewriteByName("join_and_alias");
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(CountSubstr(out, "WITH "), 1u);
  // both CTE names should appear in WITH definitions exactly once each
  EXPECT_EQ(CountSubstr(out, "WITH c1 AS ("), 1u);
  // Only the first CTE in a WITH list is prefixed by "WITH".
  // Subsequent CTEs are separated by commas, e.g. "WITH c1 AS (...), c2 AS (...)".
  EXPECT_EQ(CountSubstr(out, " c2 AS ("), 1u);
  // and the JOIN remains
  EXPECT_NE(out.find("JOIN"), std::string::npos);
}

TEST(CTEOnly, RunByName_SubqueryScope) {
  auto out = RunCTERewriteByName("subquery_scope");
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(CountSubstr(out, "WITH "), 1u);
  EXPECT_NE(out.find("WITH c1 AS"), std::string::npos);
}

TEST(CTEOnly, RunByName_NoneUsedStillInject) {
  auto out = RunCTERewriteByName("none_used_still_inject");
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(CountSubstr(out, "WITH "), 1u);
  EXPECT_NE(out.find("WITH a AS"), std::string::npos);
}

TEST(CTEOnly, RunByName_DbQualified) {
  auto out = RunCTERewriteByName("db_qualified");
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(CountSubstr(out, "WITH "), 1u);
  EXPECT_NE(out.find("WITH c1 AS"), std::string::npos);
}

TEST(CTEOnly, RunByName_MultipleRefsOnce) {
  auto out = RunCTERewriteByName("multiple_refs_once");
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(CountSubstr(out, "WITH "), 1u);
  EXPECT_EQ(CountSubstr(out, " c1 AS "), 1u);
}

TEST(CTEOnly, RunByName_CTEValueWithUnion) {
  auto out = RunCTERewriteByName("cte_value_with_union");
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(CountSubstr(out, "WITH "), 1u);
  // ensure the union parts are present in the output SQL
  EXPECT_NE(out.find("UNION ALL"), std::string::npos);
}

TEST(CTEOnly, RunByName_NestedUnionTree) {
  auto out = RunCTERewriteByName("nested_union_tree");
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(CountSubstr(out, "WITH "), 1u);
  EXPECT_NE(out.find(" c1 AS "), std::string::npos);
  EXPECT_NE(out.find(" c2 AS "), std::string::npos);
}

TEST(CTEOnly, RunByName_InsertSelectWithUnion) {
  auto out = RunCTERewriteByName("insert_select_with_union");
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(CountSubstr(out, "WITH "), 1u);
  EXPECT_NE(out.find("INSERT INTO dst"), std::string::npos);
}

TEST(CTEOnly, RunByName_HugeCTEMapUnusedMany) {
  auto out = RunCTERewriteByName("huge_cte_map_unused_many");
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(CountSubstr(out, "WITH "), 1u);
  // spot-check a few names exist in WITH list
  EXPECT_NE(out.find(" a AS "), std::string::npos);
  EXPECT_NE(out.find(" b AS "), std::string::npos);
  EXPECT_NE(out.find(" c AS "), std::string::npos);
}

TEST(CTE_Table, Combine_CTE_and_TableName) {
  // origin: select * from `sqlquery-123abc` limit 1 union select * from Withdraw
  // we quote the hyphenated identifier in origin SQL to make it parseable.
  const std::string origin_sql =
    "SELECT * FROM `sqlquery-123abc` LIMIT 1 UNION SELECT * FROM Withdraw";

  std::map<std::string, std::string> cte_map{
    {"sqlquery-123abc", "SELECT * FROM Transfer"},
    {"sqlquery-456def", "SELECT * FROM Withdraw"},
  };
  std::map<std::string, std::string> table_map{
    {"Transfer", "autogen_123"},
    {"Withdraw", "autogen_456"},
  };

  auto out = RunCTEAndTableRewrite(origin_sql, cte_map, table_map);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());

  // 1) WITH appears once
  EXPECT_EQ(CountSubstr(out, "WITH "), 1u);

  // 2) CTE name preserved on outer SELECT, and CTE body table renamed
  // allow either backticks or unquoted printing; we check for raw tokens
  EXPECT_NE(out.find("sqlquery-123abc"), std::string::npos);
  EXPECT_NE(out.find("autogen_123"), std::string::npos);

  // 3) Second UNION branch table renamed
  EXPECT_NE(out.find("autogen_456"), std::string::npos);

  // 4) No raw source table names remain as table sources (FROM/JOIN), but may remain as aliases.
  // Old names should not appear as table sources (FROM/JOIN), but may remain as aliases.
  EXPECT_EQ(out.find(" FROM Transfer"), std::string::npos);
  EXPECT_EQ(out.find(" JOIN Transfer"), std::string::npos);
  EXPECT_EQ(out.find(" FROM Withdraw"), std::string::npos);
  EXPECT_EQ(out.find(" JOIN Withdraw"), std::string::npos);
  // Alias retention is expected by our transformer:
  EXPECT_NE(out.find(" AS Transfer"), std::string::npos);
  EXPECT_NE(out.find(" AS Withdraw"), std::string::npos);
}

TEST(TableOnly, TableRewrite_NoCTE) {
  // origin: select * from `sqlquery-123abc` limit 1 union select * from Withdraw
  // We only provide table mappings; no CTE mapping supplied.
  const std::string origin_sql =
    "SELECT * FROM `sqlquery-123abc` LIMIT 1 UNION SELECT * FROM Withdraw";

  std::map<std::string, std::string> cte_map{}; // empty
  std::map<std::string, std::string> table_map{
    {"Transfer", "autogen_123"},
    {"Withdraw", "autogen_456"},
  };

  auto out = RunCTEAndTableRewrite(origin_sql, cte_map, table_map);
  TRACE_LAST_REWRITE();
  ASSERT_FALSE(out.empty());

  // 1) No WITH should be injected (since cte_map is empty)
  EXPECT_EQ(CountSubstr(out, "WITH "), 0u);

  // 2) Left branch keeps the original identifier (hyphenated name), right branch renamed
  EXPECT_NE(out.find("sqlquery-123abc"), std::string::npos);
  EXPECT_NE(out.find("autogen_456"), std::string::npos);

  // 3) Ensure source table name on right branch is gone as a table source; it may remain as an alias.
  // Old name should not be used as a table source; it may remain as an alias.
  EXPECT_EQ(out.find(" FROM Withdraw"), std::string::npos);
  EXPECT_EQ(out.find(" JOIN Withdraw"), std::string::npos);
  EXPECT_NE(out.find(" AS Withdraw"), std::string::npos);
}

#if !REWRITER_RELEASE_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif