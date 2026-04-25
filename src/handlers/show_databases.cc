// src/handlers/show_databases.cc
#include "handlers/show_databases.h"

#include <Parsers/ASTShowTablesQuery.h>

#include <algorithm>
#include <string>
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

// Build "SELECT '<logical>' AS name WHERE (SELECT count() FROM system.tables
//        WHERE database = '<physical>' AND startsWith(name, '<prefix>')) > 0"
std::string buildDatabaseMapSubquery(const std::string &logical,
                                     const std::string &physical,
                                     const std::string &prefix) {
  const std::string esc_logical  = escapeSqlLiteral(logical);
  const std::string esc_physical = escapeSqlLiteral(physical);
  const std::string esc_prefix   = escapeSqlLiteral(prefix);
  return "SELECT '" + esc_logical + "' AS name "
         "WHERE (SELECT count() FROM system.tables "
                "WHERE database = '" + esc_physical + "' "
                  "AND startsWith(name, '" + esc_prefix + "')) > 0";
}

} // namespace

bool handleShowDatabasesQuery(DB::ASTPtr ast,
                              const rewriter::RewriteSQLRequest *request,
                              rewriter::RewriteSQLResponse *response) {
  auto *show_query = ast->as<DB::ASTShowTablesQuery>();
  if (!show_query) return false;
  if (!show_query->databases) return false;

  const auto *general = findGeneralArgs(request->options());
  if (!general) {
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_SHOW_DATABASES);
    return true;
  }

  // Sort database_map entries alphabetically by logical name. protobuf map
  // iteration order is unspecified; stable SQL output is cheap and helps
  // tests / log diffing.
  std::vector<std::pair<std::string, std::string>> entries;
  entries.reserve(general->database_map().size());
  for (const auto &kv : general->database_map()) {
    entries.emplace_back(kv.first, kv.second);
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });

  std::vector<std::string> subqueries;
  subqueries.reserve(entries.size() + 1);
  for (const auto &[logical, physical] : entries) {
    const std::string prefix = buildDynamicTablePrefix(logical, *general);
    subqueries.push_back(buildDatabaseMapSubquery(logical, physical, prefix));
    recordDatabaseRewrite(response, logical, physical);
  }

  // known_physical_databases -> single SELECT against system.databases
  // filtered by IN list. Sort the names so the emitted SQL is stable.
  if (general->known_physical_databases_size() > 0) {
    std::vector<std::string> phys(general->known_physical_databases().begin(),
                                  general->known_physical_databases().end());
    std::sort(phys.begin(), phys.end());

    std::string in_list;
    for (size_t i = 0; i < phys.size(); ++i) {
      if (i > 0) in_list += ", ";
      in_list += "'" + escapeSqlLiteral(phys[i]) + "'";
    }
    subqueries.push_back(
      "SELECT name FROM system.databases WHERE name IN (" + in_list + ")");
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
