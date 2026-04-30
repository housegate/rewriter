#include "rewriter-server.h"

#include <IO/WriteBufferFromString.h>
#include <Parsers/IdentifierQuotingStyle.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>
#include <grpc++/grpc++.h>

#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>

#include "handlers/exists.h"
#include "handlers/grant.h"
#include "handlers/select.h"
#include "handlers/show_create.h"
#include "handlers/show_databases.h"
#include "handlers/show_tables.h"
#include "handlers/use.h"
#include "handlers/writes.h"
#include "query_preprocessor.h"
#include "rewriter-http-server.h"

#include <s2/s2testing.h>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using rewriter::RewriterService;
using rewriter::RewriteSQLRequest;
using rewriter::RewriteSQLResponse;

#ifndef GIT_COMMIT_HASH
#define GIT_COMMIT_HASH "?"
#endif

#ifndef REWRITER_VERSION
#define REWRITER_VERSION "unknown"
#endif

namespace {

// Cheap correlation id for access logs. Uses std::hash so we don't pull in
// a crypto dep — collisions don't matter, this is just for grepping.
inline std::string shortSqlHash(const std::string &sql) {
  const auto h = std::hash<std::string>{}(sql);
  std::ostringstream oss;
  oss << std::hex << std::setw(16) << std::setfill('0') << h;
  return oss.str();
}

} // namespace

// Regex helpers used only by doRewriteErrorMessage. Kept here so handler code
// doesn't depend on them.
namespace {

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

// Whitespace-insensitive regex for matching the rewritten SQL inside a
// (possibly ANSI-colored) error message.
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

// ECMAScript in std::regex does not support lookbehind, so we use a captured
// prefix + lookahead and must keep $1 in replacements. Boundary-aware regex
// matching an identifier optionally wrapped in backticks.
inline std::regex makeIdentRegex(const std::string &ident) {
  std::string esc = regexEscape(ident);
  std::string pat = "(^|[^A-Za-z0-9_])`?" + esc + "`?(?=[^A-Za-z0-9_]|$)";
  return std::regex(pat, std::regex::ECMAScript | std::regex::icase);
}

} // namespace

