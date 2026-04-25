// src/handlers/show_tables.h
//
// SHOW TABLES rewrite. Converts the statement into a filtered SELECT on
// system.tables that strips the dynamic logical-DB prefix.
//
// Resolution:
//   logical := FROM/IN-clause (stripped to its leading name component)
//              OR dynamic_args.upstream_logical_database_in_context
//   physical := dynamic_args.database_map[logical] OR logical itself if
//              it's in dynamic_args.known_physical_databases
//   prefix := logical + delim + (extra_args joined by delim + delim iff
//                                extra_args is non-empty)
//
// Emits:
//   SELECT multiIf(startsWith(name, '<prefix>'),
//                  substring(name, length('<prefix>') + 1),
//                  name) AS name
//   FROM (SELECT name FROM system.tables
//         WHERE database = '<physical>' AND startsWith(name, '<prefix>'))
//
// Rejects with InvalidRewriteRequest when logical can't be resolved to a
// physical DB. Passes through unchanged when the request carries no
// TableNameRewrite option.
#pragma once
#include <Parsers/IAST.h>
#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

bool handleShowTablesQuery(DB::ASTPtr ast,
                           const rewriter::RewriteSQLRequest *request,
                           rewriter::RewriteSQLResponse *response);

} // namespace rewriter_handlers
