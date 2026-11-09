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

#include <Common/Exception.h>
#include <Core/Defines.h>
#include <Parsers/ASTAlterQuery.h>
#include <Parsers/ASTColumnsTransformers.h>
#include <Parsers/ASTTTLElement.h>
#include <Parsers/ASTWindowDefinition.h>
#include <Parsers/IAST.h>
#include <Parsers/IParser.h>
#include <Parsers/IdentifierQuotingStyle.h>
#include <Parsers/parseQuery.h>
#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "rewriter.grpc.pb.h"

namespace DB::ErrorCodes {
extern const int TOO_DEEP_AST;
}

namespace rewriter_handlers {

constexpr DB::IdentifierQuotingRule  kQuotingRule  = DB::IdentifierQuotingRule::WhenNecessary;
constexpr DB::IdentifierQuotingStyle kQuotingStyle = DB::IdentifierQuotingStyle::Backticks;

// Secrets are shown: the output is the statement the backend runs, so a
// remote() password or an s3() key must reach it as written.
// ignore_redundant_parentheses: since ClickHouse 26.5 the AST remembers
// grouping parentheses the caller wrote, `(a = 1)`, and the formatter prints
// them. They are dropped, as 26.3 and the 25.8 / 26.3 backends did (the
// literal-fold markers and the fidelity measurements assume that output).
inline std::string formatAst(const DB::ASTPtr &ast) {
  return ast->formatWithPossiblyHidingSensitiveData(
    /*max_length=*/0, /*one_line=*/true, /*show_secrets=*/true,
    /*print_pretty_type_names=*/false, kQuotingRule, kQuotingStyle,
    /*ignore_redundant_parentheses=*/true);
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
//    …`) and recompression_codec (`RECOMPRESS CODEC(…)`).
// ClickHouse 26.8 keeps ASTAlterCommand::refresh (`ALTER … MODIFY REFRESH …
// DEPENDS ON …`, adopted here up to 26.3) in `children`, and its clone()
// deep-copies it.
// registerFieldOnlyChildren adopts them as children right after parsing; the
// formatter keeps reading the fields, which hold the same nodes. Clone only an
// unregistered tree (clonePolicyAst): APPLY's clone copies `children` by
// pointer, so a registered tree's clone would share the original lambda.
//
// forEachFieldOnlyMember calls f on each such member of `node` that is set and
// not already one of its children.
template <typename F>
inline void forEachFieldOnlyMember(const DB::IAST &node, F &&f) {
  const auto visit = [&node, &f](const DB::ASTPtr &field) {
    if (field && std::find(node.children.begin(), node.children.end(), field) == node.children.end())
      f(field);
  };
  if (const auto *element = node.as<DB::ASTWindowListElement>()) {
    visit(element->definition);
  } else if (const auto *apply = node.as<DB::ASTColumnsApplyTransformer>()) {
    visit(apply->lambda);
    visit(apply->parameters);
  } else if (const auto *ttl = node.as<DB::ASTTTLElement>()) {
    for (const auto &key : ttl->group_by_key) visit(key);
    for (const auto &assignment : ttl->group_by_assignments) visit(assignment);
    visit(ttl->recompression_codec);
  }
}

inline void registerFieldOnlyChildren(DB::IAST &node) {
  forEachFieldOnlyMember(node, [&node](const DB::ASTPtr &field) { node.children.push_back(field); });
  for (const auto &child : node.children)
    if (child) registerFieldOnlyChildren(*child);
}

// Parse bounds for caller SQL: ClickHouse server's own defaults
// (max_parser_depth, max_parser_backtracks, max_ast_depth), so a statement
// these refuse is one the server would refuse too. Without them the parser
// recursed without limit and a `1+1+…+1` chain built a tree deep enough to
// overflow the stack of the rewriter's own recursive walks, killing the
// process.
inline constexpr size_t kMaxParserDepth = DB::DBMS_DEFAULT_MAX_PARSER_DEPTH;
inline constexpr size_t kMaxParserBacktracks = DB::DBMS_DEFAULT_MAX_PARSER_BACKTRACKS;
inline constexpr size_t kMaxAstDepth = 1000;

// IAST::checkDepth for a tree that may hold field-only members: iterative,
// and it descends into those members as well as `children`, so the recursive
// walks that follow (clone, registerFieldOnlyChildren, the policy walkers) see
// a bounded tree whether or not it is registered yet. Throws TOO_DEEP_AST.
inline void checkPolicyAstDepth(const DB::IAST &root, size_t max_depth) {
  std::vector<std::pair<const DB::IAST *, size_t>> stack{{&root, 0}};
  while (!stack.empty()) {
    const auto [node, depth] = stack.back();
    stack.pop_back();
    if (depth >= max_depth)
      throw DB::Exception(DB::ErrorCodes::TOO_DEEP_AST, "AST is too deep. Maximum: {}", max_depth);
    for (const auto &child : node->children)
      if (child) stack.emplace_back(child.get(), depth + 1);
    forEachFieldOnlyMember(*node, [&stack, depth](const DB::ASTPtr &field) { stack.emplace_back(field.get(), depth + 1); });
  }
}

// DB::parseQuery over [begin, end) within the bounds above, the tree's depth
// (field-only members included) checked iteratively before anything walks it.
// Throws DB::Exception (TOO_DEEP_RECURSION, TOO_DEEP_AST, or the parser's own)
// like parseQuery. Keeps the (begin, end) overload so an INSERT's inline data
// still points into the caller's buffer.
inline DB::ASTPtr parseBounded(DB::IParser &parser, const char *begin, const char *end) {
  DB::ASTPtr ast = DB::parseQuery(parser, begin, end, /*description=*/"",
    /*max_query_size=*/0, kMaxParserDepth, kMaxParserBacktracks);
  if (ast) checkPolicyAstDepth(*ast, kMaxAstDepth);
  return ast;
}

// A deep copy of an UNREGISTERED parse (before registerFieldOnlyChildren),
// registered on its own. Every adopted field is deep-copied by its own
// node's clone(); a registered tree's clone would instead share them (APPLY's
// clone() copies `children` by pointer).
inline DB::ASTPtr clonePolicyAst(const DB::ASTPtr &unregistered) {
  DB::ASTPtr copy = unregistered->clone();
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