void RewriterServiceImpl::doRewrite(
  const RewriteSQLRequest *request, RewriteSQLResponse *response) {
  const String original_query = request->sql();
  const std::string sql_id = shortSqlHash(original_query);
  const auto t0 = std::chrono::steady_clock::now();

  // RAII access log: fires on every exit path (normal return, early return,
  // thrown-and-caught) so there's no way to forget a log line.
  struct AccessLog {
    const std::string &sql_id;
    const RewriteSQLResponse *response;
    std::chrono::steady_clock::time_point t0;
    size_t sql_len;
    ~AccessLog() {
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
      std::cerr << "[access] method=Rewrite sql=" << sql_id
                << " stmt=" << (response->statement_type() == rewriter::STATEMENT_TYPE_UNSPECIFIED
                                  ? "-"
                                  : rewriter::StatementType_Name(response->statement_type()))
                << " code=" << response->code()
                << " sql_len=" << sql_len
                << " took_us=" << us << std::endl;
    }
  } _access_log{sql_id, response, t0, original_query.size()};

  // Preprocess the query to split large IN clauses. The preprocessor logs a
  // single [preprocess] line only when it actually transformed the SQL; the
  // no-op path is silent and covered by the [access] line at function exit.
  String query = QueryPreprocessor::preprocess(original_query, sql_id);

  const char *pos = query.data();
  const char *end = pos + query.size();
  DB::ParserQuery parser(end, false);

  try {
    // NB: the 7-arg `parseQuery(IParser&, const char* begin, const char* end,
    // const std::string& description, size_t, size_t, size_t)` is the one we
    // want — it preserves the (begin, end) pointer pair so ASTInsertQuery's
    // `data`/`end` keep referencing our own `query` buffer. Without the
    // explicit empty description, overload resolution silently picks the
    // (..., const std::string& query, const std::string& description, ...)
    // form, which converts `pos` and `end` to temporary strings whose
    // buffers die on return — leaving INSERT inline payload pointers
    // dangling. See doRewriteErrorMessage below for the same pattern.
    DB::ASTPtr ast = DB::parseQuery(parser, pos, end, /*description=*/"", 0, 0, 0);

    if (rewriter_handlers::handleUseQuery(ast, request, response)) return;
    if (rewriter_handlers::handleShowDatabasesQuery(ast, request, response)) return;
    if (rewriter_handlers::handleShowTablesQuery(ast, request, response)) return;
    if (rewriter_handlers::handleExistsQuery(ast, request, response)
        == rewriter_handlers::ExistsDispatchResult::Handled) {
      return;
    }
    if (rewriter_handlers::handleShowCreateQuery(ast, request, response)
        == rewriter_handlers::ShowCreateDispatchResult::Handled) {
      return;
    }
    switch (rewriter_handlers::handleGrantQuery(ast, request, response)) {
      case rewriter_handlers::GrantDispatchResult::Handled:
      case rewriter_handlers::GrantDispatchResult::Rejected:
        return;
      case rewriter_handlers::GrantDispatchResult::NotAGrant:
        break;  // fall through to write/SELECT dispatch.
    }
    switch (rewriter_handlers::handleWriteQuery(ast, request, response)) {
      case rewriter_handlers::WriteDispatchResult::Handled:
      case rewriter_handlers::WriteDispatchResult::Rejected:
        return;
      case rewriter_handlers::WriteDispatchResult::NotAWrite:
        break;  // fall through to SELECT
    }
    rewriter_handlers::handleSelectQuery(ast, request, response);
  } catch (const DB::Exception &e) {
    // SQL length is useful to distinguish "empty/truncated request" from real
    // syntax errors; keep the actual SQL out of the log — it may contain PII.
    std::cerr << "[parse-fail] sql=" << sql_id
              << " len=" << original_query.size()
              << " err=\"" << e.displayText() << "\"" << std::endl;
    response->set_sql_after_rewrite("Error parsing query");
    response->set_message(e.displayText());
    response->set_code(rewriter::RewriteCode::SyntaxError);
  }
}

// gRPC thin wrapper — delegates to the protocol-agnostic doRewrite.
grpc::Status RewriterServiceImpl::Rewrite(
  ServerContext * /*context*/, const RewriteSQLRequest *request, RewriteSQLResponse *response) {
  doRewrite(request, response);
  return grpc::Status::OK;
}

