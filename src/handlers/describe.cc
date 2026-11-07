#include "handlers/describe.h"

#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/TablePropertiesQueriesASTs.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <string>

#include "handlers/common.h"
#include "handlers/name_rewrite.h"
#include "handlers/storage_integrity.h"

namespace rewriter_handlers {
namespace {

struct DescribeTarget {
  std::string object = "TABLE";
  std::string database;
  std::string table;
};

std::string unquote(std::string value) {
  if (value.size() >= 2 && ((value.front() == '`' && value.back() == '`') ||
                             (value.front() == '"' && value.back() == '"'))) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

std::optional<DescribeTarget> parseDescribeTarget(const std::string &sql) {
  static const std::regex pattern(
    R"(^\s*(?:DESCRIBE|DESC)\s+(?:(TABLE|DATABASE|VIEW|DICTIONARY)\s+)?((?:`[^`]+`|"[^"]+"|[A-Za-z_][A-Za-z0-9_]*)(?:\s*\.\s*(?:`[^`]+`|"[^"]+"|[A-Za-z_][A-Za-z0-9_]*))?))",
    std::regex::ECMAScript | std::regex::icase);
  std::smatch match;
  if (!std::regex_search(sql, match, pattern)) return std::nullopt;
  DescribeTarget out;
  if (match[1].matched) {
    out.object = match[1].str();
    std::transform(out.object.begin(), out.object.end(), out.object.begin(), [](unsigned char c) {
      return static_cast<char>(std::toupper(c));
    });
  }
  std::string name = match[2].str();
  name.erase(
    std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
    name.end());
  bool quoted = false;
  size_t dot = std::string::npos;
  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '`' || name[i] == '"')
      quoted = !quoted;
    else if (name[i] == '.' && !quoted) {
      dot = i;
      break;
    }
  }
  if (dot == std::string::npos)
    out.table = unquote(name);
  else {
    out.database = unquote(name.substr(0, dot));
    out.table = unquote(name.substr(dot + 1));
  }
  return out;
}

// The table a DESCRIBE names, as ClickHouse parsed it (decoded, whatever
// comments or spacing surround it); nullptr for a table-function or subquery
// target.
DB::ASTTableIdentifier *describeTableIdentifier(const DB::ASTPtr &ast) {
  const auto *describe = ast->as<DB::ASTDescribeQuery>();
  auto *te = describe && describe->table_expression
    ? describe->table_expression->as<DB::ASTTableExpression>() : nullptr;
  return te && te->database_and_table_name ? te->database_and_table_name->as<DB::ASTTableIdentifier>() : nullptr;
}

void reject(
  rewriter::RewriteSQLResponse *response, rewriter::RewriteCode code, const std::string &message) {
  response->set_code(code);
  response->set_message(message);
}

} // namespace

bool handleDescribeQuery(DB::ASTPtr ast,
  const rewriter::RewriteSQLRequest *request,
  rewriter::RewriteSQLResponse *response) {
  if (!ast->as<DB::ASTDescribeQuery>()) return false;
  auto *identifier = describeTableIdentifier(ast);
  auto parsed = parseDescribeTarget(request->sql());
  // The object keyword (TABLE / DATABASE / VIEW / DICTIONARY) comes from the
  // text; a table target's name comes from the AST, since the text pattern
  // misreads a comment or line break around the name (`DESCRIBE TABLE /*c*/
  // hg_safe.db1__t` read as a table named TABLE).
  if (!parsed && identifier) parsed = DescribeTarget{};
  if (!parsed) return false;
  if (parsed->object == "TABLE" && identifier && !identifier->isParam()) {
    const auto &parts = identifier->name_parts;
    parsed->database = parts.size() >= 2 ? parts.front() : std::string();
    parsed->table = parts.back();
  }
  const auto selection = findActiveTableRewrite(request->options());

  if (selection.mode == TableRewriteMode::Dynamic) {
    const auto &args = *selection.dynamic_args;
    if (parsed->object == "DATABASE" && isStorageIntegrityPhysicalDatabase(parsed->table, args)) {
      recordPhysicalStorageIntegrityAccess(response, parsed->table, "", parsed->table);
      reject(response,
        rewriter::RewriteCode::UnsupportedStatement,
        storageIntegrityPhysicalDatabaseRejectMessage(parsed->table));
      return true;
    }
    const std::string effective =
      parsed->database.empty() ? storageIntegrityExecutionDatabase(args) : parsed->database;
    if (parsed->object != "DATABASE" && isStorageIntegrityPhysicalDatabase(effective, args)) {
      recordAccessedTable(response, parsed->database, parsed->table, selection);
      reject(response,
        rewriter::RewriteCode::UnsupportedStatement,
        storageIntegrityPhysicalRejectMessage(effective + "." + parsed->table));
      return true;
    }
  }

  if (parsed->object != "TABLE") {
    reject(response,
      rewriter::RewriteCode::UnsupportedStatement,
      "DESCRIBE " + parsed->object + " is not supported; only DESCRIBE TABLE is allowed");
    return true;
  }

  recordAccessedTable(response, parsed->database, parsed->table, selection);
  if (selection.mode == TableRewriteMode::Dynamic) {
    const auto &args = *selection.dynamic_args;
    if (auto hit = lookupStorageIntegrity(parsed->database, parsed->table, args)) {
      std::string logical;
      if (!authorizeStorageIntegrityLogical(parsed->database, args, &logical)) {
        reject(response,
          rewriter::RewriteCode::InvalidRewriteRequest,
          storageIntegrityUnauthorizedMessage(logical));
        return true;
      }
      // The synthetic metadata SELECT carries no FORMAT / INTO OUTFILE /
      // SETTINGS: refused rather than dropped (rewriter-go refuses them).
      if (ast->as<DB::ASTDescribeQuery>()->hasOutputOptions()) {
        reject(response, rewriter::RewriteCode::UnsupportedStatement, "statement is not supported");
        return true;
      }
      setSuccessResponse(response,
        describeMetadataSQL(hit->table->safe_table(), reservedRowIdColumn(args)),
        rewriter::STATEMENT_TYPE_DESCRIBE);
      return true;
    }
  }

  // Spec 2026-09-26 R7, amended 2026-11-07: the target resolves like an
  // EXISTS TABLE target. An unqualified one (a bare quoted `db2.x` is a table
  // named db2.x) resolves in the session's logical database; a qualified one
  // names a logical (or known physical) database and is mapped the same way.
  // Forwarded verbatim, ClickHouse would bind the logical name as a physical
  // database (another tenant's, or `default`) while the accessed-table record
  // told the caller it was the mapped one. Only ClickHouse's metadata
  // databases stay verbatim: the preflight's system-table allowlist judged
  // their tables.
  if (selection.mode == TableRewriteMode::Dynamic && identifier && !identifier->isParam()
      && !isClickHouseMetadataDatabase(parsed->database)) {
    const auto outcome =
      applyDynamicRewrite(parsed->database, parsed->table, *selection.dynamic_args, "DESCRIBE");
    if (outcome.status == DynamicRewriteStatus::InvalidRequest) {
      reject(response, rewriter::RewriteCode::InvalidRewriteRequest, outcome.reject_reason);
      return true;
    }
    if (outcome.status == DynamicRewriteStatus::OkRemote) {
      reject(response,
        rewriter::RewriteCode::UnsupportedStatement,
        "DESCRIBE target's logical resolves to a remote upstream "
        "(via dynamic_args.logical_database_to_remote_upstream_index); "
        "remote() can only appear as a SELECT-side table function");
      return true;
    }
    identifier->resetTable(outcome.physical_db, outcome.new_table);
    recordTableRewrite(response, parsed->database, parsed->table, outcome.physical_db, outcome.new_table);
    setSuccessResponse(response, formatAst(ast), rewriter::STATEMENT_TYPE_DESCRIBE);
    return true;
  }

  setSuccessResponse(response, request->sql(), rewriter::STATEMENT_TYPE_DESCRIBE);
  return true;
}

} // namespace rewriter_handlers
