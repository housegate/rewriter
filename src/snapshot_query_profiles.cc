#include "snapshot_query_profiles.h"
#include "snapshot_executable_identity.h"

#include <openssl/sha.h>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace snapshot_profiles {
namespace {
[[noreturn]] void Bad(const char *message) {
  throw std::runtime_error(std::string("snapshot profiles: ") + message);
}
bool Space(uint32_t c) {
  return (c >= 9 && c <= 13) || c == 32 || c == 0x85 || c == 0xa0 ||
      c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 ||
      c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}
// Consume precisely Go encoding/json's canonical string language. In particular
// HTML characters and U+2028/U+2029 must be escaped, slash must not, and UTF-8
// must be shortest-form scalar values. No lossy parse/re-encode or key map.
class Decoder {
public:
  explicit Decoder(std::string_view raw) : raw_(raw) {}
  void Token(std::string_view t) {
    if (raw_.substr(pos_, t.size()) != t) Bad("noncanonical or incomplete JSON");
    pos_ += t.size();
  }
  bool Next(char c) {
    if (pos_ < raw_.size() && raw_[pos_] == c) { ++pos_; return true; }
    return false;
  }
  size_t Position() const { return pos_; }
  std::string Span(size_t start) const { return std::string(raw_.substr(start, pos_ - start)); }
  void End() const { if (pos_ != raw_.size()) Bad("trailing JSON"); }
  uint64_t Number() {
    if (pos_ == raw_.size() || raw_[pos_] < '0' || raw_[pos_] > '9') Bad("expected unsigned integer");
    uint64_t n = 0;
    if (Next('0')) return 0;
    while (pos_ < raw_.size() && raw_[pos_] >= '0' && raw_[pos_] <= '9') {
      unsigned d = raw_[pos_++] - '0';
      if (n > (std::numeric_limits<uint64_t>::max() - d) / 10) Bad("integer overflow");
      n = n * 10 + d;
    }
    return n;
  }
  void Version() { if (Number() != 1) Bad("unsupported version"); }
  std::string String(bool name = false, bool digest = false) {
    Token("\"");
    std::string out;
    uint32_t first = 0, last = 0;
    bool seen = false;
    while (!Next('"')) {
      if (pos_ == raw_.size()) Bad("unterminated string");
      uint32_t c = static_cast<unsigned char>(raw_[pos_++]);
      if (c == '\\') {
        if (pos_ == raw_.size()) Bad("incomplete escape");
        char e = raw_[pos_++];
        switch (e) {
          case '"': c = '"'; break;
          case '\\': c = '\\'; break;
          case 'b': c = 8; break;
          case 'f': c = 12; break;
          case 'n': c = 10; break;
          case 'r': c = 13; break;
          case 't': c = 9; break;
          case 'u': {
            c = 0;
            for (int i = 0; i < 4; ++i) {
              if (pos_ == raw_.size()) Bad("incomplete unicode escape");
              char h = raw_[pos_++];
              if (!(h >= '0' && h <= '9') && !(h >= 'a' && h <= 'f')) Bad("noncanonical unicode escape");
              c = c * 16 + (h <= '9' ? h - '0' : h - 'a' + 10);
            }
            if (!((c < 32 && c != 8 && c != 9 && c != 10 && c != 12 && c != 13) ||
                  c == '<' || c == '>' || c == '&' || c == 0x2028 || c == 0x2029))
              Bad("noncanonical unicode escape");
            break;
          }
          default: Bad("noncanonical escape");
        }
      } else {
        if (c < 32 || c == '<' || c == '>' || c == '&') Bad("unescaped character");
        if (c >= 128) {
          unsigned n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 ? 1 : 0;
          if (!n || c > 0xf4) Bad("invalid UTF-8");
          c &= (1u << (6 - n)) - 1;
          for (unsigned i = 0; i < n; ++i) {
            if (pos_ == raw_.size()) Bad("incomplete UTF-8");
            unsigned char b = raw_[pos_++];
            if ((b & 0xc0) != 0x80) Bad("invalid UTF-8");
            c = (c << 6) | (b & 63);
          }
          if (c < (n == 1 ? 0x80u : n == 2 ? 0x800u : 0x10000u) ||
              c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) || c == 0x2028 || c == 0x2029)
            Bad("noncanonical UTF-8");
        }
      }
      if (!seen) first = c;
      seen = true; last = c;
      if (c < 128) out += static_cast<char>(c);
      else if (c < 0x800) { out += static_cast<char>(0xc0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 63)); }
      else if (c < 0x10000) { out += static_cast<char>(0xe0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 63)); out += static_cast<char>(0x80 | (c & 63)); }
      else { out += static_cast<char>(0xf0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 63)); out += static_cast<char>(0x80 | ((c >> 6) & 63)); out += static_cast<char>(0x80 | (c & 63)); }
    }
    if (name && (!seen || Space(first) || Space(last))) Bad("noncanonical name");
    if (digest) {
      if (out.size() != 66 || out.substr(0, 2) != "0x") Bad("malformed digest");
      for (char c : std::string_view(out).substr(2))
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) Bad("malformed digest");
    }
    return out;
  }
  Entry EntryValue() {
    Entry e;
    Token("{\"query_profile_id\":"); e.id = String(false, true);
    Token(",\"record\":");
    size_t start = Position();
    Token("{\"version\":"); Version();
    Token(",\"clickhouse_build_digest\":"); String(false, true);
    Token(",\"platform\":"); String(true);
    Token(",\"native_analyzer_build_digest\":"); auto native = String(false, true);
    Token(",\"grpc_analyzer_build_digest\":"); e.grpc_digest = String(false, true);
    Token(",\"tzdata_digest\":"); String(false, true);
    Token(",\"settings\":[");
    std::string previous;
    if (!Next(']')) {
      do {
        Token("{\"name\":"); auto name = String(true);
        if (!previous.empty() && previous >= name) Bad("unsorted or duplicate settings");
        previous = name;
        Token(",\"value\":"); String(); Token("}");
      } while (Next(','));
      Token("]");
    }
    Token(",\"scalar_operators\":["); previous.clear();
    if (!Next(']')) {
      do {
        auto name = String(true);
        if (!previous.empty() && previous >= name) Bad("unsorted or duplicate operators");
        previous = name;
      } while (Next(','));
      Token("]");
    }
    Token(",\"column_profile_id\":"); String(true);
    Token(",\"output_order_id\":"); String(true);
    Token(",\"limits\":{");
    bool first = true;
    for (const auto *key : {"max_sql_bytes", "max_descriptor_bytes", "max_output_rows", "max_output_bytes", "max_restore_bytes", "max_sort_memory_bytes", "max_spill_bytes", "max_execution_ms"}) {
      if (!first) Token(","); first = false;
      Token(std::string("\"") + key + "\":");
      if (!Number()) Bad("zero limit");
    }
    Token("}}"); e.record = Span(start);
    Token(",\"native_artifact_set\":"); start = Position();
    Token("{\"version\":"); Version(); Token(",\"artifacts\":[");
    std::tuple<std::string, std::string, std::string> prev;
    first = true;
    do {
      Token("{\"platform\":"); auto platform = String(true);
      Token(",\"executable_digest\":"); auto exe = String(false, true);
      Token(",\"ffi_digest\":"); auto ffi = String(false, true); Token("}");
      auto tuple = std::make_tuple(platform, exe, ffi);
      if (!first && prev >= tuple) Bad("unsorted or duplicate native tuple");
      first = false; prev = std::move(tuple);
    } while (Next(','));
    Token("]}"); e.native_set = Span(start); Token("}");
    if (CanonicalDigest("snapshot-native-analyzer-artifact-set-v1", e.native_set) != native) Bad("native commitment mismatch");
    if (CanonicalDigest("snapshot-query-profile-v1", e.record) != e.id) Bad("query commitment mismatch");
    return e;
  }
