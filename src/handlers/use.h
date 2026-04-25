// src/handlers/use.h
//
// USE <db> rewrite. Consumes `dynamic_args.database_map` +
// `dynamic_args.known_physical_databases` from the TableNameRewrite option:
//   * `database_map` hit         -> "USE <physical>" + table_rewrites entry
//                                   for the origin logical DB
//   * known physical DB          -> leave SQL unchanged (logical == physical)
//   * otherwise                  -> InvalidRewriteRequest ("user does not
//                                   have this database")
//
// No TableNameRewrite option present -> passthrough.
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
