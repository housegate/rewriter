// Resource bounds on the request paths: parser / AST depth, request sizes,
// and the RewriteErrorMessage text passes (which replaced std::regex scans;
// the differential tests below pin them to the regexes they replaced).
#include "rewriter.grpc.pb.h"
#include "rewriter-server.h"
#include "ASTTransformers.h"
#include "error_message_rewrite.h"
#include "request_limits.h"
#include "handlers/common.h"

#include <Parsers/ASTColumnsTransformers.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ParserQuery.h>
#include <gtest/gtest.h>

#include <chrono>
#include <climits>
#include <cstdlib>
#include <optional>
#include <random>
#include <regex>
#include <string>
#include <vector>

using rewriter::RewriteErrorMessageRequest;
using rewriter::RewriteErrorMessageResponse;
using rewriter::RewriteSQLRequest;
using rewriter::RewriteSQLResponse;

namespace {

// The std::regex helpers RewriteErrorMessage used before the bounded scans,
// kept verbatim as the oracle for the differential tests.
namespace legacy {

inline void regexEscapeAppend(char c, std::string &out) {
  switch (c) {
  case '.':
  case '^':
  case '$':
  case '|':
  case '(':
  case ')':
  case '\\':
  case '[':
  case ']':
  case '*':
  case '+':
  case '?':
  case '{':
  case '}':
    out.push_back('\\');
    out.push_back(c);
    break;
  default: out.push_back(c);
  }
}

inline std::string regexEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() * 2);
  for (unsigned char ch : s) { regexEscapeAppend(static_cast<char>(ch), out); }
  return out;
}

inline std::regex makeFlexibleSqlRegex(const std::string &sql) {
  std::string out;
  out.reserve(sql.size() * 2);
  bool in_ws = false;
  for (unsigned char c : sql) {
    switch (c) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '\v':
      if (!in_ws) {
        out.append("\\s+");
        in_ws = true;
      }
      break;
    default: regexEscapeAppend(static_cast<char>(c), out); in_ws = false;
    }
  }
  return std::regex(out, std::regex::ECMAScript | std::regex::icase | std::regex::optimize);
}

inline std::regex makeIdentRegex(const std::string &ident) {
  std::string esc = regexEscape(ident);
  std::string pat = "(^|[^A-Za-z0-9_])`?" + esc + "`?(?=[^A-Za-z0-9_]|$)";
  return std::regex(pat, std::regex::ECMAScript | std::regex::icase);
}

inline std::regex makeQualifiedIdentRegex(const std::string &qualified) {
  const auto dot = qualified.find('.');
  if (dot == std::string::npos) {
    return makeIdentRegex(qualified);
  }
  const std::string db = qualified.substr(0, dot);
  const std::string table = qualified.substr(dot + 1);
  const std::string esc_db = regexEscape(db);
  const std::string esc_table = regexEscape(table);
  const bool table_has_dot = table.find('.') != std::string::npos;
  const std::string table_pat =
    table_has_dot ? ("`" + esc_table + "`") : ("`?" + esc_table + "`?");
  const std::string pat =
    "(^|[^A-Za-z0-9_])`?" + esc_db + "`?\\." + table_pat + "(?=[^A-Za-z0-9_]|$)";
  return std::regex(pat, std::regex::ECMAScript | std::regex::icase);
}

