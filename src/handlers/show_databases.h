// src/handlers/show_databases.h
//
// SHOW DATABASES rewrite. Surfaces logical DBs from dynamic_args.database_map
// plus known_physical_databases. Honors SHOW DATABASES LIKE / NOT LIKE /
// ILIKE.
//
// Per-entry handling for `database_map[logical] = physical`:
//   1. If `physical` is NOT in `known_physical_databases` -> skip the entry
//      entirely (no subquery, no `database_rewrites` record). The trusted
//      physical-DB list is the source of truth; an entry pointing outside
//      it is a config drift.
//   2. Else if `logical` is in `logical_database_to_remote_upstream_index`
//      -> emit `SELECT '<logical>' AS name` unconditionally. The local
//      physical never holds the prefixed tables for a remote-mapped
//      logical, so an existence check would always be false.
//   3. Else -> emit the existence-check subquery against the local
//      physical: `SELECT '<logical>' AS name WHERE (SELECT count() FROM
//      system.tables WHERE database = '<physical>' AND startsWith(name,
//      '<prefix>')) > 0`.
//
// Emits, with dynamic_args:
//   SELECT name FROM (
//     <per-entry subquery from rule 2 or 3>
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
