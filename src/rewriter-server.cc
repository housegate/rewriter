#include "rewriter-server.h"
#include <IO/WriteBufferFromString.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/IdentifierQuotingStyle.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>
#include <grpc++/grpc++.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "ASTTransformers.h"
#include "rewriter.grpc.pb.h"

#include <s2/s2testing.h>

namespace {
// Strip ANSI color codes: \x1b[ ... m
inline std::string stripAnsi(std::string s) {
  static const std::regex kAnsi("\x1b\\[[0-9;]*m");
  return std::regex_replace(s, kAnsi, "");
}
// Collapse all runs of whitespace to a single space
inline std::string collapseWs(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  bool in_ws = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
      if (!in_ws) {
        out.push_back(' ');
        in_ws = true;
      }
    } else {
      out.push_back(c);
      in_ws = false;
    }
  }
  return out;
}
// Fast literal escaper: avoid std::regex overhead per character
inline void regexEscapeAppend(char c, std::string &out) {
  switch (c) {
  case '.':
  case '^':
  case '$':
  case '|':
  case '(':
  case ')':
  case '\\':
  case '[':
  case ']':
  case '*':
  case '+':
  case '?':
  case '{':
  case '}':
    out.push_back('\\');
    out.push_back(c);
    break;
  default: out.push_back(c);
  }
}

inline std::string regexEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() * 2);
  for (unsigned char ch : s) { regexEscapeAppend(static_cast<char>(ch), out); }
  return out;
}
// Build a flexible regex that matches the SQL regardless of whitespace runs.
inline std::regex makeFlexibleSqlRegex(const std::string &sql) {
  std::string out;
  out.reserve(sql.size() * 2);
  bool in_ws = false;
  for (unsigned char c : sql) {
    switch (c) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '\v':
      if (!in_ws) {
        out.append("\\s+");
        in_ws = true;
      }
      break;
    default: regexEscapeAppend(static_cast<char>(c), out); in_ws = false;
    }
  }
  return std::regex(out, std::regex::ECMAScript | std::regex::icase | std::regex::optimize);
}
// Replace all occurrences of `from` (as a regex) with `to` in `s`
inline void regexReplaceAll(std::string &s, const std::regex &re, const std::string &to) {
  s = std::regex_replace(s, re, to);
}
// ECMAScript in std::regex does not support lookbehind, so we use a captured prefix + lookahead and
// must keep $1 in replacements. Build a boundary-aware regex for an identifier, optionally quoted
// with backticks.
inline std::regex makeIdentRegex(const std::string &ident) {
  std::string esc = regexEscape(ident);
  // (^|[^A-Za-z0-9_])`?IDENT`?(?=[^A-Za-z0-9_]|$)
  std::string pat = "(^|[^A-Za-z0-9_])`?" + esc + "`?(?=[^A-Za-z0-9_]|$)";
  return std::regex(pat, std::regex::ECMAScript | std::regex::icase);
}
} // namespace

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using rewriter::RewriterService;
using rewriter::RewriteSQLRequest;
using rewriter::RewriteSQLResponse;

#ifndef GIT_COMMIT_HASH
#define GIT_COMMIT_HASH "?"
#endif

