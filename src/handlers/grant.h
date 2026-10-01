// src/handlers/grant.h
//
// GRANT / REVOKE handler. Like CREATE/DROP DATABASE, this is fire-and-forget:
// the rewriter does NOT execute the GRANT/REVOKE on ClickHouse (a logical DB
// shares its physical DB with sibling tenants via prefix-sharing, so a
// CH-level grant scoped to the physical DB would leak across tenants).
// Instead the handler:
//
//   1. Validates the statement against `dynamic_args` from the request's
//      TableNameRewrite option (no TableNameRewrite -> UnsupportedStatement).
//   2. Emits one structured `PrivilegeDelta` per AccessRightsElement into
//      `response.privileges_deltas`. Privilege keywords and grantee names
//      are preserved verbatim.
//   3. Drops any `ON CLUSTER` clause from the AST.
//   4. Rewrites the SQL to `SELECT '<original SQL minus ON CLUSTER>' AS
//      gstmt` (GRANT) or `... AS rstmt` (REVOKE) so the user's intent
//      lands in ClickHouse query_log.
//
// An external auth service consumes `privileges_deltas` and reconciles the
// per-tenant permission store. The rewriter never enforces permissions.
//
// Variants rejected as UnsupportedStatement:
//   * `ON *.*` (global scope).
//   * A prefix wildcard target (`db1.o*`, `db1*.*`): `<KIND> form is not
//     supported`.
//   * A target whose database or table name contains '*' (db1.`*`, `db1.*`,
//     `*`.o): a delta consumer could read it as a wildcard.
//   * `GRANT role TO user` / `REVOKE role FROM user` (role-membership grants
//     — distinct from granting a privilege whose recipient name happens to
//     refer to a role; the latter cannot be distinguished at parse time).
//   * Column-level grants (`GRANT SELECT(c1, c2) ON db.t TO u`).
//   * `WITH REPLACE OPTION`.
//   * `REVOKE FROM ALL` / `REVOKE FROM ALL EXCEPT ...`.
//   * `GRANT CURRENT GRANTS`.
//   * `ATTACH GRANT` (system-internal form).
//   * GRANT/REVOKE without a TableNameRewrite/dynamic_args option.
//
// Rejected as InvalidRewriteRequest:
//   * Logical DB neither in `database_map` nor in
//     `known_physical_databases`.
//   * Bare table identifier with no `upstream_logical_database_in_context`.
#pragma once
#include <Parsers/IAST.h>
#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

enum class GrantDispatchResult {
  Handled,    // Accepted: response holds the marker SELECT + deltas.
  Rejected,   // Rejected: response holds a non-Success code + message.
  NotAGrant,  // AST is not ASTGrantQuery; caller falls through.
};

GrantDispatchResult handleGrantQuery(DB::ASTPtr ast,
                                     const rewriter::RewriteSQLRequest *request,
                                     rewriter::RewriteSQLResponse *response);

} // namespace rewriter_handlers
