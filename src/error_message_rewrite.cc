#include "error_message_rewrite.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace rewriter_error {

bool WorkBudget::charge(uint64_t units) {
  if (stopped_) return false;
  if (cancelled_) {
    // Checked on the first charge and then once per kCancelCheckInterval units.
    if (since_cancel_check_ == 0 || since_cancel_check_ + units >= kCancelCheckInterval) {
      since_cancel_check_ = 0;
      if (cancelled_()) {
        stopped_ = true;
        return false;
      }
    }
    since_cancel_check_ += units == 0 ? 1 : units;
  }
  if (unlimited_) return true;
  if (units > remaining_) {
    remaining_ = 0;
    stopped_ = true;
    return false;
  }
  remaining_ -= units;
  return true;
}

namespace {

inline char lowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// [A-Za-z0-9_], the class the replaced regexes used as an identifier boundary.
inline bool isIdentChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// The C locale's isspace: the set std::regex's \s matched.
inline bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

std::string lowerCopy(std::string_view s) {
  std::string out(s.size(), '\0');
  for (size_t i = 0; i < s.size(); ++i) out[i] = lowerAscii(s[i]);
  return out;
}

// Knuth-Morris-Pratt failure function of a non-empty pattern.
std::vector<size_t> failureFunction(std::string_view pattern) {
  std::vector<size_t> fail(pattern.size(), 0);
  size_t k = 0;
  for (size_t i = 1; i < pattern.size(); ++i) {
    while (k > 0 && pattern[i] != pattern[k]) k = fail[k - 1];
    if (pattern[i] == pattern[k]) ++k;
    fail[i] = k;
  }
  return fail;
}

// Calls on_match(start, stop) for every occurrence of a non-empty `pattern`
// in `text`, left to right. When on_match returns true the scan resumes after
// the occurrence (non-overlapping); when it returns false, overlapping
// occurrences are reported too. Setting `stop` ends the scan.
template <typename OnMatch>
void forEachOccurrence(std::string_view text, std::string_view pattern, OnMatch &&on_match) {
  const auto fail = failureFunction(pattern);
  size_t k = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    while (k > 0 && text[i] != pattern[k]) k = fail[k - 1];
    if (text[i] == pattern[k]) ++k;
    if (k == pattern.size()) {
      const size_t start = i + 1 - pattern.size();
      bool stop = false;
      const bool skip = on_match(start, stop);
      if (stop) return;
      k = skip ? 0 : fail[k - 1];
    }
  }
}

// One element of a name pattern: `?, `, '.', or a literal matched
// ASCII-case-insensitively.
struct NameElement {
  enum Kind { OptionalBacktick, Backtick, Dot, Literal } kind;
  std::string literal;  // lowercased; Literal only
};

// Emulates the replaced regex
//   (^|[^A-Za-z0-9_]) <elements> (?=[^A-Za-z0-9_]|$)
// (ECMAScript, icase) under std::regex_replace: leftmost match first, the
// alternatives in order, each `? greedy with backtracking; the boundary
// character before the name is kept and the name is replaced literally.
class NamePattern {
public:
  NamePattern(std::vector<NameElement> elements, std::string_view err, WorkBudget &budget)
    : elements_(std::move(elements)), err_(err), lower_(lowerCopy(err)), budget_(budget) {
    for (size_t i = 0; i < elements_.size(); ++i) {
      if (elements_[i].kind == NameElement::Literal) {
        first_literal_ = i;
        break;
      }
    }
  }