grpc::Status RewriterServiceImpl::Rewrite(
  ServerContext *context, const RewriteSQLRequest *request, RewriteSQLResponse *response) {
  String original_query = request->sql();
  std::cerr << "=== [PREPROCESSOR] BEFORE: SQL length=" << original_query.size() << " ===" << std::endl;
  std::cerr.flush();
  
  // Preprocess the query to split large IN clauses
  String query = QueryPreprocessor::preprocess(original_query);
  std::cerr << "=== [PREPROCESSOR] AFTER: SQL length=" << query.size() << " ===" << std::endl;
  std::cerr.flush();
  
  const char *pos = query.data();
  const char *end = pos + query.size();

  DB::ParserQuery parser(end, false);
  try {
    DB::ASTPtr ast = parseQuery(parser, pos, end, 0, 0, 0);

    DB::IdentifierQuotingRule identifier_quoting_rule_ = DB::IdentifierQuotingRule::WhenNecessary;
    DB::IdentifierQuotingStyle identifier_quoting_style_ = DB::IdentifierQuotingStyle::Backticks;

    // ===== USE statement handling =====
    if (auto *use_query = ast->as<DB::ASTUseQuery>()) {
      String db_name = use_query->getDatabase();

      // Look up DatabaseNameRewrite option for database name mapping
      for (const auto &option : request->options()) {
        if (option.op() == rewriter::RewriteOp::DatabaseNameRewrite) {
          const auto &db_map = option.database_name_args().database_map();
          auto it = db_map.find(db_name);
          if (it != db_map.end()) {
            // Build rewritten USE SQL with physical database name
            String result_sql = "USE " + it->second;
            response->set_sql_after_rewrite(result_sql);
            response->set_statement_type("USE");
            response->set_message("success");
            response->set_code(rewriter::RewriteCode::Success);
            return Status::OK;
          }
        }
      }
      // No mapping found, return original SQL
      String result_sql = ast->formatWithPossiblyHidingSensitiveData(
        0, true, true, false, identifier_quoting_rule_, identifier_quoting_style_);
      response->set_sql_after_rewrite(result_sql);
      response->set_statement_type("USE");
      response->set_message("success");
      response->set_code(rewriter::RewriteCode::Success);
      return Status::OK;
    }

    // ===== SHOW TABLES statement handling =====
    if (auto *show_query = ast->as<DB::ASTShowTablesQuery>()) {
      // Look up ShowTablesRewrite option for processor prefix filtering
      for (const auto &option : request->options()) {
        if (option.op() == rewriter::RewriteOp::ShowTablesRewrite) {
          const auto &args = option.show_tables_args();
          String database = args.database();
          String processor_id = args.processor_id();

          if (!database.empty() && !processor_id.empty()) {
            // Rewrite to processor-prefix-filtered SELECT
            // Escape single quotes in database and processor_id to prevent SQL injection
            auto escapeSql = [](const String &s) {
              String result;
              result.reserve(s.size());
              for (char c : s) {
                if (c == '\'') result += "''";
                else result += c;
              }
              return result;
            };
            auto escapeLike = [](const String &s) {
              String result;
              result.reserve(s.size() * 3);
              for (char c : s) {
                if (c == '\\' || c == '%' || c == '_') {
                  result += "\\\\";
                }
                result += c;
              }
              return result;
            };
            String result_sql = "SELECT name FROM system.tables WHERE database = '"
              + escapeSql(database) + "' AND name LIKE '"
              + escapeSql(escapeLike(processor_id)) + "%'";


            response->set_sql_after_rewrite(result_sql);
            response->set_statement_type("SHOW_TABLES");
            response->set_message("success");
            response->set_code(rewriter::RewriteCode::Success);
            return Status::OK;
          }
        }
      }
      // No rewrite option, return original SQL
      String result_sql = ast->formatWithPossiblyHidingSensitiveData(
        0, true, true, false, identifier_quoting_rule_, identifier_quoting_style_);
      response->set_sql_after_rewrite(result_sql);
      response->set_statement_type("SHOW_TABLES");
      response->set_message("success");
      response->set_code(rewriter::RewriteCode::Success);
      return Status::OK;
    }

    // ===== SELECT / other DML statement handling (original path) =====
    std::cout << "before transformation " << ast->dumpTree() << std::endl;

    std::unordered_set<std::string> original_accessed_table_names_set_;
    DB::collectTableNamesFromAST(ast, original_accessed_table_names_set_);

    // Pre-scan CTE to collect the original table names accessed in the CTE subquery (without altering the AST).
    for (const auto &option : request->options()) {
      if (option.op() == rewriter::RewriteOp::CommonTableExprRewrite) {
        std::map<std::string, DB::CommonTableExpr> cte_map_;
        auto ctes = option.common_table_expr_args().cte_map();
        cte_map_.clear();
        for (auto &kv : ctes) {
          cte_map_[kv.first] = {
            kv.second.rewriter::RewriteCommonTableExprArgs_CommonTableExpr::alias(),
            kv.second.rewriter::RewriteCommonTableExprArgs_CommonTableExpr::sql()};
        }
        auto cte_ast_map = DB::parseCTEMapToAST(cte_map_);
        for (const auto &kv : cte_ast_map) {
          if (auto it = original_accessed_table_names_set_.find(kv.first);
            it != original_accessed_table_names_set_.end()) {
            original_accessed_table_names_set_.erase(it);
          }
          DB::collectTableNamesFromAST(kv.second, original_accessed_table_names_set_);
        }
      }
    }

    ast = rewriteSQLImpl(request->options(), ast);

    std::string result_sql = ast->formatWithPossiblyHidingSensitiveData(
      0, true, true, false, identifier_quoting_rule_, identifier_quoting_style_);

    for (const auto &name : original_accessed_table_names_set_) {
      response->add_original_accessed_table_names(name);
    }
    response->set_sql_after_rewrite(result_sql);
    response->set_statement_type("SELECT");
    response->set_message("success");
    response->set_code(rewriter::RewriteCode::Success);
    return Status::OK;
  } catch (const DB::Exception &e) {
    response->set_sql_after_rewrite("Error parsing query");
    response->set_message(e.displayText());
    response->set_code(rewriter::RewriteCode::SyntaxError);
    return Status::OK;
  }
}

