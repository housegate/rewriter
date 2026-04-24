// src/rewriter-server-main.cc
//
// Thin entry point for the `clickhouse_housegate_rewriter` executable.
// All real logic lives in rewriter-server.cc's RunServer(); this file just
// parses argv. Split out of rewriter-server.cc so the shared code can live
// in an OBJECT library linked by both the server and the CLI.
#include <cstdint>
#include <cstdlib>

// Declared in rewriter-server.cc
void RunServer(uint16_t grpc_port, uint16_t http_port);

int main(int argc, char **argv) {
  // argv[1]: gRPC port (default 50051)
  // argv[2]: HTTP port (default = grpc_port + 1)
  uint16_t grpc_port = 50051;
  if (argc > 1) {
    int parsed = std::atoi(argv[1]);
    if (parsed > 0 && parsed <= 65535) grpc_port = static_cast<uint16_t>(parsed);
  }
  uint16_t http_port = static_cast<uint16_t>(grpc_port + 1);
  if (argc > 2) {
    int parsed = std::atoi(argv[2]);
    if (parsed > 0 && parsed <= 65535) http_port = static_cast<uint16_t>(parsed);
  }
  RunServer(grpc_port, http_port);
  return 0;
}
