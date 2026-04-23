#pragma once

// brpc-side service implementation. Derives from the plain protobuf-generated
// `rewriter::RewriterService` (in rewriter.pb.h) — NOT the gRPC variant
// (`RewriterService::Service` in rewriter.grpc.pb.h).
//
// brpc speaks baidu_std, HTTP+JSON, HTTP+protobuf, and gRPC on the same port,
// so migrating from grpc++ to brpc keeps existing gRPC clients working while
// unlocking HTTP callers.

#include <google/protobuf/service.h>
#include "rewriter-server.h"
#include "rewriter.pb.h"

namespace rewriter_brpc {

class RewriterBrpcServiceImpl : public rewriter::RewriterService {
public:
  void Rewrite(
    google::protobuf::RpcController *cntl,
    const rewriter::RewriteSQLRequest *request,
    rewriter::RewriteSQLResponse *response,
    google::protobuf::Closure *done) override;

  void RewriteErrorMessage(
    google::protobuf::RpcController *cntl,
    const rewriter::RewriteErrorMessageRequest *request,
    rewriter::RewriteErrorMessageResponse *response,
    google::protobuf::Closure *done) override;

private:
  // Shared dispatch — same object the gRPC server uses. Rewrite/Rewrite* are
  // stateless apart from parse caches so this is safe to share across calls.
  RewriterServiceImpl core_;
};

// Starts a brpc::Server on 0.0.0.0:<port> with the multi-protocol stack
// enabled. Blocks until the server stops.
void RunBrpcServer(uint16_t port);

} // namespace rewriter_brpc
