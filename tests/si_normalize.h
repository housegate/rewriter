#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace si_corpus {
namespace detail {

inline bool SIHasPrefixAt(const std::string &sql, std::size_t start,
                          std::string_view prefix) {
  return start <= sql.size() && prefix.size() <= sql.size() - start &&
         sql.compare(start, prefix.size(), prefix.data(), prefix.size()) == 0;
}

inline bool SIASCIIWordStart(char c) {
  return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

inline bool SIASCIIWordByte(char c) {
  return SIASCIIWordStart(c) || (c >= '0' && c <= '9');
}

inline char SIASCIILower(char c) {
  if (c >= 'A' && c <= 'Z') {
    return static_cast<char>(c + ('a' - 'A'));
  }
  return c;
}

inline bool SIEqualFold(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (SIASCIILower(lhs[i]) != SIASCIILower(rhs[i])) {
      return false;
    }
  }
  return true;
}

inline bool SIHasLiteralPrefix(const std::string &sql, std::size_t quote) {
  if (quote > 0 && SIASCIIWordByte(sql[quote - 1])) {
    return true;
  }
  return quote >= 2 && sql[quote - 1] == '&' &&
         (sql[quote - 2] == 'U' || sql[quote - 2] == 'u');
}

inline bool SIUnsupportedUnicodeQuoteAt(const std::string &sql,
                                        std::size_t start) {
  return SIHasPrefixAt(sql, start, "\xE2\x80\x98") ||
         SIHasPrefixAt(sql, start, "\xE2\x80\x99") ||
         SIHasPrefixAt(sql, start, "\xE2\x80\x9C") ||
         SIHasPrefixAt(sql, start, "\xE2\x80\x9D");
}

inline bool SIAppendSingleQuoted(std::string *out, const std::string &sql,
                                 std::size_t start, std::size_t *end) {
  out->push_back('\'');
  for (std::size_t i = start + 1; i < sql.size(); ++i) {
    out->push_back(sql[i]);
    switch (sql[i]) {
      case '\\':
        if (i + 1 >= sql.size()) {
          return false;
        }
        ++i;
        out->push_back(sql[i]);
        break;
      case '\'':
        if (i + 1 < sql.size() && sql[i + 1] == '\'') {
          ++i;
          out->push_back(sql[i]);
        } else {
          *end = i + 1;
          return true;
        }
        break;
      default:
        break;
    }
  }
  return false;
}

inline bool SIAppendANSIIdentifier(std::string *out, const std::string &sql,
                                   std::size_t start, std::size_t *end) {
  out->push_back('"');
  for (std::size_t i = start + 1; i < sql.size(); ++i) {
    out->push_back(sql[i]);
    if (sql[i] != '"') {
      continue;
    }
    if (i + 1 < sql.size() && sql[i + 1] == '"') {
      ++i;
      out->push_back(sql[i]);
      continue;
    }
    *end = i + 1;
    return true;
  }
  return false;
}

inline bool SIAppendBacktickIdentifier(std::string *out,
                                       const std::string &sql,
                                       std::size_t start, std::size_t *end) {
  out->push_back('"');
  for (std::size_t i = start + 1; i < sql.size(); ++i) {
    switch (sql[i]) {
      case '\\':
        if (i + 1 < sql.size() && sql[i + 1] == '`') {
          ++i;
          out->push_back('`');
        } else {
          out->push_back(sql[i]);
        }
        break;
      case '`':
        if (i + 1 < sql.size() && sql[i + 1] == '`') {
          ++i;
          out->push_back('`');
        } else {
          out->push_back('"');
          *end = i + 1;
          return true;
        }
        break;
      case '"':
        out->append("\"\"");
        break;
      default:
        out->push_back(sql[i]);
        break;
    }
  }
  return false;
}

}  // namespace detail

// NormalizeSIIdentifierQuotes canonicalizes matched ClickHouse backtick
// identifiers to ANSI double-quoted identifiers for the shared Go/C++ corpus.
// It supports only the deliberately narrow SQL subset emitted by the two
// generators: ordinary text, single-quoted literals, matched backtick
// identifiers, and matched ANSI double-quoted identifiers.
//
// This is a comparison guard, not a SQL lexer. If normal-state input contains a
// construct whose boundaries require dialect-aware tokenization -- comments,
// dollar/triple/smart quotes, prefixed literals, or INSERT ... FORMAT raw data --
// the function returns the complete original SQL unchanged. It does the same
// for every unmatched supported delimiter. That fail-closed choice may leave a
// harmless identifier-style difference visible (false inequality), but it can
// never normalize bytes inside an uncertain literal or payload and hide a real
// semantic difference (false equality).
//
// Exact mirror of rewriter-go/internal/harness/sinormalize.go. Keep the two
// implementations and their tests in lockstep.
inline std::string NormalizeSIIdentifierQuotes(const std::string &sql) {
  std::string out;
  out.reserve(sql.size());

  bool saw_insert = false;
  for (std::size_t i = 0; i < sql.size();) {
    switch (sql[i]) {
      case '\'': {
        if (detail::SIHasPrefixAt(sql, i, "'''") ||
            detail::SIHasLiteralPrefix(sql, i)) {
          return sql;
        }
        std::size_t end = 0;
        if (!detail::SIAppendSingleQuoted(&out, sql, i, &end)) {
          return sql;
        }
        i = end;
        break;
      }
      case '"': {
        if (detail::SIHasPrefixAt(sql, i, "\"\"\"") ||
            detail::SIHasLiteralPrefix(sql, i)) {
          return sql;
        }
        std::size_t end = 0;
        if (!detail::SIAppendANSIIdentifier(&out, sql, i, &end)) {
          return sql;
        }
        i = end;
        break;
      }
      case '`': {
        std::size_t end = 0;
        if (!detail::SIAppendBacktickIdentifier(&out, sql, i, &end)) {
          return sql;
        }
        i = end;
        break;
      }
      case '-':
        if (i + 1 < sql.size() && sql[i + 1] == '-') {
          return sql;
        }
        out.push_back(sql[i]);
        ++i;
        break;
      case '#':
      case '$':
        return sql;
      case '/':
        if (i + 1 < sql.size() &&
            (sql[i + 1] == '/' || sql[i + 1] == '*')) {
          return sql;
        }
        out.push_back(sql[i]);
        ++i;
        break;
      default:
        if (detail::SIUnsupportedUnicodeQuoteAt(sql, i)) {
          return sql;
        }
        if (detail::SIASCIIWordStart(sql[i])) {
          std::size_t end = i + 1;
          while (end < sql.size() && detail::SIASCIIWordByte(sql[end])) {
            ++end;
          }
          const std::string_view word(sql.data() + i, end - i);
          if (detail::SIEqualFold(word, "INSERT")) {
            saw_insert = true;
          } else if (saw_insert && detail::SIEqualFold(word, "FORMAT")) {
            return sql;
          }
          out.append(sql, i, end - i);
          i = end;
          continue;
        }
        out.push_back(sql[i]);
        ++i;
        break;
    }
  }
  return out;
}

}  // namespace si_corpus
