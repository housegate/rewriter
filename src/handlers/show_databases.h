// src/handlers/show_databases.h
//
// SHOW DATABASES rewrite. Returns logical DBs from dynamic_args.database_map
// (filtered by "does any table with the dynamic prefix exist in the
// physical DB") plus known_physical_databases that are present in
// system.databases. Honors SHOW DATABASES LIKE / NOT LIKE / ILIKE.
//
// Emits, with dynamic_args:
//   SELECT name FROM (
//     SELECT '<logical_a>' AS name WHERE (SELECT count() FROM system.tables
//        WHERE database = '<phys_a>' AND startsWith(name, '<prefix_a>')) > 0
//     UNION ALL ...
//     UNION ALL
//     SELECT name FROM system.databases WHERE name IN ('<known1>', ...)
//   ) [WHERE name LIKE '<pat>'] ORDER BY name
//
// Without dynamic_args: passthrough as STATEMENT_TYPE_SHOW_DATABASES.
//
// Returns true iff the AST is an ASTShowTablesQuery with `databases == true`
// (so the dispatcher can fall through to handleShowTablesQuery for the
// other SHOW * variants).
#pragma once
#include <Parsers/IAST.h>
#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

bool handleShowDatabasesQuery(DB::ASTPtr ast,
                              const rewriter::RewriteSQLRequest *request,
                              rewriter::RewriteSQLResponse *response);

} // namespace rewriter_handlers