private:
  std::string_view raw_;
  size_t pos_ = 0;
};
} // namespace

std::string CanonicalDigest(std::string_view domain, std::string_view json) {
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  const std::string prefix = std::string("housegate-replay-mvp-v0:") + std::string(domain) + '\0';
  SHA256_Update(&ctx, prefix.data(), prefix.size());
  SHA256_Update(&ctx, json.data(), json.size());
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256_Final(digest.data(), &ctx);
  return identity::HexDigest(digest.data(), digest.size());
}
std::vector<Entry> Decode(std::string_view raw) {
  Decoder d(raw);
  d.Token("{\"version\":"); d.Version(); d.Token(",\"profiles\":[");
  std::vector<Entry> entries;
  do {
    auto e = d.EntryValue();
    if (!entries.empty() && entries.back().id >= e.id) Bad("unsorted or duplicate profiles");
    entries.push_back(std::move(e));
  } while (d.Next(','));
  d.Token("]}"); d.End();
  return entries;
}
std::shared_ptr<const Profiles> Profiles::Load(const std::string &path) {
  if (path.empty()) Bad("explicit profile path required");
  auto entries = Decode(identity::ReadProfileFile(path));
  auto digest = identity::MeasureRunningExecutable();
  std::map<std::string, std::string> local;
  for (auto &entry : entries)
    if (entry.grpc_digest == digest) local.emplace(std::move(entry.id), std::move(entry.record));
  if (local.empty()) Bad("no locally measured candidate profile");
  return std::shared_ptr<const Profiles>(new Profiles(std::move(digest), std::move(local)));
}
const std::string *Profiles::FindCandidate(const std::string &id) const {
  auto it = candidates_.find(id);
  return it == candidates_.end() ? nullptr : &it->second;
}
} // namespace snapshot_profiles
