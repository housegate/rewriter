// src/handlers/grant.cc
#include "handlers/grant.h"

#include <Access/Common/AccessFlags.h>
#include <Access/Common/AccessRightsElement.h>
#include <Parsers/Access/ASTGrantQuery.h>
#include <Parsers/Access/ASTRolesOrUsersSet.h>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "handlers/common.h"
#include "handlers/name_rewrite.h"
#include "handlers/storage_integrity.h"

namespace rewriter_handlers {

namespace {

void rejectGrant(rewriter::RewriteSQLResponse *response,
                 rewriter::RewriteCode code,
                 const std::string &msg) {
  // Stderr so operators can see what kinds of GRANT/REVOKE statements are
  // hitting reject paths (column grants, ALL EXCEPT, missing dynamic_args,
  // ...) without correlating every error response back to a client.
  std::cerr << "[grant-reject] " << msg << std::endl;
  response->set_message(msg);
  response->set_code(code);
}

inline void rejectUnsupported(rewriter::RewriteSQLResponse *response, const std::string &msg) {
  rejectGrant(response, rewriter::RewriteCode::UnsupportedStatement, msg);
}

inline void rejectInvalidRequest(rewriter::RewriteSQLResponse *response, const std::string &msg) {
  rejectGrant(response, rewriter::RewriteCode::InvalidRewriteRequest, msg);
}

// Translate the ASTRolesOrUsersSet `grantees` field to the proto Grantee
// repeated message. Rejects (and returns false) on the variants the
// privilege-delta path doesn't model — ALL / ALL EXCEPT / *...EXCEPT u1*.
bool buildGrantees(rewriter::RewriteSQLResponse *response,
                   const DB::ASTRolesOrUsersSet &set,
                   bool is_revoke,
                   std::vector<rewriter::PrivilegeDelta::Grantee> &out) {
  const std::string_view direction = is_revoke ? "REVOKE FROM " : "GRANT TO ";
  if (set.all) {
    rejectUnsupported(response,
      std::string(direction) + "ALL is not supported");
    return false;
  }
  if (!set.except_names.empty() || set.except_current_user) {
    rejectUnsupported(response,
      std::string(direction) + "... EXCEPT is not supported");
    return false;
  }
  for (const auto &name : set.names) {
    rewriter::PrivilegeDelta::Grantee g;
    g.set_name(name);
    g.set_is_current_user(false);
    out.push_back(std::move(g));
  }
  if (set.current_user) {
    rewriter::PrivilegeDelta::Grantee g;
    g.set_is_current_user(true);
    out.push_back(std::move(g));
  }
  return true;
}

} // namespace

GrantDispatchResult handleGrantQuery(DB::ASTPtr ast,
                                     const rewriter::RewriteSQLRequest *request,
                                     rewriter::RewriteSQLResponse *response) {
  auto *grant = ast->as<DB::ASTGrantQuery>();
  if (!grant) return GrantDispatchResult::NotAGrant;

  const bool is_revoke = grant->is_revoke;
  const std::string_view kind = is_revoke ? "REVOKE" : "GRANT";

  // SI classification must precede every generic GRANT/REVOKE reject,
  // including ATTACH GRANT and database scope. Legacy consumers fail closed
  // only when the rejection carries an SI access marker.
  const auto selection = findActiveTableRewrite(request->options());
  const auto *dynamic = findDynamicArgs(request->options());
  if (selection.mode == TableRewriteMode::Dynamic
      && storageIntegritySurfaceActive(*selection.dynamic_args)) {
    const auto &args = *selection.dynamic_args;
    for (const auto &elem : grant->access_rights_elements) {
      const std::string original_database = elem.default_database
        ? std::string() : elem.database;
      const std::string effective_database = original_database.empty()
        ? args.upstream_logical_database_in_context() : original_database;
      if (elem.anyTable()
          && isStorageIntegrityPhysicalDatabase(effective_database, args)) {
        recordPhysicalStorageIntegrityAccess(
          response, effective_database, "", effective_database);
        rejectUnsupported(response,
          storageIntegrityPhysicalDatabaseRejectMessage(effective_database));
        return GrantDispatchResult::Rejected;
      }
      if (elem.anyTable()) continue;
      if (isStorageIntegrityPhysicalDatabase(effective_database, args)) {
        recordAccessedTable(
          response, original_database, elem.table, selection);
        rejectUnsupported(response, storageIntegrityPhysicalRejectMessage(
          effective_database + "." + elem.table));
        return GrantDispatchResult::Rejected;
      }
      if (auto hit = lookupStorageIntegrity(
            original_database, elem.table, args)) {
        recordAccessedTable(
          response, original_database, elem.table, selection);
        std::string logical;
        if (!authorizeStorageIntegrityLogical(
              original_database, args, &logical)) {
          rejectInvalidRequest(response,
            storageIntegrityUnauthorizedMessage(logical));
        } else {
          rejectUnsupported(response,
            storageIntegrityWriteRejectMessage(hit->logical_key));
        }
        return GrantDispatchResult::Rejected;
      }
    }
  }

  // ATTACH GRANT is internal (CH loads users from disk). User SQL never
  // sets it, but reject defensively rather than treat as a regular grant.
  if (grant->attach_mode) {
    rejectUnsupported(response, "ATTACH GRANT is not supported");
    return GrantDispatchResult::Rejected;
  }
  // Role-membership form: `GRANT some_role TO user` / `REVOKE some_role
  // FROM user`. Distinct from granting a privilege whose recipient happens
  // to be a role — the latter goes through access_rights_elements and has
  // a null/empty `roles` field.
  if (grant->roles) {
    rejectUnsupported(response,
      std::string(kind)
        + " of a role to a user/role (role-membership grant) is not supported");
    return GrantDispatchResult::Rejected;
  }
  if (grant->replace_access || grant->replace_granted_roles) {
    rejectUnsupported(response,
      std::string(kind) + " WITH REPLACE OPTION is not supported");
    return GrantDispatchResult::Rejected;
  }
  if (grant->current_grants) {
    rejectUnsupported(response, "GRANT CURRENT GRANTS is not supported");
    return GrantDispatchResult::Rejected;
  }
  // A prefix wildcard grant (`db1.o*`, `db1*.*`) covers every matching
  // object; a delta has no shape for it. rewriter-go cannot structure it and
  // answers the same message.
  for (const auto &elem : grant->access_rights_elements) {
    if (elem.wildcard) {
      rejectUnsupported(response, std::string(kind) + " form is not supported");
      return GrantDispatchResult::Rejected;
    }
  }

  // Validation context.
  if (!dynamic) {
    rejectUnsupported(response,
      std::string(kind)
        + " requires a TableNameRewrite/dynamic_args option to validate against");
    return GrantDispatchResult::Rejected;
  }

  // Translate the grantees once — the same TO/FROM list applies to every
  // AccessRightsElement emitted from this statement.
  std::vector<rewriter::PrivilegeDelta::Grantee> grantees;
  if (grant->grantees) {
    if (!buildGrantees(response, *grant->grantees, is_revoke, grantees)) {
      return GrantDispatchResult::Rejected;
    }
  }
  if (grantees.empty()) {
    // Defensive: a well-formed GRANT/REVOKE always names at least one
    // grantee. A null/empty grantees list would produce deltas with no
    // recipient, which downstream auth services can't act on.
    rejectInvalidRequest(response,
      std::string(kind) + " has no grantees");
    return GrantDispatchResult::Rejected;
  }

  // A database or table name containing '*' (db1.`*`, `db1.*`, `*`.o, any
  // quoting or escape ClickHouse decodes to '*') is one object to ClickHouse,
  // but a delta consumer could read the name as a wildcard: refuse rather
  // than risk widening it. Checked for every element before the per-element
  // rejects, in rewriter-go's order (after grantees, before *.*).
  for (const auto &elem : grant->access_rights_elements) {
    if (elem.anyDatabase()) continue;
    if (elem.database.find('*') != std::string::npos
        || (!elem.anyTable() && elem.table.find('*') != std::string::npos)) {
      rejectUnsupported(response, std::string(kind)
        + " target names a database or table whose name contains '*', which a privilege delta cannot represent");
      return GrantDispatchResult::Rejected;
    }
  }

  const auto action = is_revoke ? rewriter::PrivilegeDelta::ACTION_REVOKE
                                : rewriter::PrivilegeDelta::ACTION_GRANT;

  // Walk every AccessRightsElement, validating and emitting one delta each.
  // Reject early on the first invalid element — partial deltas are allowed
  // by the proto contract on a non-Success response, but we keep the failure
  // mode simple here.
  for (const auto &elem : grant->access_rights_elements) {
    if (elem.anyDatabase()) {
      rejectUnsupported(response,
        std::string(kind) + " ON *.* (global scope) is not supported");
      return GrantDispatchResult::Rejected;
    }
    if (!elem.columns.empty()) {
      // Column-level grant. Rejecting (rather than silently dropping the
      // column list) avoids broadening privilege scope in the audit trail.
      rejectUnsupported(response,
        std::string(kind)
          + " with column-level granularity is not supported");
      return GrantDispatchResult::Rejected;
    }

    // SQL qualifier: when the parser saw "ON t" (no qualifier) it sets
    // default_database=true and leaves `database` empty. Treat that as
    // "fall back to upstream_logical_database_in_context"; an explicit
    // qualifier (default_database=false, database non-empty) is the
    // logical the caller wrote.
    const std::string original_database = elem.default_database
      ? std::string()
      : elem.database;
    const std::string logical = original_database.empty()
      ? dynamic->upstream_logical_database_in_context()
      : original_database;
    if (logical.empty()) {
      std::ostringstream oss;
      oss << kind << " target '" << elem.table
          << "' is unqualified and no upstream_logical_database_in_context "
             "is set; caller must send `USE <db>` or qualify the target";
      rejectInvalidRequest(response, oss.str());
      return GrantDispatchResult::Rejected;
    }

    auto physical = resolvePhysicalDatabase(logical, *dynamic);
    if (!physical) {
      std::ostringstream oss;
      oss << kind << " target references logical database '" << logical
          << "' which is not in database_map and not a known physical "
             "database; user does not have this database to grant on";
      rejectInvalidRequest(response, oss.str());
      return GrantDispatchResult::Rejected;
    }

    auto *delta = response->add_privileges_deltas();
    delta->set_action(action);
    delta->set_original_database(original_database);
    delta->set_logical_database(logical);
    delta->set_physical_database(*physical);
    // The proto field collapses GRANT's WITH GRANT OPTION and REVOKE's
    // GRANT OPTION FOR into a single bool — different parser flags, same
    // semantic ("the GRANT-OPTION attribute changes"). OR the two so we
    // catch whichever the parser populated.
    delta->set_grant_option(elem.grant_option || elem.is_partial_revoke);

    if (elem.anyTable()) {
      delta->set_scope(rewriter::PrivilegeDelta::SCOPE_DATABASE);
      // original_table / physical_table left empty for SCOPE_DATABASE.
    } else {
      delta->set_scope(rewriter::PrivilegeDelta::SCOPE_TABLE);
      delta->set_original_table(elem.table);
      delta->set_physical_table(buildDynamicTableName(logical, elem.table, *dynamic));
    }

    // Privilege names exactly as ClickHouse spells them (one keyword per
    // bit set in access_flags). Preserved verbatim.
    for (const auto &kw : elem.access_flags.toKeywords()) {
      delta->add_privileges(std::string(kw));
    }

    // Grantees fan out to every delta produced by this statement.
    for (const auto &g : grantees) {
      *delta->add_grantees() = g;
    }
  }

  // Drop ON CLUSTER from the AST so the marker SELECT contains a clean
  // "<original SQL minus ON CLUSTER>" string. The cluster fragment is
  // irrelevant downstream — rewriter never executes the GRANT, the auth
  // service does not branch on cluster, and leaving it in would just
  // pollute query_log with stale dispatch metadata.
  grant->cluster.clear();

  const std::string original_sql = formatAst(ast);
  const std::string marker = is_revoke ? "rstmt" : "gstmt";
  const std::string debug_sql =
    "SELECT '" + escapeSqlLiteral(original_sql) + "' AS " + marker;

  setSuccessResponse(response, debug_sql,
                     is_revoke ? rewriter::STATEMENT_TYPE_REVOKE
                               : rewriter::STATEMENT_TYPE_GRANT);
  return GrantDispatchResult::Handled;
}

} // namespace rewriter_handlers
