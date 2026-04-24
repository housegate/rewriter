// src/query_preprocessor.h
//
// Regex-based preprocessor that splits oversized `IN (...)` / `IN [...]`
// clauses into OR-joined batches of `IN_CLAUSE_BATCH_SIZE` elements each.
// Runs before DB::parseQuery() as a guard against stack overflow in the
// ClickHouse recursive-descent parser. Does not touch the AST; only the
// raw SQL string.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace QueryLimits {
constexpr size_t IN_CLAUSE_SPLIT_THRESHOLD = 50;  // Split IN clauses larger than this.
constexpr size_t IN_CLAUSE_BATCH_SIZE = 50;       // Each OR-joined batch has this many elements.
} // namespace QueryLimits

class QueryPreprocessor {
public:
  // Preprocess a SQL query to split large IN clauses.
  // Returns the rewritten SQL, or the original SQL if nothing needed splitting.
  // When a split actually happens, emits a single [preprocess] log line
  // tagged with `sql_id` for correlation with the access log. The no-op
  // path is silent.
  static std::string preprocess(const std::string &sql, const std::string &sql_id = "");

private:
  struct INClauseInfo {
    size_t start_pos;          // Start offset in the SQL string.
    size_t end_pos;            // End offset in the SQL string.
    std::string full_match;    // Full matched substring.
    std::string prefix;        // Text before "IN" — e.g. "coin_type" or "(owner_address, coin_type)".
    std::string content;       // Everything inside the brackets.
    char bracket_type;         // '(' or '['.
    size_t element_count;      // Estimated element count (comma-count at top level + 1).
  };

  static std::vector<INClauseInfo> findLargeINClauses(const std::string &sql);
  static std::string splitINClause(const INClauseInfo &info);
  static size_t estimateElementCount(const std::string &content);
  static std::vector<std::string> extractElements(const std::string &content);
};