DB::ASTPtr RewriterServiceImpl::rewriteSQLImpl(
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options, DB::ASTPtr ast) {

  std::map<std::string, DB::CommonTableExpr> cte_map_;
  std::map<std::string, std::string> table_name_map_;
  std::map<std::string, DB::RemoteTable> remote_table_map_;
  std::map<std::string, DB::TableWithDatabase> table_with_database_map_;
  bool has_table_name_written = false;

  for (const auto &option : options) {
    switch (option.op()) {
    case rewriter::RewriteOp::LimitRewrite: {
      auto limit = option.limit_args().replace_limit().replace_to();
      auto force_limit = option.limit_args().force_limit();
      auto threshold = option.limit_args().replace_limit().threshold();
      DB::ASTRewriteLimitTransformer limit_transformer = DB::ASTRewriteLimitTransformer();
      if (force_limit) {
        limit_transformer.setLimit(force_limit, true);
      } else {
        limit_transformer.setLimit(limit);
        limit_transformer.setThreshold(threshold);
      }
      limit_transformer.transform(ast);
      std::cout << "after limit transformation " << ast->dumpTree() << std::endl;
      break;
    }
    case rewriter::RewriteOp::TableNameRewrite: {
      // table_name
      {
        auto table_name_map = option.rewriter::RewriteOption::table_name_args()
                                .rewriter::RewriteTableNameArgs::table_map();
        table_name_map_.clear();
        for (auto &kv : table_name_map) { table_name_map_[kv.first] = kv.second; }
      }
      // remote_table
      {
        auto remote_table_map = option.rewriter::RewriteOption::table_name_args()
                                  .rewriter::RewriteTableNameArgs::remote_table_map();
        remote_table_map_.clear();
        for (auto &kv : remote_table_map) {
          DB::RemoteTable remote_table;
          remote_table.addr = kv.second.rewriter::RewriteTableNameArgs_RemoteTable::addr();
          remote_table.database_name =
            kv.second.rewriter::RewriteTableNameArgs_RemoteTable::database();
          remote_table.table_name = kv.second.rewriter::RewriteTableNameArgs_RemoteTable::table();
          remote_table.user_name = kv.second.rewriter::RewriteTableNameArgs_RemoteTable::user();
          remote_table.password = kv.second.rewriter::RewriteTableNameArgs_RemoteTable::password();
          remote_table_map_[kv.first] = remote_table;
        }
      }
      // table_with_database
      {
        auto table_with_database_map = option.rewriter::RewriteOption::table_name_args()
                                         .rewriter::RewriteTableNameArgs::table_with_database_map();
        table_with_database_map_.clear();
        for (auto &kv : table_with_database_map) {
          DB::TableWithDatabase table_with_database;
          table_with_database.database_name =
            kv.second.rewriter::RewriteTableNameArgs_TableWithDatabase::database();
          table_with_database.table_name =
            kv.second.rewriter::RewriteTableNameArgs_TableWithDatabase::table();
          table_with_database_map_[kv.first] = table_with_database;
        }
      }
      doTableNameRewrite(ast, table_name_map_, remote_table_map_, table_with_database_map_);
      has_table_name_written = true;
      break;
    }
    case rewriter::RewriteOp::OffsetRewrite: {
      auto offset = option.offset_args().offset();
      DB::ASTRewriteOffsetTransformer offset_transformer = DB::ASTRewriteOffsetTransformer();
      offset_transformer.setOffset(offset);
      offset_transformer.transform(ast);
      std::cout << "after offset transformation " << ast->dumpTree() << std::endl;
      break;
    }
    case rewriter::RewriteOp::SettingsRewrite: {
      auto settings = option.settings_args().settings();
      DB::ASTSettingsTransformer settings_transformer = DB::ASTSettingsTransformer();
      std::vector<DB::Setting> settings_;
      for (const auto &setting : settings) {
        DB::Setting s;
        s.name = setting.key();
        s.value_type = setting.type();
        switch (setting.type()) {
        case rewriter::SettingType::String: {
          s.string_value = setting.string_value();
          break;
        }
        case rewriter::SettingType::Int: {
          s.int_value = setting.int_value();
          break;
        }
        case rewriter::SettingType::Bool: {
          s.bool_value = setting.bool_value();
          break;
        }
        case rewriter::SettingType::Uint64: {
          s.uint64_value = setting.uint64_value();
          break;
        }
        default: break;
        }
        settings_.push_back(s);
      }
      settings_transformer.setSettings(settings_);
      settings_transformer.transform(ast);
      std::cout << "after settings transformation " << ast->dumpTree() << std::endl;
      break;
    }
    case rewriter::RewriteOp::CommonTableExprRewrite: {
      DB::ASTRewriteCTETransformer cte_transformer = DB::ASTRewriteCTETransformer();
      auto ctes = option.common_table_expr_args().cte_map();
      cte_map_.clear();
      for (auto &kv : ctes) {
        cte_map_[kv.first] = {
          kv.second.rewriter::RewriteCommonTableExprArgs_CommonTableExpr::alias(),
          kv.second.rewriter::RewriteCommonTableExprArgs_CommonTableExpr::sql()};
      }
      auto cte_ast_map = DB::parseCTEMapToAST(cte_map_);

      cte_transformer.setCTEASTMap(cte_ast_map);
      cte_transformer.setCTEMap(cte_map_);
      cte_transformer.transform(ast);

      // If the table name has been rewritten previously, apply it again after the CTE rewrite.
      if (has_table_name_written) {
        doTableNameRewrite(ast, table_name_map_, remote_table_map_, table_with_database_map_);
      }
      break;
    }
    default: break;
    }
  }

  return std::move(ast);
}

