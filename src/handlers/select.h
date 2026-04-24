// src/handlers/select.h
//
// SELECT and other DML handler: applies the rewrite-option pipeline
// (Limit, TableName, Offset, Settings, CTE) via ASTTransformers and
// populates original_accessed_table_names from the pre-transformer AST.
#pragma once
#include <Parsers/IAST.h>
#include <google/protobuf/repeated_ptr_field.h>

#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

// Populate `response` with the rewritten SELECT/DML SQL and the set of
// originally accessed table names (including any tables reached through CTEs).
void handleSelectQuery(DB::ASTPtr ast,
                       const rewriter::RewriteSQLRequest *request,
                       rewriter::RewriteSQLResponse *response);

// Apply only the AST-level transformer pipeline. Used by
// doRewriteErrorMessage to recompute the rewritten SQL for message
// substitution without touching response state.
DB::ASTPtr applyRewriteOptions(
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options,
  DB::ASTPtr ast);

} // namespace rewriter_handlers