inline std::vector<size_t> buildOffsetMap(std::string_view rewritten,
                                          std::string_view original) {
  const size_t N = rewritten.size(), M = original.size();
  std::vector<size_t> pos_map(N + 1);
  pos_map[N] = M;
  if (N == 0) return pos_map;
  if (M == 0) {
    for (size_t i = 0; i < N; i++) pos_map[i] = 0;
    return pos_map;
  }

  const int MAX = static_cast<int>(N + M);
  auto kIdx = [MAX](int k) { return k + MAX; };
  std::vector<int> V(2 * static_cast<size_t>(MAX) + 1, 0);
  std::vector<std::vector<int>> trace;
  trace.reserve(static_cast<size_t>(MAX) + 1);

  int found_d = -1;
  for (int d = 0; d <= MAX; d++) {
    trace.push_back(V);
    bool done = false;
    for (int k = -d; k <= d && !done; k += 2) {
      int x;
      const bool down = (k == -d) || (k != d && V[kIdx(k - 1)] < V[kIdx(k + 1)]);
      if (down) {
        x = V[kIdx(k + 1)];
      } else {
        x = V[kIdx(k - 1)] + 1;
      }
      int y = x - k;
      while (x < static_cast<int>(N) && y < static_cast<int>(M) && rewritten[x] == original[y]) {
        x++; y++;
      }
      V[kIdx(k)] = x;
      if (x >= static_cast<int>(N) && y >= static_cast<int>(M)) {
        found_d = d;
        done = true;
      }
    }
    if (done) break;
  }

  if (found_d < 0) {
    for (size_t i = 0; i < N; i++) pos_map[i] = M;
    return pos_map;
  }

  int x = static_cast<int>(N), y = static_cast<int>(M);
  for (int d = found_d; d > 0; d--) {
    const auto &Vp = trace[d];
    const int k = x - y;
    bool down;
    if (k == -d) {
      down = true;
    } else if (k == d) {
      down = false;
    } else {
      down = Vp[kIdx(k - 1)] < Vp[kIdx(k + 1)];
    }
    const int prev_k = down ? k + 1 : k - 1;
    const int prev_x = Vp[kIdx(prev_k)];
    const int prev_y = prev_x - prev_k;
    const int slide_start_x = down ? prev_x : prev_x + 1;
    const int slide_start_y = down ? prev_y + 1 : prev_y;

    while (x > slide_start_x && y > slide_start_y) {
      x--; y--;
      if (x < static_cast<int>(N)) pos_map[x] = static_cast<size_t>(y);
    }
    if (down) {
      y = prev_y;
    } else {
      if (prev_x < static_cast<int>(N)) pos_map[prev_x] = static_cast<size_t>(prev_y);
      x = prev_x;
    }
  }
  while (x > 0 && y > 0) {
    x--; y--;
    if (x < static_cast<int>(N)) pos_map[x] = static_cast<size_t>(y);
  }
  while (x > 0) {
    x--;
    pos_map[x] = 0;
  }
  return pos_map;
}

inline std::string remapErrorPositions(const std::string &err,
                                       const std::vector<size_t> &pos_map) {
  if (pos_map.size() <= 1) return err;
  static const std::regex re(R"(([Pp]osition[\s]+)(\d+))");
  std::string out;
  out.reserve(err.size());
  size_t last = 0;
  for (auto it = std::sregex_iterator(err.begin(), err.end(), re),
            end = std::sregex_iterator();
       it != end; ++it) {
    const auto &m = *it;
    const size_t mp = static_cast<size_t>(m.position(0));
    out.append(err, last, mp - last);
    out.append(m[1].str());
    size_t pos1 = 0;
    bool ok = true;
    try { pos1 = std::stoull(m[2].str()); } catch (...) { ok = false; }
    if (!ok || pos1 == 0 || pos1 > pos_map.size() - 1) {
      out.append(m[2].str());
    } else {
      out.append(std::to_string(pos_map[pos1 - 1] + 1));
    }
    last = mp + static_cast<size_t>(m.length(0));
  }
  out.append(err, last, err.size() - last);
  return out;
}

} // namespace legacy

// Sets an environment variable for one test and restores it afterwards.
class ScopedEnv {
public:
  ScopedEnv(const char *name, const std::string &value) : name_(name) {
    if (const char *old = std::getenv(name)) old_ = old;
    ::setenv(name, value.c_str(), 1);
  }
  ~ScopedEnv() {
    if (old_) ::setenv(name_, old_->c_str(), 1);
    else ::unsetenv(name_);
  }

private:
  const char *name_;
  std::optional<std::string> old_;
};

std::string repeat(const std::string &s, size_t n) {
  std::string out;
  out.reserve(s.size() * n);
  for (size_t i = 0; i < n; ++i) out += s;
  return out;
}

void addDynamicOption(google::protobuf::RepeatedPtrField<rewriter::RewriteOption> *options) {
  auto *opt = options->Add();
  opt->set_op(rewriter::RewriteOp::TableNameRewrite);
  auto *dyn = opt->mutable_table_name_args()->mutable_dynamic_args();
  (*dyn->mutable_database_map())["tenant1"] = "physical_db_1";
  dyn->set_upstream_logical_database_in_context("tenant1");
}

