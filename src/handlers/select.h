// src/handlers/select.h
//
// SELECT and other DML handler: applies the rewrite-option pipeline
// (Limit, TableName, Offset, Settings, CTE) via ASTTransformers and
// populates original_accessed_tables (one AccessedTable per
// ASTTableExpression collected from the pre-transformer AST, with
// logical/physical/is_remote resolved against the active TableNameRewrite
// selection). Non-SELECT handlers populate the same field via
// recordAccessedTable in handlers/name_rewrite.cc.
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
struct TableRewriteSelection;

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

// Rewrite a SELECT subtree embedded in a DDL statement — the body of a
// CREATE VIEW / CREATE MATERIALIZED VIEW. Applies the full rewrite-option
// pipeline (Limit / TableName / Offset / Settings / CTE) and the
// cross-shard GLOBAL correctness pass, then records the tables the body
// accesses into response.original_accessed_tables and any rewrite hits
// into response.table_rewrites. Mirrors handleSelectQuery minus the final
// setSuccessResponse — the caller owns the response's code / sql /
// statement_type.
void rewriteEmbeddedViewBody(
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options,
  DB::ASTPtr body,
  rewriter::RewriteSQLResponse *response);

// Fail-closed preflight for SELECT subtrees embedded in a write statement
// (CREATE TABLE AS SELECT / INSERT ... SELECT). Only SI sources are recorded;
// ordinary write-target bookkeeping remains owned by writes.cc.
bool rejectEmbeddedStorageIntegrityRead(
  DB::ASTPtr body,
  const TableRewriteSelection &selection,
  rewriter::RewriteSQLResponse *response,
  rewriter::RewriteCode code = rewriter::RewriteCode::UnsupportedStatement);

} // namespace rewriter_handlers
