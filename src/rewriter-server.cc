#include "rewriter-server.h"

#include <IO/WriteBufferFromString.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTDropQuery.h>
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
#include <unordered_map>

#include "handlers/common.h"
#include "handlers/exists.h"
#include "handlers/materialize.h"
#include "handlers/name_rewrite.h"
#include "handlers/grant.h"
#include "handlers/join_swap.h"
#include "handlers/select.h"
#include "handlers/show_create.h"
#include "handlers/show_databases.h"
#include "handlers/show_tables.h"
#include "handlers/storage_integrity.h"
#include "handlers/system_tables.h"
#include "handlers/table_reference.h"
#include "handlers/describe.h"
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

// Forward Myers' O(ND) diff between `rewritten` and `original`, returning a
// position map: result[i] (for i in [0, rewritten.size()]) is the byte offset
// in `original` corresponding to byte offset i in `rewritten`. Positions
// inside a deleted span (chars present in rewritten but not in original)
// collapse to the boundary of the surrounding equal range — best effort, but
// stable enough that "position N" inside a rewritten table identifier remaps
// to the start of the user's original identifier.
//
// Caller is responsible for bounding input size if the diff distance D could
// blow up — runtime is O((N+M)*D), memory O((N+M)*D). For SQL-style edits
// (a handful of identifier swaps) D is small and this finishes in ms.
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