RewriteSQLResponse rewrite(const std::string &sql, bool dynamic = true) {
  RewriteSQLRequest req;
  req.set_sql(sql);
  if (dynamic) addDynamicOption(req.mutable_options());
  RewriteSQLResponse resp;
  RewriterServiceImpl service;
  service.doRewrite(&req, &resp);
  return resp;
}

RewriteErrorMessageResponse rewriteError(
  const std::string &sql, const std::string &err, std::function<bool()> cancelled = {}) {
  RewriteErrorMessageRequest req;
  req.set_sql(sql);
  req.set_error_message(err);
  addDynamicOption(req.mutable_options());
  RewriteErrorMessageResponse resp;
  RewriterServiceImpl service;
  service.doRewriteErrorMessage(&req, &resp, std::move(cancelled));
  return resp;
}

double secondsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

bool isWithheld(const RewriteErrorMessageResponse &resp) {
  return resp.message().rfind("error message withheld", 0) == 0;
}

std::string randomString(std::mt19937 &rng, std::string_view alphabet, size_t max_len) {
  std::uniform_int_distribution<size_t> len_dist(0, max_len);
  std::uniform_int_distribution<size_t> char_dist(0, alphabet.size() - 1);
  std::string s(len_dist(rng), '\0');
  for (auto &c : s) c = alphabet[char_dist(rng)];
  return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Parser and AST depth
// ---------------------------------------------------------------------------

TEST(ParseBounds, DeeplyNestedParenthesesAreASyntaxError) {
  const auto resp = rewrite("SELECT " + repeat("(", 1100) + "1" + repeat(")", 1100));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::SyntaxError) << resp.message();
}

TEST(ParseBounds, LongOperatorChainIsASyntaxError) {
  // Left-associative: the tree is as deep as the chain is long. Unbounded, it
  // overflowed the stack of the walks that follow the parse.
  const auto t0 = std::chrono::steady_clock::now();
  const auto resp = rewrite("SELECT 1" + repeat("+1", 200000) + " FROM events");
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::SyntaxError) << resp.message();
  EXPECT_LT(secondsSince(t0), 10.0);
}

TEST(ParseBounds, DeeplyNestedSubqueriesAreASyntaxError) {
  const auto resp = rewrite(repeat("SELECT * FROM (", 3000) + "SELECT 1" + repeat(")", 3000));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::SyntaxError) << resp.message();
}

TEST(ParseBounds, OrdinaryNestingStillRewrites) {
  const auto resp = rewrite("SELECT " + repeat("(", 30) + "1" + repeat(")", 30)
    + " FROM " + repeat("(SELECT * FROM ", 10) + "events" + repeat(")", 10));
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
}

TEST(ParseBounds, DeepCteBodyIsAFailedAlias) {
  RewriteSQLRequest req;
  req.set_sql("SELECT * FROM deep");
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::CommonTableExprRewrite);
  auto &cte = (*opt->mutable_common_table_expr_args()->mutable_cte_map())["deep"];
  cte.set_alias("deep");
  cte.set_sql("SELECT 1" + repeat("+1", 200000));
  RewriteSQLResponse resp;
  RewriterServiceImpl service;
  service.doRewrite(&req, &resp);
  ASSERT_GE(resp.failed_cte_aliases_size(), 1) << resp.DebugString().substr(0, 2000);
  for (const auto &alias : resp.failed_cte_aliases()) EXPECT_EQ(alias, "deep");
}

TEST(ParseBounds, OptimizeRefusesDeepSql) {
  rewriter::OptimizeRequest req;
  req.set_sql("SELECT 1" + repeat("+1", 200000));
  rewriter::OptimizeResponse resp;
  RewriterServiceImpl service;
  service.doOptimize(&req, &resp);
  EXPECT_EQ(resp.code(), rewriter::OptimizeCode::OptimizeSyntaxError);
  EXPECT_EQ(resp.sql_after_optimize(), req.sql());
}

TEST(ParseBounds, MaterializeRefusesDeepSql) {
  rewriter::MaterializeSQLRequest req;
  req.set_sql("SELECT 1" + repeat("+1", 200000));
  rewriter::MaterializeSQLResponse resp;
  RewriterServiceImpl service;
  service.doMaterializeSQL(&req, &resp);
  EXPECT_EQ(resp.code(), rewriter::MaterializeCode::MaterializeSyntaxError);
  EXPECT_EQ(resp.sql_after_materialization(), req.sql());
}

