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

#include <Parsers/ASTAlterQuery.h>
#include <Parsers/ASTColumnsTransformers.h>
#include <Parsers/ASTTTLElement.h>
#include <Parsers/ASTWindowDefinition.h>
#include <Parsers/IAST.h>
#include <Parsers/IdentifierQuotingStyle.h>
#include <algorithm>
#include <functional>
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

// ClickHouse's parser keeps some subtrees only in a field, outside
// `children`. Every policy walker (T2, T3, T5, T6, R2, the SI checks, the
// rewrite walk) iterates `children`, so a subquery, table operand, parameter,
// table function or lookup there went unexamined and unrewritten. The fields
// that can carry an expression in a statement the engine can answer Success
// for (the structural enumeration in the I1 report, from the ClickHouse
// headers and parsers):
//  - ASTWindowListElement::definition (a WINDOW clause; clone() adds it);
//  - ASTColumnsApplyTransformer::lambda / parameters (`APPLY(x -> …)`);
//  - ASTTTLElement::group_by_key / group_by_assignments (TTL `GROUP BY … SET
//    …`) and recompression_codec (`RECOMPRESS CODEC(…)`);
//  - ASTAlterCommand::refresh (`ALTER … MODIFY REFRESH … DEPENDS ON …`).
// registerFieldOnlyChildren adopts them as children right after parsing; the
// formatter keeps reading the fields, which hold the same nodes. Clone only an
// unregistered tree (clonePolicyAst): APPLY's clone copies `children` by
// pointer, so a registered tree's clone would share the original lambda.
inline void registerFieldOnlyChildren(DB::IAST &node) {
  const auto adopt = [&node](const DB::ASTPtr &field) {
    if (field && std::find(node.children.begin(), node.children.end(), field) == node.children.end())
      node.children.push_back(field);
  };
  if (const auto *element = node.as<DB::ASTWindowListElement>()) {
    adopt(element->definition);
  } else if (const auto *apply = node.as<DB::ASTColumnsApplyTransformer>()) {
    adopt(apply->lambda);
    adopt(apply->parameters);
  } else if (const auto *ttl = node.as<DB::ASTTTLElement>()) {
    for (const auto &key : ttl->group_by_key) adopt(key);
    for (const auto &assignment : ttl->group_by_assignments) adopt(assignment);
    adopt(ttl->recompression_codec);
  } else if (const auto *command = node.as<DB::ASTAlterCommand>()) {
    adopt(command->refresh);
  }
  for (const auto &child : node.children)
    if (child) registerFieldOnlyChildren(*child);
}

// A deep copy of an UNREGISTERED parse (before registerFieldOnlyChildren),
// registered on its own. ASTAlterCommand::clone() copies `refresh` by
// pointer, so it is deep-copied here; the other adopted fields are
// deep-copied by their own clone().
inline DB::ASTPtr clonePolicyAst(const DB::ASTPtr &unregistered) {
  DB::ASTPtr copy = unregistered->clone();
  std::function<void(DB::IAST &)> deepen = [&](DB::IAST &node) {
    if (auto *command = node.as<DB::ASTAlterCommand>(); command && command->refresh)
      command->refresh = command->refresh->clone();
    for (const auto &child : node.children)
      if (child) deepen(*child);
  };
  deepen(*copy);
  registerFieldOnlyChildren(*copy);
  return copy;
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
