#pragma once

#include <Parsers/IAST.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace rewriter_handlers {

// Apply JOIN left-right swaps based on table size metadata provided by the
// caller. Walks the AST bottom-up looking for ASTSelectQuery nodes with JOINs.
// When the right table of a JOIN is larger than the left table (per
// `table_sizes` and `swap_threshold`), it swaps the two sides and adjusts the
// JOIN kind (Left↔Right, others unchanged) to preserve semantics.
//
// Only the first JOIN (the FROM table vs. the first JOIN target) is swapped,
// since subsequent JOINs accumulate a compound left side whose size is hard
// to estimate. Both sides must be simple table references (not subqueries or
// table functions) for the swap to trigger.
//
// JOIN kinds whose semantics are not preserved under a naive left-right swap
// are skipped. This matches ClickHouse's own conservative policy in
// src/Processors/QueryPlan/Optimizations/optimizeJoin.cpp:
//   * ASOF — the ON-clause inequality is interpreted relative to which side
//     is the lookup (right) table.
//   * PASTE — joins parts by row position with no ON/USING; swap reorders
//     the projected columns.
//   * USING — CH's note: "USING clause handled specially in join algorithm,
//     so swap breaks it."
//   * SEMI / ANTI strictness — theoretically swappable with kind flip but
//     CH has not enabled it; we follow suit.
//
// Returns true iff at least one JOIN was swapped.
bool applyJoinSwap(
    DB::ASTPtr ast,
    const std::unordered_map<std::string, uint64_t> &table_sizes,
    uint64_t default_size,
    double swap_threshold);

} // namespace rewriter_handlers
