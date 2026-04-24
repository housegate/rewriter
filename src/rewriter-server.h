#pragma once

#include <IO/WriteBufferFromString.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTShowTablesQuery.h>
#include <Parsers/ASTUseQuery.h>
#include <Parsers/IdentifierQuotingStyle.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>
#include <grpc++/grpc++.h>

#include <iostream>
#include <memory>
#include <string>

#include "ASTTransformers.h"
#include "query_preprocessor.h"
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

class RewriterServiceImpl final : public RewriterService::Service {
public:
  // gRPC entrypoints — thin wrappers over the protocol-agnostic do* methods.
  Status Rewrite(ServerContext *context,
    const RewriteSQLRequest *request,
    RewriteSQLResponse *response) override;

  Status RewriteErrorMessage(ServerContext *context,
    const RewriteErrorMessageRequest *request,
    RewriteErrorMessageResponse *response) override;

  // Protocol-agnostic dispatch. The HTTP façade (rewriter-http-server.cc)
  // delegates straight to these; the gRPC wrappers above do the same and just
  // synthesize a Status::OK. No failure paths — errors are reported inside
  // `response` via the `RewriteCode` field.
  void doRewrite(
    const RewriteSQLRequest *request, RewriteSQLResponse *response);
  void doRewriteErrorMessage(
    const RewriteErrorMessageRequest *request, RewriteErrorMessageResponse *response);

  Status RewriteForTest(
    ServerContext *context, const RewriteSQLRequest *request, RewriteSQLResponse *response) {
    return Rewrite(context, request, response);
  }
};