TEST(ParseBounds, ParseBoundedChecksTheTreeDepth) {
  const std::string sql = "SELECT " + repeat("NOT ", 5000) + "1";
  DB::ParserQuery parser(sql.data() + sql.size(), false);
  EXPECT_THROW(rewriter_handlers::parseBounded(parser, sql.data(), sql.data() + sql.size()), DB::Exception);
}

TEST(ParseBounds, DepthCheckDescendsIntoFieldOnlyMembers) {
  // A chain deeper than the bound held only in an APPLY lambda, outside
  // `children`: IAST::checkDepth cannot see it.
  const size_t bound = rewriter_handlers::kMaxAstDepth;
  DB::ASTPtr deep = DB::makeRewriterAST<DB::ASTExpressionList>();
  for (size_t i = 0; i < bound + 10; ++i) {
    auto parent = DB::makeRewriterAST<DB::ASTExpressionList>();
    parent->children.push_back(deep);
    deep = parent;
  }
  auto apply = DB::makeRewriterAST<DB::ASTColumnsApplyTransformer>();
  apply->lambda = deep;
  auto root = DB::makeRewriterAST<DB::ASTExpressionList>();
  root->children.push_back(apply);
  EXPECT_NO_THROW(root->checkDepth(bound));
  EXPECT_THROW(rewriter_handlers::checkPolicyAstDepth(*root, bound), DB::Exception);
}

TEST(ParseBounds, DepthCheckVisitsAnAdoptedMemberOnce) {
  // Nested APPLY lambdas, registered: each lambda is both a field and a child.
  // Visiting it through both would double the work per level.
  DB::ASTPtr node = DB::makeRewriterAST<DB::ASTExpressionList>();
  for (int i = 0; i < 64; ++i) {
    auto apply = DB::makeRewriterAST<DB::ASTColumnsApplyTransformer>();
    apply->lambda = node;
    auto list = DB::makeRewriterAST<DB::ASTExpressionList>();
    list->children.push_back(apply);
    node = list;
  }
  rewriter_handlers::registerFieldOnlyChildren(*node);
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_NO_THROW(rewriter_handlers::checkPolicyAstDepth(*node, rewriter_handlers::kMaxAstDepth));
  EXPECT_LT(secondsSince(t0), 1.0);
}

// ---------------------------------------------------------------------------
// Request sizes
// ---------------------------------------------------------------------------

TEST(RequestLimits, OversizedSqlIsRefusedBeforeParsing) {
  ScopedEnv env("REWRITER_MAX_SQL_BYTES", "1024");
  const std::string sql = "SELECT * FROM events WHERE x = '" + std::string(2000, 'a') + "'";
  const auto resp = rewrite(sql);
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::RewriteError);
  EXPECT_EQ(resp.message(), "query exceeds maximum supported size");
  EXPECT_EQ(resp.sql_after_rewrite(), sql);
}

TEST(RequestLimits, CteBodiesCountTowardsTheSqlLimit) {
  ScopedEnv env("REWRITER_MAX_SQL_BYTES", "1024");
  RewriteSQLRequest req;
  req.set_sql("SELECT * FROM c");
  auto *opt = req.add_options();
  opt->set_op(rewriter::RewriteOp::CommonTableExprRewrite);
  auto &cte = (*opt->mutable_common_table_expr_args()->mutable_cte_map())["c"];
  cte.set_alias("c");
  cte.set_sql("SELECT '" + std::string(2000, 'a') + "'");
  RewriteSQLResponse resp;
  RewriterServiceImpl service;
  service.doRewrite(&req, &resp);
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::RewriteError);
}

TEST(RequestLimits, SqlLimitOfZeroDisablesTheCheck) {
  ScopedEnv env("REWRITER_MAX_SQL_BYTES", "0");
  const auto resp = rewrite("SELECT * FROM events WHERE x = '" + std::string(2000, 'a') + "'");
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success) << resp.message();
}

