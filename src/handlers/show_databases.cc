// src/handlers/show_databases.cc
#include "handlers/show_databases.h"

#include <Parsers/ASTShowTablesQuery.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "handlers/common.h"
#include "handlers/name_rewrite.h"

namespace rewriter_handlers {

namespace {

// Build the outer "WHERE name [NOT] (I)LIKE '<pat>'" clause. Returns ""
// when the AST has no LIKE filter.
std::string buildLikeClause(const DB::ASTShowTablesQuery &show_query) {
  if (show_query.like.empty()) return "";
  std::string op;
  if (show_query.case_insensitive_like) {
    op = show_query.not_like ? "NOT ILIKE" : "ILIKE";
  } else {
    op = show_query.not_like ? "NOT LIKE" : "LIKE";
  }
  return " WHERE name " + op + " '" + escapeSqlLiteral(show_query.like) + "'";
}

// Per-entry subquery: unconditionally surface the logical name.
//
//   SELECT '<logical>' AS name
//
// We don't filter by "any tables exist with the dynamic prefix" anymore.
// A logical DB that the caller declared (via `database_map`) and trusted
// (its `physical` is in `known_physical_databases`) is always returned —
// an empty logical DB is still a valid DB the caller can write into, and
// for remote-mapped logicals the existence check would have been false
// anyway (the prefixed tables live on the upstream cluster).
std::string buildLogicalSubquery(const std::string &logical) {
  return "SELECT '" + escapeSqlLiteral(logical) + "' AS name";
}

} // namespace

bool handleShowDatabasesQuery(DB::ASTPtr ast,
                              const rewriter::RewriteSQLRequest *request,
                              rewriter::RewriteSQLResponse *response) {
  auto *show_query = ast->as<DB::ASTShowTablesQuery>();
  if (!show_query) return false;
  if (!show_query->databases) return false;

  const auto *dynamic = findDynamicArgs(request->options());
  if (!dynamic) {
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_SHOW_DATABASES);
    return true;
  }

  // Sort database_map entries alphabetically by logical name. protobuf map
  // iteration order is unspecified; stable SQL output is cheap and helps
  // tests / log diffing.
  std::vector<std::pair<std::string, std::string>> entries;
  entries.reserve(dynamic->database_map().size());
  for (const auto &kv : dynamic->database_map()) {
    entries.emplace_back(kv.first, kv.second);
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });

  // Trust-anchor: only surface a database_map entry when its physical DB is
  // declared in known_physical_databases. Anything else is a config drift
  // (or a DB the caller doesn't actually own a physical for) and gets
  // skipped — no subquery, no `database_rewrites` entry.
  const std::unordered_set<std::string> known_physical(
      dynamic->known_physical_databases().begin(),
      dynamic->known_physical_databases().end());

  // known_physical_databases is a trust anchor only — physical DBs are
  // never surfaced to the caller directly. Multi-tenant isolation: the
  // caller speaks logical names; physical names are an implementation
  // detail of the rewriter and should never leak into a `SHOW DATABASES`
  // result.
  std::vector<std::string> subqueries;
  subqueries.reserve(entries.size());
  for (const auto &[logical, physical] : entries) {
    if (!known_physical.count(physical)) continue;
    subqueries.push_back(buildLogicalSubquery(logical));
    recordDatabaseRewrite(response, logical, physical);
  }

  std::string body;
  if (subqueries.empty()) {
    body = "SELECT '' AS name WHERE 0";
  } else {
    for (size_t i = 0; i < subqueries.size(); ++i) {
      if (i > 0) body += " UNION ALL ";
      body += subqueries[i];
    }
  }

  std::string result_sql =
    "SELECT name FROM (" + body + ")"
    + buildLikeClause(*show_query)
    + " ORDER BY name";

  setSuccessResponse(response, result_sql, rewriter::STATEMENT_TYPE_SHOW_DATABASES);
  return true;
}

} // namespace rewriter_handlers