  std::optional<std::string> replaceAll(std::string_view replacement) {
    const size_t n = err_.size();
    if (!budget_.charge(n + patternLength())) return std::nullopt;

    // Every offset where the first literal occurs: a match must place it at
    // its start position plus at most the optional backtick before it, so
    // these offsets bound the candidate match starts.
    std::vector<size_t> starts;
    occurs_.assign(n + 1, 0);
    const std::string &first = elements_[first_literal_].literal;
    if (first.empty()) {
      for (size_t i = 0; i <= n; ++i) {
        occurs_[i] = 1;
        starts.push_back(i);
      }
    } else {
      forEachOccurrence(lower_, first, [&](size_t start, bool &) {
        occurs_[start] = 1;
        starts.push_back(start);
        return false;
      });
    }

    std::string out;
    out.reserve(n);
    size_t cur = 0;         // where the next search starts
    size_t next_try = 0;    // smallest start not tried yet
    const size_t lead = leadingOptionalBackticks();
    for (const size_t q : starts) {
      // A match whose first literal sits at q starts its boundary character
      // at q - 1 - b for b optional backticks consumed in front of it.
      const size_t lowest = q >= lead + 1 ? q - lead - 1 : 0;
      for (size_t s = std::max({lowest, cur, next_try}); s <= q; ++s) {
        next_try = s + 1;
        size_t end = 0;
        bool keep_boundary = false;
        if (s == 0 && cur == 0 && matchFrom(0, 0, &end)) {
          keep_boundary = false;
        } else if (s < n && !isIdentChar(err_[s]) && matchFrom(0, s + 1, &end)) {
          keep_boundary = true;
        } else {
          if (budget_.exhausted()) return std::nullopt;
          continue;
        }
        out.append(err_.substr(cur, s - cur));
        if (keep_boundary) out.push_back(err_[s]);
        out.append(replacement);
        cur = end;
        next_try = end;
        break;
      }
      if (budget_.exhausted()) return std::nullopt;
    }
    out.append(err_.substr(cur));
    return out;
  }

private:
  size_t patternLength() const {
    size_t len = 0;
    for (const auto &e : elements_) len += e.kind == NameElement::Literal ? e.literal.size() : 1;
    return len;
  }

  size_t leadingOptionalBackticks() const {
    size_t count = 0;
    for (size_t i = 0; i < first_literal_; ++i) {
      if (elements_[i].kind == NameElement::OptionalBacktick || elements_[i].kind == NameElement::Backtick) ++count;
    }
    return count;
  }

  bool matchFrom(size_t idx, size_t pos, size_t *end) {
    const size_t n = err_.size();
    if (idx == elements_.size()) {
      if (pos == n || !isIdentChar(err_[pos])) {
        *end = pos;
        return true;
      }
      return false;
    }
    const auto &e = elements_[idx];
    switch (e.kind) {
    case NameElement::OptionalBacktick:
      if (pos < n && err_[pos] == '`' && matchFrom(idx + 1, pos + 1, end)) return true;
      return matchFrom(idx + 1, pos, end);
    case NameElement::Backtick:
      return pos < n && err_[pos] == '`' && matchFrom(idx + 1, pos + 1, end);
    case NameElement::Dot:
      return pos < n && err_[pos] == '.' && matchFrom(idx + 1, pos + 1, end);
    case NameElement::Literal:
      if (idx == first_literal_) {
        if (pos > n || !occurs_[pos]) return false;
      } else {
        if (!budget_.charge(e.literal.size() + 1)) return false;
        if (e.literal.size() > n - std::min(pos, n)) return false;
        if (lower_.compare(pos, e.literal.size(), e.literal) != 0) return false;
      }
      return matchFrom(idx + 1, pos + e.literal.size(), end);
    }
    return false;
  }

  std::vector<NameElement> elements_;
  std::string_view err_;
  std::string lower_;
  WorkBudget &budget_;
  size_t first_literal_ = 0;
  std::vector<uint8_t> occurs_;
};

} // namespace

