// src/handlers/writes.h
//
// Write-statement handler. Covers CREATE/DROP/ALTER TABLE, INSERT, UPDATE,
// DELETE, RENAME TABLE, CREATE/DROP VIEW, CREATE MATERIALIZED VIEW, plus the
// special-cased CREATE/DROP DATABASE and TRUNCATE TABLE paths.
// Rejects the unsupported variants (CREATE LIVE/WINDOW VIEW, CREATE
// DICTIONARY, OPTIMIZE, DETACH, UNDROP, MOVE, KILL, BACKUP/RESTORE,
// ATTACH/REPLACE/FETCH PARTITION FROM ..., ...).
//
// GRANT and REVOKE are handled by handlers/grant.cc, which dispatches
// before this handler in rewriter-server.cc.
//
// Applies TableNameRewrite options from `request->options()` to each named
// table slot before echoing the SQL. Other rewrite ops (Limit, Settings,
// CTE, ...) are ignored for writes. A Remote hit for any write target is
// rejected — remote_table_map is SELECT-only.
#pragma once
#include <Parsers/IAST.h>

#include <string>

#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

enum class WriteDispatchResult {
  Handled,     // Accepted: response holds the normalized SQL + classification.
  Rejected,    // Rejected: response holds RewriteError + message. Caller returns.
  NotAWrite,   // Not a write statement - caller falls through to SELECT.
};

// ClickHouse 26.3 removed LIVE VIEW from ParserQuery. Preserve the rewriter's
// established classification and storage-integrity preflight for syntactically
// valid CREATE/ATTACH LIVE VIEW statements before the normal parse path.
bool handleRemovedLiveViewCompatibility(
  const std::string &query,
  const rewriter::RewriteSQLRequest *request,
  rewriter::RewriteSQLResponse *response);

WriteDispatchResult handleWriteQuery(DB::ASTPtr ast,
                                     const rewriter::RewriteSQLRequest *request,
                                     rewriter::RewriteSQLResponse *response);

} // namespace rewriter_handlers
