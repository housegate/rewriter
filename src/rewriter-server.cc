#include <IO/WriteBufferFromString.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/IdentifierQuotingStyle.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>
#include <grpc++/grpc++.h>
#include "rewriter-server.h"

#include <iostream>
#include <memory>

#include "ASTTransformers.h"
#include "rewriter.grpc.pb.h"

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

grpc::Status RewriterServiceImpl::Rewrite(ServerContext *context,
  const RewriteSQLRequest *request,
  RewriteSQLResponse *response) {
  String query = request->sql();
  const char *pos = query.data();
  const char *end = pos + query.size();

  DB::ParserQuery parser(end, false);
  try {
    DB::ASTPtr ast = parseQuery(parser, pos, end, 0, 0, 0);
    std::cout << "before transformation " << ast->dumpTree() << std::endl;

    std::map<std::string, DB::CommonTableExpr> cte_map_;
    std::map<std::string, std::string> table_name_map_;
    std::map<std::string, DB::RemoteTable> remote_table_map_;
    std::map<std::string, DB::TableWithDatabase> table_with_database_map_;
    bool has_table_name_written = false;

    // TODO: 把取req map的逻辑和应用transform的逻辑分离, 需判断options是否会存在多个同一op
    for (auto &option : request->options()) {
      switch (option.op()) {
        case rewriter::RewriteOp::LimitRewrite: {
          auto limit = option.limit_args().replace_limit().replace_to();
          auto force_limit = option.limit_args().force_limit();
          auto threshold = option.limit_args().replace_limit().threshold();
          DB::ASTRewriteLimitTransformer limit_transformer =
            DB::ASTRewriteLimitTransformer();
          if (force_limit) {
            limit_transformer.setLimit(force_limit, true);
          } else {
            limit_transformer.setLimit(limit);
            limit_transformer.setThreshold(threshold);
          }
          limit_transformer.transform(ast);
          std::cout << "after limit transformation " << ast->dumpTree()
            << std::endl;
          break;
        }
        case rewriter::RewriteOp::TableNameRewrite: {
          // table_name
          {
            auto table_name_map = option.rewriter::RewriteOption::table_name_args().
                                         rewriter::RewriteTableNameArgs::table_map();
            table_name_map_.clear();
            for (auto &kv : table_name_map) {
              table_name_map_[kv.first] = kv.second;
            }
          }
          // remote_table
          {
            auto remote_table_map = option.rewriter::RewriteOption::table_name_args().
                                           rewriter::RewriteTableNameArgs::remote_table_map();
            remote_table_map_.clear();
            for (auto &kv : remote_table_map) {
              DB::RemoteTable remote_table;
              remote_table.addr = kv.second.rewriter::RewriteTableNameArgs_RemoteTable::addr();
              remote_table.database_name = kv.second.
                                              rewriter::RewriteTableNameArgs_RemoteTable::database();
              remote_table.table_name = kv.second.
                                           rewriter::RewriteTableNameArgs_RemoteTable::table();
              remote_table.user_name = kv.second.
                                          rewriter::RewriteTableNameArgs_RemoteTable::user();
              remote_table.password = kv.second.
                                         rewriter::RewriteTableNameArgs_RemoteTable::password();
              remote_table_map_[kv.first] = remote_table;
            }
          }
          // table_with_database
          {
            auto table_with_database_map =
              option.rewriter::RewriteOption::table_name_args().
                     rewriter::RewriteTableNameArgs::table_with_database_map();
            table_with_database_map_.clear();
            for (auto &kv : table_with_database_map) {
              DB::TableWithDatabase table_with_database;
              table_with_database.database_name = kv.second.
                                                     rewriter::RewriteTableNameArgs_TableWithDatabase::database();
              table_with_database.table_name = kv.second.
                                                  rewriter::RewriteTableNameArgs_TableWithDatabase::table();
              table_with_database_map_[kv.first] = table_with_database;
            }
          }
          doTableNameRewrite(ast, table_name_map_, remote_table_map_, table_with_database_map_);
          has_table_name_written = true;
          break;
        }
        case rewriter::RewriteOp::OffsetRewrite: {
          auto offset = option.offset_args().offset();
          DB::ASTRewriteOffsetTransformer offset_transformer =
            DB::ASTRewriteOffsetTransformer();
          offset_transformer.setOffset(offset);
          offset_transformer.transform(ast);
          std::cout << "after offset transformation " << ast->dumpTree()
            << std::endl;
          break;
        }
        case rewriter::RewriteOp::SettingsRewrite: {
          auto settings = option.settings_args().settings();
          DB::ASTSettingsTransformer settings_transformer =
            DB::ASTSettingsTransformer();
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
              default:
                break;
            }
            settings_.push_back(s);
          }
          settings_transformer.setSettings(settings_);
          settings_transformer.transform(ast);
          std::cout << "after settings transformation " << ast->dumpTree()
            << std::endl;
          break;
        }
        case rewriter::RewriteOp::CommonTableExprRewrite: {
          DB::ASTRewriteCTETransformer cte_transformer = DB::ASTRewriteCTETransformer();
          auto ctes = option.common_table_expr_args().cte_map();
          cte_map_.clear();
          for (auto &kv : ctes) {
            cte_map_[kv.first] = {
              kv.second.rewriter::RewriteCommonTableExprArgs_CommonTableExpr::alias(),
              kv.second.rewriter::RewriteCommonTableExprArgs_CommonTableExpr::sql()
            };
          }
          cte_transformer.setCTEMap(cte_map_);
          cte_transformer.transform(ast);
          if (has_table_name_written) {
            doTableNameRewrite(ast, table_name_map_, remote_table_map_, table_with_database_map_);
          }
          break;
        }
        default:
          break;
      }
    }

    DB::IdentifierQuotingRule identifier_quoting_rule_ =
      DB::IdentifierQuotingRule::WhenNecessary;
    DB::IdentifierQuotingStyle identifier_quoting_style_ =
      DB::IdentifierQuotingStyle::Backticks;
    std::string result_sql = ast->formatWithPossiblyHidingSensitiveData(
      0,
      true,
      true,
      false,
      identifier_quoting_rule_,
      identifier_quoting_style_);

    response->set_sql_after_rewrite(result_sql);
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

void RewriterServiceImpl::doTableNameRewrite(DB::ASTPtr ast,
  const std::map<std::string, std::string> &table_name_map_,
  const std::map<std::string, DB::RemoteTable> &remote_table_map_,
  const std::map<std::string, DB::TableWithDatabase> &table_with_database_map_) {
  DB::ASTReplaceTransformer transformer = DB::ASTReplaceTransformer();
  transformer.setTableNameMap(table_name_map_);
  transformer.setRemoteTableMap(remote_table_map_);
  transformer.setTableWithDatabaseMap(table_with_database_map_);
  transformer.transform(ast);
  std::cout << "after table name transformation " << ast->dumpTree()
    << std::endl;
}


// Define a constant for the version
const std::string VERSION = "0.1.14";
const std::string hash = GIT_COMMIT_HASH;

void RunServer(uint16_t port) {
  // Print the version information
  std::cout << "Rewriter Server Version: " << VERSION << " (" << hash << ")"
    << std::endl;

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