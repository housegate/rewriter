#pragma once

#include <IO/WriteBufferFromString.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/IdentifierQuotingStyle.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>
#include <grpc++/grpc++.h>

#include <iostream>
#include <memory>
#include <string>

#include "ASTTransformers.h"
#include "rewriter.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using rewriter::RewriteErrorMessageRequest;
using rewriter::RewriteErrorMessageResponse;
using rewriter::RewriterService;
using rewriter::RewriteSQLRequest;
using rewriter::RewriteSQLResponse;

// Query preprocessing constants to prevent stack overflow
namespace QueryLimits {
constexpr size_t IN_CLAUSE_SPLIT_THRESHOLD = 50;  // 超过这个值就拆分 IN 子句
constexpr size_t IN_CLAUSE_BATCH_SIZE = 50;        // 拆分后每批的大小
} // namespace QueryLimits

// Query preprocessor to handle large IN clauses by splitting them
// This prevents stack overflow in the ClickHouse SQL parser
class QueryPreprocessor {
public:
  // Preprocess a SQL query to split large IN clauses
  // Returns the rewritten SQL or the original SQL if no rewriting is needed
  static std::string preprocess(const std::string& sql);
  
private:
  struct INClauseInfo {
    size_t start_pos;        // 在 SQL 中的起始位置
    size_t end_pos;          // 在 SQL 中的结束位置
    std::string full_match;  // 完整的匹配内容
    std::string prefix;      // IN 之前的部分，如 "coin_type IN" 或 "(owner_address, coin_type) IN"
    std::string content;     // IN 子句的内容（括号或方括号内的部分）
    char bracket_type;       // '(' 或 '['
    size_t element_count;    // 估算的元素数量
  };
  
  // Find all large IN clauses in the query
  static std::vector<INClauseInfo> findLargeINClauses(const std::string& sql);
  
  // Split a large IN clause into multiple smaller ones connected by OR
  static std::string splitINClause(const INClauseInfo& info);
  
  // Estimate the number of elements in an IN clause content
  static size_t estimateElementCount(const std::string& content);
  
  // Extract elements from IN clause content
  static std::vector<std::string> extractElements(const std::string& content);
};

class RewriterServiceImpl final : public RewriterService::Service {
public:
  Status Rewrite(ServerContext *context,
    const RewriteSQLRequest *request,
    RewriteSQLResponse *response) override;

  Status RewriteErrorMessage(ServerContext *context,
    const RewriteErrorMessageRequest *request,
    RewriteErrorMessageResponse *response) override;

private:
  DB::ASTPtr rewriteSQLImpl(
    const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options, DB::ASTPtr ast);

  void doTableNameRewrite(DB::ASTPtr ast,
    const std::map<std::string, std::string> &table_name_map_,
    const std::map<std::string, DB::RemoteTable> &remote_table_map_,
    const std::map<std::string, DB::TableWithDatabase> &table_with_database_map_);

public:
  Status RewriteForTest(
    ServerContext *context, const RewriteSQLRequest *request, RewriteSQLResponse *response) {
    return Rewrite(context, request, response);
  }
};