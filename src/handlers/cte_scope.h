// Name binding for FROM / IN names, shared by SELECT rewriting, the SI
// preflight, the system-table collector and the accessed-table collectors.
// Port of rewriter-go v0.15.0 readSourceScope (internal/engine/nodes.go):
// spec 2026-09-26 §5 "Names that bind an unqualified IN operand", "Stored view
// bodies" and "No WITH binding in an R2 position".
#pragma once

#include <Parsers/ASTAlterQuery.h>
#include <Parsers/ASTAsterisk.h>
#include <Parsers/ASTColumnsMatcher.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTDeleteQuery.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTQualifiedAsterisk.h>
#include <Parsers/ASTQueryParameter.h>
#include <Parsers/ASTSelectIntersectExceptQuery.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTSubquery.h>
#include <Parsers/ASTUpdateQuery.h>
#include <Parsers/ASTWithAlias.h>
#include <Parsers/ASTWithElement.h>
#include <Parsers/IAST.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>

namespace rewriter_handlers {

// The names an unqualified FROM / IN name may bind to instead of a table
// (measured by rewriter-go on ClickHouse 26.2 and 25.8 under both analyzers):
//  - ctes: a read-query CTE name (`WITH c AS (SELECT …)`) binds a FROM table
//    and an IN operand in every nested scope, even under a same-named table
//    alias;
//  - aliases: a WITH expression alias whose value is provably not a table
//    reference binds an IN operand, never a FROM name, in every nested scope
//    (withValueIsNotTableReference). Any other value binds nothing and shadows
//    an enclosing binding of its name: ClickHouse reads a table through it;
//  - projection: a projection alias binds an IN operand only in its own
//    SELECT (a nested SELECT reads the table under enable_analyzer=0).
// A table-source alias never binds. Inside a stored view body (CREATE VIEW /
// MATERIALIZED VIEW, every depth: `stored_view`) a WITH alias binds only a
// single folded literal and no projection alias binds. In an R2 expression
// position (`unbound`: a structured DELETE / UPDATE / ALTER, INSERT VALUES
// rows, a CREATE column / constraint / index / TTL / storage expression)
// ClickHouse qualifies every name with the current database first, so no WITH
// or SELECT declares a binding there.
struct CTEAliasScope {
  std::unordered_set<std::string> ctes;
  std::unordered_set<std::string> aliases;
  std::unordered_set<std::string> projection;
  bool stored_view = false;
  bool unbound = false;

