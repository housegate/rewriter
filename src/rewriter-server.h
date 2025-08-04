#pragma once

#include <IO/WriteBufferFromString.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/IdentifierQuotingStyle.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>
#include <grpc++/grpc++.h>

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

class RewriterServiceImpl final : public RewriterService::Service {
public:
  Status Rewrite(ServerContext *context,
    const RewriteSQLRequest *request,
    RewriteSQLResponse *response) override;

private:
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