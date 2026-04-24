// src/handlers/writes.h
//
// Single-table write handler. Covers CREATE/DROP/ALTER TABLE, INSERT,
// UPDATE, DELETE, and rejects the unsupported variants (CREATE VIEW,
// DROP DATABASE, TRUNCATE, RENAME, OPTIMIZE, GRANT, KILL, BACKUP, ...).
//
// Writes are parsed for classification only; no rewrite-option pipeline
// is applied. sql_after_rewrite is the normalized original SQL,
// write_stmt_value.target_table carries the single target.
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
