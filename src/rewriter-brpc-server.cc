#include "rewriter-brpc-server.h"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <brpc/server.h>

#include <iostream>

namespace rewriter_brpc {

void RewriterBrpcServiceImpl::Rewrite(
  google::protobuf::RpcController * /*cntl*/,
  const rewriter::RewriteSQLRequest *request,
  rewriter::RewriteSQLResponse *response,
  google::protobuf::Closure *done) {
  brpc::ClosureGuard done_guard(done);
  core_.doRewrite(request, response);
}

void RewriterBrpcServiceImpl::RewriteErrorMessage(
  google::protobuf::RpcController * /*cntl*/,
  const rewriter::RewriteErrorMessageRequest *request,
  rewriter::RewriteErrorMessageResponse *response,
  google::protobuf::Closure *done) {
  brpc::ClosureGuard done_guard(done);
  core_.doRewriteErrorMessage(request, response);
}

void RunBrpcServer(uint16_t port) {
  brpc::Server server;
  RewriterBrpcServiceImpl service;

  // SERVER_DOESNT_OWN_SERVICE: we keep ownership on the stack; brpc must not
  // delete it on shutdown.
  if (server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
    std::cerr << "[brpc] Failed to add RewriterService" << std::endl;
    return;
  }

  brpc::ServerOptions options;
  // Default is enough for a POC; tune max_concurrency / num_threads later.

  if (server.Start(port, &options) != 0) {
    std::cerr << "[brpc] Failed to start server on port " << port << std::endl;
    return;
  }

  std::cout << "[brpc] RewriterService listening on 0.0.0.0:" << port
            << " (gRPC + HTTP+JSON + HTTP+protobuf + baidu_std)" << std::endl;

  server.RunUntilAskedToQuit();
}

} // namespace rewriter_brpc
