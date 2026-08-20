// src/handlers/materialize.cc
#include "handlers/materialize.h"

#include <Parsers/ASTFunction.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ExpressionListParsers.h>
#include <Parsers/Lexer.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>
#include <Core/Field.h>
#include <Common/Exception.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "ASTTransformers.h"
#include "handlers/common.h"

namespace rewriter_handlers {

namespace {

using DB::ASTPtr;

std::string toUpper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return s;
}

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Literal-node builders. Integers/strings render through ClickHouse's formatter
// exactly as the rewriter-go contract expects (e.g. "1710000000", "'UTC'").
ASTPtr litI(int64_t v)  { return DB::makeRewriterAST<DB::ASTLiteral>(DB::Field(static_cast<Int64>(v))); }
ASTPtr litU(uint64_t v) { return DB::makeRewriterAST<DB::ASTLiteral>(DB::Field(static_cast<UInt64>(v))); }
ASTPtr litF(double v)   { return DB::makeRewriterAST<DB::ASTLiteral>(DB::Field(static_cast<Float64>(v))); }
ASTPtr litS(const std::string &v) { return DB::makeRewriterAST<DB::ASTLiteral>(DB::Field(v)); }

constexpr int64_t kNanosPerSecond = 1'000'000'000;
constexpr int64_t kNanosPerMilli = 1'000'000;
constexpr int64_t kSecondsPerDay = 86'400;
constexpr uint64_t kMaxUInt32 = 0xFFFFFFFFull;

enum class MatErr { None, Unsupported, InvalidInput };

struct Rep { std::string function_name, literal_sql, value_type; };

// Lowercased names ClickHouse treats as non-deterministic that we KNOW about.
// Mirrors rewriter-go's isKnownNondeterministic (materialize_nodes.go) exactly.
// A name not in this set is left untouched (system/aggregate/deterministic fns).
// NOTE: some entries (e.g. localtime/localtimestamp) are NOT ClickHouse
// functions — ClickHouse parses those as bare identifiers, so they never reach
// the ASTFunction walk; keeping them here is harmless and preserves the mirror.
bool isKnownNondeterministic(const std::string &n) {
  static const std::unordered_set<std::string> known = {
    "any","anylast","blocknumber","blocksize","curdate","current_date",
    "current_timestamp","datetimetouuidv7","fuzzbits","fuzzquery",
    "generaterandomstructure","generateserialid","generatesnowflakeid",
    "generateuuidv4","generateuuidv7","localtime","localtimestamp","now","now64",
    "nowinblock","nowinblock64","obfuscatequery","quantile","quantiles","rand",
    "rand32","rand64","randbernoulli","randbinomial","randcanonical",
    "randchisquared","randconstant","randexponential","randfisherf",
    "randlognormal","randnegativebinomial","randnormal","randpoisson",
    "randstudentt","randuniform","random","randomfixedstring",
    "randomprintableascii","randomstring","randomstringutf8",
    "rownumberinallblocks","rownumberinblock","runningaccumulate",
    "runningconcurrency","runningdifference",
    "runningdifferencestartingwithfirstvalue","today","utc_timestamp",
    "utctimestamp","yesterday"};
  return known.count(n) != 0;
}

struct Materializer {
  // inputs (non-owning views into the request)
  std::optional<int64_t> now_unix_ns;
  const google::protobuf::RepeatedField<uint64_t> *rand_u64 = nullptr;
  const google::protobuf::RepeatedField<double> *rand_f64 = nullptr;
  const google::protobuf::RepeatedPtrField<std::string> *uuids = nullptr;
  // cursors / output
  size_t rand_i = 0, float_i = 0, uuid_i = 0;
  MatErr err = MatErr::None;
  std::string err_msg;
  std::vector<Rep> reps;

  void fail(MatErr e, std::string msg) {
    if (err == MatErr::None) { err = e; err_msg = std::move(msg); }
  }

  // Returns the replacement node for a supported function, or nullptr + sets
  // err. `lname` is the lowercased name; `original` is the verbatim spelling.
  ASTPtr build(const std::string &lname, const std::string &original, std::string &value_type);

