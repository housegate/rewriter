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
//        new_table := logical + delim + (extra_args joined by delim + delim
//                                        iff extra_args is non-empty)
//                             + original_table
//
//      `logical` is the SQL's qualified db if present, else
//      `upstream_logical_database_in_context`. SELECT applies this lenient
//      (skip on InvalidRequest); non-SELECT applies it strict (reject).
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
  OK,                 // Rewrite applied; physical_db + new_table are populated.
  InvalidRequest,     // Logical DB can't be resolved (not in database_map,
                      // not in known_physical_databases, and no upstream
                      // context). Caller should return InvalidRewriteRequest.
  NoRewriteRequested, // Request carries no TableNameRewrite option with
                      // `dynamic_args` set — handler should passthrough
                      // (echo the original SQL).
};

struct DynamicRewriteOutcome {
  DynamicRewriteStatus status = DynamicRewriteStatus::NoRewriteRequested;
  std::string physical_db;   // OK: target physical database.
  std::string new_table;     // OK: constructed table name (empty for USE —
                             // which is DB-only — and for SHOW TABLES,
                             // whose handler builds its own output SQL).
  std::string logical_db;    // OK: the resolved logical db name (for
                             // table_rewrites bookkeeping).
  std::string reject_reason; // InvalidRequest: human-readable explanation.
};

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

// Build the dynamic table name:
//   logical + delim + (extra_args joined by delim + delim if non-empty)
//                   + original_table
// With empty extra_args: logical + delim + original_table (no double delim).
// `delim` defaults to "_" when `args.delim()` is empty.
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
