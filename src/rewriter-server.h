#pragma once

#include <IO/WriteBufferFromString.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTShowTablesQuery.h>
#include <Parsers/ASTUseQuery.h>
#include <Parsers/IdentifierQuotingStyle.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>
#include <grpc++/grpc++.h>

#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include "ASTTransformers.h"
#include "snapshot_query_profiles.h"
#include "handlers/snapshot_query.h"
#include "rewriter.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using rewriter::OptimizeRequest;
using rewriter::OptimizeResponse;
using rewriter::RewriteErrorMessageRequest;
using rewriter::RewriteErrorMessageResponse;
using rewriter::RewriterService;
using rewriter::MaterializeSQLRequest;
using rewriter::MaterializeSQLResponse;
using rewriter::RewriteSQLRequest;
using rewriter::RewriteSQLResponse;

class RewriterServiceImpl final : public RewriterService::Service {
public:
  RewriterServiceImpl() = default;
  explicit RewriterServiceImpl(std::shared_ptr<const snapshot_profiles::Profiles> profiles)
      : snapshot_profiles_(std::move(profiles)) {}
  const snapshot_profiles::Profiles *SnapshotCandidates() const { return snapshot_profiles_.get(); }
  Status AnalyzeSnapshotQuery(ServerContext *, const rewriter::AnalyzeSnapshotQueryRequest *, rewriter::AnalyzeSnapshotQueryResponse *) override;
  Status PrepareSnapshotQuery(ServerContext *, const rewriter::PrepareSnapshotQueryRequest *, rewriter::PrepareSnapshotQueryResponse *) override;
  void doAnalyzeSnapshotQuery(const rewriter::AnalyzeSnapshotQueryRequest *, rewriter::AnalyzeSnapshotQueryResponse *, snapshot_query::Execution = {});
  void doPrepareSnapshotQuery(const rewriter::PrepareSnapshotQueryRequest *, rewriter::PrepareSnapshotQueryResponse *, snapshot_query::Execution = {});
  // gRPC entrypoints — thin wrappers over the protocol-agnostic do* methods.
  Status MaterializeSQL(ServerContext *context,
    const MaterializeSQLRequest *request,
    MaterializeSQLResponse *response) override;

  Status Rewrite(ServerContext *context,
    const RewriteSQLRequest *request,
    RewriteSQLResponse *response) override;

  Status RewriteErrorMessage(ServerContext *context,
    const RewriteErrorMessageRequest *request,
    RewriteErrorMessageResponse *response) override;

  Status Optimize(ServerContext *context,
    const OptimizeRequest *request,
    OptimizeResponse *response) override;

  // Protocol-agnostic dispatch. The HTTP façade (rewriter-http-server.cc)
  // delegates straight to these; the gRPC wrappers above do the same and just
  // synthesize a Status::OK. No failure paths — errors are reported inside
  // `response` via the `RewriteCode` / `OptimizeCode` / `MaterializeCode` field.
  void doMaterializeSQL(
    const MaterializeSQLRequest *request, MaterializeSQLResponse *response);
  void doRewrite(
    const RewriteSQLRequest *request, RewriteSQLResponse *response);
  // `cancelled`, when set, reports that the caller gave up; the error text is
  // then withheld rather than finished.
  void doRewriteErrorMessage(
    const RewriteErrorMessageRequest *request, RewriteErrorMessageResponse *response,
    std::function<bool()> cancelled = {});
  void doOptimize(
    const OptimizeRequest *request, OptimizeResponse *response);

  Status RewriteForTest(
    ServerContext *context, const RewriteSQLRequest *request, RewriteSQLResponse *response) {
    return Rewrite(context, request, response);
  }
private:
  // Shared by both protocol facades. Immutable identity candidates acknowledge
  // semantics only after the independent closed-profile support check.
  const std::shared_ptr<const snapshot_profiles::Profiles> snapshot_profiles_;
};
