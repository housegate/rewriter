// src/handlers/show_databases.h
//
// SHOW DATABASES rewrite. Surfaces logical DBs from dynamic_args.database_map
// only — physical DB names are NEVER returned. `known_physical_databases`
// is consulted as a trust anchor (which physicals the caller actually owns)
// but its contents are not exposed to the caller; multi-tenant isolation
// requires the SQL surface to speak only logical names. Honors SHOW
// DATABASES LIKE / NOT LIKE / ILIKE.
//
// Per-entry handling for `database_map[logical] = physical`:
//   1. If `physical` is NOT in `known_physical_databases` -> skip the entry
//      entirely (no subquery, no `database_rewrites` record). The trusted
//      physical-DB list is the source of truth; an entry pointing outside
//      it is a config drift.
//   2. Otherwise -> emit `SELECT '<logical>' AS name` unconditionally.
//      A declared+trusted logical DB is always returned; we don't filter
//      by "any tables exist with the dynamic prefix" — an empty logical
//      DB is still a valid DB the caller can write into, and remote-mapped
//      logicals would have failed the existence check anyway.
//
// Emits, with dynamic_args:
//   SELECT name FROM (
//     SELECT '<logical_a>' AS name
//     UNION ALL
//     SELECT '<logical_b>' AS name
//     ...
//   ) [WHERE name LIKE '<pat>'] ORDER BY name
//
// When no entries survive the filters, the body is a sentinel that returns
// zero rows (`SELECT '' AS name WHERE 0`).
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
