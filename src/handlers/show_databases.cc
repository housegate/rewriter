// src/handlers/show_databases.cc
#include "handlers/show_databases.h"

#include <Parsers/ASTShowTablesQuery.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "handlers/common.h"
#include "handlers/name_rewrite.h"
#include "handlers/sql_tokens.h"
#include "handlers/table_reference.h"

namespace rewriter_handlers {

namespace {

// Build the outer "WHERE name [NOT] (I)LIKE '<pat>'" clause. Returns ""
// when the statement has no LIKE filter. An empty pattern (`LIKE ''`) is a
// filter too: ClickHouse's AST keeps it as an empty string, so the keyword is
// read from the text.
std::string buildLikeClause(const DB::ASTShowTablesQuery &show_query, const std::string &sql) {
  if (show_query.like.empty() && !textHasWord(sql, "like") && !textHasWord(sql, "ilike")) return "";
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

  // The synthetic listing below keeps only a LIKE / ILIKE pattern: a WHERE,
  // LIMIT, FORMAT, INTO OUTFILE or SETTINGS clause would be dropped, so the
  // statement is refused rather than silently changed (rewriter-go
  // dispatchShowDatabases). R5 outranks it in both SI states.
  if (rejectQuerySettings(ast, response, &request->sql())) return true;
  if (show_query->where_expression || show_query->limit_length || show_query->hasOutputOptions()
      || show_query->full || show_query->temporary) {
    response->set_code(rewriter::RewriteCode::UnsupportedStatement);
    response->set_message(std::string(kUnsupportedStatementMessage));
    return true;
  }
  // Spec 2026-09-26 R7 holds for every SHOW trailing clause.
  if (rejectShowBody(*show_query, request->sql(), dynamic, response)) return true;

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
    + buildLikeClause(*show_query, request->sql())
    + " ORDER BY name";

  setSuccessResponse(response, result_sql, rewriter::STATEMENT_TYPE_SHOW_DATABASES);
  return true;
}

} // namespace rewriter_handlers
