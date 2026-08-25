// Ordered WITH-alias visibility shared by SELECT rewriting and SI preflight.
#pragma once

#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTWithElement.h>
#include <Parsers/IAST.h>

#include <string>
#include <unordered_set>

namespace rewriter_handlers {

using CTEAliasScope = std::unordered_set<std::string>;

inline void declareCTEAlias(
  const DB::ASTPtr &node, CTEAliasScope &scope) {
  if (const auto *element = node->as<DB::ASTWithElement>();
      element && !element->name.empty()) {
    scope.insert(element->name);
  }
}

// Visit a node's direct children with the scope visible at each source
// position. A non-recursive WITH exposes each alias only after its definition;
// WITH RECURSIVE exposes every alias to every definition and to the body.
// The returned scope is the one visible to the SELECT body.
template <typename Visitor>
CTEAliasScope visitChildrenWithCTEVisibility(
  const DB::IAST &node,
  const CTEAliasScope &parent_scope,
  Visitor &&visitor) {
  const auto *select = node.as<DB::ASTSelectQuery>();
  if (!select) {
    for (const auto &child : node.children) visitor(child, parent_scope);
    return parent_scope;
  }

  CTEAliasScope scope = parent_scope;
  const auto with_expr = select->with();
  if (with_expr && select->recursive_with) {
    for (const auto &child : with_expr->children) declareCTEAlias(child, scope);
  }
  if (with_expr) {
    for (const auto &child : with_expr->children) {
      visitor(child, scope);
      if (!select->recursive_with) declareCTEAlias(child, scope);
    }
  }
  for (const auto &child : node.children) {
    if (with_expr && child.get() == with_expr.get()) continue;
    visitor(child, scope);
  }
  return scope;
}

} // namespace rewriter_handlers
