// src/error_message_rewrite.h
//
// The text passes of RewriteErrorMessage: mapping "position N" back to the
// caller's SQL, swapping the rewritten statement back to the original one, and
// replacing physical table / database names with the logical ones.
//
// These used to be std::regex scans: one compiled regex per mapping run over
// the whole error text, a regex built from the entire rewritten statement, and
// a Myers diff whose memory grew with (statement length x edit distance).
// Upstream sql-rewriter traced its 2026-09-03 CPU saturation to the same
// per-mapping regex pattern; a large IN list made the diff alone allocate
// gigabytes. Every pass here is a linear scan that charges the bytes it reads
// to one WorkBudget per request, and the diff is bounded in edit distance.
//
// The matching rules are those of the replaced regexes (pinned by a
// differential test against them), except that replacement text is inserted
// literally: std::regex_replace read `$&`, `$1`, `$$`, ... in the caller's
// SQL and logical names as format escapes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rewriter_error {

// The byte budget one RewriteErrorMessage request spends across its passes.
// A pass charges what it reads before reading it; once the budget is spent, or
// the caller's cancellation hook reports true, charge() returns false for the
// rest of the request.
class WorkBudget {
public:
  // limit == 0 means unlimited (the hook is still consulted).
  explicit WorkBudget(uint64_t limit, std::function<bool()> cancelled = {})
    : remaining_(limit), unlimited_(limit == 0), cancelled_(std::move(cancelled)) {}

  bool charge(uint64_t units);
  bool exhausted() const { return stopped_; }

private:
  static constexpr uint64_t kCancelCheckInterval = 1u << 20;
  uint64_t remaining_;
  bool unlimited_;
  std::function<bool()> cancelled_;
  uint64_t since_cancel_check_ = 0;
  bool stopped_ = false;
};

// Largest edit distance buildOffsetMap computes. Its trace keeps (d + 1)^2
// ints, so this bounds the map's memory at about 16 MiB.
inline constexpr size_t kMaxOffsetMapEdits = 2048;

// Myers' O(ND) diff of `rewritten` against `original`, as a position map:
// result[i] (i in [0, rewritten.size()]) is the byte offset in `original`
// matching offset i in `rewritten`. Positions inside a span present only in
// `rewritten` collapse to the boundary of the surrounding equal range.
// nullopt when the edit distance exceeds `max_edits` or the budget runs out.
std::optional<std::vector<size_t>> buildOffsetMap(
  std::string_view rewritten, std::string_view original, size_t max_edits, WorkBudget &budget);

// One "position N" reference (`[Pp]osition`, whitespace, digits) in an error.
struct PositionRef {
  size_t digits_begin;
  size_t digits_end;
};

// Every position reference, left to right, without overlap.
std::vector<PositionRef> findPositionRefs(std::string_view err);

// `err` with each 1-based position in `refs` that falls inside the map
// replaced by its mapped value. A position of 0, one past the map, or one that
// does not fit in an integer is left as written.
std::string remapErrorPositions(
  std::string_view err, const std::vector<PositionRef> &refs, const std::vector<size_t> &pos_map);

// The first exact occurrence of `needle` in `haystack`, in linear time.
std::optional<size_t> findExact(std::string_view haystack, std::string_view needle, WorkBudget &budget);

// Replaces every occurrence of `sql` in `err`, matched ASCII-case-
// insensitively with each whitespace run in `sql` matching one or more
// whitespace characters, by `replacement`. nullopt when the budget runs out.
std::optional<std::string> replaceSqlBlock(
  std::string_view err, std::string_view sql, std::string_view replacement, WorkBudget &budget);

// Replaces every boundary-delimited, ASCII-case-insensitive mention of
// `ident` (optionally backtick-quoted) by `replacement`, keeping the boundary
// character before it. nullopt when the budget runs out.
std::optional<std::string> replaceIdentifier(
  std::string_view err, std::string_view ident, std::string_view replacement, WorkBudget &budget);

// replaceIdentifier for a "<db>.<table>" name split on its first dot; each
// part may be backtick-quoted, and a table part holding a dot must be (the
// formatter always quotes it). Without a dot this is replaceIdentifier.
std::optional<std::string> replaceQualifiedIdentifier(
  std::string_view err, std::string_view qualified, std::string_view replacement, WorkBudget &budget);

} // namespace rewriter_error
