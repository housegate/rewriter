#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace snapshot_profiles {
// The decoder has no host measurement input. These values are availability
// witnesses, not admission policy or proof of implemented snapshot semantics.
struct Entry {
  std::string id;
  std::string record;
  std::string native_set;
  std::string grpc_digest;
};
std::vector<Entry> Decode(std::string_view canonical_json);
std::string CanonicalDigest(std::string_view domain, std::string_view canonical_json);

class Profiles final {
public:
  // Measures the actual running ELF internally; there is no expected-self-hash
  // parameter or environment override. Explicit configuration requires a match.
  static std::shared_ptr<const Profiles> Load(const std::string &path);
  const std::string *FindCandidate(const std::string &id) const;
  size_t CandidateCount() const { return candidates_.size(); }
  const std::string &ExecutableDigest() const { return executable_digest_; }
private:
  Profiles(std::string digest, std::map<std::string, std::string> candidates)
      : executable_digest_(std::move(digest)), candidates_(std::move(candidates)) {}
  const std::string executable_digest_;
  const std::map<std::string, std::string> candidates_;
};
} // namespace snapshot_profiles
