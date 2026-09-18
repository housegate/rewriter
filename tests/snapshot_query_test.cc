#include "rewriter-server.h"
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <cstdlib>
#include <fstream>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/message_differencer.h>
#include <gtest/gtest.h>
#include <iomanip>
#include <openssl/sha.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
constexpr auto kRedQ = "0x1111111111111111111111111111111111111111111111111111111111111111";
constexpr auto kQSlot = "${QUERY_PROFILE_ID}";
using Object = Poco::JSON::Object::Ptr;
std::string Encode(const Object &object) {
  std::ostringstream out;
  object->stringify(out);
  return out.str();
}
void Resolve(rewriter::AnalyzeSnapshotQueryRequest &m) {
  if (m.query_profile_id() == kQSlot) m.set_query_profile_id(kRedQ);
}
void Resolve(rewriter::AnalyzeSnapshotQueryResponse &m) {
  if (m.query_profile_id() == kQSlot) m.set_query_profile_id(kRedQ);
}
void Resolve(rewriter::PrepareSnapshotQueryRequest &m) {
  if (m.has_analysis()) Resolve(*m.mutable_analysis());
}
void Resolve(rewriter::PrepareSnapshotQueryResponse &m) {
  if (m.query_profile_id() == kQSlot) m.set_query_profile_id(kRedQ);
}
template <class Message> Message Decode(const Object &object) {
  Message out;
  auto status = google::protobuf::util::JsonStringToMessage(Encode(object), &out);
  if (!status.ok()) throw std::runtime_error(status.ToString());
  Resolve(out);
  return out;
}
void Keys(
  const Object &o, std::set<std::string> required, const std::set<std::string> &optional = {}) {
  for (auto it = o->begin(); it != o->end(); ++it) {
    if (!required.erase(it->first) && !optional.contains(it->first))
      throw std::runtime_error("unknown corpus key: " + it->first);
  }
  if (!required.empty()) throw std::runtime_error("missing corpus key: " + *required.begin());
}
Poco::JSON::Array::Ptr Load() {
  const char *path = std::getenv("SNAPSHOT_QUERY_CORPUS");
  std::ifstream input(path ? path : REWRITER_TEST_DATA_DIR "/snapshot_query_cases.json");
  if (!input) throw std::runtime_error("cannot open snapshot corpus");
  std::ostringstream inputBytes;
  inputBytes << input.rdbuf();
  std::ifstream frozenInput(REWRITER_TEST_DATA_DIR "/snapshot_query_cases.json");
  std::ostringstream frozenBytes;
  frozenBytes << frozenInput.rdbuf();
  auto frozen = frozenBytes.str();
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(frozen.data()), frozen.size(), hash);
  std::ostringstream hex;
  for (auto byte : hash) hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
  if (hex.str() != "3d7c1f1077b1ac6c1bd73d7703b135e95c92bde10d1ce4393c8cb091cca1c933")
    throw std::runtime_error("frozen snapshot source corpus changed");
  Poco::JSON::Parser parser;
  auto root = parser.parse(inputBytes.str()).extract<Object>();
  if (path) {
    const char *profile = std::getenv("SNAPSHOT_QUERY_PROFILE_FILE");
    if (!profile) throw std::runtime_error("resolved corpus requires actual measured profile");
    std::ifstream profileInput(profile);
    std::ostringstream profileBytes;
    profileBytes << profileInput.rdbuf();
    auto entries = snapshot_profiles::Decode(profileBytes.str());
    if (entries.size() != 1) throw std::runtime_error("corpus requires one measured profile");
    std::string needle = "\"" + std::string(kQSlot) + "\"",
                replacement = "\"" + entries[0].id + "\"";
    size_t pos = 0, count = 0;
    while ((pos = frozen.find(needle, pos)) != std::string::npos) {
      frozen.replace(pos, needle.size(), replacement);
      pos += replacement.size();
      ++count;
    }
    if (count != 1519) throw std::runtime_error("frozen corpus identity slot count changed");
    Poco::JSON::Parser expectedParser;
    auto expected = expectedParser.parse(frozen).extract<Object>();
    if (Encode(expected) != Encode(root))
      throw std::runtime_error("resolved corpus changed outside measured identity slots");
  }

  Keys(root, {"schema_version", "purpose", "cases"});
  if (root->getValue<int>("schema_version") != 1 || root->getValue<std::string>("purpose").empty())
    throw std::runtime_error("invalid corpus metadata");
  auto cases = root->getArray("cases");
  if (!cases || cases->size() == 0) throw std::runtime_error("empty corpus");
  size_t preparations = 0;
  std::set<std::string> names;
  for (size_t i = 0; i < cases->size(); ++i) {
    auto c = cases->getObject(i);
    Keys(c,
      {"name", "request", "expected_response"},
      {"prepare_request", "expected_prepare_response"});
    auto name = c->getValue<std::string>("name");
    if (name.empty() || !names.insert(name).second)
      throw std::runtime_error("empty/duplicate case name");
    if (c->has("prepare_request")) ++preparations;
    if (c->has("prepare_request") != c->has("expected_prepare_response"))
      throw std::runtime_error("unpaired preparation");
  }
  if (cases->size() != 636 || preparations != 130)
    throw std::runtime_error("frozen corpus counts changed");
  return cases;
}
class SnapshotCase : public testing::Test {
  Object c;

public:
  explicit SnapshotCase(Object value) : c(std::move(value)) {}
  void TestBody() override {
    auto request = Decode<rewriter::AnalyzeSnapshotQueryRequest>(c->getObject("request"));
    auto expected =
      Decode<rewriter::AnalyzeSnapshotQueryResponse>(c->getObject("expected_response"));
    static const auto profiles = [] {
      const char *path = std::getenv("SNAPSHOT_QUERY_PROFILE_FILE");
      if (!path) throw std::runtime_error("snapshot corpus requires externally measured profile");
      return snapshot_profiles::Profiles::Load(path);
    }();
    RewriterServiceImpl service(profiles);
    rewriter::AnalyzeSnapshotQueryResponse actual;
    service.doAnalyzeSnapshotQuery(&request, &actual);
    EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(expected, actual))
      << "want: " << expected.DebugString() << "got: " << actual.DebugString();
    if (c->has("prepare_request")) {
      auto p = Decode<rewriter::PrepareSnapshotQueryRequest>(c->getObject("prepare_request"));
      auto w =
        Decode<rewriter::PrepareSnapshotQueryResponse>(c->getObject("expected_prepare_response"));
      rewriter::PrepareSnapshotQueryResponse got;
      service.doPrepareSnapshotQuery(&p, &got);
      EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(w, got))
        << "want prepare: " << w.DebugString() << "got: " << got.DebugString();
    }
  }
};
// Every JSON case is a real named GoogleTest in the normal ctest target.
const bool registered = [] {
  auto cases = Load();
  for (size_t i = 0; i < cases->size(); ++i) {
    auto c = cases->getObject(i);
    auto name = c->getValue<std::string>("name");
    testing::RegisterTest(
      "SnapshotQuery", name.c_str(), nullptr, nullptr, __FILE__, __LINE__, [c]() -> SnapshotCase * {
        return new SnapshotCase(c);
      });
  }
  return true;
}();
TEST(SnapshotQueryHarness, ExactComparisonRejectsInvalidClassification) {
  rewriter::AnalyzeSnapshotQueryResponse want;
  want.set_contract_version(1);
  want.set_query_profile_id(kRedQ);
  want.set_code(rewriter::NOT_SNAPSHOT_QUERY);
  for (int mutation = 0; mutation < 10; ++mutation) {
    SCOPED_TRACE(mutation);
    auto got = want;
    switch (mutation) {
    case 0: got.set_contract_version(0); break;
    case 1: got.set_contract_version(2); break;
    case 2: got.clear_query_profile_id(); break;
    case 3: got.set_query_profile_id("wrong"); break;
    case 4: got.set_code(static_cast<rewriter::SnapshotQueryCode>(127)); break;
    case 5: got.set_code(rewriter::UNSPECIFIED); break;
    case 6: got.set_sql_after_materialization("SELECT 7"); break;
    case 7: got.set_target_table_id("unexpected"); break;
    case 8: got.add_target_columns("value"); break;
    case 9: got.add_read_table_ids("unexpected"); break;
    }
    EXPECT_FALSE(google::protobuf::util::MessageDifferencer::Equals(want, got));
  }
}
} // namespace
