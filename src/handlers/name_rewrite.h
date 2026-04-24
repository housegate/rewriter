// src/handlers/name_rewrite.h
//
// Shared helpers for applying TableNameRewrite options.
//
// SELECT path continues to use the three-map policy (`select_stmt_args`)
// through ASTReplaceTransformer + planTableRewrite() — see ASTTransformers.h.
//
// Non-SELECT path (writes, USE, SHOW TABLES, EXISTS TABLE, SHOW CREATE
// TABLE, RENAME TABLE) uses the dynamic construction driven by
// `general_args`:
//
//     physical := database_map[logical]  (or logical itself if it's in
//                                         known_physical_databases)
//     new_table := logical + delim + (extra_args joined by delim + delim
//                                     iff extra_args is non-empty)
//                          + original_table
//
// `logical` is the SQL's qualified db if present, else
// `upstream_logical_database_in_context`.
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
                      // `general_args` set — handler should passthrough
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

// Pick the last TableNameRewrite option in `options` whose `general_args`
// sub-message is present, or return nullptr. Multiple options last-wins.
const rewriter::RewriteTableForDynamicArgs* findGeneralArgs(
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options);

// Apply the dynamic construction to one (db, table) target. Intended for
// single-table handlers (INSERT/CREATE/DROP/ALTER/UPDATE/DELETE/EXISTS/
// SHOW CREATE/RENAME side). USE passes `origin_table = ""` and discards
// `new_table` from the result.
//
// `stmt_kind` is for error messages (e.g. "INSERT", "EXISTS TABLE").
DynamicRewriteOutcome applyDynamicRewrite(
  const std::string &origin_db,
  const std::string &origin_table,
  const rewriter::RewriteTableForDynamicArgs &args,
  std::string_view stmt_kind);

// Resolve a logical DB to a physical one via database_map ∪
// known_physical_databases. Returns std::nullopt when the user does not
// have that logical DB (caller should emit InvalidRewriteRequest).
std::optional<std::string> resolvePhysicalDatabase(
  const std::string &logical_db,
  const rewriter::RewriteTableForDynamicArgs &args);

// Build the dynamic table name:
//   logical + delim + (extra_args joined by delim + delim if non-empty)
//                   + original_table
// With empty extra_args: logical + delim + original_table (no double delim).
// `delim` defaults to "_" when `args.delim()` is empty.
std::string buildDynamicTableName(
  const std::string &logical_db,
  const std::string &original_table,
  const rewriter::RewriteTableForDynamicArgs &args);

// Build the prefix that gets prepended to every table name in the logical
// database, i.e. buildDynamicTableName(logical, "", args). Used by SHOW
// TABLES to filter system.tables and to strip the prefix from the output
// column.
std::string buildDynamicTablePrefix(
  const std::string &logical_db,
  const rewriter::RewriteTableForDynamicArgs &args);

// Append an {origin -> rewritten} entry to response.table_rewrites. No-op
// when origin == rewritten. Either side may be bare "table" (no db part).
void recordTableRewrite(
  rewriter::RewriteSQLResponse *response,
  const std::string &origin_db,
  const std::string &origin_table,
  const std::string &new_db,
  const std::string &new_table);

} // namespace rewriter_handlers
