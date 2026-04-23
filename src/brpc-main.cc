// Separate entrypoint for the brpc POC binary. The existing gRPC binary
// (`rewriter-server.cc`'s REWRITER_RELEASE_MAIN block) is unchanged.

#include <cstdlib>
#include <iostream>

#include "rewriter-brpc-server.h"

int main(int argc, char **argv) {
  uint16_t port = 50051;
  if (argc > 1) {
    int parsed = std::atoi(argv[1]);
    if (parsed > 0 && parsed <= 65535) port = static_cast<uint16_t>(parsed);
  }
  rewriter_brpc::RunBrpcServer(port);
  return 0;
}