TEST(RequestLimits, MalformedValuesFallBackToTheDefault) {
  for (const char *bad : {"-1", "+5", "abc", "12abc", "", "99999999999999999999999"}) {
    ScopedEnv env("REWRITER_TEST_LIMIT", bad);
    EXPECT_EQ(rewriter_limits::envSize("REWRITER_TEST_LIMIT", 5), 5u) << bad;
  }
  ScopedEnv env("REWRITER_TEST_LIMIT", " 7");
  EXPECT_EQ(rewriter_limits::envSize("REWRITER_TEST_LIMIT", 5), 7u);
}

TEST(RequestLimits, ServerThreadsHaveAFloorOfTwo) {
  {
    ScopedEnv env("REWRITER_MAX_SERVER_THREADS", "1");
    EXPECT_EQ(rewriter_limits::maxServerThreads(), 2);
  }
  {
    ScopedEnv env("REWRITER_MAX_SERVER_THREADS", "32");
    EXPECT_EQ(rewriter_limits::maxServerThreads(), 32);
  }
}

TEST(RequestLimits, ReceiveCapCoversTheApplicationCaps) {
  EXPECT_EQ(rewriter_limits::maxReceiveMessageBytes(), 6 * 1024 * 1024);
  {
    ScopedEnv env("REWRITER_MAX_SQL_BYTES", "0");
    EXPECT_EQ(rewriter_limits::maxReceiveMessageBytes(), INT_MAX);
  }
  {
    ScopedEnv env("REWRITER_MAX_RECV_MESSAGE_BYTES", "1000");
    EXPECT_EQ(rewriter_limits::maxReceiveMessageBytes(), 1000);
  }
}

// ---------------------------------------------------------------------------
// RewriteErrorMessage bounds
// ---------------------------------------------------------------------------

TEST(ErrorMessageBounds, OversizedErrorTextIsWithheld) {
  ScopedEnv env("REWRITER_MAX_ERROR_BYTES", "1024");
  const std::string err = "Code: 60. Table physical_db_1.`tenant1.events` does not exist. " + std::string(2000, 'x');
  const auto resp = rewriteError("SELECT * FROM tenant1.events", err);
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_TRUE(isWithheld(resp)) << resp.message();
  EXPECT_EQ(resp.error_after_rewrite().find("physical_db_1"), std::string::npos);
}

TEST(ErrorMessageBounds, OversizedSqlIsWithheldRatherThanPassedThrough) {
  ScopedEnv env("REWRITER_MAX_SQL_BYTES", "1024");
  const std::string sql = "SELECT * FROM tenant1.events WHERE x = '" + std::string(2000, 'a') + "'";
  const auto resp = rewriteError(sql, "Code: 60. Table physical_db_1.`tenant1.events` does not exist.");
  EXPECT_TRUE(isWithheld(resp)) << resp.message();
  EXPECT_EQ(resp.error_after_rewrite().find("physical_db_1"), std::string::npos);
}

TEST(ErrorMessageBounds, CancelledCallIsWithheld) {
  const auto resp = rewriteError("SELECT * FROM tenant1.events",
    "Code: 60. Table physical_db_1.`tenant1.events` does not exist.", [] { return true; });
  EXPECT_TRUE(isWithheld(resp)) << resp.message();
  EXPECT_EQ(resp.error_after_rewrite().find("physical_db_1"), std::string::npos);
}

TEST(ErrorMessageBounds, SpentScanBudgetIsWithheld) {
  ScopedEnv env("REWRITER_ERROR_SCAN_BUDGET", "64");
  const auto resp = rewriteError("SELECT * FROM tenant1.events",
    "Code: 60. Table physical_db_1.`tenant1.events` does not exist. " + std::string(200, 'x'));
  EXPECT_TRUE(isWithheld(resp)) << resp.message();
  EXPECT_EQ(resp.error_after_rewrite().find("physical_db_1"), std::string::npos);
}

