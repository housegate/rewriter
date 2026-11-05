#include "rewriter-server.h"

#include <IO/WriteBufferFromString.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/IdentifierQuotingStyle.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>
#include <grpc++/grpc++.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
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
#include "error_message_rewrite.h"
#include "query_preprocessor.h"
#include "request_limits.h"
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

// Bounds shared by the request paths below.
namespace {

// Shown in place of a ClickHouse error that RewriteErrorMessage cannot map
// back within its bounds (an oversized request, a spent scan budget, a
// cancelled call): returned half-rewritten it could still name physical
// databases and tables.
constexpr std::string_view kWithheldErrorMessage =
  "Query failed. The error details were withheld: the rewriter could not map them "
  "back to the original query within its limits.";

// Bytes of caller SQL a request makes the rewriter parse: the statement plus
// every CommonTableExprRewrite body.
size_t requestSqlBytes(
  const std::string &sql,
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options) {
  size_t total = sql.size();
  for (const auto &option : options) {
    if (option.op() != rewriter::RewriteOp::CommonTableExprRewrite) continue;
    for (const auto &kv : option.common_table_expr_args().cte_map()) total += kv.second.sql().size();
  }
  return total;
}

bool sqlExceedsLimit(
  const std::string &sql,
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options) {
  const size_t limit = rewriter_limits::maxRequestSqlBytes();
  return limit != 0 && requestSqlBytes(sql, options) > limit;
}

// What a RewriteErrorMessage result may not contain once the mapping passes
// have run: the physical side of the request's routing that the caller has no
// name for. A mention left over means a pass missed one (an error naming a
// database the statement's rewrite did not touch, a remote host, a protocol
// database), so the text is withheld rather than returned.
struct HiddenNames {
  std::vector<std::string> identifiers;  // physical databases
  std::vector<std::string> literals;     // remote addresses and credentials
};

std::string_view databasePart(std::string_view qualified) {
  const size_t dot = qualified.find('.');
  return dot == std::string_view::npos ? std::string_view{} : qualified.substr(0, dot);
}

// A remote address as configured, and every host it names: ClickHouse's
// remote() takes a comma-separated list of shards and `|`-separated replicas,
// each `host`, `host:port`, `[ipv6]` or `[ipv6]:port`. An error may name a
// host without the rest of the address, so each host is a literal of its own.
// Parts shorter than four bytes are dropped, as they would match ordinary text.
void addRemoteAddress(std::vector<std::string> &literals, const std::string &addr) {
  constexpr size_t kMinLiteral = 4;
  const auto add = [&literals](std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    if (value.size() >= kMinLiteral) literals.emplace_back(value);
  };
  add(addr);
  std::string_view rest = addr;
  while (!rest.empty()) {
    const size_t cut = rest.find_first_of(",|");
    std::string_view part = rest.substr(0, cut);
    rest = cut == std::string_view::npos ? std::string_view{} : rest.substr(cut + 1);
    while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.remove_prefix(1);
    add(part);
    if (!part.empty() && part.front() == '[') {
      if (const size_t close = part.find(']'); close != std::string_view::npos) add(part.substr(1, close - 1));
    } else if (const size_t colon = part.find(':'); colon != std::string_view::npos && part.rfind(':') == colon) {
      add(part.substr(0, colon));
    }
  }
}

void addSecret(std::vector<std::string> &literals, const std::string &secret) {
  constexpr size_t kMinLiteral = 4;
  if (secret.size() >= kMinLiteral) literals.push_back(secret);
}

HiddenNames hiddenNamesFor(
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options) {
  HiddenNames hidden;
  std::set<std::string> physical;
  std::set<std::string> visible;  // names the caller may write itself
  const auto selection = rewriter_handlers::findActiveTableRewrite(options);
  if (selection.mode == rewriter_handlers::TableRewriteMode::Dynamic) {
    const auto &args = *selection.dynamic_args;
    // Candidates, kept only when the table-reference policy protects them
    // (protectedDatabase): the SI databases count while the surface is active,
    // and reserved_databases only under contract V2, as for every other rule.
    std::set<std::string> candidates(args.protected_databases().begin(), args.protected_databases().end());
    for (const auto &[logical, db] : args.database_map()) {
      visible.insert(logical);
      candidates.insert(db);
    }
    const auto &si = args.storage_integrity();
    candidates.insert(si.reserved_databases().begin(), si.reserved_databases().end());
    for (const auto &[logical, table] : si.tables()) {
      candidates.insert(std::string(databasePart(table.safe_table())));
      candidates.insert(std::string(databasePart(table.unsafe_table())));
    }
    for (const auto &db : candidates)
      if (rewriter_handlers::protectedDatabase(db, args)) physical.insert(db);
    for (const auto &db : args.known_physical_databases())
      if (!physical.contains(db)) visible.insert(db);
    if (!args.upstream_logical_database_in_context().empty())
      visible.insert(args.upstream_logical_database_in_context());
    for (const auto &[key, upstream] : args.remote_upstreams()) {
      addRemoteAddress(hidden.literals, upstream.addr());
      addSecret(hidden.literals, upstream.password());
    }
  } else if (selection.mode == rewriter_handlers::TableRewriteMode::Static) {
    const auto &args = *selection.static_args;
    const auto addOrigin = [&visible](const std::string &origin) {
      if (const auto db = databasePart(origin); !db.empty()) visible.insert(std::string(db));
    };
    for (const auto &[origin, table] : args.table_map()) addOrigin(origin);
    for (const auto &[origin, target] : args.table_with_database_map()) {
      addOrigin(origin);
      physical.insert(target.database());
    }
    for (const auto &[origin, remote] : args.remote_table_map()) {
      addOrigin(origin);
      physical.insert(remote.database());
      addRemoteAddress(hidden.literals, remote.addr());
      addSecret(hidden.literals, remote.password());
    }
  }
  for (const auto &db : physical)
    if (!db.empty() && !visible.contains(db)) hidden.identifiers.push_back(db);
  return hidden;
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

  // Bound the SQL this request makes the rewriter preprocess and parse (the
  // statement and every CTE body) before any of that work starts.
  if (sqlExceedsLimit(original_query, request->options())) {
    response->set_message("query exceeds maximum supported size");
    response->set_code(rewriter::RewriteCode::RewriteError);
    return;
  }

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
    parsed_ast = rewriter_handlers::parseBounded(parser, pos, end);
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
        marked_ast = rewriter_handlers::parseBounded(marked_parser, marked_pos, marked_end);
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
  RewriteErrorMessageResponse *response,
  std::function<bool()> cancelled) {

  const std::string sql_id = shortSqlHash(request->sql());
  const size_t sql_len = request->sql().size();
  const size_t err_len = request->error_message().size();
  const auto t0 = std::chrono::steady_clock::now();
  // Why the error text was withheld, for the access log; empty otherwise.
  std::string withheld;

  struct AccessLog {
    const std::string &sql_id;
    const RewriteErrorMessageResponse *response;
    const std::string &withheld;
    std::chrono::steady_clock::time_point t0;
    size_t sql_len;
    size_t err_len;
    ~AccessLog() {
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
      std::ostringstream line;
      line << "[access] method=RewriteErrorMessage sql=" << sql_id
           << " code=" << response->code()
           << " sql_len=" << sql_len
           << " err_len=" << err_len;
      if (!withheld.empty()) line << " withheld=\"" << withheld << "\"";
      line << " took_us=" << us << '\n';
      std::cerr << line.str() << std::flush;
    }
  } _access_log{sql_id, response, withheld, t0, sql_len, err_len};

  if (request->error_message().empty()) {
    response->set_message("Error message is empty");
    response->set_code(rewriter::Success);
    return;
  }

  // Fail closed: the caller shows error_after_rewrite to the client, so text
  // the passes below could not finish with is replaced, never returned raw.
  const auto withhold = [&](const char *reason) {
    withheld = reason;
    response->set_error_after_rewrite(std::string(kWithheldErrorMessage));
    response->set_message(std::string("error message withheld: ") + reason);
    response->set_code(rewriter::Success);
  };

  const size_t max_error_bytes = rewriter_limits::maxErrorMessageBytes();
  if (sqlExceedsLimit(request->sql(), request->options())
      || (max_error_bytes != 0 && err_len > max_error_bytes)) {
    withhold("input too large");
    return;
  }
  const auto isCancelled = [&cancelled] { return cancelled && cancelled(); };
  if (isCancelled()) {
    withhold("cancelled");
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
    // Without the rewrite there is nothing to invert, and the raw error can
    // name physical databases and tables: withhold it.
    withhold("rewrite failed");
    return;
  }

  // Every pass below is a linear scan charged to one budget, so the work is
  // bounded however many mappings the statement produced.
  rewriter_error::WorkBudget budget(rewriter_limits::errorRewriteScanBudget(), isCancelled);
  const auto budgetStopped = [&]() {
    withhold(isCancelled() ? "cancelled" : "scan budget exhausted");
  };
  std::string err = request->error_message();

  const std::string &rewritten_sql = rewrite_resp.sql_after_rewrite();
  const std::string &original_sql = request->sql();
  if (!rewritten_sql.empty() && rewritten_sql != original_sql) {
    // Remap byte-position references ("failed at position N") FIRST, while
    // they still match the rewritten SQL we're about to swap out. Without
    // this the user reads "position N" against their original SQL and ends
    // up at a totally wrong token (the rewritten table prefix shifts every
    // position past its insertion point). The diff runs only when the error
    // cites a position, and gives up past kMaxOffsetMapEdits edits, leaving
    // the positions as ClickHouse printed them.
    if (!budget.charge(err.size())) return budgetStopped();
    if (const auto refs = rewriter_error::findPositionRefs(err); !refs.empty()) {
      const auto pos_map = rewriter_error::buildOffsetMap(
        rewritten_sql, original_sql, rewriter_error::kMaxOffsetMapEdits, budget);
      if (budget.exhausted()) return budgetStopped();
      if (pos_map) err = rewriter_error::remapErrorPositions(err, refs, *pos_map);
    }

    // Whole-SQL replacement: when the error embeds the rewritten SQL (the
    // common "In scope SELECT ..." form), swap the whole block back to the
    // user's original SQL. Catches the remote() case too — its full
    // remote('addr', 'db', 'table', 'user', '[HIDDEN]') call lives only in
    // sql_after_rewrite, not in table_rewrites. An exact first occurrence
    // wins; otherwise every case- and whitespace-insensitive occurrence.
    const auto exact = rewriter_error::findExact(err, rewritten_sql, budget);
    if (budget.exhausted()) return budgetStopped();
    if (exact) {
      err.replace(*exact, rewritten_sql.size(), original_sql);
    } else {
      auto swapped = rewriter_error::replaceSqlBlock(err, rewritten_sql, original_sql, budget);
      if (!swapped) return budgetStopped();
      err = std::move(*swapped);
    }
  }

  // Per-table substitution: rewritten qualified name -> origin. Picks up
  // out-of-SQL-block mentions like "Table physical_db.`tenant1.events`
  // does not exist".
  for (const auto &kv : rewrite_resp.table_rewrites()) {
    const std::string &origin = kv.first;
    const std::string &rewritten = kv.second;
    if (origin == rewritten || rewritten.empty()) continue;
    auto replaced = rewriter_error::replaceQualifiedIdentifier(err, rewritten, origin, budget);
    if (!replaced) return budgetStopped();
    err = std::move(*replaced);
  }

  // Per-database substitution: physical -> logical. Runs after table-level
  // replacement so dotted physical_db.`tenant1.events` references are
  // resolved as a unit first; only standalone physical_db tokens (e.g.
  // "Database physical_db_1 does not exist") remain to match here.
  for (const auto &kv : rewrite_resp.database_rewrites()) {
    const std::string &origin = kv.first;
    const std::string &physical = kv.second;
    if (origin == physical || physical.empty()) continue;
    auto replaced = rewriter_error::replaceIdentifier(err, physical, origin, budget);
    if (!replaced) return budgetStopped();
    err = std::move(*replaced);
  }

  // Fail closed on what the passes left: a physical database, remote address
  // or credential of this request's routing still in the text.
  const HiddenNames hidden = hiddenNamesFor(request->options());
  for (const auto &ident : hidden.identifiers) {
    const auto mentioned = rewriter_error::containsIdentifier(err, ident, budget);
    if (!mentioned) return budgetStopped();
    if (*mentioned) return withhold("physical name remained");
  }
  for (const auto &literal : hidden.literals) {
    const auto found = rewriter_error::findExact(err, literal, budget);
    if (budget.exhausted()) return budgetStopped();
    if (found) return withhold("physical name remained");
  }

  response->set_error_after_rewrite(err);
  response->set_message("success");
  response->set_code(rewriter::Success);
}

// gRPC thin wrapper — delegates to doRewriteErrorMessage, handing it the
// call's cancellation (the client gave up or its deadline passed).
grpc::Status RewriterServiceImpl::RewriteErrorMessage(
  ServerContext *context,
  const RewriteErrorMessageRequest *request,
  RewriteErrorMessageResponse *response) {
  doRewriteErrorMessage(request, response, [context] { return context && context->IsCancelled(); });
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
    ast = rewriter_handlers::parseBounded(parser, pos, end);
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
  // Bound the sync server's thread pool: past it, new calls fail fast with
  // RESOURCE_EXHAUSTED (and the client can retry another replica) instead of
  // every call slowing down together.
  const int max_threads = rewriter_limits::maxServerThreads();
  grpc::ResourceQuota quota("rewriter");
  quota.SetMaxThreads(max_threads);
  builder.SetResourceQuota(quota);
  // gRPC's 4 MiB default would cap requests below the configured SQL and
  // error-text limits; derive the cap from them.
  const int max_recv_bytes = rewriter_limits::maxReceiveMessageBytes();
  builder.SetMaxReceiveMessageSize(max_recv_bytes);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  if (!server) {
    std::cerr << "failed to start the gRPC server on " << server_address << std::endl;
    std::exit(1);
  }
  std::cout << "Server listening on " << server_address << " (gRPC)"
            << " max_threads=" << max_threads
            << " max_recv_bytes=" << max_recv_bytes
            << " max_sql_bytes=" << rewriter_limits::maxRequestSqlBytes()
            << " max_error_bytes=" << rewriter_limits::maxErrorMessageBytes() << std::endl;

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
