// src/handlers/materialize.cc
#include "handlers/materialize.h"

#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>
#include <Common/Exception.h>

#include <algorithm>
#include <cctype>
#include <string>

#include "handlers/common.h"

namespace rewriter_handlers {

namespace {

std::string toUpper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return s;
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
  }

  // Materialization wired in Task 2+. For now, deterministic SQL passes through
  // unchanged (echo already set above).
  (void)ast;
}

} // namespace rewriter_handlers
