// src/handlers/select.h
//
// SELECT and other DML handler: applies the rewrite-option pipeline
// (Limit, TableName, Offset, Settings, CTE) via ASTTransformers and
// populates original_accessed_table_names from the pre-transformer AST.
#pragma once
#include <Parsers/IAST.h>
#include <google/protobuf/repeated_ptr_field.h>

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

using ParsedCTEASTMap = std::unordered_map<std::string, DB::ASTPtr>;

// Populate `response` with the rewritten SELECT/DML SQL and the set of
// originally accessed table names (including any tables reached through CTEs).
void handleSelectQuery(DB::ASTPtr ast,
                       const rewriter::RewriteSQLRequest *request,
                       rewriter::RewriteSQLResponse *response);

// Apply only the AST-level transformer pipeline. Used by
// doRewriteErrorMessage to recompute the rewritten SQL for message
// substitution without touching response state.
//
// When `out_table_rewrites` is non-null, every TableNameRewrite hit is
// appended as "origin" -> "rewritten" (later writes overwrite earlier ones
// if the same identifier is visited twice). Pass nullptr to skip the
// bookkeeping.
DB::ASTPtr applyRewriteOptions(
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options,
  DB::ASTPtr ast,
  const std::vector<ParsedCTEASTMap> *preparsed_cte_ast_maps = nullptr,
  std::map<std::string, std::string> *out_table_rewrites = nullptr);

} // namespace rewriter_handlers