grpc::Status RewriterServiceImpl::RewriteErrorMessage(ServerContext *context,
  const RewriteErrorMessageRequest *request,
  RewriteErrorMessageResponse *response) {

  auto error_message = request->error_message();
  if (error_message.empty()) {
    response->set_message("Error message is empty");
    response->set_code(rewriter::Success);
    return Status::OK;
  }

  // Step 1: parse original SQL to AST
  String query = request->sql();
  const char *pos = query.data();
  const char *end = pos + query.size();

  DB::ParserQuery parser(end, /*allow_alias_without_as*/ false);
  try {
    DB::ASTPtr ast = parseQuery(parser, pos, end, 0, 0, 0);

    String origin_sql_without_space = ast->formatWithSecretsOneLine();

    // Step 2: rewrite the AST
    ast = rewriteSQLImpl(request->options(), ast);

    // Step 3: format the AST back to SQL
    String rewritten_sql = ast->formatForErrorMessage();

    // --- Step 4: perform replacements in error_message ---
    // Use the raw `error_message` and `sql` without stripping ANSI or normalizing whitespace.
    std::string err = error_message;

    // 4.1 Replace the entire SQL if present (exact first, then flexible whitespace-insensitive)
    {
      auto p = err.find(rewritten_sql);
      if (p != std::string::npos) {
        err.replace(p, rewritten_sql.size(), origin_sql_without_space);
      } else {
        std::regex flex = makeFlexibleSqlRegex(rewritten_sql);
        regexReplaceAll(err, flex, origin_sql_without_space);
      }
    }

    // 4.2 Replace rewritten table names back to original names (boundary-aware)
    // Invert table_map from options (original -> rewritten) to (rewritten -> original)
    for (const auto &opt : request->options()) {
      if (opt.op() != rewriter::RewriteOp::TableNameRewrite) continue;
      const auto &m = opt.table_name_args().table_map();
      for (const auto &kv : m) {
        const std::string &original = kv.first;
        const std::string &rewritten = kv.second;
        if (rewritten.empty() || original.empty()) continue;

        // Replace with boundary-aware regex, allow optional backticks
        std::regex re_ident = makeIdentRegex(rewritten);
        err = std::regex_replace(err, re_ident, std::string("$1") + original);
      }
    }

    // Write back into response
    response->set_error_after_rewrite(err);
    response->set_message("success");
    response->set_code(rewriter::Success);

    return Status::OK;
  } catch (const DB::Exception &e) {
    response->set_error_after_rewrite("Error when fine tune error message");
    response->set_message(e.displayText());
    response->set_code(rewriter::SyntaxError);
    return Status::OK;
  }
}

