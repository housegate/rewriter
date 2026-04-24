// src/handlers/writes.h
//
// Write-statement handler. Covers CREATE/DROP/ALTER TABLE, INSERT,
// UPDATE, DELETE, RENAME TABLE, and rejects the unsupported variants
// (CREATE VIEW, DROP DATABASE, TRUNCATE, OPTIMIZE, GRANT, KILL, BACKUP,
// ATTACH/REPLACE/FETCH PARTITION FROM ..., ...).
//
// Applies TableNameRewrite options from `request->options()` to each named
// table slot before echoing the SQL. Other rewrite ops (Limit, Settings,
// CTE, ...) are ignored for writes. A Remote hit for any write target is
// rejected — remote_table_map is SELECT-only.
#pragma once
#include <Parsers/IAST.h>
#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

enum class WriteDispatchResult {
  Handled,     // Accepted: response holds the normalized SQL + classification.
  Rejected,    // Rejected: response holds RewriteError + message. Caller returns.
  NotAWrite,   // Not a write statement - caller falls through to SELECT.
};

WriteDispatchResult handleWriteQuery(DB::ASTPtr ast,
                                     const rewriter::RewriteSQLRequest *request,
                                     rewriter::RewriteSQLResponse *response);

} // namespace rewriter_handlers