std::optional<std::vector<size_t>> buildOffsetMap(
  std::string_view rewritten, std::string_view original, size_t max_edits, WorkBudget &budget) {
  const size_t N = rewritten.size(), M = original.size();
  std::vector<size_t> pos_map(N + 1);
  pos_map[N] = M;
  if (N == 0) return pos_map;
  if (M == 0) {
    for (size_t i = 0; i < N; i++) pos_map[i] = 0;
    return pos_map;
  }
  if (N + M > static_cast<size_t>(INT32_MAX)) return std::nullopt;

  // The same forward Myers search as before, with V and its per-step
  // snapshots holding only the diagonals a step can reach (|k| <= d + 1)
  // rather than all N + M of them.
  const long long D = static_cast<long long>(std::min<size_t>(max_edits, N + M));
  const long long off = D + 1;
  auto kIdx = [off](long long k) { return static_cast<size_t>(k + off); };
  std::vector<int32_t> V(static_cast<size_t>(2 * off + 1), 0);
  // trace[d] is V over diagonals [-d, d] before step d.
  std::vector<std::vector<int32_t>> trace;
  const long long n = static_cast<long long>(N), m = static_cast<long long>(M);

  long long found_d = -1;
  for (long long d = 0; d <= D; d++) {
    if (!budget.charge(static_cast<uint64_t>(2 * d + 1))) return std::nullopt;
    trace.emplace_back(V.begin() + static_cast<long long>(kIdx(-d)), V.begin() + static_cast<long long>(kIdx(d)) + 1);
    uint64_t slid = 0;
    bool done = false;
    for (long long k = -d; k <= d && !done; k += 2) {
      long long x;
      const bool down = (k == -d) || (k != d && V[kIdx(k - 1)] < V[kIdx(k + 1)]);
      x = down ? V[kIdx(k + 1)] : V[kIdx(k - 1)] + 1;
      long long y = x - k;
      const long long x0 = x;
      while (x < n && y < m && rewritten[static_cast<size_t>(x)] == original[static_cast<size_t>(y)]) {
        x++; y++;
      }
      slid += static_cast<uint64_t>(x - x0);
      V[kIdx(k)] = static_cast<int32_t>(x);
      if (x >= n && y >= m) {
        found_d = d;
        done = true;
      }
    }
    if (!budget.charge(slid)) return std::nullopt;
    if (done) break;
  }
  if (found_d < 0) return std::nullopt;  // more than max_edits edits

  auto traceAt = [&trace](long long d, long long k) { return trace[static_cast<size_t>(d)][static_cast<size_t>(k + d)]; };
  long long x = n, y = m;
  for (long long d = found_d; d > 0; d--) {
    const long long k = x - y;
    bool down;
    if (k == -d) {
      down = true;
    } else if (k == d) {
      down = false;
    } else {
      down = traceAt(d, k - 1) < traceAt(d, k + 1);
    }
    const long long prev_k = down ? k + 1 : k - 1;
    const long long prev_x = traceAt(d, prev_k);
    const long long prev_y = prev_x - prev_k;
    const long long slide_start_x = down ? prev_x : prev_x + 1;
    const long long slide_start_y = down ? prev_y + 1 : prev_y;

    while (x > slide_start_x && y > slide_start_y) {
      x--; y--;
      if (x < n) pos_map[static_cast<size_t>(x)] = static_cast<size_t>(y);
    }
    if (down) {
      y = prev_y;
    } else {
      if (prev_x < n) pos_map[static_cast<size_t>(prev_x)] = static_cast<size_t>(prev_y);
      x = prev_x;
    }
  }
  while (x > 0 && y > 0) {
    x--; y--;
    if (x < n) pos_map[static_cast<size_t>(x)] = static_cast<size_t>(y);
  }
  while (x > 0) {
    x--;
    pos_map[static_cast<size_t>(x)] = 0;
  }
  return pos_map;
}

std::vector<PositionRef> findPositionRefs(std::string_view err) {
  // `[Pp]osition\s+\d+`, case-sensitive past the first letter.
  static constexpr std::string_view kTail = "osition";
  std::vector<PositionRef> refs;
  size_t cur = 0;
  while (cur < err.size()) {
    const size_t j = err.find(kTail, cur + 1);
    if (j == std::string_view::npos) break;
    if (err[j - 1] != 'P' && err[j - 1] != 'p') {
      cur = j;
      continue;
    }
    size_t k = j + kTail.size();
    const size_t ws_begin = k;
    while (k < err.size() && isSpace(err[k])) ++k;
    const size_t digits_begin = k;
    while (k < err.size() && isDigit(err[k])) ++k;
    if (k == digits_begin || digits_begin == ws_begin) {
      cur = j;
      continue;
    }
    refs.push_back({digits_begin, k});
    cur = k;
  }
  return refs;
}

std::string remapErrorPositions(
  std::string_view err, const std::vector<PositionRef> &refs, const std::vector<size_t> &pos_map) {
  if (pos_map.size() <= 1) return std::string(err);
  std::string out;
  out.reserve(err.size());
  size_t last = 0;
  for (const auto &ref : refs) {
    out.append(err.substr(last, ref.digits_begin - last));
    const std::string digits(err.substr(ref.digits_begin, ref.digits_end - ref.digits_begin));
    size_t pos1 = 0;  // 1-based, as ClickHouse prints it
    bool ok = true;
    try { pos1 = std::stoull(digits); } catch (...) { ok = false; }
    if (!ok || pos1 == 0 || pos1 > pos_map.size() - 1) {
      out.append(digits);
    } else {
      out.append(std::to_string(pos_map[pos1 - 1] + 1));
    }
    last = ref.digits_end;
  }
  out.append(err.substr(last));
  return out;
}

