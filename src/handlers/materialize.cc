// src/handlers/materialize.cc
#include "handlers/materialize.h"

#include <Parsers/ASTFunction.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>
#include <Core/Field.h>
#include <Common/Exception.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

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
ASTPtr litI(int64_t v)  { return std::make_shared<DB::ASTLiteral>(DB::Field(static_cast<Int64>(v))); }
ASTPtr litU(uint64_t v) { return std::make_shared<DB::ASTLiteral>(DB::Field(static_cast<UInt64>(v))); }
ASTPtr litS(const std::string &v) { return std::make_shared<DB::ASTLiteral>(DB::Field(v)); }

constexpr int64_t kNanosPerSecond = 1'000'000'000;

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

ASTPtr Materializer::build(const std::string &lname, const std::string &original,
                           std::string &value_type) {
  if (lname == "now" || lname == "current_timestamp" || lname == "localtimestamp" ||
      lname == "localtime" || lname == "nowinblock") {
    auto s = nowSeconds(*this, lname);
    if (!s) return nullptr;
    value_type = "DateTime";
    return DB::makeASTFunction("toDateTime", litI(*s));
  }
  // Remaining builders land in Task 3 (date family, rand, uuid, …).
  fail(MatErr::Unsupported, original);
  return nullptr;
}

void Materializer::visit(ASTPtr &node) {
  if (err != MatErr::None || !node) return;

  if (auto *fn = node->as<DB::ASTFunction>()) {
    const std::string lname = toLower(fn->name);
    if (isKnownNondeterministic(lname)) {
      // Every supported function is nullary; arguments → unsupported variant.
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

  if (m.reps.empty()) return;  // echo verbatim, Success

  response->set_sql_after_materialization(formatAst(ast));
  for (size_t i = 0; i < m.reps.size(); ++i) {
    auto *r = response->add_replacements();
    r->set_function_name(m.reps[i].function_name);
    r->set_ordinal(static_cast<uint32_t>(i));
    r->set_literal_sql(m.reps[i].literal_sql);
    r->set_value_type(m.reps[i].value_type);
  }
}

} // namespace rewriter_handlers
