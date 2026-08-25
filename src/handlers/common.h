// src/handlers/common.h
//
// Small helpers shared by every per-AST handler:
//  - `formatAst` wraps ClickHouse's verbose `formatWithPossiblyHidingSensitiveData`
//    with the quoting rule/style the downstream ClickHouse cluster expects.
//  - `setSuccessResponse` writes the Success code + message + statement_type
//    + sql_after_rewrite in one place.
//  - `escapeSqlLiteral` doubles single quotes for embedding a string inside
//    a single-quoted ClickHouse literal.
#pragma once

#include <Parsers/IAST.h>
#include <Parsers/IdentifierQuotingStyle.h>
#include <string>

#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

constexpr DB::IdentifierQuotingRule  kQuotingRule  = DB::IdentifierQuotingRule::WhenNecessary;
constexpr DB::IdentifierQuotingStyle kQuotingStyle = DB::IdentifierQuotingStyle::Backticks;

inline std::string formatAst(const DB::ASTPtr &ast) {
  return ast->formatWithPossiblyHidingSensitiveData(
    /*max_length=*/0, /*one_line=*/true, /*hilite=*/true,
    /*show_secrets=*/false, kQuotingRule, kQuotingStyle);
}

inline void setSuccessResponse(rewriter::RewriteSQLResponse *response,
                               const std::string &sql,
                               rewriter::StatementType statement_type) {
  response->set_sql_after_rewrite(sql);
  response->set_statement_type(statement_type);
  response->set_message("success");
  response->set_code(rewriter::RewriteCode::Success);
}

// Escape `s` so it can be embedded inside a single-quoted ClickHouse string
// literal. ClickHouse honours BOTH '' and \' as an escaped quote, so a value
// ending in a backslash would otherwise escape the closing quote and swallow
// whatever follows. Backslashes are doubled BEFORE quotes (Spec I D4).
inline std::string escapeSqlLiteral(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\\') out += "\\\\";
    else if (c == '\'') out += "''";
    else out += c;
  }
  return out;
}

} // namespace rewriter_handlers