TEST(ErrorMessageBounds, LargeInListWithAPositionFinishesQuickly) {
  // A long literal list written without spaces: the formatter adds one per
  // element, so the diff behind "position N" has tens of thousands of edits.
  // Unbounded, its trace needed (edits x statement length) memory.
  std::string list;
  for (int i = 0; i < 20000; ++i) list += (i ? "," : "") + std::to_string(i);
  const std::string sql = "select x from tenant1.events where x in (" + list + ")";
  const auto rw = rewrite(sql);
  ASSERT_EQ(rw.code(), rewriter::RewriteCode::Success) << rw.message();
  const std::string err = "Code: 62. Syntax error: failed at position 40 (x): In scope "
    + rw.sql_after_rewrite() + ". Table physical_db_1.`tenant1.events` is broken.";

  const auto t0 = std::chrono::steady_clock::now();
  const auto resp = rewriteError(sql, err);
  EXPECT_LT(secondsSince(t0), 10.0);
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_FALSE(isWithheld(resp)) << resp.message();
  EXPECT_EQ(resp.error_after_rewrite().find("physical_db_1"), std::string::npos);
  EXPECT_NE(resp.error_after_rewrite().find(sql), std::string::npos);
}

TEST(ErrorMessageBounds, ManyMentionsInALargeErrorFinishQuickly) {
  const std::string err = repeat("Table physical_db_1.`tenant1.events` is not `PHYSICAL_DB_1`.`tenant1.events`; ", 12000);
  ASSERT_LT(err.size(), rewriter_limits::maxErrorMessageBytes());
  const auto t0 = std::chrono::steady_clock::now();
  const auto resp = rewriteError("SELECT * FROM tenant1.events", err);
  EXPECT_LT(secondsSince(t0), 10.0);
  EXPECT_EQ(resp.code(), rewriter::RewriteCode::Success);
  EXPECT_EQ(resp.error_after_rewrite().find("physical_db_1"), std::string::npos);
}

TEST(ErrorMessageBounds, ReplacementTextIsInsertedLiterally) {
  // std::regex_replace read `$&` and `$1` in the caller's SQL as format
  // escapes; the original statement must come back verbatim.
  const std::string sql = "SELECT '$& $1 $$' FROM tenant1.events";
  const auto rw = rewrite(sql);
  ASSERT_EQ(rw.code(), rewriter::RewriteCode::Success) << rw.message();
  std::string echoed = rw.sql_after_rewrite();
  // Different spacing forces the whitespace-insensitive pass.
  echoed = std::regex_replace(echoed, std::regex(" FROM "), "\n  FROM ");
  const auto resp = rewriteError(sql, "Code: 1. In scope " + echoed + ". (BAD)");
  EXPECT_NE(resp.error_after_rewrite().find(sql), std::string::npos) << resp.error_after_rewrite();
}

// ---------------------------------------------------------------------------
// The bounded scans against the regexes they replaced
// ---------------------------------------------------------------------------

TEST(ErrorMessageScan, IdentifierReplacementMatchesLegacyRegex) {
  std::mt19937 rng(20261105);
  const std::vector<std::string> idents{"ab", "a", "a_b", "Ab1", "b-a"};
  rewriter_error::WorkBudget budget(0);
  for (int i = 0; i < 20000; ++i) {
    const std::string err = randomString(rng, "aAbB1_-`. ", 40);
    const std::string &ident = idents[i % idents.size()];
    const std::string expected = std::regex_replace(err, legacy::makeIdentRegex(ident), std::string("$1") + "R.r");
    const auto got = rewriter_error::replaceIdentifier(err, ident, "R.r", budget);
    ASSERT_TRUE(got);
    ASSERT_EQ(*got, expected) << "err=[" << err << "] ident=[" << ident << "]";
  }
}

TEST(ErrorMessageScan, QualifiedReplacementMatchesLegacyRegex) {
  std::mt19937 rng(7);
  const std::vector<std::string> names{"ab.cd", "a.b.c", "ab.c.d", "a.b", "b.a_b", "ab"};
  rewriter_error::WorkBudget budget(0);
  for (int i = 0; i < 20000; ++i) {
    const std::string err = randomString(rng, "aAbBcCd_`. 1", 40);
    const std::string &name = names[i % names.size()];
    const std::string expected = std::regex_replace(err, legacy::makeQualifiedIdentRegex(name), std::string("$1") + "L");
    const auto got = rewriter_error::replaceQualifiedIdentifier(err, name, "L", budget);
    ASSERT_TRUE(got);
    ASSERT_EQ(*got, expected) << "err=[" << err << "] name=[" << name << "]";
  }
}

