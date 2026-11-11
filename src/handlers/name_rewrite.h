// src/handlers/name_rewrite.h
//
// Shared helpers for applying TableNameRewrite options.
//
// Unified static→dynamic precedence: every table-targeting handler — SELECT
// plus the non-SELECT single-target handlers (writes, EXISTS TABLE, SHOW
// CREATE TABLE, RENAME TABLE) — picks mode the same way:
//
//   1. If `static_args` is set -> Static. Three-map lookup (table_map /
//      remote_table_map / table_with_database_map). SELECT walks via
//      ASTReplaceTransformer + planTableRewrite() (see ASTTransformers.h);
//      non-SELECT goes through lookupStaticTableRewrite() defined here.
//      Empty static_args (set but every map empty) is intentional — it
//      says "static-only, no rewrite", so every target naturally lands in
//      passthrough. Unmatched targets pass through unchanged in all cases
//      (statement-level rule — no per-target fallback to dynamic).
//      Non-SELECT remote_table_map hits reject as UnsupportedStatement
//      (you can't write to a remote() function).
//   2. Else if `dynamic_args` is set -> Dynamic. The construction:
//
//        physical := database_map[logical]  (or logical itself if it's in
//                                            known_physical_databases)
//        new_table := logical + (delim + extras.join(delim) iff extras
//                                non-empty) + "." + original_table
//
//      `logical` is the SQL's qualified db if present, else
//      `upstream_logical_database_in_context`. The trailing separator
//      between the prefix and `original_table` is always a literal "."
//      (not the configurable `delim`) — see buildDynamicTableName for why.
//      `delim` defaults to "_" when not set. Examples (delim="_"):
//
//        extras=[]           tenant1.events       -> phys.`tenant1.events`
//        extras=[]           events (USE tenant1) -> phys.`tenant1.events`
//        extras=["shard0"]   tenant1.events       -> phys.`tenant1_shard0.events`
//
//      SELECT skips on InvalidRequest only for `system` tables and CTE
//      aliases (every other unresolvable read is refused first,
//      housegate/housegate#336); non-SELECT applies it strict (reject).
//   3. Else -> None (passthrough).
//
// Database-level handlers (USE, SHOW TABLES, SHOW DATABASES, CREATE
// DATABASE) bypass this precedence and read `dynamic_args` directly via
// findDynamicArgs — `static_args` is table-keyed and has nothing to say
// about a database-level operation.
#pragma once

#include <google/protobuf/repeated_ptr_field.h>
#include <optional>
#include <string>

#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

enum class DynamicRewriteStatus {
  OK,                 // Regular rewrite — physical_db + new_table populated;
                      // caller emits a bare `physical_db.new_table` identifier.
  OkRemote,           // Remote rewrite — physical_db + new_table populated
                      // PLUS remote_addr / remote_user / remote_password.
                      // SELECT path replaces the identifier with a
                      // `remote(addr, db, table, user, password)` table
                      // function call; non-SELECT handlers reject this with
                      // RewriteCode.UnsupportedStatement.
  InvalidRequest,     // Logical DB can't be resolved (not in database_map,
                      // not in known_physical_databases, and no upstream
                      // context), or a remote upstream key is missing
                      // from `remote_upstreams`. Caller should return
                      // InvalidRewriteRequest.
  NoRewriteRequested, // Request carries no TableNameRewrite option with
                      // `dynamic_args` set — handler should passthrough
                      // (echo the original SQL).
};

struct DynamicRewriteOutcome {
  DynamicRewriteStatus status = DynamicRewriteStatus::NoRewriteRequested;
  std::string physical_db;   // OK / OkRemote: target physical database.
  std::string new_table;     // OK / OkRemote: constructed table name (empty
                             // for USE — which is DB-only — and for SHOW
                             // TABLES, whose handler builds its own output).
  std::string logical_db;    // OK / OkRemote: the resolved logical db name
                             // (for table_rewrites bookkeeping).
  std::string reject_reason; // InvalidRequest: human-readable explanation.

  // Populated only when status == OkRemote. Read straight from the matched
  // RewriteTableDynamicArgs.RemoteUpstream entry.
  std::string remote_addr;
  std::string remote_user;
  std::string remote_password;
};

// True iff `logical` is in `dynamic_args.logical_database_to_remote_upstream_index`.
// Used by USE (which doesn't go through applyDynamicRewrite) to safety-reject
// when the target db points at a remote cluster — without this check USE
// would silently rewrite the request against the local physical instead of
// warning the caller. SHOW TABLES does its own inline lookup so it can
// also fetch the matching `remote_upstreams` entry in one step.
bool isLogicalRemoteMapped(
  const std::string &logical,
  const rewriter::RewriteTableDynamicArgs &args);

