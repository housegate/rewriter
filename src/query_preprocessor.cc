// src/query_preprocessor.cc
#include "query_preprocessor.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <regex>
#include <sstream>

namespace {

bool maybeContainsInKeyword(const std::string &sql) {
  for (size_t i = 0; i + 1 < sql.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(sql[i])) != 'i'
        || std::tolower(static_cast<unsigned char>(sql[i + 1])) != 'n') {
      continue;
    }

    const bool left_boundary =
      i == 0 || (!std::isalnum(static_cast<unsigned char>(sql[i - 1])) && sql[i - 1] != '_');
    const bool right_boundary =
      i + 2 >= sql.size()
      || (!std::isalnum(static_cast<unsigned char>(sql[i + 2])) && sql[i + 2] != '_');
    if (left_boundary && right_boundary) return true;
  }
  return false;
}

} // namespace

std::string QueryPreprocessor::preprocess(const std::string& sql, const std::string& sql_id) {
  if (!maybeContainsInKeyword(sql)) {
    return sql;
  }

  // Happy path is the common case — find + bail silently so we don't spam
  // stderr on every request. The access log in doRewrite already captures
  // the fact that a request ran and the outer sql length.
  auto large_in_clauses = findLargeINClauses(sql);
  if (large_in_clauses.empty()) {
    return sql;
  }

  // Replace large IN clauses back-to-front so earlier positions stay valid.
  std::string result = sql;
  for (auto it = large_in_clauses.rbegin(); it != large_in_clauses.rend(); ++it) {
    const auto& info = *it;
    std::string replacement = splitINClause(info);
    result.replace(info.start_pos, info.end_pos - info.start_pos, replacement);
  }

  // One-liner summary, only when we actually transformed something. The
  // sql=<hash> prefix matches the [access] line so operators can grep for
  // all log lines belonging to the same request.
  std::cerr << "[preprocess] sql=" << (sql_id.empty() ? "-" : sql_id)
            << " split_in_clauses=" << large_in_clauses.size()
            << " sql_len_in=" << sql.size()
            << " sql_len_out=" << result.size() << std::endl;
  return result;
}

std::vector<QueryPreprocessor::INClauseInfo> QueryPreprocessor::findLargeINClauses(const std::string& sql) {
  std::vector<INClauseInfo> result;

  // Regex matches an IN clause.
  // Supports: column IN [...], (col1, col2) IN [...], column IN (...), (col1, col2) IN (...)
  // Match either a bare identifier or a parenthesized identifier list.
  static const std::regex in_pattern(
    R"((\([^)]+\)|[\w.]+)\s+IN\s*([\[\(]))",
    std::regex::icase
  );

  auto words_begin = std::sregex_iterator(sql.begin(), sql.end(), in_pattern);
  auto words_end = std::sregex_iterator();

  for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
    std::smatch match = *i;
    size_t match_start = match.position();
    std::string prefix = match[1].str();
    char bracket_type = match[2].str()[0];
    char closing_bracket = (bracket_type == '[') ? ']' : ')';

    // Find the closing bracket
    size_t bracket_start = match_start + match.length();
    int bracket_count = 1;
    size_t pos = bracket_start;
    bool in_string = false;
    char string_char = '\0';

    while (pos < sql.size() && bracket_count > 0) {
      char c = sql[pos];

      // Handle string literals
      if ((c == '\'' || c == '"') && (pos == 0 || sql[pos-1] != '\\')) {
        if (!in_string) {
          in_string = true;
          string_char = c;
        } else if (c == string_char) {
          in_string = false;
        }
      }

      if (!in_string) {
        if (c == bracket_type) {
          bracket_count++;
        } else if (c == closing_bracket) {
          bracket_count--;
        }
      }

      pos++;
    }

    if (bracket_count == 0) {
      // Successfully found closing bracket
      std::string content = sql.substr(bracket_start, pos - bracket_start - 1);
      size_t element_count = estimateElementCount(content);

      if (element_count > QueryLimits::IN_CLAUSE_SPLIT_THRESHOLD) {
        INClauseInfo info;
        info.start_pos = match_start;
        info.end_pos = pos;
        info.prefix = prefix;
        info.content = content;
        info.bracket_type = bracket_type;
        info.element_count = element_count;
        info.full_match = sql.substr(match_start, pos - match_start);

        result.push_back(info);
      }
    }
  }

  return result;
}