void RewriterServiceImpl::doRewriteErrorMessage(
  const RewriteErrorMessageRequest *request,
  RewriteErrorMessageResponse *response) {

  const std::string sql_id = shortSqlHash(request->sql());
  const size_t sql_len = request->sql().size();
  const size_t err_len = request->error_message().size();
  const auto t0 = std::chrono::steady_clock::now();

  struct AccessLog {
    const std::string &sql_id;
    const RewriteErrorMessageResponse *response;
    std::chrono::steady_clock::time_point t0;
    size_t sql_len;
    size_t err_len;
    ~AccessLog() {
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
      std::cerr << "[access] method=RewriteErrorMessage sql=" << sql_id
                << " code=" << response->code()
                << " sql_len=" << sql_len
                << " err_len=" << err_len
                << " took_us=" << us << std::endl;
    }
  } _access_log{sql_id, response, t0, sql_len, err_len};

  auto error_message = request->error_message();
  if (error_message.empty()) {
    response->set_message("Error message is empty");
    response->set_code(rewriter::Success);
    return;
  }

  // Step 1: parse original SQL to AST.
  String query = request->sql();
  const char *pos = query.data();
  const char *end = pos + query.size();

  DB::ParserQuery parser(end, /*allow_alias_without_as*/ false);
  try {
    // See doRewrite for why the explicit empty description matters.
    DB::ASTPtr ast = DB::parseQuery(parser, pos, end, /*description=*/"", 0, 0, 0);

    String origin_sql_without_space = ast->formatWithSecretsOneLine();

    // Step 2: rewrite the AST.
    ast = rewriter_handlers::applyRewriteOptions(request->options(), ast);

    // Step 3: format the AST back to SQL.
    String rewritten_sql = ast->formatForErrorMessage();

    // Step 4: perform replacements in error_message.
    // Use the raw `error_message` and `sql` without stripping ANSI or normalizing whitespace.
    std::string err = error_message;

    // 4.1 Replace the entire SQL if present (exact first, then flexible
    // whitespace-insensitive).
    {
      auto p = err.find(rewritten_sql);
      if (p != std::string::npos) {
        err.replace(p, rewritten_sql.size(), origin_sql_without_space);
      } else {
        std::regex flex = makeFlexibleSqlRegex(rewritten_sql);
        err = std::regex_replace(err, flex, origin_sql_without_space);
      }
    }

    // 4.2 Replace rewritten table names back to original names (boundary-aware).
    // Only the SELECT-path table_map produces deterministic rewritten->original
    // pairs suitable for inverse substitution; the dynamic construction in
    // dynamic_args doesn't give us a stable inverse (the rewritten name can
    // collide with other tables in the physical DB), so we don't invert it
    // here — the caller already has the table_rewrites map on the rewrite
    // response and can do a smarter substitution if it wants.
    for (const auto &opt : request->options()) {
      if (opt.op() != rewriter::RewriteOp::TableNameRewrite) continue;
      if (!opt.table_name_args().has_static_args()) continue;
      const auto &m = opt.table_name_args().static_args().table_map();
      for (const auto &kv : m) {
        const std::string &original = kv.first;
        const std::string &rewritten = kv.second;
        if (rewritten.empty() || original.empty()) continue;

        // Replace with boundary-aware regex, allow optional backticks.
        std::regex re_ident = makeIdentRegex(rewritten);
        err = std::regex_replace(err, re_ident, std::string("$1") + original);
      }
    }

    response->set_error_after_rewrite(err);
    response->set_message("success");
    response->set_code(rewriter::Success);

  } catch (const DB::Exception &e) {
    std::cerr << "[parse-fail] method=RewriteErrorMessage sql=" << sql_id
              << " len=" << sql_len
              << " err=\"" << e.displayText() << "\"" << std::endl;
    response->set_error_after_rewrite("Error when fine tune error message");
    response->set_message(e.displayText());
    response->set_code(rewriter::SyntaxError);
  }
}

// gRPC thin wrapper — delegates to doRewriteErrorMessage.
grpc::Status RewriterServiceImpl::RewriteErrorMessage(
  ServerContext * /*context*/,
  const RewriteErrorMessageRequest *request,
  RewriteErrorMessageResponse *response) {
  doRewriteErrorMessage(request, response);
  return grpc::Status::OK;
}

// Injected at build time from `git describe --tags --always --dirty`
// (see CMakeLists.txt). Stays in lock-step with git tags instead of drifting
// from a hand-maintained literal.
const std::string VERSION = REWRITER_VERSION;
const std::string hash = GIT_COMMIT_HASH;

void RunServer(uint16_t grpc_port, uint16_t http_port) {
  std::cout << "Rewriter Server Version: " << VERSION << " (" << hash << ")" << std::endl;

  std::string server_address = "0.0.0.0:" + std::to_string(grpc_port);
  RewriterServiceImpl service;

  // HTTP+JSON façade — same `service` instance, different protocol. Starts a
  // Poco HTTPServer in its own thread pool and returns immediately so the
  // gRPC Wait() below remains the process-blocking call.
  rewriter_http::RunHttpServer(http_port, service);

  grpc::EnableDefaultHealthCheckService(true);
  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << " (gRPC)" << std::endl;

  server->Wait();
}