// Pick the last TableNameRewrite option in `options` whose `dynamic_args`
// sub-message is present, or return nullptr. Multiple options last-wins.
//
// Used by handlers that don't have a per-table target (USE, SHOW TABLES,
// SHOW DATABASES, CREATE DATABASE) — they bypass the unified
// static→dynamic precedence below since `static_args` is table-keyed and
// has nothing to say about a database-level operation.
const rewriter::RewriteTableDynamicArgs* findDynamicArgs(
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options);

// Which TableNameRewrite mode applies to a given request, per the
// statement-level precedence:
//
//   1. If `static_args` is set -> Static (even when every map is empty;
//      the caller explicitly opted into static-only and wants no rewrite
//      beyond what they listed).
//   2. Else if `dynamic_args` is set -> Dynamic.
//   3. Else                          -> None.
//
// Last-wins across multiple TableNameRewrite options: each option that
// carries either sub-message overrides any prior selection. Options with
// no TableNameRewrite-relevant args are skipped (don't reset the prior).
enum class TableRewriteMode { None, Static, Dynamic };

struct TableRewriteSelection {
  TableRewriteMode mode = TableRewriteMode::None;
  const rewriter::RewriteTableStaticArgs *static_args = nullptr;    // Mode::Static
  const rewriter::RewriteTableDynamicArgs *dynamic_args = nullptr;  // Mode::Dynamic
};

TableRewriteSelection findActiveTableRewrite(
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options);

// Static-mode lookup of one (origin_db, origin_table) target against the
// three-map policy. Lookup key is "origin_db.origin_table" (or bare
// "origin_table" when origin_db is empty), matching how ASTReplaceTransformer
// keys the SELECT-side maps.
//
// Lookup precedence matches the SELECT path: table_map -> remote_table_map
// -> table_with_database_map.
//
// Used by the non-SELECT single-target handlers (writes, EXISTS, SHOW
// CREATE, RENAME). SELECT goes through ASTReplaceTransformer directly and
// doesn't need this helper.
struct StaticLookupOutcome {
  enum class Status {
    Matched,            // Rewrite applied; new_db / new_table populated.
    NotMatched,         // Target absent from all three maps; caller passes
                        // through (no rewrite, no rejection — matches the
                        // statement-level "static-mode picked once" rule).
    RemoteUnsupported,  // Target hit remote_table_map; non-SELECT can't
                        // target a remote() function, caller should reject
                        // as UnsupportedStatement.
  };
  Status status = Status::NotMatched;
  // Matched: target db. Empty when the matched entry only renamed the
  // table (table_map hit with origin_db empty, or table_with_database_map
  // hit whose `database` field was empty). Callers should leave the
  // origin_db in place when this is empty.
  std::string new_db;
  std::string new_table;
};

StaticLookupOutcome lookupStaticTableRewrite(
  const std::string &origin_db,
  const std::string &origin_table,
  const rewriter::RewriteTableStaticArgs &args);

// Apply the dynamic construction to one (db, table) target. Intended for
// single-table handlers (INSERT/CREATE/DROP/ALTER/UPDATE/DELETE/EXISTS/
// SHOW CREATE/RENAME side). USE passes `origin_table = ""` and discards
// `new_table` from the result.
//
// `stmt_kind` is for error messages (e.g. "INSERT", "EXISTS TABLE").
DynamicRewriteOutcome applyDynamicRewrite(
  const std::string &origin_db,
  const std::string &origin_table,
  const rewriter::RewriteTableDynamicArgs &args,
  std::string_view stmt_kind);

// Resolve a logical DB to a physical one via database_map ∪
// known_physical_databases. Returns std::nullopt when the user does not
// have that logical DB (caller should emit InvalidRewriteRequest).
std::optional<std::string> resolvePhysicalDatabase(
  const std::string &logical_db,
  const rewriter::RewriteTableDynamicArgs &args);

// True for ClickHouse's built-in metadata databases, in the spellings
// ClickHouse accepts (`system`, `information_schema`, `INFORMATION_SCHEMA`).
// Their tables are judged by the system-table allowlist, not by the
// logical-to-physical mapping.
bool isClickHouseMetadataDatabase(const std::string &db);

// Build the dynamic table name:
//   logical + (delim + extras.join(delim) iff extras non-empty)
//           + "." + original_table
//
// Examples (delim="_", extra_arguments=[]):
//   tenant1.events       -> physical_db_1.`tenant1.events`
//   events (USE tenant1) -> physical_db_1.`tenant1.events`
//
// Example (delim="_", extra_arguments=["shard0"]):
//   tenant1.events       -> physical_db_1.`tenant1_shard0.events`
//
// `delim` defaults to "_" when `args.delim()` is empty. The trailing
// separator before `original_table` is always a literal "." — it is NOT
// configurable. The CH formatter wraps the resulting table name in
// backticks via IdentifierQuotingRule::WhenNecessary because it now
// contains a dot, so the SQL output above is what callers actually see.
std::string buildDynamicTableName(
  const std::string &logical_db,
  const std::string &original_table,
  const rewriter::RewriteTableDynamicArgs &args);

