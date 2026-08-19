#include "handlers/describe.h"

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
  const auto parsed = parseDescribeTarget(request->sql());
  if (!parsed) return false;
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
      setSuccessResponse(response,
        describeMetadataSQL(hit->table->safe_table(), reservedRowIdColumn(args)),
        rewriter::STATEMENT_TYPE_DESCRIBE);
      return true;
    }
  }

  setSuccessResponse(response, request->sql(), rewriter::STATEMENT_TYPE_DESCRIBE);
  return true;
}

} // namespace rewriter_handlers