std::string QueryPreprocessor::splitINClause(const INClauseInfo& info) {
  // Extract elements from the IN clause
  auto elements = extractElements(info.content);

  if (elements.empty()) {
    // If we can't parse elements, return original
    return info.full_match;
  }

  // Split into batches and create OR conditions
  std::ostringstream oss;
  oss << "(";

  for (size_t i = 0; i < elements.size(); i += QueryLimits::IN_CLAUSE_BATCH_SIZE) {
    if (i > 0) {
      oss << " OR ";
    }

    size_t batch_end = std::min(i + QueryLimits::IN_CLAUSE_BATCH_SIZE, elements.size());

    oss << info.prefix << " IN " << info.bracket_type;
    for (size_t j = i; j < batch_end; ++j) {
      if (j > i) {
        oss << ", ";
      }
      oss << elements[j];
    }
    oss << (info.bracket_type == '[' ? ']' : ')');
  }

  oss << ")";

  return oss.str();
}

size_t QueryPreprocessor::estimateElementCount(const std::string& content) {
  // Simple estimation: count commas at bracket level 0
  int bracket_count = 0;
  int paren_count = 0;
  bool in_string = false;
  char string_char = '\0';
  size_t comma_count = 0;

  for (size_t i = 0; i < content.size(); ++i) {
    char c = content[i];

    // Handle string literals
    if ((c == '\'' || c == '"') && (i == 0 || content[i-1] != '\\')) {
      if (!in_string) {
        in_string = true;
        string_char = c;
      } else if (c == string_char) {
        in_string = false;
      }
      continue;
    }

    if (in_string) continue;

    if (c == '[') bracket_count++;
    else if (c == ']') bracket_count--;
    else if (c == '(') paren_count++;
    else if (c == ')') paren_count--;
    else if (c == ',' && bracket_count == 0 && paren_count == 0) {
      comma_count++;
    }
  }

  // Number of elements = comma_count + 1
  return comma_count + 1;
}

std::vector<std::string> QueryPreprocessor::extractElements(const std::string& content) {
  std::vector<std::string> elements;

  int bracket_count = 0;
  int paren_count = 0;
  bool in_string = false;
  char string_char = '\0';
  size_t element_start = 0;

  for (size_t i = 0; i < content.size(); ++i) {
    char c = content[i];

    // Handle string literals
    if ((c == '\'' || c == '"') && (i == 0 || content[i-1] != '\\')) {
      if (!in_string) {
        in_string = true;
        string_char = c;
      } else if (c == string_char) {
        in_string = false;
      }
      continue;
    }

    if (in_string) continue;

    if (c == '[') bracket_count++;
    else if (c == ']') bracket_count--;
    else if (c == '(') paren_count++;
    else if (c == ')') paren_count--;
    else if (c == ',' && bracket_count == 0 && paren_count == 0) {
      // Found a delimiter
      std::string element = content.substr(element_start, i - element_start);
      // Trim whitespace
      size_t start = element.find_first_not_of(" \t\n\r\f\v");
      size_t end = element.find_last_not_of(" \t\n\r\f\v");
      if (start != std::string::npos && end != std::string::npos) {
        elements.push_back(element.substr(start, end - start + 1));
      }
      element_start = i + 1;
    }
  }

  // Add the last element
  if (element_start < content.size()) {
    std::string element = content.substr(element_start);
    size_t start = element.find_first_not_of(" \t\n\r\f\v");
    size_t end = element.find_last_not_of(" \t\n\r\f\v");
    if (start != std::string::npos && end != std::string::npos) {
      elements.push_back(element.substr(start, end - start + 1));
    }
  }

  return elements;
}
