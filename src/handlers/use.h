// src/handlers/use.h
//
// USE <db> rewrite. Three branches, matching the proxy's regex semantics
// (see RewriteDatabaseNameArgs in rewriter.proto):
//   1. database_map hit        -> "USE <physical>"
//   2. known_physical_databases -> leave SQL unchanged
//   3. default_database        -> "USE <default>" + use_stmt_value.specified_database
#pragma once
#include <Parsers/IAST.h>
#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

// Returns true if the AST was a USE statement and the handler populated
// `response`. The caller must return immediately when this returns true.
bool handleUseQuery(DB::ASTPtr ast,
                    const rewriter::RewriteSQLRequest *request,
                    rewriter::RewriteSQLResponse *response);

} // namespace rewriter_handlers
