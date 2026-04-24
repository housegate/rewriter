// src/handlers/exists.h
//
// EXISTS TABLE dispatcher. ClickHouse parses "EXISTS TABLE foo",
// "EXISTS foo" (bare), "EXISTS TEMPORARY TABLE foo" into
// ASTExistsTableQuery; "EXISTS VIEW", "EXISTS DICTIONARY",
// "EXISTS DATABASE" get their own AST types and are rejected here
// (the rewrite pipeline is table-oriented).
//
// Accepts EXISTS TABLE, applies TableNameRewrite to the target, and returns
// statement_type = "EXISTS_TABLE".
#pragma once
#include <Parsers/IAST.h>
#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

enum class ExistsDispatchResult {
  Handled,       // Accepted or rejected with a message — caller returns.
  NotExists,     // Not an EXISTS query — caller falls through to next dispatcher.
};

ExistsDispatchResult handleExistsQuery(DB::ASTPtr ast,
                                       const rewriter::RewriteSQLRequest *request,
                                       rewriter::RewriteSQLResponse *response);

} // namespace rewriter_handlers
