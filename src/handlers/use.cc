// src/handlers/use.cc
#include "handlers/use.h"

#include <Parsers/ASTUseQuery.h>
#include <string>

#include "handlers/common.h"

namespace rewriter_handlers {

bool handleUseQuery(DB::ASTPtr ast,
                    const rewriter::RewriteSQLRequest *request,
                    rewriter::RewriteSQLResponse *response) {
  auto *use_query = ast->as<DB::ASTUseQuery>();
  if (!use_query) return false;

  const std::string db_name = use_query->getDatabase();

  for (const auto &option : request->options()) {
    if (option.op() != rewriter::RewriteOp::DatabaseNameRewrite) continue;
    const auto &args = option.database_name_args();
    const auto &db_map = args.database_map();

    // 1. database_map hit -> rewrite to the mapped physical DB.
    auto it = db_map.find(db_name);
    if (it != db_map.end()) {
      setSuccessResponse(response, "USE " + it->second, "USE");
      return true;
    }

    // 2. Target already names a known physical DB -> leave SQL unchanged.
    bool is_known_physical = false;
    for (const auto &known : args.known_physical_databases()) {
      if (known == db_name) {
        is_known_physical = true;
        break;
      }
    }
    if (is_known_physical) {
      setSuccessResponse(response, formatAst(ast), "USE");
      return true;
    }

    // 3. default_database fallback -> rewrite to default, report the original
    //    target so the caller can track it on the connection.
    const std::string &default_db = args.default_database();
    if (!default_db.empty()) {
      setSuccessResponse(response, "USE " + default_db, "USE");
      response->mutable_use_stmt_value()->set_specified_database(db_name);
      return true;
    }
  }

  // No matching option - echo back the original SQL.
  setSuccessResponse(response, formatAst(ast), "USE");
  return true;
}

} // namespace rewriter_handlers