void RewriterServiceImpl::doTableNameRewrite(DB::ASTPtr ast,
  const std::map<std::string, std::string> &table_name_map_,
  const std::map<std::string, DB::RemoteTable> &remote_table_map_,
  const std::map<std::string, DB::TableWithDatabase> &table_with_database_map_) {
  DB::ASTReplaceTransformer transformer = DB::ASTReplaceTransformer();
  transformer.setTableNameMap(table_name_map_);
  transformer.setRemoteTableMap(remote_table_map_);
  transformer.setTableWithDatabaseMap(table_with_database_map_);
  transformer.transform(ast);
  std::cout << "after table name transformation " << ast->dumpTree() << std::endl;
}

// Define a constant for the version
const std::string VERSION = "0.1.18";
const std::string hash = GIT_COMMIT_HASH;

void RunServer(uint16_t port) {
  // Print the version information
  std::cout << "Rewriter Server Version: " << VERSION << " (" << hash << ")" << std::endl;

  std::string server_address = "0.0.0.0:" + std::to_string(port);
  RewriterServiceImpl service;

  grpc::EnableDefaultHealthCheckService(true);
  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;

  server->Wait();
}

// ============================================================================
// QueryPreprocessor Implementation
// ============================================================================

std::string QueryPreprocessor::preprocess(const std::string& sql) {
  std::cerr << "[QueryPreprocessor] Starting preprocessing, SQL length: " << sql.size() << std::endl;
  std::cerr.flush();
  
  // Find all large IN clauses
  auto large_in_clauses = findLargeINClauses(sql);
  
  if (large_in_clauses.empty()) {
    std::cerr << "[QueryPreprocessor] No large IN clauses found, returning original SQL" << std::endl;
    std::cerr.flush();
    return sql;
  }
  
  std::cerr << "[QueryPreprocessor] Found " << large_in_clauses.size() << " large IN clause(s)" << std::endl;
  std::cerr.flush();
  
  // Replace large IN clauses from back to front to maintain positions
  std::string result = sql;
  for (auto it = large_in_clauses.rbegin(); it != large_in_clauses.rend(); ++it) {
    const auto& info = *it;
    std::cerr << "[QueryPreprocessor] Splitting IN clause at pos " << info.start_pos 
              << " with ~" << info.element_count << " elements" << std::endl;
    std::cerr.flush();
    
    std::string replacement = splitINClause(info);
    result.replace(info.start_pos, info.end_pos - info.start_pos, replacement);
  }
  
  std::cerr << "[QueryPreprocessor] Preprocessing complete, new SQL length: " << result.size() << std::endl;
  std::cerr.flush();
  return result;
}

std::vector<QueryPreprocessor::INClauseInfo> QueryPreprocessor::findLargeINClauses(const std::string& sql) {
  std::vector<INClauseInfo> result;
  
  // 正则表达式匹配 IN 子句
  // 支持: column IN [...], (col1, col2) IN [...], column IN (...), (col1, col2) IN (...)
  // 更精确的正则：匹配单个标识符或括号内的标识符列表
  std::regex in_pattern(
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
  
  std::cerr << "[QueryPreprocessor] Extracted " << elements.size() << " elements from IN clause" << std::endl;
  std::cerr.flush();
  
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

#if REWRITER_RELEASE_MAIN
int main(int argc, char **argv) {
  if (argc > 1) {
    // Convert the first command line argument to an integer for the port
    int port = std::atoi(argv[1]);
    RunServer(port);
  } else {
    // Use default port 50051
    RunServer(50051);
  }
  return 0;
}
#endif