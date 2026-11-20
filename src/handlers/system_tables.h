// src/handlers/system_tables.h — the system-table allowlist (housegate spec
// 2026-09-26 §5 "The system database", step 1), mirrored from rewriter-go
// v0.15.0 (#58): nameresolve/systemtables.go, engine/systemrefs.go and
// handlers/systemtables.go.
//
// Every housegate tenant reaches ClickHouse as one shared user, so
// ClickHouse's own per-user filtering of system.* filters nothing between
// tenants. Caller SQL may name only the system tables on the allowlist; every
// other one, a table a future ClickHouse release adds included, is refused
// with UnsupportedStatement `system table system.<t> is not accessible`, in
// dynamic mode and both SI states.
#pragma once

#include <Parsers/IAST.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "handlers/name_rewrite.h"
#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

// ClickHouse's `system` database. Database and table names are
// case-sensitive (measured on 26.2: SYSTEM.processes is UNKNOWN_DATABASE,
// system.Tables is UNKNOWN_TABLE).
constexpr std::string_view kSystemDatabase = "system";

// rewriter-go nameresolve.SystemTableAllowed: an exact allowlist entry.
bool systemTableAllowed(const std::string &table);
// rewriter-go nameresolve.SystemTableRefusedMessage. The class (c) tables
// awaiting a filtered read name housegate/housegate#218.
std::string systemTableRefusedMessage(const std::string &table);

// Step 2 (housegate/housegate#218), rewriter-go
// nameresolve.SystemTableTenantFiltered: a read of system.<table> (the
// decoded name) is served from a tenant-filtered derived table in dynamic
// mode. These are the class (c) tables of the allowlist: tables, columns,
// data_skipping_indices, projections, parts, mutations and databases.
bool systemTableTenantFiltered(const std::string &table);

// rewriter-go handlers.systemTenantTable: the decoded class (c) table a read
// target names when it is served tenant-filtered, that is a table qualified
// with `system` whose name ClickHouse decodes to a systemTableTenantFiltered
// table. `database` / `table` are the AST's name parts (decoded once by
// ClickHouse's parser); a name still holding a backslash is decoded again,
// as the allowlist decodes it. An unqualified name is never one: `system`
// is never a mapped logical database.
std::optional<std::string> systemTenantTable(const std::string &database, const std::string &table);

// rewriter-go nameresolve.SystemTableTenantFilterSQL: the derived-table body
// that replaces a read of system.<table> (systemTableTenantFiltered) in
// dynamic mode:
//
//   SELECT * FROM system.<table> WHERE <filter>
//
// For a table with a tenant column (`name` for tables, `table` for the
// others) the filter keeps the rows of every logical database in
// database_map: `database = '<physical>' AND startsWith(<column>,
// '<prefix>')` per logical, OR-ed in (physical, logical) order, each term
// parenthesised when there are several. For system.databases it is `name IN
// ('<physical>', …)`. An empty database_map (an unauthenticated session)
// renders `WHERE 0`. The prefix is buildDynamicTablePrefix's, which always
// ends in "." while logical names never contain a dot. The filter sits
// inside the derived table, out of reach of the caller's select-list
// aliases (measured on 26.8.37.168: a predicate in the caller's own WHERE
// resolves `name` against `SELECT 'l1.x' AS name` first).
std::string systemTableTenantFilterSQL(const std::string &table, const rewriter::RewriteTableDynamicArgs &args);

// The parsed derived table (an ASTSubquery, no alias) of
// systemTableTenantFilterSQL.
DB::ASTPtr buildSystemTenantDerivedTable(const std::string &table, const rewriter::RewriteTableDynamicArgs &args);

// rewriter-go nameresolve.MetadataDatabaseRefused: information_schema in
// either spelling ClickHouse accepts. Every table of it is refused.
bool metadataDatabaseRefused(const std::string &database);
// rewriter-go nameresolve.MetadataDatabaseRefusedMessage.
std::string metadataDatabaseRefusedMessage(const std::string &database);

// rewriter-go handlers.systemRefusal: the first refused metadata reference
// of a statement, a system table outside the allowlist (`database` is
// `system`) or any table of information_schema (`database` as spelled).
struct SystemTableRefusal {
  std::string database;
  std::string table;
};

// One caller-input reference to a table-like object (rewriter-go
// engine.ObjectRef). `database` is empty for an unqualified name, which
// ClickHouse resolves in the session database. `table` is the object name, or
// a merge() / Merge pattern as spelled, or empty when the position names no
// static object. `exact` reports that `table` is an object name that may be
// compared with a name list; a pattern or an unknown name never is.
struct ObjectRef {
  std::string database;
  std::string table;
  bool exact = false;
};