// Patch byte-position references in `err` so they point into the original
// SQL coordinates instead of the rewritten ones. CH parse errors emit
// "Syntax error[: ...]: failed at position N (...)" with N being a 1-based
// byte offset (clickHouse/src/Parsers/parseQuery.cpp). We only see errors on
// SQL the rewriter parsed successfully, so practically this fires when CH's
// own parser is stricter than ours and rejects the rewritten output — rare
// but it does happen, and the position is unhelpful when the user reads it
// against their original SQL.
//
// Line/col `(line L, col C)` is intentionally NOT remapped: the rewriter
// always emits one-line SQL (formatWithPossiblyHidingSensitiveData,
// one_line=true), so CH's getLineAndCol guard `nl + 1 < end` is false and
// CH never prints line/col for our queries.
inline std::string remapErrorPositions(const std::string &err,
                                       const std::vector<size_t> &pos_map) {
  // pos_map.size() - 1 is the rewritten SQL byte length.
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
    size_t pos1 = 0;  // 1-based as printed.
    bool ok = true;
    try { pos1 = std::stoull(m[2].str()); } catch (...) { ok = false; }
    // CH uses 1-based; subtract 1 to index pos_map. Out-of-range stays put
    // (probably not a SQL position — could be a version digit run).
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

// Boundary-aware regex matching a qualified table reference, allowing each
// component to be optionally backtick-quoted. Splits on the first dot — that
// matches how recordTableRewrite encodes "<db>.<table>" (and how the dynamic
// rewriter packs the prefix-prefixed table into the table half, so its dot is
// always inside the backticks ClickHouse's WhenNecessary formatter emits).
inline std::regex makeQualifiedIdentRegex(const std::string &qualified) {
  const auto dot = qualified.find('.');
  if (dot == std::string::npos) {
    return makeIdentRegex(qualified);
  }
  const std::string db = qualified.substr(0, dot);
  const std::string table = qualified.substr(dot + 1);
  const std::string esc_db = regexEscape(db);
  const std::string esc_table = regexEscape(table);
  // If the table half itself contains a dot, ClickHouse's formatter will have
  // wrapped it in backticks (the dot is illegal bare). Require them so we
  // don't greedy-match across an unrelated `.foo` suffix.
  const bool table_has_dot = table.find('.') != std::string::npos;
  const std::string table_pat =
    table_has_dot ? ("`" + esc_table + "`") : ("`?" + esc_table + "`?");
  const std::string pat =
    "(^|[^A-Za-z0-9_])`?" + esc_db + "`?\\." + table_pat + "(?=[^A-Za-z0-9_]|$)";
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

  // Positive SI proof (V1 or V2) is selected by the same last-wins
  // table-rewrite option as the handlers. V2 activates the surface by
  // version, V1 only with a non-empty table map. Validate the entire active
  // contract before publishing acknowledgement, then stamp it before
  // preprocessing/parsing so syntax errors and every handler rejection
  // retain the proof.
  const auto si_selection =
    rewriter_handlers::findActiveTableRewrite(request->options());
  const bool si_active =
    si_selection.mode == rewriter_handlers::TableRewriteMode::Dynamic
    && rewriter_handlers::storageIntegritySurfaceActive(*si_selection.dynamic_args);
  if (si_active) {
    const auto &si = si_selection.dynamic_args->storage_integrity();
    std::string validation_error;
    if (si.contract_version() != rewriter::STORAGE_INTEGRITY_CONTRACT_V1
        && si.contract_version() != rewriter::STORAGE_INTEGRITY_CONTRACT_V2) {
      response->set_sql_after_rewrite(original_query);
      response->set_message(std::string(
        rewriter_handlers::kStorageIntegrityContractMessage));
      response->set_code(rewriter::RewriteCode::InvalidRewriteRequest);
      return;
    }
    if (!rewriter_handlers::validateStorageIntegrity(
          *si_selection.dynamic_args, &validation_error)) {
      response->set_sql_after_rewrite(original_query);
      response->set_message(validation_error);
      response->set_code(rewriter::RewriteCode::InvalidRewriteRequest);
      return;
    }
    response->set_storage_integrity_contract_version(si.contract_version());
  }
  // Spec 2026-09-26 §6: every protected_databases entry is a simple
  // identifier, validated for every dynamic request whatever the SI state.
  if (si_selection.mode == rewriter_handlers::TableRewriteMode::Dynamic) {
    std::string protected_error;
    if (!rewriter_handlers::validateProtectedDatabases(
          *si_selection.dynamic_args, &protected_error)) {
      response->set_sql_after_rewrite(original_query);
      response->set_message(protected_error);
      response->set_code(rewriter::RewriteCode::InvalidRewriteRequest);
      return;
    }
  }

  // Every structured rejection must echo the original SQL. Successful
  // handlers replace this value with their formatted rewrite, while the
  // parser catch below retains its established diagnostic sentinel.
  response->set_sql_after_rewrite(original_query);

  // The Go contract accepts a small set of SELECT/ATTACH forms that the
  // pinned ClickHouse parser predates. Token-aware preparation rejects SI
  // wrappers before parsing and temporarily removes ordinary wrappers. The
  // guard restores those ordinary semantics on every successful exit path.
  rewriter_handlers::StorageIntegritySQLCompatibility si_compatibility;
  si_compatibility.original_sql = original_query;
  si_compatibility.parse_sql = original_query;
  if (si_active && rewriter_handlers::prepareStorageIntegritySQL(
        original_query, *si_selection.dynamic_args, response,
        &si_compatibility)) return;
  struct StorageIntegrityRestoreGuard {
    RewriteSQLResponse *response;
    const rewriter_handlers::StorageIntegritySQLCompatibility *compatibility;
    ~StorageIntegrityRestoreGuard() {
      rewriter_handlers::restoreStorageIntegritySQL(response, *compatibility);
    }
  } _si_restore{response, &si_compatibility};
  DB::ASTPtr parsed_ast;
  // The annotator reads the statement as the client wrote it (rewriter-go
  // annotates its original parse): handlers rewrite names in place, and a
  // target a handler already mapped to hg_safe is not one the client named.
  DB::ASTPtr annotate_ast;
  // Spec I D2: one place upgrades every reject's message to name the SI object
  // it touched, on every exit path, so no handler has to grow its own SI
  // branch. Mirrors rewriter-go's finalize() hook.
  struct StorageIntegrityRejectAnnotator {
    RewriteSQLResponse *response;
    const DB::ASTPtr *ast;
    const rewriter::RewriteTableDynamicArgs *args;  // null when SI is inactive
    ~StorageIntegrityRejectAnnotator() {
      if (!args) return;
      rewriter_handlers::annotateStorageIntegrityReject(response, *ast, *args);
    }
  } _si_annotate{response, &annotate_ast, si_active ? si_selection.dynamic_args : nullptr};

  // Preprocess the query to split large IN clauses. The preprocessor logs a
  // single [preprocess] line only when it actually transformed the SQL; the
  // no-op path is silent and covered by the [access] line at function exit.
  String query = QueryPreprocessor::preprocess(
    si_compatibility.parse_sql, sql_id);

  if (rewriter_handlers::handleRemovedLiveViewCompatibility(
        query, request, response)) return;

  const char *pos = query.data();
  const char *end = pos + query.size();
  DB::ParserQuery parser(end, false);
  // The text the preserved literal folds are parsed from (N1, below). It must
  // outlive the AST: an INSERT's inline data points into it.
  String marked_query;

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
    parsed_ast = DB::parseQuery(parser, pos, end, /*description=*/"", 0, 0, 0);
    // A literal the embedded parser folds into SQL that ClickHouse 25.8 / 26.2
    // read differently. A literal tuple in an extra pair of grouping
    // parentheses (`x IN ((1, 0, '0'))`, N1) is preserved: the text is parsed
    // again with a marker on each such group, and the marked tree replaces
    // the source's when it is that tree but for the groups (adoptLiteralFolds,
    // table_reference.h). Anything else (`1e-310`, N2, or a group the check
    // cannot preserve) is refused on a would-be Success, in both SI states.
    bool refuse_literal_fold = false;
    // `NOT (a) = 1` is `not(a) = 1` to 25.8 / 26.2 and `NOT (a = 1)` to the
    // embedded 26.3 parser: refused on a would-be Success, in both SI states
    // (notCallPrecedenceDiverges, table_reference.h). Read from the source.
    const bool refuse_not_precedence = rewriter_handlers::notCallPrecedenceDiverges(parsed_ast, query);
    if (const auto fold_plan = rewriter_handlers::planLiteralFolds(parsed_ast, query); fold_plan.refuse) {
      refuse_literal_fold = true;
    } else if (!fold_plan.folds.empty()) {
      marked_query = rewriter_handlers::markLiteralFolds(query, fold_plan);
      DB::ASTPtr marked_ast;
      try {
        const char *marked_pos = marked_query.data();
        const char *marked_end = marked_pos + marked_query.size();
        DB::ParserQuery marked_parser(marked_end, false);
        marked_ast = DB::parseQuery(marked_parser, marked_pos, marked_end, /*description=*/"", 0, 0, 0);
      } catch (const DB::Exception &) {
        marked_ast = nullptr;
      }
      if (marked_ast && rewriter_handlers::adoptLiteralFolds(marked_ast, parsed_ast, fold_plan.folds.size()))
        parsed_ast = marked_ast;
      else
        refuse_literal_fold = true;
    }
    // The annotator's copy is taken before registration (clonePolicyAst):
    // a registered tree's clone would share adopted subtrees with it.
    if (si_active) annotate_ast = rewriter_handlers::clonePolicyAst(parsed_ast);
    rewriter_handlers::registerFieldOnlyChildren(*parsed_ast);
    DB::ASTPtr &ast = parsed_ast;

    // Surface the existence-check clause (IF NOT EXISTS / IF EXISTS) as a
    // pure syntactic property of the AST, before any handler runs — so it's
    // accurate even when the statement is later rejected. ASTDropQuery backs
    // DROP TABLE/DATABASE/VIEW *and* TRUNCATE, so one branch covers them all.
    if (const auto *create = ast->as<DB::ASTCreateQuery>()) {
      if (create->if_not_exists)
        response->set_existence_clause(rewriter::EXISTENCE_CLAUSE_IF_NOT_EXISTS);
    } else if (const auto *drop = ast->as<DB::ASTDropQuery>()) {
      if (drop->if_exists)
        response->set_existence_clause(rewriter::EXISTENCE_CLAUSE_IF_EXISTS);
    }

    // Spec 2026-09-26 §5: the table-reference policy runs before any handler
    // can rewrite a name it refuses.
    std::optional<std::string> deferred_system_table;
    if (rewriter_handlers::preflightTableReferences(ast, request, response, &deferred_system_table)) return;

    // A read where no handler rewrites it would reach ClickHouse unrewritten
    // (a cross-tenant or protected read). R2 refuses these in the preflight or
    // the SI write path; this re-check is defence in depth, and the only guard
    // of an IN operand inside a CREATE's `AS <table function>` source
    // (table_reference.h; a SHOW filter is R7's). Decided on the parse tree
    // before a handler mutates it; applied only to a statement the handlers
    // accepted, so their own (storage-integrity) rejections keep their
    // messages.
    const bool refuse_unrewritten_in_operand =
      si_selection.mode == rewriter_handlers::TableRewriteMode::Dynamic
      && rewriter_handlers::inOperandInUnrewrittenPosition(ast);

    // R5 with the SI surface active: the SI handlers answer first; a statement
    // they accept (SHOW, DESCRIBE, EXISTS, DROP, … carry no hook of their own)
    // is refused here, ahead of the R2 refusal below. Decided on the parse
    // tree before a handler mutates it.
    rewriter::RewriteSQLResponse settings_refusal;
    const bool refuse_settings = si_active
      && rewriter_handlers::rejectQuerySettings(ast, &settings_refusal, &original_query);

    // A column declaration's own SETTINGS clause: refused on a would-be
    // Success, in both SI states (rewriter-go refuses it last).
    const bool refuse_column_settings = si_selection.mode == rewriter_handlers::TableRewriteMode::Dynamic
      && rewriter_handlers::columnDeclarationHasSettings(ast);

    [&]() {
      if (rewriter_handlers::handleUseQuery(ast, request, response)) return;
      if (rewriter_handlers::handleShowDatabasesQuery(ast, request, response)) return;
      if (rewriter_handlers::handleShowTablesQuery(ast, request, response)) return;
      // Database-level SHOW dispatch stays ahead of EXISTS / SHOW CREATE, mirroring
      // rewriter-go's order. SHOW COLUMNS / INDEXES parse into their own AST
      // classes, so they need their own entry here (Spec N D2).
      if (rewriter_handlers::handleShowColumnsQuery(ast, request, response)) return;
      // The SHOW forms that name no table (PROCESSLIST, USERS, GRANTS, …):
      // the system-table allowlist's SHOW mapping (spec 2026-09-26 §5).
      if (rewriter_handlers::handleShowSystemFormQuery(ast, request, response)) return;
      if (rewriter_handlers::handleExistsQuery(ast, request, response)
          == rewriter_handlers::ExistsDispatchResult::Handled) {
        return;
      }
      if (rewriter_handlers::handleShowCreateQuery(ast, request, response)
          == rewriter_handlers::ShowCreateDispatchResult::Handled) {
        return;
      }
      if (rewriter_handlers::handleDescribeQuery(ast, request, response)) return;
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
      if (si_active && rewriter_handlers::rejectUnmodelledStorageIntegrityStatement(
            ast, *si_selection.dynamic_args, response))
        return;
      rewriter_handlers::handleSelectQuery(ast, request, response);
    }();

    const auto refuseAcceptedStatement = [&](rewriter::RewriteCode code, const std::string &message) {
      response->clear_statement_type();
      response->clear_table_rewrites();
      response->clear_database_rewrites();
      response->clear_original_accessed_tables();
      response->clear_privileges_deltas();
      response->clear_failed_cte_aliases();
      response->set_sql_after_rewrite(original_query);
      response->set_code(code);
      response->set_message(message);
    };
    if (refuse_settings && response->code() == rewriter::RewriteCode::Success)
      refuseAcceptedStatement(settings_refusal.code(), settings_refusal.message());
    if (refuse_unrewritten_in_operand && response->code() == rewriter::RewriteCode::Success)
      refuseAcceptedStatement(rewriter::RewriteCode::UnsupportedStatement,
                              std::string(rewriter_handlers::kUnsupportedStatementMessage));
    // The system-table refusal the preflight deferred to the SI pipeline
    // (the statement names an SI object while the surface is active) applies
    // to that pipeline's would-be Success, after every refusal it issues
    // (rewriter-go RejectSystemTablesOnSuccess).
    if (deferred_system_table)
      rewriter_handlers::rejectDeferredSystemTable(response, *deferred_system_table, original_query, si_selection);
    // Last, as rewriter-go's mid-statement drop gate.
    if (refuse_column_settings && response->code() == rewriter::RewriteCode::Success)
      refuseAcceptedStatement(rewriter::RewriteCode::UnsupportedStatement,
                              std::string(rewriter_handlers::kUnsupportedStatementMessage));
    if ((refuse_literal_fold || refuse_not_precedence) && response->code() == rewriter::RewriteCode::Success)
      refuseAcceptedStatement(rewriter::RewriteCode::UnsupportedStatement,
                              std::string(rewriter_handlers::kUnsupportedStatementMessage));
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

  if (request->error_message().empty()) {
    response->set_message("Error message is empty");
    response->set_code(rewriter::Success);
    return;
  }

  // Re-run the full rewrite path on (sql, options) so we have the exact
  // origin->rewritten map this query went through. This covers every
  // statement kind the rewriter knows about (SELECT/USE/SHOW/EXISTS/
  // SHOW CREATE/writes/GRANT/...) and every TableNameRewrite variant
  // (static three-map + dynamic_args), since they all funnel into
  // response.table_rewrites / response.database_rewrites with deterministic
  // entries scoped to *this* query — so a name that's ambiguous in the
  // physical DB at large is unambiguous here.
  RewriteSQLRequest rewrite_req;
  rewrite_req.set_sql(request->sql());
  *rewrite_req.mutable_options() = request->options();
  RewriteSQLResponse rewrite_resp;
  doRewrite(&rewrite_req, &rewrite_resp);

  if (rewrite_resp.code() != rewriter::Success) {
    // We don't know what to invert. Surface the rewrite-side reason and
    // pass the error message through so the caller can still see it.
    response->set_error_after_rewrite(request->error_message());
    response->set_message(rewrite_resp.message());
    response->set_code(rewrite_resp.code());
    return;
  }

  std::string err = request->error_message();

  const std::string &rewritten_sql = rewrite_resp.sql_after_rewrite();
  const std::string &original_sql = request->sql();
  if (!rewritten_sql.empty() && rewritten_sql != original_sql) {
    // Remap byte-position references ("failed at position N") FIRST, while
    // they still match the rewritten SQL we're about to swap out. Without
    // this the user reads "position N" against their original SQL and ends
    // up at a totally wrong token (the rewritten table prefix shifts every
    // position past its insertion point).
    const auto pos_map = buildOffsetMap(rewritten_sql, original_sql);
    err = remapErrorPositions(err, pos_map);

    // Whole-SQL replacement: when the error embeds the rewritten SQL (the
    // common "In scope SELECT ..." form), swap the whole block back to the
    // user's original SQL. Catches the remote() case too — its full
    // remote('addr', 'db', 'table', 'user', '[HIDDEN]') call lives only in
    // sql_after_rewrite, not in table_rewrites.
    auto p = err.find(rewritten_sql);
    if (p != std::string::npos) {
      err.replace(p, rewritten_sql.size(), original_sql);
    } else {
      std::regex flex = makeFlexibleSqlRegex(rewritten_sql);
      err = std::regex_replace(err, flex, original_sql);
    }
  }

  // Per-table substitution: rewritten qualified name -> origin. Picks up
  // out-of-SQL-block mentions like "Table physical_db.`tenant1.events`
  // does not exist".
  for (const auto &kv : rewrite_resp.table_rewrites()) {
    const std::string &origin = kv.first;
    const std::string &rewritten = kv.second;
    if (origin == rewritten || rewritten.empty()) continue;
    std::regex re = makeQualifiedIdentRegex(rewritten);
    err = std::regex_replace(err, re, std::string("$1") + origin);
  }

  // Per-database substitution: physical -> logical. Runs after table-level
  // replacement so dotted physical_db.`tenant1.events` references are
  // resolved as a unit first; only standalone physical_db tokens (e.g.
  // "Database physical_db_1 does not exist") remain to match here.
  for (const auto &kv : rewrite_resp.database_rewrites()) {
    const std::string &origin = kv.first;
    const std::string &physical = kv.second;
    if (origin == physical || physical.empty()) continue;
    std::regex re = makeIdentRegex(physical);
    err = std::regex_replace(err, re, std::string("$1") + origin);
  }

  response->set_error_after_rewrite(err);
  response->set_message("success");
  response->set_code(rewriter::Success);
}

// gRPC thin wrapper — delegates to doRewriteErrorMessage.
grpc::Status RewriterServiceImpl::RewriteErrorMessage(
  ServerContext * /*context*/,
  const RewriteErrorMessageRequest *request,
  RewriteErrorMessageResponse *response) {
  doRewriteErrorMessage(request, response);
  return grpc::Status::OK;
}

void RewriterServiceImpl::doOptimize(
  const OptimizeRequest *request, OptimizeResponse *response) {
  const std::string &sql = request->sql();
  const std::string sql_id = shortSqlHash(sql);
  const auto t0 = std::chrono::steady_clock::now();

  struct AccessLog {
    const std::string &sql_id;
    const OptimizeResponse *response;
    std::chrono::steady_clock::time_point t0;
    size_t sql_len;
    ~AccessLog() {
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
      std::cerr << "[access] method=Optimize sql=" << sql_id
                << " code=" << response->code()
                << " swap_applied=" << (response->swap_applied() ? "1" : "0")
                << " sql_len=" << sql_len
                << " took_us=" << us << std::endl;
    }
  } _access_log{sql_id, response, t0, sql.size()};

  // Default response: original SQL echoed back. Any branch that bails sets
  // the appropriate code and leaves sql_after_optimize as the input.
  response->set_sql_after_optimize(sql);
  response->set_swap_applied(false);

  const char *pos = sql.data();
  const char *end = pos + sql.size();
  DB::ParserQuery parser(end, /*allow_settings_after_format_in_insert=*/false);
  DB::ASTPtr ast;
  try {
    ast = DB::parseQuery(parser, pos, end, /*description=*/"", 0, 0, 0);
  } catch (const DB::Exception &e) {
    response->set_code(rewriter::OptimizeCode::OptimizeSyntaxError);
    response->set_message(e.displayText());
    return;
  }

  std::unordered_map<std::string, uint64_t> sizes;
  sizes.reserve(request->table_sizes().size());
  for (const auto &kv : request->table_sizes()) {
    sizes[kv.first] = kv.second;
  }
  const uint64_t default_size = request->has_default_size() ? request->default_size() : 0;
  const double swap_threshold = request->has_swap_threshold() ? request->swap_threshold() : 1.0;

  const bool swapped = rewriter_handlers::applyJoinSwap(
    ast, sizes, default_size, swap_threshold);

  if (!swapped) {
    response->set_code(rewriter::OptimizeCode::OptimizeUnoptimizable);
    response->set_message("no swappable JOIN found");
    return;
  }

  response->set_sql_after_optimize(rewriter_handlers::formatAst(ast));
  response->set_swap_applied(true);
  response->set_code(rewriter::OptimizeCode::OptimizeOK);
  response->set_message("success");
}

// gRPC thin wrapper — delegates to doOptimize.
grpc::Status RewriterServiceImpl::Optimize(
  ServerContext * /*context*/,
  const OptimizeRequest *request,
  OptimizeResponse *response) {
  doOptimize(request, response);
  return grpc::Status::OK;
}

void RewriterServiceImpl::doMaterializeSQL(
  const MaterializeSQLRequest *request, MaterializeSQLResponse *response) {
  const std::string sql_id = shortSqlHash(request->sql());
  const auto t0 = std::chrono::steady_clock::now();

  struct AccessLog {
    const std::string &sql_id;
    const MaterializeSQLResponse *response;
    std::chrono::steady_clock::time_point t0;
    size_t sql_len;
    ~AccessLog() {
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
      std::cerr << "[access] method=MaterializeSQL sql=" << sql_id
                << " code=" << response->code()
                << " replacements=" << response->replacements_size()
                << " sql_len=" << sql_len
                << " took_us=" << us << std::endl;
    }
  } _access_log{sql_id, response, t0, request->sql().size()};

  rewriter_handlers::materialize(request, response);
}

grpc::Status RewriterServiceImpl::MaterializeSQL(
  ServerContext * /*context*/,
  const MaterializeSQLRequest *request,
  MaterializeSQLResponse *response) {
  doMaterializeSQL(request, response);
  return grpc::Status::OK;
}

// Injected at build time from `git describe --tags --always --dirty`
// (see CMakeLists.txt). Stays in lock-step with git tags instead of drifting
// from a hand-maintained literal.
const std::string VERSION = REWRITER_VERSION;
const std::string hash = GIT_COMMIT_HASH;

void RunServer(uint16_t grpc_port, uint16_t http_port,
               std::shared_ptr<const snapshot_profiles::Profiles> profiles) {
  std::cout << "Rewriter Server Version: " << VERSION << " (" << hash << ")" << std::endl;

  std::string server_address = "0.0.0.0:" + std::to_string(grpc_port);
  RewriterServiceImpl service(std::move(profiles));

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

Status RewriterServiceImpl::AnalyzeSnapshotQuery(ServerContext *context, const rewriter::AnalyzeSnapshotQueryRequest *request, rewriter::AnalyzeSnapshotQueryResponse *response) {
  snapshot_query::Execution execution;
  execution.cancelled = [context] { return context && context->IsCancelled(); };
  doAnalyzeSnapshotQuery(request, response, std::move(execution));
  return Status::OK;
}
Status RewriterServiceImpl::PrepareSnapshotQuery(ServerContext *context, const rewriter::PrepareSnapshotQueryRequest *request, rewriter::PrepareSnapshotQueryResponse *response) {
  snapshot_query::Execution execution;
  execution.cancelled = [context] { return context && context->IsCancelled(); };
  doPrepareSnapshotQuery(request, response, std::move(execution));
  return Status::OK;
}
