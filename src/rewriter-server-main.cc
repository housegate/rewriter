// Thin process entrypoint. Profile validation and complete executable
// measurement must finish before either shared-service listener is exposed.
#include "snapshot_query_profiles.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

void RunServer(uint16_t grpc_port, uint16_t http_port,
               std::shared_ptr<const snapshot_profiles::Profiles> profiles);

int main(int argc, char **argv) {
  try {
    std::vector<std::string> ports;
    std::string profile_path;
    bool configured = false;
    const std::string flag = "--snapshot-query-profile-file";
    for (int i = 1; i < argc; ++i) {
      std::string arg(argv[i]);
      if (arg == flag || arg.starts_with(flag + "=")) {
        if (configured) throw std::runtime_error("duplicate snapshot profile option");
        configured = true;
        if (arg == flag) {
          if (++i == argc || std::string(argv[i]).starts_with("--"))
            throw std::runtime_error("snapshot profile option requires a file");
          profile_path = argv[i];
        } else profile_path = arg.substr(flag.size() + 1);
        if (profile_path.empty()) throw std::runtime_error("snapshot profile option requires a file");
      } else if (arg.starts_with("--")) {
        throw std::runtime_error("unknown option: " + arg);
      } else ports.push_back(std::move(arg));
    }
    // Keep legacy positional port/default semantics, including invalid numeric
    // fallback. Named configuration never reaches atoi.
    uint16_t grpc_port = 50051;
    if (!ports.empty()) {
      int parsed = std::atoi(ports[0].c_str());
      if (parsed > 0 && parsed <= 65535) grpc_port = static_cast<uint16_t>(parsed);
    }
    uint16_t http_port = static_cast<uint16_t>(grpc_port + 1);
    if (ports.size() > 1) {
      int parsed = std::atoi(ports[1].c_str());
      if (parsed > 0 && parsed <= 65535) http_port = static_cast<uint16_t>(parsed);
    }
    auto profiles = configured ? snapshot_profiles::Profiles::Load(profile_path) : nullptr;
    if (profiles) std::cerr << "Snapshot profile identity candidates: " << profiles->CandidateCount() << '\n';
    RunServer(grpc_port, http_port, std::move(profiles));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Rewriter startup refused: " << error.what() << '\n';
    return 1;
  }
}