TEST(ErrorMessageScan, SqlBlockReplacementMatchesLegacyRegex) {
  std::mt19937 rng(42);
  rewriter_error::WorkBudget budget(0);
  for (int i = 0; i < 20000; ++i) {
    std::string sql = randomString(rng, "aB \n(.*`", 8);
    if (sql.empty()) sql = "a";
    const std::string err = randomString(rng, "abAB \t\n(.*`", 40);
    const std::string expected = std::regex_replace(err, legacy::makeFlexibleSqlRegex(sql), "Q");
    const auto got = rewriter_error::replaceSqlBlock(err, sql, "Q", budget);
    ASSERT_TRUE(got);
    ASSERT_EQ(*got, expected) << "err=[" << err << "] sql=[" << sql << "]";
  }
}

TEST(ErrorMessageScan, PositionRemapMatchesLegacyRegex) {
  std::mt19937 rng(3);
  const std::vector<std::string> pieces{"position 3", "Position\t12", "positions 4", "position",
    "Position  99999999999999999999999", "pposition 2", "POSITION 5", "position 0", " ", "x", "1",
    "position\n7", "osition 8"};
  std::uniform_int_distribution<size_t> pick(0, pieces.size() - 1);
  std::uniform_int_distribution<size_t> count(0, 6);
  for (int i = 0; i < 5000; ++i) {
    std::string err;
    for (size_t n = count(rng); n > 0; --n) err += pieces[pick(rng)];
    std::vector<size_t> pos_map(1 + (i % 15));
    for (size_t j = 0; j < pos_map.size(); ++j) pos_map[j] = j * 3 + 1;
    const std::string expected = legacy::remapErrorPositions(err, pos_map);
    const auto refs = rewriter_error::findPositionRefs(err);
    ASSERT_EQ(rewriter_error::remapErrorPositions(err, refs, pos_map), expected) << "err=[" << err << "]";
  }
}

TEST(ErrorMessageScan, OffsetMapMatchesLegacyDiff) {
  std::mt19937 rng(11);
  rewriter_error::WorkBudget budget(0);
  for (int i = 0; i < 5000; ++i) {
    const std::string a = randomString(rng, "abc ", 30);
    const std::string b = randomString(rng, "abc ", 30);
    const auto got = rewriter_error::buildOffsetMap(a, b, a.size() + b.size(), budget);
    ASSERT_TRUE(got);
    ASSERT_EQ(*got, legacy::buildOffsetMap(a, b)) << "a=[" << a << "] b=[" << b << "]";
  }
}

TEST(ErrorMessageScan, OffsetMapGivesUpPastMaxEdits) {
  rewriter_error::WorkBudget budget(0);
  const std::string a(5000, 'x'), b(5000, 'y');
  EXPECT_FALSE(rewriter_error::buildOffsetMap(a, b, rewriter_error::kMaxOffsetMapEdits, budget));
  EXPECT_FALSE(budget.exhausted());
  // Within the bound it still maps.
  EXPECT_TRUE(rewriter_error::buildOffsetMap("SELECT 1", "select 1", rewriter_error::kMaxOffsetMapEdits, budget));
}

TEST(ErrorMessageScan, SearchesStayLinearOnAdversarialInput) {
  rewriter_error::WorkBudget budget(0);
  const std::string haystack(1 << 20, 'a');
  const std::string needle = std::string(1 << 18, 'a') + "b";
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(rewriter_error::findExact(haystack, needle, budget));
  const auto replaced = rewriter_error::replaceSqlBlock(haystack, needle, "Q", budget);
  ASSERT_TRUE(replaced);
  EXPECT_EQ(*replaced, haystack);
  const auto ident = rewriter_error::replaceIdentifier(haystack, std::string(200, 'a') + "b", "Q", budget);
  ASSERT_TRUE(ident);
  EXPECT_LT(secondsSince(t0), 5.0);
}

TEST(ErrorMessageScan, BudgetStopsAndStaysStopped) {
  rewriter_error::WorkBudget budget(100);
  EXPECT_TRUE(budget.charge(60));
  EXPECT_FALSE(budget.charge(60));
  EXPECT_TRUE(budget.exhausted());
  EXPECT_FALSE(budget.charge(1));

  bool cancel = false;
  rewriter_error::WorkBudget cancellable(0, [&cancel] { return cancel; });
  EXPECT_TRUE(cancellable.charge(10));
  cancel = true;
  EXPECT_FALSE(cancellable.charge(2u << 20));
  EXPECT_TRUE(cancellable.exhausted());
}
