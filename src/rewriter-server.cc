#include "rewriter-server.h"
#include <IO/WriteBufferFromString.h>
#include <Parsers/ASTAlterQuery.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTDeleteQuery.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTUpdateQuery.h>
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
#include "rewriter-http-server.h"
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

void RewriterServiceImpl::doRewrite(
  const RewriteSQLRequest *request, RewriteSQLResponse *response) {
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

      // Look up DatabaseNameRewrite option for database name mapping.
      // Three branches, matching the proxy's regex semantics. Priority:
      //   1. database_map hit  → rewrite to mapped physical DB
      //   2. known_physical_databases hit → leave SQL unchanged
      //   3. default_database non-empty  → rewrite to default, report use_stmt_value.specified_database
      for (const auto &option : request->options()) {
        if (option.op() != rewriter::RewriteOp::DatabaseNameRewrite) continue;

        const auto &args = option.database_name_args();
        const auto &db_map = args.database_map();

        auto it = db_map.find(db_name);
        if (it != db_map.end()) {
          // Case 1: processorID → physical DB
          String result_sql = "USE " + it->second;
          response->set_sql_after_rewrite(result_sql);
          response->set_statement_type("USE");
          response->set_message("success");
          response->set_code(rewriter::RewriteCode::Success);
          return;
        }

        // Case 2: target is already a known physical database — no rewrite.
        bool is_known_physical = false;
        for (const auto &known : args.known_physical_databases()) {
          if (known == db_name) {
            is_known_physical = true;
            break;
          }
        }
        if (is_known_physical) {
          String result_sql = ast->formatWithPossiblyHidingSensitiveData(
            0, true, true, false, identifier_quoting_rule_, identifier_quoting_style_);
          response->set_sql_after_rewrite(result_sql);
          response->set_statement_type("USE");
          response->set_message("success");
          response->set_code(rewriter::RewriteCode::Success);
          return;
        }

        // Case 3: fall back to default database; report the original target
        // name in use_stmt_value.specified_database so the caller can track it
        // on the connection.
        const String &default_db = args.default_database();
        if (!default_db.empty()) {
          String result_sql = "USE " + default_db;
          response->set_sql_after_rewrite(result_sql);
          response->set_statement_type("USE");
          response->mutable_use_stmt_value()->set_specified_database(db_name);
          response->set_message("success");
          response->set_code(rewriter::RewriteCode::Success);
          return;
        }
      }
      // No option matched, return original SQL
      String result_sql = ast->formatWithPossiblyHidingSensitiveData(
        0, true, true, false, identifier_quoting_rule_, identifier_quoting_style_);
      response->set_sql_after_rewrite(result_sql);
      response->set_statement_type("USE");
      response->set_message("success");
      response->set_code(rewriter::RewriteCode::Success);
      return;
    }

    // ===== SHOW TABLES statement handling =====
    if (auto *show_query = ast->as<DB::ASTShowTablesQuery>()) {
      // SHOW DATABASES and other SHOW variants: leave untouched.
      if (show_query->databases || show_query->clusters || show_query->cluster
          || show_query->dictionaries || show_query->m_settings || show_query->merges
          || show_query->caches) {
        String result_sql = ast->formatWithPossiblyHidingSensitiveData(
          0, true, true, false, identifier_quoting_rule_, identifier_quoting_style_);
        response->set_sql_after_rewrite(result_sql);
        response->set_statement_type("SHOW_TABLES");
        response->set_message("success");
        response->set_code(rewriter::RewriteCode::Success);
        return;
      }

      // Extract "FROM <db>" from the AST (backticks already stripped by
      // parser). Under the new semantics this is treated as a *logical* name,
      // i.e. the table-name prefix — not a physical DB name.
      String from_logical = show_query->getFrom();
      // getFrom() returns "database.table" or "database"; keep the leading part.
      if (auto dot = from_logical.find('.'); dot != String::npos) {
        from_logical = from_logical.substr(0, dot);
      }

      // Look up ShowTablesRewrite option.
      for (const auto &option : request->options()) {
        if (option.op() != rewriter::RewriteOp::ShowTablesRewrite) continue;

        const auto &args = option.show_tables_args();
        const String &physical = args.physical_database();
        // logical := SHOW TABLES FROM-clause, else args.logical_database.
        String logical = from_logical.empty() ? args.logical_database() : from_logical;

        // Empty logical → passthrough (let the caller decide).
        if (logical.empty()) {
          String result_sql = ast->formatWithPossiblyHidingSensitiveData(
            0, true, true, false, identifier_quoting_rule_, identifier_quoting_style_);
          response->set_sql_after_rewrite(result_sql);
          response->set_statement_type("SHOW_TABLES");
          response->mutable_show_stmt_value()->set_show_tables_passthrough(true);
          response->set_message("success");
          response->set_code(rewriter::RewriteCode::Success);
          return;
        }

        auto escapeSql = [](const String &s) {
          String result;
          result.reserve(s.size());
          for (char c : s) {
            if (c == '\'') result += "''";
            else result += c;
          }
          return result;
        };

        // Full rewrite: outer multiIf strips the logical prefix, inner subquery
        // filters system.tables by database = physical AND startsWith(name, logical).
        // The subquery avoids alias shadowing of the outer "name" column.
        String result_sql =
          "SELECT multiIf("
            "startsWith(name, '" + escapeSql(logical) + ".'), "
              "substring(name, length('" + escapeSql(logical) + ".') + 1), "
            "startsWith(name, '" + escapeSql(logical) + "'), "
              "substring(name, length('" + escapeSql(logical) + "') + 1), "
            "name) AS name "
          "FROM (SELECT name FROM system.tables "
                "WHERE database = '" + escapeSql(physical) + "' "
                  "AND startsWith(name, '" + escapeSql(logical) + "'))";

        response->set_sql_after_rewrite(result_sql);
        response->set_statement_type("SHOW_TABLES");
        response->set_message("success");
        response->set_code(rewriter::RewriteCode::Success);
        return;
      }
      // No rewrite option, return original SQL
      String result_sql = ast->formatWithPossiblyHidingSensitiveData(
        0, true, true, false, identifier_quoting_rule_, identifier_quoting_style_);
      response->set_sql_after_rewrite(result_sql);
      response->set_statement_type("SHOW_TABLES");
      response->set_message("success");
      response->set_code(rewriter::RewriteCode::Success);
      return;
    }

    // ===== Single-table write statement handling =====
    // Supported: CREATE TABLE, DROP TABLE, ALTER TABLE, INSERT, UPDATE, DELETE.
    // Writes are parsed for classification only — no rewrite-option pipeline
    // is applied. sql_after_rewrite is the normalized original SQL.
    // write_stmt_value.target_table carries the single target;
    // original_accessed_table_names stays empty for writes. Unsupported
    // variants return RewriteError.
    auto qualifyTable = [](const String &db, const String &table) -> String {
      return db.empty() ? table : db + "." + table;
    };
    auto acceptWrite = [&](const String &stmt_type, const String &target) {
      String result_sql = ast->formatWithPossiblyHidingSensitiveData(
        0, true, true, false, identifier_quoting_rule_, identifier_quoting_style_);
      response->set_sql_after_rewrite(result_sql);
      response->set_statement_type(stmt_type);
      response->mutable_write_stmt_value()->set_target_table(target);
      response->set_message("success");
      response->set_code(rewriter::RewriteCode::Success);
    };
    auto rejectWrite = [&](const String &msg) {
      response->set_message(msg);
      response->set_code(rewriter::RewriteCode::RewriteError);
    };

    if (auto *create_query = ast->as<DB::ASTCreateQuery>()) {
      if (create_query->is_ordinary_view || create_query->is_materialized_view
          || create_query->is_live_view || create_query->is_window_view) {
        rejectWrite("CREATE VIEW is not supported; only CREATE TABLE is allowed");
        return;
      }
      if (create_query->is_dictionary) {
        rejectWrite("CREATE DICTIONARY is not supported");
        return;
      }
      if (!create_query->table) {
        rejectWrite("CREATE DATABASE is not supported; only CREATE TABLE is allowed");
        return;
      }
      if (create_query->as_table_function) {
        rejectWrite("CREATE TABLE AS table_function(...) is not supported");
        return;
      }
      acceptWrite("CREATE_TABLE",
        qualifyTable(create_query->getDatabase(), create_query->getTable()));
      return;
    }

    if (auto *drop_query = ast->as<DB::ASTDropQuery>()) {
      if (drop_query->kind == DB::ASTDropQuery::Kind::Truncate) {
        rejectWrite("TRUNCATE is not supported");
        return;
      }
      if (drop_query->kind == DB::ASTDropQuery::Kind::Detach) {
        rejectWrite("DETACH is not supported");
        return;
      }
      if (drop_query->is_view || drop_query->is_dictionary) {
        rejectWrite("DROP VIEW/DICTIONARY is not supported; only DROP TABLE is allowed");
        return;
      }
      if (drop_query->database_and_tables) {
        rejectWrite("multi-table DROP is not supported");
        return;
      }
      if (!drop_query->table) {
        rejectWrite("DROP DATABASE is not supported; only DROP TABLE is allowed");
        return;
      }
      acceptWrite("DROP_TABLE",
        qualifyTable(drop_query->getDatabase(), drop_query->getTable()));
      return;
    }

    if (auto *alter_query = ast->as<DB::ASTAlterQuery>()) {
      if (alter_query->alter_object != DB::ASTAlterQuery::AlterObjectType::TABLE) {
        rejectWrite("only ALTER TABLE is supported");
        return;
      }
      acceptWrite("ALTER_TABLE",
        qualifyTable(alter_query->getDatabase(), alter_query->getTable()));
      return;
    }

    if (auto *insert_query = ast->as<DB::ASTInsertQuery>()) {
      if (insert_query->table_function) {
        rejectWrite("INSERT INTO FUNCTION(...) is not supported");
        return;
      }
      if (!insert_query->table) {
        rejectWrite("INSERT target table is missing");
        return;
      }
      acceptWrite("INSERT",
        qualifyTable(insert_query->getDatabase(), insert_query->getTable()));
      return;
    }

    if (auto *update_query = ast->as<DB::ASTUpdateQuery>()) {
      acceptWrite("UPDATE",
        qualifyTable(update_query->getDatabase(), update_query->getTable()));
      return;
    }

    if (auto *delete_query = ast->as<DB::ASTDeleteQuery>()) {
      acceptWrite("DELETE",
        qualifyTable(delete_query->getDatabase(), delete_query->getTable()));
      return;
    }

    // Reject other write-ish statement kinds that would otherwise pass
    // silently through the SELECT fallthrough below.
    switch (ast->getQueryKind()) {
      case DB::IAST::QueryKind::Rename:
        rejectWrite("RENAME is not supported");
        return;
      case DB::IAST::QueryKind::Optimize:
        rejectWrite("OPTIMIZE is not supported");
        return;
      case DB::IAST::QueryKind::Undrop:
        rejectWrite("UNDROP is not supported");
        return;
      case DB::IAST::QueryKind::Grant:
      case DB::IAST::QueryKind::Revoke:
        rejectWrite("GRANT/REVOKE is not supported");
        return;
      case DB::IAST::QueryKind::Move:
        rejectWrite("MOVE is not supported");
        return;
      case DB::IAST::QueryKind::Backup:
      case DB::IAST::QueryKind::Restore:
        rejectWrite("BACKUP/RESTORE is not supported");
        return;
      case DB::IAST::QueryKind::KillQuery:
        rejectWrite("KILL is not supported");
        return;
      case DB::IAST::QueryKind::ExternalDDL:
        rejectWrite("external DDL is not supported");
        return;
      case DB::IAST::QueryKind::AsyncInsertFlush:
        rejectWrite("async insert flush is not supported");
        return;
      case DB::IAST::QueryKind::Copy:
        rejectWrite("COPY is not supported");
        return;
      default:
        break;  // fall through to SELECT path
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
    return;
  } catch (const DB::Exception &e) {
    response->set_sql_after_rewrite("Error parsing query");
    response->set_message(e.displayText());
    response->set_code(rewriter::RewriteCode::SyntaxError);
    return;
  }
}

