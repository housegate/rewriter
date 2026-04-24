// src/handlers/show_tables.cc
#include "handlers/show_tables.h"

#include <Parsers/ASTShowTablesQuery.h>
#include <string>

#include "handlers/common.h"

namespace rewriter_handlers {

namespace {
std::string escapeSqlLiteral(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\'') out += "''";
    else out += c;
  }
  return out;
}
} // namespace

bool handleShowTablesQuery(DB::ASTPtr ast,
                           const rewriter::RewriteSQLRequest *request,
                           rewriter::RewriteSQLResponse *response) {
  auto *show_query = ast->as<DB::ASTShowTablesQuery>();
  if (!show_query) return false;

  // SHOW DATABASES / SHOW CLUSTERS / ... - not our problem, pass through.
  if (show_query->databases || show_query->clusters || show_query->cluster
      || show_query->dictionaries || show_query->m_settings
      || show_query->merges || show_query->caches) {
    setSuccessResponse(response, formatAst(ast), "SHOW_TABLES");
    return true;
  }

  // "FROM <db>" is treated as a logical name (table-name prefix), not a
  // physical DB. getFrom() returns "database.table" or "database"; keep the
  // leading part only.
  std::string from_logical = show_query->getFrom();
  if (auto dot = from_logical.find('.'); dot != std::string::npos) {
    from_logical = from_logical.substr(0, dot);
  }

  for (const auto &option : request->options()) {
    if (option.op() != rewriter::RewriteOp::ShowTablesRewrite) continue;

    const auto &args = option.show_tables_args();
    const std::string &physical = args.physical_database();
    const std::string logical =
      from_logical.empty() ? args.logical_database() : from_logical;

    // Empty logical -> passthrough (caller decides).
    if (logical.empty()) {
      setSuccessResponse(response, formatAst(ast), "SHOW_TABLES");
      response->mutable_show_stmt_value()->set_show_tables_passthrough(true);
      return true;
    }

    // Full rewrite: outer multiIf strips the logical prefix; inner subquery
    // filters system.tables by database = physical AND startsWith(name, logical).
    // Subquery avoids alias shadowing of the outer "name" column.
    const std::string esc_logical  = escapeSqlLiteral(logical);
    const std::string esc_physical = escapeSqlLiteral(physical);
    std::string result_sql =
      "SELECT multiIf("
        "startsWith(name, '" + esc_logical + ".'), "
          "substring(name, length('" + esc_logical + ".') + 1), "
        "startsWith(name, '" + esc_logical + "'), "
          "substring(name, length('" + esc_logical + "') + 1), "
        "name) AS name "
      "FROM (SELECT name FROM system.tables "
            "WHERE database = '" + esc_physical + "' "
              "AND startsWith(name, '" + esc_logical + "'))";

    setSuccessResponse(response, result_sql, "SHOW_TABLES");
    return true;
  }

  // No rewrite option - echo back the original SQL.
  setSuccessResponse(response, formatAst(ast), "SHOW_TABLES");
  return true;
}

} // namespace rewriter_handlers