// rewriter-go engine.CollectObjectRefs: every caller-input reference in every
// position the table-reference policy governs, in the cross-engine collector
// order (the first refused reference names the refusal):
//   - a command statement (DESCRIBE, EXISTS, SHOW *, RENAME / EXCHANGE,
//     GRANT / REVOKE, USE, an ALTER carrying an UPDATE action): every
//     qualified run of its text in text order, then the AST targets;
//   - every statement: its write / DDL targets, then every read source and
//     carrier in walker order (FROM / JOIN, subqueries, CTE bodies, UNION
//     arms, view() and view / MV bodies, INSERT … SELECT and CTAS sources,
//     IN operands, column / constraint / storage / ALTER-action expressions,
//     table-function and table-engine arguments, and the members Task I1
//     adopted from outside `children`);
//   - a structured statement: then the qualified runs of the text rewriter-go
//     keeps opaque (the actions of an ALTER that is not structured
//     throughout, the query text after an INSERT column list and SETTINGS,
//     a view column list's INDEX / PROJECTION / PRIMARY KEY items);
//   - then the string-lookup arguments (joinGet / dictGet / hasColumnInTable
//     families) in walk order.
// A qualified run of three or more parts `a.b.….z` is reported as a.z
// (ClickHouse's SHOW COLUMNS / INDEX reading) and then as a.b (an
// expression's reading). Names are decoded as ClickHouse's ParserIdentifier
// decodes them. Only caller input is read, so the rewriter's own
// system.tables / system.columns synthesis is never a reference. `sql` is the
// statement text as the caller sent it; `values_rows` an INSERT's parsed
// VALUES rows (insertValuesRows), read like any other expression, or null to
// parse them here. std::nullopt when a text cannot be tokenized (the caller
// fails closed).
std::optional<std::vector<ObjectRef>> collectObjectRefs(const DB::ASTPtr &ast, const std::string &sql,
                                                        const DB::ASTs *values_rows = nullptr);

// rewriter-go handlers.systemTableScan: the first reference, in collector
// order, to a system table outside the allowlist (`refused`, `table`), and
// whether the statement also names a storage-integrity object while the SI
// surface is active (an SI physical or reserved database, an Active SI table,
// or an SI logical database through a pattern carrier), in which case the SI
// pipeline answers first and the check runs on its would-be Success.
// `failed` reports a text that could not be tokenized.
struct SystemTableScan {
  bool refused = false;
  SystemTableRefusal refusal;
  bool storage_integrity_object = false;
  bool failed = false;
};
SystemTableScan scanSystemTables(const DB::ASTPtr &ast, const std::string &sql,
                                 const rewriter::RewriteTableDynamicArgs &args,
                                 const DB::ASTs *values_rows = nullptr);

// rewriter-go handlers.rejectSystemTable: UnsupportedStatement with the
// system-table message, the system table recorded as accessed (once), no
// rewrite maps, the statement echoed. The caller's other accessed tables
// are kept.
void rejectSystemTable(rewriter::RewriteSQLResponse *response, const SystemTableRefusal &refusal,
                       const std::string &sql, const TableRewriteSelection &selection);

// rewriter-go RejectSystemTablesOnSuccess: `refusal` is the one the
// preflight deferred to the SI pipeline (the statement names an SI object
// while the surface is active); applied only when that pipeline answered
// Success.
void rejectDeferredSystemTable(rewriter::RewriteSQLResponse *response, const SystemTableRefusal &refusal,
                               const std::string &sql, const TableRewriteSelection &selection);

// A SHOW form's refusal (rewriter-go rejectShowSystemTable): nothing is
// recorded as accessed, the caller named no table.
void rejectShowSystemTable(rewriter::RewriteSQLResponse *response, const std::string &table);

// SHOW [EXTENDED] [FULL] {COLUMNS | FIELDS | INDEX | INDEXES | INDICES | KEYS}
// {FROM | IN} a.b.c…: the table clause has three or more parts. ClickHouse
// reads it as database a, table c (rewriter-go
// DBLevelInfo.ShowTableMultiPart); the AST keeps no part count, so the text
// is read. A text the lexer cannot tokenize counts as multi-part (fail
// closed).
bool showTargetIsMultiPart(const std::string &sql);

// The SHOW statements whose AST names no table and has no handler of its
// own: SHOW PROCESSLIST, USERS / ROLES / [SETTINGS] PROFILES / [ROW]
// POLICIES / QUOTA(S) / CURRENT … / ENABLED ROLES, GRANTS, ACCESS,
// FUNCTIONS, ENGINES, SETTING, PRIVILEGES, SHOW CREATE <access entity>, and
// DESCRIBE FILESYSTEM CACHE. In dynamic mode, rewriter-go's SHOW dispatch
// (systemTableShowRefusal / showKindClass, keyed by the word after SHOW):
// a kind that reads a refused system table is refused with its message in
// both SI states (CURRENT / ENABLED only with the surface inactive; the SI
// catch-all answers them while it is active); FUNCTIONS and ENGINES pass in
// both states; SETTING and PRIVILEGES pass while the surface is inactive;
// SHOW CREATE of an access entity, DESCRIBE FILESYSTEM CACHE and an unknown
// kind are `statement is not supported`. Returns true when it answered.
bool handleShowSystemFormQuery(const DB::ASTPtr &ast, const rewriter::RewriteSQLRequest *request,
                               rewriter::RewriteSQLResponse *response);

} // namespace rewriter_handlers