  // A one-part FROM / JOIN name that is not a table.
  bool bindsTable(const std::string &name) const { return ctes.count(name) > 0; }
  // A one-part IN operand that is not a table.
  bool bindsInOperand(const std::string &name) const {
    return ctes.count(name) > 0 || aliases.count(name) > 0 || projection.count(name) > 0;
  }
};

inline CTEAliasScope unboundScope() {
  CTEAliasScope scope;
  scope.unbound = true;
  return scope;
}

inline std::string lowerFunctionName(const std::string &name) {
  std::string lname = name;
  std::transform(lname.begin(), lname.end(), lname.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lname;
}

// view(), matched as ClickHouse matches it (26.2 accepts `VIEW(…)`, `View(…)`).
inline bool isViewTableFunction(const std::string &name) {
  return lowerFunctionName(name) == "view";
}

// A function whose SELECT arguments ClickHouse analyses on their own, without
// the enclosing WITH: the view family (view, viewIfPermitted, viewExplain) and
// executable()'s input query. Measured on 26.2, `WITH x AS (…) SELECT * FROM
// view(SELECT * FROM x)` reads the table x, at any depth — including a view()
// nested in remote / remoteSecure / cluster / clusterAllReplicas — and so does
// executable(…, (SELECT * FROM x)). A name there is a table, never the CTE.
inline bool opensFreshCTEScope(const std::string &name) {
  const std::string lname = lowerFunctionName(name);
  return lname == "view" || lname == "viewifpermitted" || lname == "viewexplain"
    || lname == "executable";
}

inline bool opensFreshCTEScope(const DB::IAST &node) {
  const auto *fn = node.as<DB::ASTFunction>();
  return fn && opensFreshCTEScope(fn->name);
}

// A node that may name something ClickHouse resolves to a table, or a query
// that can (rewriter-go nameReferenceKeys): an identifier of any kind, a
// query parameter, an asterisk or columns matcher, a lambda, a query.
inline bool containsNameReference(const DB::IAST &node) {
  if (dynamic_cast<const DB::ASTIdentifier *>(&node) || node.as<DB::ASTQueryParameter>()
      || node.as<DB::ASTAsterisk>() || node.as<DB::ASTQualifiedAsterisk>()
      || node.as<DB::ASTColumnsListMatcher>() || node.as<DB::ASTColumnsRegexpMatcher>()
      || node.as<DB::ASTQualifiedColumnsListMatcher>() || node.as<DB::ASTQualifiedColumnsRegexpMatcher>()
      || node.as<DB::ASTSubquery>() || node.as<DB::ASTSelectWithUnionQuery>()
      || node.as<DB::ASTSelectQuery>() || node.as<DB::ASTSelectIntersectExceptQuery>())
    return true;
  if (const auto *fn = node.as<DB::ASTFunction>(); fn && lowerFunctionName(fn->name) == "lambda") return true;
  for (const auto &child : node.children)
    if (child && containsNameReference(*child)) return true;
  return false;
}

// A function written with call syntax that rewriter-go's parser types as a
// generic `function` node, so its value binds whatever its arguments name
// (Go returns `!noParens` for such a node, regardless of its arguments): not
// an operator (`a + b`, `-x`, `(a, b)`, `[a, b]`, `x::T`), not CAST in any
// spelling, not a lambda, and not one of the forms Polyglot types as its own
// node — CASE → multiIf / caseWithExpression, EXTRACT, SUBSTRING … FROM,
// POSITION … IN, TRIM, DATEDIFF, EXISTS, and the modelled `if` / `ifNull` /
// `coalesce`. Measured on ClickHouse 26.2 / 25.8: Go rewrites the operand for
// `if` / `ifNull` / `coalesce` / `cast` with an identifier argument (Polyglot
// gives each its own node, so Go's classifier falls to containsNameReference),
// while it binds `multiIf` / `and` / `or` / `nullIf` / `identity` /
// `arrayElement` / `tupleElement` / an ordinary call. `if` and `multiIf` are
// the only call forms whose value the old analyzer (`enable_analyzer = 0`)
// folds to an identifier and reads as a table (analyzer on gives a safe
// UNKNOWN_IDENTIFIER); `if` is excluded here to match Go, and `multiIf` is
// excluded too — stricter than Go, which binds it (a Go-side finding) — as
// defence in depth. Unsure is "not a call": the operand stays a table.
inline bool plainFunctionCall(const DB::ASTFunction &fn) {
  if (fn.isOperator()) return false;
  static const std::unordered_set<std::string> special = {"cast", "_cast", "accuratecast",
    "accuratecastornull", "lambda", "if", "multiif", "ifnull", "coalesce", "casewithexpression",
    "extract", "substring", "position", "trimboth", "trimleft", "trimright", "datediff", "exists",
    "tuple", "array"};
  return special.count(lowerFunctionName(fn.name)) == 0;
}

// rewriter-go withValueIsNotTableReference, over ClickHouse's AST: a literal,
// a scalar subquery, a generic call-syntax function (plainFunctionCall), or
// any expression that names nothing (operators over literals, `tuple(1)`,
// `CAST(1 AS UInt64)`). An identifier value — bare, quoted, qualified, a
// no-parens keyword such as NOW or CURRENT_DATE (an identifier to ClickHouse),
// or another alias — makes ClickHouse read a table through the alias.
//
// Parenthesised-tuple fidelity note (measured 26.2 / 25.8): a server reads
// `((1, 2))` as a 1-tuple of a 2-tuple, `Tuple(Tuple(...))`, not the 2-tuple
// `(1, 2)`. The engine's embedded 26.3 parser folds `((1, 2))` into the flat
// literal `(1, 2)`, so after parsing the two are the identical AST and this
// classifier cannot tell them apart. A statement whose source holds such a
// fold is refused as a whole (foldsMeaningChangingLiteral, Task I3 fix
// rounds 2–3), so a binding decision here never reaches ClickHouse for a
// folded `((1, 2))`.
inline bool withValueIsNotTableReference(const DB::IAST &value) {
  if (value.as<DB::ASTLiteral>() || value.as<DB::ASTSubquery>()) return true;
  if (const auto *fn = value.as<DB::ASTFunction>(); fn && plainFunctionCall(*fn)) return true;
  if (dynamic_cast<const DB::ASTIdentifier *>(&value)) return false;
  return !containsNameReference(value);
}

// One scalar a stored view body binds: a number, a string, NULL or a boolean.
inline bool storedViewScalarField(const DB::Field &field) {
  switch (field.getType()) {
  case DB::Field::Types::Tuple:
  case DB::Field::Types::Array:
  case DB::Field::Types::Map:
  case DB::Field::Types::Object:
  case DB::Field::Types::AggregateFunctionState:
  case DB::Field::Types::CustomType:
    return false;
  default:
    return true;
  }
}

// rewriter-go withValueIsStoredViewLiteral: in a stored view body only a
// value ClickHouse's parser folds into one literal binds — a number (negated,
// hex, binary, inf / nan), a string (quoted, `x'…'`, heredoc), NULL or a
// boolean, optionally parenthesised, or a flat tuple / bracket array of those
// (a folded `((1, 2))` is refused as a whole — see the fidelity note on
// withValueIsNotTableReference). A function call, cast,
// operator, scalar subquery, `tuple(…)` / `array(…)` or a nested tuple make
// ClickHouse read the table named after the alias.
inline bool withValueIsStoredViewLiteral(const DB::IAST &value) {
  const auto *literal = value.as<DB::ASTLiteral>();
  if (!literal) return false;
  const auto &field = literal->value;
  if (storedViewScalarField(field)) return true;
  if (field.getType() == DB::Field::Types::Tuple) {
    const auto &items = field.safeGet<DB::Tuple>();
    return !items.empty() && std::all_of(items.begin(), items.end(), storedViewScalarField);
  }
  if (field.getType() == DB::Field::Types::Array) {
    const auto &items = field.safeGet<DB::Array>();
    return !items.empty() && std::all_of(items.begin(), items.end(), storedViewScalarField);
  }
  return false;
}

// Declares one WITH element (rewriter-go declareCTEBinding). A read-query CTE
// (`name AS (SELECT …)`) binds FROM and IN; an expression alias binds IN when
// its value allows it and otherwise removes every enclosing binding of its
// name. Nothing is declared in an R2 position.
inline void declareCTEAlias(const DB::ASTPtr &node, CTEAliasScope &scope) {
  if (!node || scope.unbound) return;
  if (const auto *element = node->as<DB::ASTWithElement>()) {
    if (!element->name.empty()) scope.ctes.insert(element->name);
    return;
  }
  if (!dynamic_cast<const DB::ASTWithAlias *>(node.get())) return;
  const std::string name = node->tryGetAlias();
  if (name.empty()) return;
  const bool binds = scope.stored_view ? withValueIsStoredViewLiteral(*node) : withValueIsNotTableReference(*node);
  if (binds) {
    scope.aliases.insert(name);
    return;
  }
  scope.aliases.erase(name);
  scope.ctes.erase(name);
}

// The projection aliases a SELECT gives its own IN operands (none in a stored
// view body or an R2 position).
inline void collectProjectionAliases(const DB::ASTSelectQuery &select, CTEAliasScope &scope) {
  scope.projection.clear();
  if (scope.unbound || scope.stored_view) return;
  const auto list = select.select();
  if (!list) return;
  for (const auto &expression : list->children) {
    if (!expression) continue;
    const std::string alias = expression->tryGetAlias();
    if (!alias.empty()) scope.projection.insert(alias);
  }
}

// The SELECT whose WITH clause ClickHouse applies to every arm of a set
// operation (enable_global_with_statement, the default; ApplyWithGlobalVisitor):
// the leading SELECT of the set expression, reached through INTERSECT /
// EXCEPT nodes but not through a parenthesised query. Measured on 26.2 and
// 25.8, both analyzers: `WITH 1 AS s SELECT 1 INTERSECT SELECT 1 WHERE 1 IN s`
// and the UNION / EXCEPT forms bind s in the later arm, and
// `(WITH 1 AS s SELECT …) UNION ALL SELECT … IN s` reads the table s.
inline const DB::ASTSelectQuery *leadingSetOperationSelect(const DB::IAST &node) {
  const DB::IAST *first = nullptr;
  if (const auto *set = node.as<DB::ASTSelectWithUnionQuery>()) {
    if (set->list_of_selects && !set->list_of_selects->children.empty())
      first = set->list_of_selects->children.front().get();
  } else if (node.as<DB::ASTSelectIntersectExceptQuery>()) {
    if (!node.children.empty()) first = node.children.front().get();
  }
  while (first && first->as<DB::ASTSelectIntersectExceptQuery>())
    first = first->children.empty() ? nullptr : first->children.front().get();
  return first ? first->as<DB::ASTSelectQuery>() : nullptr;
}

// The body a detached stored view walk starts from (rewriteEmbeddedBody for a
// CREATE VIEW / MATERIALIZED VIEW body, which the walkers reach without the
// CREATE node): StoredViewBodyGuard marks it for the duration of that walk.
inline const DB::IAST *&storedViewBodyRoot() {
  thread_local const DB::IAST *root = nullptr;
  return root;
}

class StoredViewBodyGuard {
public:
  explicit StoredViewBodyGuard(const DB::IAST *body) : previous_(storedViewBodyRoot()) {
    storedViewBodyRoot() = body;
  }
  ~StoredViewBodyGuard() { storedViewBodyRoot() = previous_; }
  StoredViewBodyGuard(const StoredViewBodyGuard &) = delete;
  StoredViewBodyGuard &operator=(const StoredViewBodyGuard &) = delete;

private:
  const DB::IAST *previous_;
};

// Visit a node's direct children with the scope visible at each position.
// A non-recursive WITH exposes each alias only after its definition; WITH
// RECURSIVE exposes every alias to every definition and to the body. The
// returned scope is the one visible to a SELECT's body.
template <typename Visitor>
CTEAliasScope visitChildrenWithCTEVisibility(
  const DB::IAST &node,
  const CTEAliasScope &parent_scope,
  Visitor &&visitor) {
  // The branches below that do not change the scope visit with `parent_scope`
  // directly, so an ordinary node (a function, an expression, a table
  // expression) does not copy the three sets. The stored-view-body root is
  // always a read-query node, handled in the subquery / set / select branches
  // that take a copy; it never reaches the create / alter / fresh / plain
  // paths, so those read parent_scope.
  if (opensFreshCTEScope(node)) {
    // A query-bodied table function's arguments are their own query: no outer
    // binding reaches them, but a view() inside a stored view body is stored
    // with it (rewriter-go tableFunctionArgScope), and a view() in an R2
    // position binds nothing (m3: unbound carried in; R2 refuses table
    // functions, so this is masked defence in depth).
    CTEAliasScope fresh;
    fresh.stored_view = parent_scope.stored_view;
    fresh.unbound = parent_scope.unbound;
    for (const auto &child : node.children) visitor(child, fresh);
    return parent_scope;
  }

  if (const auto *create = node.as<DB::ASTCreateQuery>()) {
    // A view / MV body is a stored view body; a CTAS body and an `AS
    // <table function>` source follow the plain rule; every column,
    // constraint, index, TTL and storage expression is an R2 position.
    CTEAliasScope body = parent_scope;
    body.projection.clear();
    if (create->is_ordinary_view || create->is_materialized_view) body.stored_view = true;
    const auto r2 = unboundScope();
    for (const auto &child : node.children) {
      const bool is_body = child.get() == static_cast<const DB::IAST *>(create->select)
        || child == create->as_table_function;
      visitor(child, is_body ? body : r2);
    }
    return parent_scope;
  }

  if (node.as<DB::ASTAlterQuery>() || node.as<DB::ASTDeleteQuery>() || node.as<DB::ASTUpdateQuery>()) {
    const auto r2 = unboundScope();
    for (const auto &child : node.children) visitor(child, r2);
    return parent_scope;
  }

  const auto *select = node.as<DB::ASTSelectQuery>();
  const bool is_query_node = select || node.as<DB::ASTSubquery>()
    || node.as<DB::ASTSelectWithUnionQuery>() || node.as<DB::ASTSelectIntersectExceptQuery>();
  if (!is_query_node) {
    // An ordinary node: no scope change, so no copy.
    for (const auto &child : node.children) visitor(child, parent_scope);
    return parent_scope;
  }

  CTEAliasScope scope = parent_scope;
  if (storedViewBodyRoot() == &node) scope.stored_view = true;

  if (!select) {
    // A nested query never sees the enclosing SELECT's projection aliases.
    scope.projection.clear();
    const auto *leading = leadingSetOperationSelect(node);
    if (!leading || !leading->with()) {
      for (const auto &child : node.children) visitor(child, scope);
      return scope;
    }
    // Every arm after the first sees the leading SELECT's WITH; the first
    // arm declares it itself.
    CTEAliasScope later_arms = scope;
    for (const auto &element : leading->with()->children) declareCTEAlias(element, later_arms);
    const auto visitArms = [&](const DB::ASTs &arms) {
      for (size_t i = 0; i < arms.size(); ++i) visitor(arms[i], i == 0 ? scope : later_arms);
    };
    if (const auto *set = node.as<DB::ASTSelectWithUnionQuery>()) {
      for (const auto &child : node.children) {
        if (child == set->list_of_selects) visitArms(child->children);
        else visitor(child, scope);
      }
    } else {
      visitArms(node.children);
    }
    return scope;
  }

  scope.projection.clear();
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
  collectProjectionAliases(*select, scope);
  for (const auto &child : node.children) {
    if (with_expr && child.get() == with_expr.get()) continue;
    visitor(child, scope);
  }
  return scope;
}

} // namespace rewriter_handlers
