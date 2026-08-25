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

// SHOW [EXTENDED] [FULL] COLUMNS|FIELDS {FROM|IN} <table> [{FROM|IN} <database>]
// and SHOW [EXTENDED] INDEX|INDEXES|INDICES|KEYS with the same shape. ClickHouse
// parses them into ASTShowColumnsQuery / ASTShowIndexesQuery, which
// handleShowTablesQuery's ASTShowTablesQuery cast never matched — so under an
// active storage-integrity contract they used to reach the Spec I D1 catch-all
// (accidentally fail-closed, with the generic unmodelled-statement message) and
// without one they fell all the way through to the SELECT dispatch.
//
// This family names BOTH a database and a table, so Spec N D2 gates it
// deliberately: the resolved (database, table) pair for a reserved physical
// database, the database alone for a storage-integrity logical database, and a
// verbatim pass-through once the namespace is proved ordinary.
bool handleShowColumnsQuery(DB::ASTPtr ast,
                            const rewriter::RewriteSQLRequest *request,
                            rewriter::RewriteSQLResponse *response);

} // namespace rewriter_handlers