  // Pre-order DFS: replace supported function nodes in place.
  void visit(ASTPtr &node);
};

std::optional<int64_t> nowSeconds(Materializer &m, const std::string &name) {
  if (!m.now_unix_ns) {
    m.fail(MatErr::InvalidInput, name + " requires now_unix_ns");
    return std::nullopt;
  }
  return *m.now_unix_ns / kNanosPerSecond;
}

bool nextUint(Materializer &m, const std::string &name, uint64_t &out) {
  if (!m.rand_u64 || m.rand_i >= static_cast<size_t>(m.rand_u64->size())) {
    m.fail(MatErr::InvalidInput, name + " requires random_uint64_values");
    return false;
  }
  out = m.rand_u64->Get(static_cast<int>(m.rand_i++));
  return true;
}

bool nextFloat(Materializer &m, const std::string &name, double &out) {
  if (!m.rand_f64 || m.float_i >= static_cast<size_t>(m.rand_f64->size())) {
    m.fail(MatErr::InvalidInput, name + " requires random_float64_values");
    return false;
  }
  out = m.rand_f64->Get(static_cast<int>(m.float_i++));
  return true;
}

bool nextUuid(Materializer &m, const std::string &name, std::string &out) {
  if (!m.uuids || m.uuid_i >= static_cast<size_t>(m.uuids->size())) {
    m.fail(MatErr::InvalidInput, name + " requires uuid_values");
    return false;
  }
  out = m.uuids->Get(static_cast<int>(m.uuid_i++));
  return true;
}

bool isUuid(const std::string &s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < 36; ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
    else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

ASTPtr toDateExpr(int64_t seconds) {
  return DB::makeASTFunction("toDate", DB::makeASTFunction("toDateTime", litI(seconds)));
}

ASTPtr Materializer::build(const std::string &lname, const std::string &original,
                           std::string &value_type) {
  // now / current_timestamp() / nowInBlock() are real ClickHouse niladic
  // functions. localtime/localtimestamp are kept only to mirror rewriter-go's
  // name set — ClickHouse has no such functions, so they never appear here from
  // valid CH SQL (bare forms parse as identifiers; see BareKeywordIdentifiers).
  if (lname == "now" || lname == "current_timestamp" || lname == "localtimestamp" ||
      lname == "localtime" || lname == "nowinblock") {
    auto s = nowSeconds(*this, lname);
    if (!s) return nullptr;
    value_type = "DateTime";
    return DB::makeASTFunction("toDateTime", litI(*s));
  }
  if (lname == "utctimestamp" || lname == "utc_timestamp") {
    auto s = nowSeconds(*this, lname);
    if (!s) return nullptr;
    value_type = "DateTime";
    return DB::makeASTFunction("toDateTime", litI(*s), litS("UTC"));
  }
  if (lname == "now64" || lname == "nowinblock64") {
    if (!now_unix_ns) { fail(MatErr::InvalidInput, lname + " requires now_unix_ns"); return nullptr; }
    value_type = "DateTime64(3)";
    return DB::makeASTFunction("fromUnixTimestamp64Milli", litI(*now_unix_ns / kNanosPerMilli));
  }
  if (lname == "today" || lname == "current_date" || lname == "curdate") {
    auto s = nowSeconds(*this, lname);
    if (!s) return nullptr;
    value_type = "Date";
    return toDateExpr(*s);
  }
  if (lname == "yesterday") {
    auto s = nowSeconds(*this, lname);
    if (!s) return nullptr;
    value_type = "Date";
    return toDateExpr(*s - kSecondsPerDay);
  }
  if (lname == "rand" || lname == "rand32" || lname == "randconstant") {
    uint64_t v;
    if (!nextUint(*this, lname, v)) return nullptr;
    if (v > kMaxUInt32) { fail(MatErr::InvalidInput, lname + " value exceeds UInt32"); return nullptr; }
    value_type = "UInt32";
    return litU(v);
  }
  if (lname == "rand64" || lname == "random") {
    uint64_t v;
    if (!nextUint(*this, lname, v)) return nullptr;
    value_type = "UInt64";
    return litU(v);
  }
  if (lname == "randcanonical") {
    double x;
    if (!nextFloat(*this, lname, x)) return nullptr;
    if (!std::isfinite(x) || x < 0.0 || x >= 1.0) {
      fail(MatErr::InvalidInput, lname + " value outside [0,1)");
      return nullptr;
    }
    value_type = "Float64";
    return DB::makeASTFunction("toFloat64", litF(x));
  }
  if (lname == "generateuuidv4" || lname == "generateuuidv7") {
    std::string u;
    if (!nextUuid(*this, lname, u)) return nullptr;
    u = toLower(u);
    if (!isUuid(u)) { fail(MatErr::InvalidInput, lname + " invalid uuid"); return nullptr; }
    value_type = "UUID";
    return DB::makeASTFunction("toUUID", litS(u));
  }
  fail(MatErr::Unsupported, original);
  return nullptr;
}

void Materializer::visit(ASTPtr &node) {
  if (err != MatErr::None || !node) return;

  if (auto *fn = node->as<DB::ASTFunction>()) {
    const std::string lname = toLower(fn->name);
    if (isKnownNondeterministic(lname)) {
      // Every supported function is nullary; arguments → unsupported variant.
      // A nullary call `f()` may carry a non-null but empty `arguments`
      // ASTExpressionList, so test `children.empty()`, not just non-null.
      if (fn->arguments && !fn->arguments->children.empty()) {
        fail(MatErr::Unsupported, fn->name + " with arguments");
        return;
      }
      std::string value_type;
      ASTPtr replacement = build(lname, fn->name, value_type);
      if (err != MatErr::None) return;
      if (replacement) {
        reps.push_back({fn->name, formatAst(replacement), value_type});
        node = replacement;  // reassign parent's child slot; don't recurse in
        return;
      }
    }
  }

  for (auto &child : node->children) {
    visit(child);
    if (err != MatErr::None) return;
  }
}

// ---- INSERT … VALUES data section -----------------------------------------
// ClickHouse leaves the VALUES payload off-AST (raw [data, end) bytes), so the
// AST walk above never sees functions inside it. We tokenize with ClickHouse's
// own lexer to split top-level "(...)" rows, parse each row's columns, run the
// same Materializer over them, and re-emit — preserving row/column structure.

// Split raw VALUES bytes into top-level "(...)" row interiors (text between a
// depth-0 '(' and its matching ')'), skipping string/comment tokens via the
// lexer. Returns false on malformed input (unbalanced parens / stray tokens).
bool splitValuesRows(const char *data, const char *end, std::vector<std::string> &rows) {
  DB::Lexer lexer(data, end);
  int depth = 0;
  const char *row_start = nullptr;
  for (DB::Token tok = lexer.nextToken(); !tok.isEnd(); tok = lexer.nextToken()) {
    if (tok.isError()) return false;
    if (tok.type == DB::TokenType::OpeningRoundBracket) {
      if (depth == 0) row_start = tok.end;  // interior begins after '('
      ++depth;
    } else if (tok.type == DB::TokenType::ClosingRoundBracket) {
      if (depth == 0) return false;
      --depth;
      if (depth == 0 && row_start) {
        rows.emplace_back(row_start, tok.begin);  // interior = [start, ')')
        row_start = nullptr;
      }
    } else if (depth == 0) {
      switch (tok.type) {  // only separators/terminators allowed between rows
        case DB::TokenType::Whitespace:
        case DB::TokenType::Comment:
        case DB::TokenType::Comma:
        case DB::TokenType::Semicolon:
          break;
        default:
          return false;
      }
    }
  }
  return depth == 0 && !rows.empty();
}

// Parse one row interior into column expressions, materialize each through `m`,
// and re-emit "(col0, col1, ...)" into `out`. Returns false on a parse failure;
// a per-column fail-closed condition sets m.err (caller checks it).
bool materializeRow(const std::string &interior, Materializer &m, std::string &out) {
  DB::ParserExpressionList parser(/*allow_alias_without_as=*/false);
  const char *p = interior.data();
  const char *e = p + interior.size();
  DB::ASTPtr list;
  try {
    list = DB::parseQuery(parser, p, e, /*description=*/"", 0, 0, 0);
  } catch (const std::exception &) {
    return false;
  }
  out = "(";
  bool first = true;
  for (auto &col : list->children) {
    m.visit(col);
    if (m.err != MatErr::None) { out.clear(); return true; }  // caller checks m.err
    if (!first) out += ", ";
    out += formatAst(col);
    first = false;
  }
  out += ")";
  return true;
}

// Materialize a VALUES data section into `out` (the full "(row), (row)" text).
// Returns false on a hard tokenize/parse failure (caller → MaterializeError).
bool materializeValuesData(const char *data, const char *end, Materializer &m,
                           std::string &out) {
  std::vector<std::string> rows;
  if (!splitValuesRows(data, end, rows)) return false;
  std::string acc;
  for (size_t i = 0; i < rows.size(); ++i) {
    std::string row_sql;
    if (!materializeRow(rows[i], m, row_sql)) return false;
    if (m.err != MatErr::None) return true;  // fail-closed column; stop
    if (i) acc += ", ";
    acc += row_sql;
  }
  out = std::move(acc);
  return true;
}

// Returns the resolved profile id, or std::nullopt + sets `err` on a bad policy.
std::optional<std::string> resolveProfile(const rewriter::MaterializationPolicy &policy,
                                          std::string &err) {
  std::string profile = policy.profile_id();
  if (profile.empty()) profile = kDefaultMaterializationProfileId;
  if (profile != kDefaultMaterializationProfileId) {
    err = "unsupported materialization profile \"" + policy.profile_id() + "\"";
    return std::nullopt;
  }
  const std::string tz = toUpper(policy.timezone());
  if (!tz.empty() && tz != "UTC") {
    err = "unsupported materialization timezone \"" + policy.timezone() + "\"";
    return std::nullopt;
  }
  return profile;
}

} // namespace

void materialize(const rewriter::MaterializeSQLRequest *request,
                 rewriter::MaterializeSQLResponse *response) {
  const std::string &sql = request->sql();
  response->set_sql(sql);
  response->set_sql_after_materialization(sql);
  response->set_code(rewriter::MaterializeCode::MaterializeSuccess);
  response->set_materializer_profile_id(kDefaultMaterializationProfileId);

  std::string policy_err;
  auto profile = resolveProfile(request->policy(), policy_err);
  if (!profile) {
    response->set_code(rewriter::MaterializeCode::MaterializeInvalidRequest);
    response->set_message(policy_err);
    return;
  }
  response->set_materializer_profile_id(*profile);

  const char *pos = sql.data();
  const char *end = pos + sql.size();
  DB::ParserQuery parser(end, false);
  DB::ASTPtr ast;
  try {
    ast = DB::parseQuery(parser, pos, end, /*description=*/"", 0, 0, 0);
  } catch (const DB::Exception &e) {
    response->set_code(rewriter::MaterializeCode::MaterializeSyntaxError);
    response->set_message(e.displayText());
    return;
  } catch (const std::exception &e) {
    // "Never throws past the boundary": ClickHouse internals can surface a
    // non-DB::Exception (std/Poco) from allocation/encoding paths. Fail closed
    // as an internal error rather than letting it crash the RPC thread.
    response->set_code(rewriter::MaterializeCode::MaterializeError);
    response->set_message(e.what());
    return;
  }

  Materializer m;
  if (request->inputs().has_now_unix_ns()) m.now_unix_ns = request->inputs().now_unix_ns();
  m.rand_u64 = &request->inputs().random_uint64_values();
  m.rand_f64 = &request->inputs().random_float64_values();
  m.uuids = &request->inputs().uuid_values();

  m.visit(ast);

  if (m.err != MatErr::None) {
    response->set_code(m.err == MatErr::Unsupported
                         ? rewriter::MaterializeCode::MaterializeUnsupportedStatement
                         : rewriter::MaterializeCode::MaterializeInvalidRequest);
    response->set_message(m.err_msg);
    return;  // sql_after_materialization stays == sql
  }

  // INSERT … VALUES: the data section is off-AST, so materialize it separately
  // through the SAME Materializer (cursors/ordinals continue after any prelude
  // rewrites). ASTInsertQuery formats the prelude only, so we append the tail.
  std::string insert_tail;
  bool has_insert_tail = false;
  if (auto *insert = ast->as<DB::ASTInsertQuery>()) {
    if (insert->data && insert->data != insert->end) {
      has_insert_tail = true;
      const std::string &fmt = insert->format;
      if (fmt.empty() || fmt == "Values") {
        if (!materializeValuesData(insert->data, insert->end, m, insert_tail)) {
          response->set_code(rewriter::MaterializeCode::MaterializeError);
          response->set_message("could not materialize INSERT VALUES data section");
          return;
        }
        if (m.err != MatErr::None) {
          response->set_code(m.err == MatErr::Unsupported
                               ? rewriter::MaterializeCode::MaterializeUnsupportedStatement
                               : rewriter::MaterializeCode::MaterializeInvalidRequest);
          response->set_message(m.err_msg);
          return;
        }
      } else {
        // Non-VALUES inline data (RowBinary/JSON/…) carries no SQL function
        // calls; preserve it verbatim so a prelude rewrite doesn't drop it.
        insert_tail.assign(insert->data, insert->end);
      }
    }
  }

  if (m.reps.empty()) return;  // echo verbatim, Success

  std::string out = formatAst(ast);  // prelude only for INSERT
  if (has_insert_tail) out += insert_tail;
  response->set_sql_after_materialization(out);
  for (size_t i = 0; i < m.reps.size(); ++i) {
    auto *r = response->add_replacements();
    r->set_function_name(m.reps[i].function_name);
    r->set_ordinal(static_cast<uint32_t>(i));
    r->set_literal_sql(m.reps[i].literal_sql);
    r->set_value_type(m.reps[i].value_type);
  }
}

} // namespace rewriter_handlers
