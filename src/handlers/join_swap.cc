#include "handlers/join_swap.h"

#include <Core/Joins.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ASTIdentifier.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace rewriter_handlers {

namespace {

bool applyJoinSwapImpl(
    DB::IAST *node,
    const std::unordered_map<std::string, uint64_t> &table_sizes,
    uint64_t default_size,
    double swap_threshold) {
  if (!node) return false;

  // Process children first (bottom-up) so nested SELECTs are handled before
  // their parents, matching the forceGlobalForRemoteAsymmetry approach.
  bool any_swapped = false;
  for (auto &ch : node->children) {
    if (applyJoinSwapImpl(ch.get(), table_sizes, default_size, swap_threshold))
      any_swapped = true;
  }

  auto *select = node->as<DB::ASTSelectQuery>();
  if (!select) return any_swapped;

  auto tables = select->tables();
  if (!tables) return any_swapped;
  if (tables->children.size() < 2) return any_swapped;

  // Only swap the first JOIN (children[0] = FROM, children[1] = first JOIN).
  // Subsequent JOINs have an accumulated left side whose size is not a simple
  // table lookup.
  auto *from_el = tables->children[0]->as<DB::ASTTablesInSelectQueryElement>();
  auto *join_el = tables->children[1]->as<DB::ASTTablesInSelectQueryElement>();
  if (!from_el || !join_el) return any_swapped;
  if (!join_el->table_join) return any_swapped;

  auto *left_te = from_el->table_expression
                     ? from_el->table_expression->as<DB::ASTTableExpression>()
                     : nullptr;
  auto *right_te = join_el->table_expression
                      ? join_el->table_expression->as<DB::ASTTableExpression>()
                      : nullptr;
  if (!left_te || !right_te) return any_swapped;

  if (!left_te->database_and_table_name || !right_te->database_and_table_name)
    return any_swapped;

  auto *left_id = left_te->database_and_table_name->as<DB::ASTTableIdentifier>();
  auto *right_id = right_te->database_and_table_name->as<DB::ASTTableIdentifier>();
  if (!left_id || !right_id) return any_swapped;

  // Skip joins whose semantics are not preserved under a left-right swap.
  // This matches ClickHouse's own conservative policy in
  // src/Processors/QueryPlan/Optimizations/optimizeJoin.cpp (the
  // optimizeJoinLogical / optimizeJoinLegacy passes that back
  // `query_plan_join_swap_table`):
  //   * ASOF: the ON-clause inequality (e.g. `t1.t >= t2.t`) is interpreted
  //     relative to which side is the "lookup" (right) table. Swapping the
  //     tables without flipping the inequality changes the matched row set.
  //   * PASTE: joins parts by row position with no ON/USING — swapping
  //     reorders the projected columns.
  //   * USING: CH's note — "USING clause handled specially in join
  //     algorithm, so swap breaks it." Stay conservative.
  //   * SEMI / ANTI: CH allows only `strictness == All` (and ANY when partial
  //     merge join is out of the picture). SEMI/ANTI swap is theoretically
  //     correct with kind flip, but CH's own comment is "should be alright,
  //     need to try to enable it and test" — they haven't enabled it, so
  //     neither do we.
  if (auto *tj_pre = join_el->table_join->as<DB::ASTTableJoin>()) {
    if (tj_pre->strictness == DB::JoinStrictness::Asof) return any_swapped;
    if (tj_pre->strictness == DB::JoinStrictness::Semi) return any_swapped;
    if (tj_pre->strictness == DB::JoinStrictness::Anti) return any_swapped;
    if (tj_pre->kind == DB::JoinKind::Paste) return any_swapped;
    if (tj_pre->using_expression_list) return any_swapped;
  }

  const std::string left_key = left_id->name();
  const std::string right_key = right_id->name();

  auto left_it = table_sizes.find(left_key);
  auto right_it = table_sizes.find(right_key);
  uint64_t left_size = left_it != table_sizes.end() ? left_it->second : default_size;
  uint64_t right_size = right_it != table_sizes.end() ? right_it->second : default_size;

  if (right_size <= left_size) return any_swapped;

  double ratio = (left_size == 0)
    ? (right_size > 0 ? 1.0e6 : 1.0)
    : static_cast<double>(right_size) / static_cast<double>(left_size);
  if (ratio <= swap_threshold) return any_swapped;

  // Swap member pointers — table_join stays on the JOIN element.
  std::swap(from_el->table_expression, join_el->table_expression);

  // Rebuild children lists to match the swapped members.
  // The parser orders children as [table_expression, table_join, array_join]
  // (see ParserTablesInSelectQueryElement in ClickHouse's parser).
  auto rebuildChildren = [](DB::ASTTablesInSelectQueryElement *el) {
    el->children.clear();
    if (el->table_expression) el->children.push_back(el->table_expression);
    if (el->table_join)       el->children.push_back(el->table_join);
    if (el->array_join)       el->children.push_back(el->array_join);
  };
  rebuildChildren(from_el);
  rebuildChildren(join_el);

  auto *tj = join_el->table_join->as<DB::ASTTableJoin>();
  if (tj) {
    tj->kind = DB::reverseJoinKind(tj->kind);
  }
  return true;
}

} // namespace

bool applyJoinSwap(
    DB::ASTPtr ast,
    const std::unordered_map<std::string, uint64_t> &table_sizes,
    uint64_t default_size,
    double swap_threshold) {
  return applyJoinSwapImpl(ast.get(), table_sizes, default_size, swap_threshold);
}

} // namespace rewriter_handlers