// Build the prefix that gets prepended to every table name in the logical
// database, i.e. buildDynamicTableName(logical, "", args). Used by SHOW
// TABLES to filter system.tables and to strip the prefix from the output
// column.
std::string buildDynamicTablePrefix(
  const std::string &logical_db,
  const rewriter::RewriteTableDynamicArgs &args);

// Best-effort resolution of one (origin_db, origin_table) access against the
// active TableNameRewrite mode, for response.original_accessed_tables. Does
// NOT mutate any AST and does NOT reject — when a target can't be resolved
// (static-mode unmatched, dynamic-mode unresolvable logical, no TableNameRewrite
// option), the unresolved fields stay empty. SELECT calls this for every
// ASTTableExpression it collects so callers can see, per access, the logical
// db and physical db that the rewriter routed (or would have routed) it to.
struct AccessedTableResolution {
  std::string logical_database;   // empty in static mode and when dynamic mode
                                  // has no qualifier + no upstream context.
  std::string physical_database;  // empty when the rewriter doesn't know
                                  // (unmatched static / unresolvable dynamic).
  bool is_remote = false;         // true iff this access maps to a `remote(...)`
                                  // table function call (static remote_table_map
                                  // hit or dynamic logical_database_to_remote_upstream_index hit).
  bool is_storage_integrity = false; // true for logical SI hits and direct
                                     // protocol-owned physical namespaces.
};

AccessedTableResolution resolveAccessedTable(
  const std::string &origin_db,
  const std::string &origin_table,
  const TableRewriteSelection &selection);

// Append one AccessedTable entry to response.original_accessed_tables for the
// (origin_db, origin_table) target the handler is about to process. Resolves
// `logical_database` / `physical_database` / `is_remote` against the active
// TableRewriteSelection via resolveAccessedTable. No-op when origin_table is
// empty (db-level operations record their access via `database_rewrites` and
// don't populate this field).
//
// Called by every non-SELECT table-targeting handler (writes / EXISTS / SHOW
// CREATE / RENAME) once per target it intends to rewrite. SELECT populates
// the field via its own pre-rewrite AST walk in handlers/select.cc.
void recordAccessedTable(
  rewriter::RewriteSQLResponse *response,
  const std::string &origin_db,
  const std::string &origin_table,
  const TableRewriteSelection &selection);

// Append one AccessedTable entry for a database-level access (CREATE
// DATABASE / DROP DATABASE), where the SQL target is a database name and
// has no table component. Populates:
//   * original_database = target_db
//   * original_table    = ""
//   * logical_database  = target_db (same as original — the SQL named it
//                                    explicitly as a logical)
//   * physical_database = resolvePhysicalDatabase(target_db, *dynamic_args)
//                         when dynamic_args is non-null and the logical
//                         resolves; empty otherwise (notably for CREATE
//                         DATABASE on a brand-new logical not yet in
//                         database_map).
//   * is_remote         = false (DDL never routes through remote()).
//
// Use this instead of recordAccessedTable for db-level statements; the
// table-level helper requires a non-empty origin_table. USE / SHOW TABLES
// / SHOW DATABASES still don't populate this field — those are pure DB
// rewrites surfaced via `database_rewrites` instead. Only CREATE DATABASE
// and DROP DATABASE call this helper.
void recordAccessedDatabase(
  rewriter::RewriteSQLResponse *response,
  const std::string &target_db,
  const rewriter::RewriteTableDynamicArgs *dynamic_args);

// Append an {origin -> rewritten} entry to response.table_rewrites. Both
// sides are formatted as "db.table" (or bare "table" when origin_db is
// empty). No-op when origin == rewritten or when origin_table is empty —
// DB-only rewrites belong in `database_rewrites`, not here.
void recordTableRewrite(
  rewriter::RewriteSQLResponse *response,
  const std::string &origin_db,
  const std::string &origin_table,
  const std::string &new_db,
  const std::string &new_table);

// Append an {origin -> rewritten} entry to response.database_rewrites. No-op
// when origin_db == new_db. Used by USE and SHOW TABLES.
void recordDatabaseRewrite(
  rewriter::RewriteSQLResponse *response,
  const std::string &origin_db,
  const std::string &new_db);

} // namespace rewriter_handlers
