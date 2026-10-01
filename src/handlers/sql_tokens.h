// Lexical helpers shared by the table-reference preflight and the
// system-table collector: ClickHouse's own Lexer, with whitespace and
// comments dropped.
#pragma once

#include <Parsers/Lexer.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace rewriter_handlers {

// The significant tokens of `text` (whitespace and comments dropped: they do
// not separate a qualified name or a keyword for ClickHouse). false on a lexer
// error; `tokens` then holds the tokens before it, which a caller must not
// trust.
inline bool significantTokens(std::string_view text, std::vector<DB::Token> &tokens) {
  DB::Lexer lexer(text.data(), text.data() + text.size());
  for (DB::Token token = lexer.nextToken(); !token.isEnd(); token = lexer.nextToken()) {
    if (token.isError()) return false;
    if (token.isSignificant()) tokens.push_back(token);
  }
  return true;
}

inline std::string lowerName(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

inline std::string upperWord(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
}

// Whether `token` is the bare word `word` (given in lower case), in any case.
inline bool wordIs(const DB::Token &token, std::string_view word) {
  if (token.type != DB::TokenType::BareWord || token.size() != word.size()) return false;
  for (size_t i = 0; i < word.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(token.begin[i])) != word[i]) return false;
  return true;
}

// tokens[at] upper-cased when it is a bare word, else "".
inline std::string bareWordUpper(const std::vector<DB::Token> &tokens, size_t at) {
  return at < tokens.size() && tokens[at].type == DB::TokenType::BareWord
    ? upperWord(std::string(tokens[at].begin, tokens[at].end)) : std::string();
}

// Whether `text` holds the bare word `word` (lower case) as a token (outside
// strings, quoted names and comments). A text that does not tokenize counts
// as holding it, so a caller refusing on it fails closed.
inline bool textHasWord(std::string_view text, std::string_view word) {
  std::vector<DB::Token> tokens;
  if (!significantTokens(text, tokens)) return true;
  for (const auto &token : tokens)
    if (wordIs(token, word)) return true;
  return false;
}

} // namespace rewriter_handlers