std::optional<size_t> findExact(std::string_view haystack, std::string_view needle, WorkBudget &budget) {
  if (needle.empty()) return 0;
  if (needle.size() > haystack.size()) return std::nullopt;
  if (!budget.charge(haystack.size() + needle.size())) return std::nullopt;
  std::optional<size_t> found;
  forEachOccurrence(haystack, needle, [&](size_t start, bool &stop) {
    found = start;
    stop = true;
    return true;
  });
  return found;
}

std::optional<bool> containsIdentifier(std::string_view err, std::string_view ident, WorkBudget &budget) {
  if (ident.empty() || ident.size() > err.size()) return false;
  if (!budget.charge(err.size() + ident.size())) return std::nullopt;
  bool found = false;
  forEachOccurrence(err, ident, [&](size_t start, bool &stop) {
    const size_t stop_at = start + ident.size();
    if ((start == 0 || !isIdentChar(err[start - 1])) && (stop_at == err.size() || !isIdentChar(err[stop_at]))) {
      found = true;
      stop = true;
    }
    return false;
  });
  return found;
}

std::optional<std::string> replaceSqlBlock(
  std::string_view err, std::string_view sql, std::string_view replacement, WorkBudget &budget) {
  if (sql.empty()) return std::string(err);
  if (!budget.charge(err.size() + sql.size())) return std::nullopt;

  // Fold case and collapse each whitespace run to one space on both sides; a
  // match of the folded texts is a match of the old whitespace-flexible
  // regex. starts[i] is where folded character i begins in `err`.
  auto fold = [](std::string_view s, std::vector<size_t> *starts) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
      if (starts) starts->push_back(i);
      if (isSpace(s[i])) {
        out.push_back(' ');
        while (i < s.size() && isSpace(s[i])) ++i;
      } else {
        out.push_back(lowerAscii(s[i]));
        ++i;
      }
    }
    if (starts) starts->push_back(s.size());
    return out;
  };
  const std::string pattern = fold(sql, nullptr);
  std::vector<size_t> starts;
  starts.reserve(err.size() + 1);
  const std::string text = fold(err, &starts);
  if (pattern.size() > text.size()) return std::string(err);

  std::string out;
  out.reserve(err.size());
  size_t last = 0;
  forEachOccurrence(text, pattern, [&](size_t start, bool &) {
    const size_t begin = starts[start];
    const size_t end = starts[start + pattern.size()];
    out.append(err.substr(last, begin - last));
    out.append(replacement);
    last = end;
    return true;
  });
  out.append(err.substr(last));
  return out;
}

std::optional<std::string> replaceIdentifier(
  std::string_view err, std::string_view ident, std::string_view replacement, WorkBudget &budget) {
  std::vector<NameElement> elements{
    {NameElement::OptionalBacktick, {}},
    {NameElement::Literal, lowerCopy(ident)},
    {NameElement::OptionalBacktick, {}},
  };
  return NamePattern(std::move(elements), err, budget).replaceAll(replacement);
}

std::optional<std::string> replaceQualifiedIdentifier(
  std::string_view err, std::string_view qualified, std::string_view replacement, WorkBudget &budget) {
  const size_t dot = qualified.find('.');
  if (dot == std::string_view::npos) return replaceIdentifier(err, qualified, replacement, budget);
  const std::string_view db = qualified.substr(0, dot);
  const std::string_view table = qualified.substr(dot + 1);
  std::vector<NameElement> elements{
    {NameElement::OptionalBacktick, {}},
    {NameElement::Literal, lowerCopy(db)},
    {NameElement::OptionalBacktick, {}},
    {NameElement::Dot, {}},
  };
  // A table part holding a dot is always backtick-quoted by the formatter.
  const auto quote = table.find('.') != std::string_view::npos ? NameElement::Backtick : NameElement::OptionalBacktick;
  elements.push_back({quote, {}});
  elements.push_back({NameElement::Literal, lowerCopy(table)});
  elements.push_back({quote, {}});
  return NamePattern(std::move(elements), err, budget).replaceAll(replacement);
}

} // namespace rewriter_error
