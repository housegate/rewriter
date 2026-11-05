// Large IN lists go through Rewrite as written. The regex preprocessor that
// split lists of more than 50 elements into OR batches before parsing was
// removed: the bounded parser (parseBounded) builds such a list as one flat
// tuple, and the preprocessor changed meaning (`abs(x) IN (...)` became
// `abs((x) IN (...) OR ...)`), refused NOT IN / GLOBAL IN, and rewrote text
// inside string literals and INSERT data.
#include "rewriter.grpc.pb.h"
#include "rewriter-server.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <sstream>
#include <string>

using rewriter::RewriteSQLRequest;
using rewriter::RewriteSQLResponse;

namespace {

RewriteSQLResponse rewrite(const std::string &sql) {
  RewriteSQLRequest req;
  req.set_sql(sql);
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
  (*dyn->mutable_database_map())["tenant1"] = "physical_db_1";
  dyn->set_upstream_logical_database_in_context("tenant1");
  RewriteSQLResponse resp;
  RewriterServiceImpl service;
  service.doRewrite(&req, &resp);
  return resp;
}

std::string list(size_t n, const std::function<std::string(size_t)> &element) {
  std::ostringstream out;
  for (size_t i = 0; i < n; ++i) {
    if (i) out << ", ";
    out << element(i);
  }
  return out.str();
}

std::string number(size_t i) { return std::to_string(i); }

size_t count(const std::string &haystack, const std::string &needle) {
  size_t found = 0;
  for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1)) ++found;
  return found;
}

void expectSuccess(const RewriteSQLResponse &resp) {
  ASSERT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
}

} // namespace

TEST(LargeInList, FunctionOperandKeepsItsMeaning) {
  // The regex captured `(x)` as the operand and emitted
  // `abs((x) IN (...) OR (x) IN (...))`.
  const auto resp = rewrite("SELECT * FROM events WHERE abs(x) IN (" + list(60, number) + ")");
  expectSuccess(resp);
  EXPECT_NE(resp.sql_after_rewrite().find("abs(x) IN (0, 1, 2"), std::string::npos) << resp.sql_after_rewrite();
  EXPECT_EQ(resp.sql_after_rewrite().find(" OR "), std::string::npos) << resp.sql_after_rewrite();
  EXPECT_NE(resp.sql_after_rewrite().find("58, 59)"), std::string::npos);
}

TEST(LargeInList, NotInAndGlobalInAreAccepted) {
  for (const std::string op : {"NOT IN", "GLOBAL IN", "GLOBAL NOT IN"}) {
    const auto resp = rewrite("SELECT * FROM events WHERE x " + op + " (" + list(60, number) + ")");
    expectSuccess(resp);
    EXPECT_NE(resp.sql_after_rewrite().find("x " + op + " (0, 1"), std::string::npos) << resp.sql_after_rewrite();
    EXPECT_EQ(resp.sql_after_rewrite().find(" OR "), std::string::npos) << resp.sql_after_rewrite();
  }
}

TEST(LargeInList, StringLiteralsAreNotRewritten) {
  // A string that reads like a large IN clause stays byte for byte.
  const std::string text = "a IN (" + list(60, number) + ")";
  const auto resp = rewrite("SELECT '" + text + "' AS s FROM events");
  expectSuccess(resp);
  EXPECT_NE(resp.sql_after_rewrite().find("'" + text + "'"), std::string::npos) << resp.sql_after_rewrite();
}

TEST(LargeInList, CommasInsideStringElementsSurvive) {
  const auto resp = rewrite("SELECT * FROM events WHERE d IN (" +
                            list(60, [](size_t i) { return "'item " + std::to_string(i) + ", with comma'"; }) + ")");
  expectSuccess(resp);
  EXPECT_EQ(count(resp.sql_after_rewrite(), ", with comma'"), 60u);
}

TEST(LargeInList, TupleListsAndArraysStayFlat) {
  const auto tuple = [](size_t i) { return "('0x" + std::to_string(i) + "', '0x" + std::to_string(i * 7) + "')"; };
  for (const auto &[open, close] : {std::pair{"[", "]"}, std::pair{"(", ")"}}) {
    const auto resp = rewrite(std::string("SELECT ((a, b) IN ") + open + list(320, tuple) + close + ") AS f FROM events");
    expectSuccess(resp);
    EXPECT_EQ(resp.sql_after_rewrite().find(" OR "), std::string::npos);
    EXPECT_NE(resp.sql_after_rewrite().find("('0x319', '0x2233')"), std::string::npos);
  }
}

TEST(LargeInList, TheQueryThatOnceCrashedTheParserRewrites) {
  // The 320-tuple shape the preprocessor was added for (2025-11).
  std::ostringstream sql;
  const auto tuple = [](size_t i) {
    return "('0x" + std::to_string(i * 1000) + "', '0x" + std::to_string(i * 2000) + "::token::TOKEN" +
           std::to_string(i) + "')";
  };
  sql << "SELECT coin_type, countDistinctIf(owner_address, balance > 0) AS holders "
      << "FROM (SELECT coin_type, owner_address, sum(amount_number) AS balance, "
      << "((owner_address, coin_type) IN [" << list(320, tuple) << "]) AS is_dev, "
      << "((owner_address, coin_type) IN [" << list(320, tuple) << "]) AS is_sniper "
      << "FROM tenant1.balance_changes WHERE coin_type IN ("
      << list(320, [](size_t i) { return "'0x" + std::to_string(i) + "::token::TOKEN'"; })
      << ") GROUP BY coin_type, owner_address) AS balances GROUP BY coin_type";
  const auto resp = rewrite(sql.str());
  expectSuccess(resp);
  EXPECT_NE(resp.sql_after_rewrite().find("physical_db_1"), std::string::npos);
}

TEST(LargeInList, HundredThousandElementsRewriteQuickly) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto resp = rewrite("SELECT * FROM events WHERE x IN (" + list(100000, number) + ")");
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  expectSuccess(resp);
  EXPECT_NE(resp.sql_after_rewrite().find("99998, 99999)"), std::string::npos);
  EXPECT_EQ(resp.sql_after_rewrite().find(" OR "), std::string::npos);
  EXPECT_LT(seconds, 10.0);
}

TEST(LargeInList, InsertDataIsNotRewritten) {
  const std::string row = "(1, 'x IN (" + list(60, number) + ")')";
  const auto resp = rewrite("INSERT INTO events VALUES " + row);
  expectSuccess(resp);
  EXPECT_NE(resp.sql_after_rewrite().find(row), std::string::npos) << resp.sql_after_rewrite();
}
