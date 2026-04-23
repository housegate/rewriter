#pragma once

// HTTP+JSON façade in front of the same RewriterServiceImpl that serves gRPC.
// Implemented on top of Poco::Net (already linked transitively via
// `clickhouse_common_io`), so no new dependency.
//
// Routes (POST, JSON body matching the proto message):
//   /Rewrite                                       → RewriterService.Rewrite
//   /RewriteErrorMessage                           → RewriterService.RewriteErrorMessage
//   /rewriter.RewriterService/Rewrite              ↑ gRPC-style alias
//   /rewriter.RewriterService/RewriteErrorMessage  ↑
//   GET /healthz                                   → 200 "ok"

#include <cstdint>

namespace DB { class IAST; }
class RewriterServiceImpl;

namespace rewriter_http {

// Starts a Poco HTTPServer on 0.0.0.0:<port> using `core` as the dispatch
// backend. Returns immediately — the HTTPServer owns its own thread pool and
// keeps running until the process exits. The server object itself is held in
// a function-local static so it outlives the call.
//
// `core` MUST outlive the process (typically a stack instance in main()).
void RunHttpServer(uint16_t port, RewriterServiceImpl &core);

} // namespace rewriter_http