// gRPC thin wrapper — delegates to the protocol-agnostic doRewrite.
grpc::Status RewriterServiceImpl::Rewrite(
  ServerContext * /*context*/, const RewriteSQLRequest *request, RewriteSQLResponse *response) {
  doRewrite(request, response);
  return grpc::Status::OK;
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

void RewriterServiceImpl::doRewriteErrorMessage(
  const RewriteErrorMessageRequest *request,
  RewriteErrorMessageResponse *response) {

  auto error_message = request->error_message();
  if (error_message.empty()) {
    response->set_message("Error message is empty");
    response->set_code(rewriter::Success);
    return;
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

    return;
  } catch (const DB::Exception &e) {
    response->set_error_after_rewrite("Error when fine tune error message");
    response->set_message(e.displayText());
    response->set_code(rewriter::SyntaxError);
    return;
  }
}

// gRPC thin wrapper — delegates to doRewriteErrorMessage.
grpc::Status RewriterServiceImpl::RewriteErrorMessage(
  ServerContext * /*context*/,
  const RewriteErrorMessageRequest *request,
  RewriteErrorMessageResponse *response) {
  doRewriteErrorMessage(request, response);
  return grpc::Status::OK;
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

void RunServer(uint16_t grpc_port, uint16_t http_port) {
  // Print the version information
  std::cout << "Rewriter Server Version: " << VERSION << " (" << hash << ")" << std::endl;

  std::string server_address = "0.0.0.0:" + std::to_string(grpc_port);
  RewriterServiceImpl service;

  // HTTP+JSON façade — same `service` instance, different protocol. Starts a
  // Poco HTTPServer in its own thread pool and returns immediately so the
  // gRPC Wait() below remains the process-blocking call.
  rewriter_http::RunHttpServer(http_port, service);

  grpc::EnableDefaultHealthCheckService(true);
  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << " (gRPC)" << std::endl;

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
