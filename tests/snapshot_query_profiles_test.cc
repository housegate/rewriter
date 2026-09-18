#include "rewriter-server.h"
#include "snapshot_executable_identity.h"
#include "snapshot_query_profiles.h"
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <algorithm>
#include <cstdlib>
#include <elf.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace {
using snapshot_profiles::CanonicalDigest;
using snapshot_profiles::Decode;
using snapshot_profiles::Profiles;
std::string Read(const std::string &path) {
  std::ifstream stream(path);
  std::ostringstream out;
  out << stream.rdbuf();
  return out.str();
}
Poco::JSON::Object::Ptr Fixture() {
  Poco::JSON::Parser parser;
  return parser
    .parse(Read(std::string(REWRITER_TEST_DATA_DIR) + "/snapshot_query_build_identity_v1.json"))
    .extract<Poco::JSON::Object::Ptr>();
}
std::string Raw() {
  return Fixture()->getValue<std::string>("profile_file_json");
}
std::string Replace(std::string s, const std::string &old, const std::string &value) {
  auto pos = s.find(old);
  if (pos == std::string::npos) throw std::runtime_error("test replacement absent");
  s.replace(pos, old.size(), value);
  return s;
}
std::string EntryJSON(const snapshot_profiles::Entry &e) {
  return "{\"query_profile_id\":\"" + e.id + "\",\"record\":" + e.record +
         ",\"native_artifact_set\":" + e.native_set + "}";
}
std::string Bundle(std::vector<snapshot_profiles::Entry> entries) {
  std::sort(entries.begin(), entries.end(), [](auto &a, auto &b) { return a.id < b.id; });
  std::string s = "{\"version\":1,\"profiles\":[";
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i) s += ',';
    s += EntryJSON(entries[i]);
  }
  return s + "]}";
}
void Rehash(snapshot_profiles::Entry &e) {
  e.id = CanonicalDigest("snapshot-query-profile-v1", e.record);
}
class Temp {
public:
  Temp() {
    char path[] = "/tmp/rewriter-profile-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) throw std::runtime_error("mkstemp");
    close(fd);
    name = path;
  }
  ~Temp() {
    unlink(name.c_str());
  }
  void Write(const std::string &s) {
    std::ofstream f(name, std::ios::binary | std::ios::trunc);
    f << s;
  }
  std::string name;
};
size_t ValueEnd(const std::string &s, size_t p) {
  bool quoted = false, escaped = false;
  int depth = 0;
  for (size_t i = p; i < s.size(); ++i) {
    char c = s[i];
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"') {
        quoted = false;
        if (!depth) return i + 1;
      }
      continue;
    }
    if (c == '"')
      quoted = true;
    else if (c == '{' || c == '[')
      ++depth;
    else if (c == '}' || c == ']') {
      if (!depth) return i;
      if (!--depth) return i + 1;
    } else if (!depth && c == ',')
      return i;
  }
  return s.size();
}
} // namespace

TEST(SnapshotProfiles, IndependentLiteralVectors) {
  auto f = Fixture();
  auto entries = Decode(Raw());
  ASSERT_EQ(entries.size(), 2);
  for (size_t i = 0; i < entries.size(); ++i) {
    EXPECT_EQ(
      entries[i].record, f->getArray("query_profile_records_json")->getElement<std::string>(i));
    EXPECT_EQ(
      entries[i].native_set, f->getArray("native_artifact_sets_json")->getElement<std::string>(i));
    EXPECT_EQ(entries[i].id, f->getArray("query_profile_ids")->getElement<std::string>(i));
    EXPECT_EQ(CanonicalDigest("snapshot-native-analyzer-artifact-set-v1", entries[i].native_set),
      f->getArray("native_analyzer_build_digests")->getElement<std::string>(i));
  }
  EXPECT_EQ(Bundle(entries), Raw());
}
TEST(SnapshotProfiles, EveryNestedFieldStrictShape) {
  auto raw = Raw();
  size_t count = 0;
  // Each actual member occurrence is tested, including repeated field names in
  // later records/tuples. Canonical input has no escaped quotes in member keys.
  for (size_t key = 0; key < raw.size(); ++key) {
    if (raw[key] != '"' || key == 0 || (raw[key - 1] != '{' && raw[key - 1] != ',')) continue;
    auto colon = raw.find("\":", key + 1);
    if (colon == std::string::npos) continue;
    auto quote = raw.find('"', key + 1);
    if (quote != colon) continue;
    size_t value = colon + 2, end = ValueEnd(raw, value);
    ++count;
    SCOPED_TRACE(raw.substr(key, colon - key + 1) + " at " + std::to_string(key));
    auto s = raw;
    s.replace(value, end - value, "null");
    EXPECT_THROW(Decode(s), std::runtime_error);
    s = raw;
    s.insert(key, raw.substr(key, end - key) + ',');
    EXPECT_THROW(Decode(s), std::runtime_error);
    s = raw;
    s.insert(key, "\"unknown\":0,");
    EXPECT_THROW(Decode(s), std::runtime_error);
    s = raw;
    if (raw[end] == ',')
      s.erase(key, end - key + 1);
    else
      s.erase(key - 1, end - key + 1);
    EXPECT_THROW(Decode(s), std::runtime_error);
    if (raw[end] == ',' && raw[end + 1] == '"') {
      auto next_colon = raw.find("\":", end + 2);
      auto next_end = ValueEnd(raw, next_colon + 2);
      s = raw;
      s.replace(key,
        next_end - key,
        raw.substr(end + 1, next_end - end - 1) + ',' + raw.substr(key, end - key));
      EXPECT_THROW(Decode(s), std::runtime_error);
    }
  }
  EXPECT_EQ(count, 66);
}
TEST(SnapshotProfiles, CanonicalBytesAndScalarRefusals) {
  auto raw = Raw();
  for (auto s : {raw + "\n",
         " " + raw,
         raw + "{}",
         Replace(raw, "\"version\":1", "\"version\":01"),
         Replace(raw, "\"version\":1", "\"version\":1.0"),
         Replace(raw, "\"version\":1", "\"version\":2"),
         Replace(raw, "65537", "0"),
         Replace(raw, "65537", "-1"),
         Replace(raw, "65537", "18446744073709551616"),
         Replace(raw, "65537", "1e3"),
         Replace(raw, "0xaaaa", "0xAaaa"),
         Replace(raw, "linux/amd64", " linux/amd64"),
         Replace(raw, "linux/amd64", "linux\\/amd64"),
         Replace(raw, "linux/amd64", "\\u006cinux/amd64"),
         Replace(raw, "max_threads", "\\u00a0max_threads"),
         Replace(raw, "max_threads", ""),
         Replace(raw, "[\"column\",\"literal\"]", "[\"literal\",\"column\"]"),
         Replace(raw, "[\"column\",\"literal\"]", "[\"column\",\"column\"]")}) {
    EXPECT_THROW(Decode(s), std::runtime_error) << s.substr(0, 120);
  }
  auto entries = Decode(raw);
  EXPECT_THROW(Decode("{\"version\":1,\"profiles\":[]}"), std::runtime_error);
  EXPECT_THROW(Decode(Bundle({entries[0], entries[0]})), std::runtime_error);
  EXPECT_THROW(Decode("{\"version\":1,\"profiles\":[" + EntryJSON(entries[1]) + ',' +
                      EntryJSON(entries[0]) + "]}"),
    std::runtime_error);
}
TEST(SnapshotProfiles, ValueValidationAfterRehash) {
  auto original = Decode(Raw())[0];
  for (const auto &mutation : std::vector<std::pair<std::string, std::string>>{{"65537", "0"},
         {"linux/amd64", ""},
         {"column-profile-a", " column-profile-a"},
         {"\"column\",\"literal\"", "\"literal\",\"column\""},
         {"\"max_threads\"", "\"result_overflow_mode\""},
         {"\"version\":1", "\"version\":4294967296"}}) {
    auto e = original;
    e.record = Replace(e.record, mutation.first, mutation.second);
    Rehash(e);
    EXPECT_THROW(Decode(Bundle({e})), std::runtime_error);
  }
  auto e = original;
  e.record = Replace(e.record, "65537", "18446744073709551615");
  Rehash(e);
  EXPECT_NO_THROW(Decode(Bundle({e})));
  e = original;
  e.record = Replace(
    e.record, "\"value\":\"1\"", "\"value\":\"\\u003c\\u0026\\u003e\\u2028\\u2029\\u0000\\n😀\"");
  Rehash(e);
  EXPECT_NO_THROW(Decode(Bundle({e})));
  for (const auto &invalid : {"\\u003C", "<", "\\/", "\\u0061", "\\ud800", "\\u00a0"}) {
    auto bad = original;
    bad.record =
      Replace(bad.record, "\"value\":\"1\"", "\"value\":\"" + std::string(invalid) + "\"");
    Rehash(bad);
    EXPECT_THROW(Decode(Bundle({bad})), std::runtime_error);
  }
}
TEST(SnapshotProfiles, NativeWitnessAndCommitments) {
  auto e = Decode(Raw())[0];
  auto bad = e;
  bad.id[5] = 'f';
  EXPECT_THROW(Decode(Bundle({bad})), std::runtime_error);
  bad = e;
  bad.native_set = Replace(bad.native_set, "0x1111", "0x9111");
  EXPECT_THROW(Decode(Bundle({bad})), std::runtime_error);
  for (const auto &replacement :
    {"{\"version\":1,\"artifacts\":[]}", "{\"version\":1,\"artifacts\":[null]}"}) {
    bad = e;
    bad.native_set = replacement;
    auto old = CanonicalDigest("snapshot-native-analyzer-artifact-set-v1", e.native_set);
    bad.record = Replace(
      bad.record, old, CanonicalDigest("snapshot-native-analyzer-artifact-set-v1", bad.native_set));
    Rehash(bad);
    EXPECT_THROW(Decode(Bundle({bad})), std::runtime_error);
  }
  auto begin = e.native_set.find('[') + 1;
  auto end = ValueEnd(e.native_set, begin);
  auto first = e.native_set.substr(begin, end - begin),
       second = e.native_set.substr(end + 1, e.native_set.size() - end - 3);
  for (auto members : {first + ',' + first, second + ',' + first}) {
    bad = e;
    bad.native_set = "{\"version\":1,\"artifacts\":[" + members + "]}";
    bad.record = Replace(bad.record,
      CanonicalDigest("snapshot-native-analyzer-artifact-set-v1", e.native_set),
      CanonicalDigest("snapshot-native-analyzer-artifact-set-v1", bad.native_set));
    Rehash(bad);
    EXPECT_THROW(Decode(Bundle({bad})), std::runtime_error);
  }
}
TEST(SnapshotProfiles, OrdinaryConstructionHasNoCapability) {
  RewriterServiceImpl service;
  EXPECT_EQ(service.SnapshotCandidates(), nullptr);
  rewriter::AnalyzeSnapshotQueryRequest req;
  rewriter::AnalyzeSnapshotQueryResponse res;
  service.doAnalyzeSnapshotQuery(&req, &res);
  EXPECT_EQ(res.contract_version(), 0);
}
TEST(SnapshotProfiles, MeasuredImmutableCandidatesAndSupportedSemanticAcknowledgement) {
  const char *path = getenv("SNAPSHOT_QUERY_PROFILE_FILE");
  if (!path) GTEST_SKIP() << "requires externally measured test executable profile";
  auto raw = Read(path);
  auto entries = Decode(raw);
  Temp temp;
  temp.Write(raw);
  auto profiles = Profiles::Load(temp.name);
  ASSERT_EQ(profiles->CandidateCount(), entries.size());
  EXPECT_EQ(profiles->ExecutableDigest(), entries[0].grpc_digest);
  EXPECT_NE(profiles->FindCandidate(entries[0].id), nullptr);
  temp.Write("{}");
  EXPECT_EQ(*profiles->FindCandidate(entries[0].id), entries[0].record);
  EXPECT_THROW(Profiles::Load(temp.name), std::runtime_error);
  RewriterServiceImpl service(profiles);
  EXPECT_EQ(service.SnapshotCandidates(), profiles.get());
  for (const auto &q : {entries[0].id, std::string(66, '0'), std::string()}) {
    rewriter::AnalyzeSnapshotQueryRequest req;
    req.set_query_profile_id(q);
    req.set_contract_version(1);
    req.set_sql("SELECT 1");
    rewriter::AnalyzeSnapshotQueryResponse res;
    service.doAnalyzeSnapshotQuery(&req, &res);
    if (q == entries[0].id) {
      EXPECT_EQ(res.contract_version(), 1);
      EXPECT_EQ(res.query_profile_id(), q);
      EXPECT_EQ(res.code(), rewriter::NOT_SNAPSHOT_QUERY);
    } else {
      EXPECT_EQ(res.contract_version(), 0);
      EXPECT_TRUE(res.query_profile_id().empty());
      EXPECT_EQ(res.code(), rewriter::PROFILE_UNAVAILABLE);
    }
    rewriter::PrepareSnapshotQueryRequest preq;
    *preq.mutable_analysis() = req;
    rewriter::PrepareSnapshotQueryResponse pres;
    service.doPrepareSnapshotQuery(&preq, &pres);
    if (q == entries[0].id) {
      EXPECT_EQ(pres.contract_version(), 1);
      EXPECT_EQ(pres.query_profile_id(), q);
      EXPECT_EQ(pres.code(), rewriter::INVALID_INPUT);
    } else {
      EXPECT_EQ(pres.contract_version(), 0);
      EXPECT_TRUE(pres.query_profile_id().empty());
      EXPECT_EQ(pres.code(), rewriter::PROFILE_UNAVAILABLE);
    }
  }
  auto foreign = Decode(Raw())[0];
  entries.push_back(foreign);
  temp.Write(Bundle(entries));
  auto mixed = Profiles::Load(temp.name);
  EXPECT_EQ(mixed->CandidateCount(), profiles->CandidateCount());
  EXPECT_EQ(mixed->FindCandidate(foreign.id), nullptr);
  temp.Write(Raw());
  EXPECT_THROW(Profiles::Load(temp.name), std::runtime_error);
}
TEST(SnapshotProfiles, RefusesMissingNonregularAndMalformedFiles) {
  EXPECT_THROW(Profiles::Load(""), std::runtime_error);
  EXPECT_THROW(Profiles::Load("/snapshot-profile-file-does-not-exist"), std::runtime_error);
  EXPECT_THROW(Profiles::Load("/tmp"), std::runtime_error);
  Temp t;
  t.Write("{}");
  EXPECT_THROW(Profiles::Load(t.name), std::runtime_error);
}
TEST(SnapshotProfiles, ActualELFMetadataMutations) {
  Temp temp;
  std::filesystem::copy_file(
    "/proc/self/exe", temp.name, std::filesystem::copy_options::overwrite_existing);
  std::filesystem::permissions(
    temp.name, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  int fd = open(temp.name.c_str(), O_RDWR | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  struct Close {
    int fd;
    ~Close() {
      close(fd);
    }
  } closer{fd};
  EXPECT_NO_THROW(snapshot_profiles::identity::InspectExecutable(fd));
  auto patch = [&](uint64_t offset, const auto &replacement) {
    using T = std::decay_t<decltype(replacement)>;
    T saved{};
    ASSERT_EQ(pread(fd, &saved, sizeof(T), offset), sizeof(T));
    ASSERT_EQ(pwrite(fd, &replacement, sizeof(T), offset), sizeof(T));
    EXPECT_THROW(snapshot_profiles::identity::InspectExecutable(fd), std::runtime_error)
      << "offset=" << offset;
    ASSERT_EQ(pwrite(fd, &saved, sizeof(T), offset), sizeof(T));
  };
  Elf64_Ehdr h{};
  ASSERT_EQ(pread(fd, &h, sizeof(h), 0), sizeof(h));
  patch(offsetof(Elf64_Ehdr, e_machine), uint16_t(0));
  patch(offsetof(Elf64_Ehdr, e_shnum), uint16_t(0));
  Elf64_Shdr dynsym{}, versym{};
  for (uint64_t i = 0; i < h.e_shnum; ++i) {
    uint64_t off = h.e_shoff + i * sizeof(Elf64_Shdr);
    Elf64_Shdr s{};
    ASSERT_EQ(pread(fd, &s, sizeof(s), off), sizeof(s));
    if (s.sh_type == SHT_GNU_versym) versym = s;
    if (s.sh_type == SHT_DYNSYM) {
      dynsym = s;
      patch(off + offsetof(Elf64_Shdr, sh_size), s.sh_size - sizeof(Elf64_Sym));
      patch(off + offsetof(Elf64_Shdr, sh_entsize), uint64_t(1));
    }
  }
  ASSERT_NE(dynsym.sh_size, 0);
  ASSERT_NE(versym.sh_size, 0);
  bool weak_checked = false;
  for (uint64_t i = 1; i < dynsym.sh_size / sizeof(Elf64_Sym); ++i) {
    auto offset = dynsym.sh_offset + i * sizeof(Elf64_Sym);
    Elf64_Sym original{};
    ASSERT_EQ(pread(fd, &original, sizeof(original), offset), sizeof(original));
    if (original.st_shndx != SHN_UNDEF || ELF64_ST_BIND(original.st_info) != STB_GLOBAL) continue;
    auto weak = original;
    weak.st_info = ELF64_ST_INFO(STB_WEAK, ELF64_ST_TYPE(original.st_info));
    ASSERT_EQ(pwrite(fd, &weak, sizeof(weak), offset), sizeof(weak));
    // A weak import with a real non-instrumentation name cannot evade version
    // provenance. The same full dynsym extent also covers symbols after a
    // forged shorter section (the independent section mutation above).
    patch(versym.sh_offset + i * sizeof(Elf64_Half), Elf64_Half(0));
    ASSERT_EQ(pwrite(fd, &original, sizeof(original), offset), sizeof(original));
    weak_checked = true;
    break;
  }
  EXPECT_TRUE(weak_checked);
  for (uint64_t i = 0; i < h.e_phnum; ++i) {
    Elf64_Phdr p{};
    ASSERT_EQ(pread(fd, &p, sizeof(p), h.e_phoff + i * sizeof(p)), sizeof(p));
    if (p.p_type != PT_DYNAMIC) continue;
    for (uint64_t off = p.p_offset; off < p.p_offset + p.p_filesz; off += sizeof(Elf64_Dyn)) {
      Elf64_Dyn d{};
      ASSERT_EQ(pread(fd, &d, sizeof(d), off), sizeof(d));
      if (d.d_tag == DT_NULL) break;
      if (d.d_tag == DT_DEBUG) {
        auto bad = d;
        bad.d_tag = DT_RUNPATH;
        patch(off, bad);
        bad.d_tag = DT_AUDIT;
        patch(off, bad);
      }
      if (d.d_tag == DT_GNU_HASH) {
        auto bad = d;
        bad.d_un.d_val += 4;
        patch(off, bad);
      }
      if (d.d_tag == DT_SYMENT) {
        auto bad = d;
        bad.d_un.d_val = 1;
        patch(off, bad);
      }
      if (d.d_tag == DT_NEEDED) {
        auto bad = d;
        bad.d_un.d_val = 0;
        patch(off, bad);
      }
    }
  }
  EXPECT_NO_THROW(snapshot_profiles::identity::InspectExecutable(fd));
}

namespace {
snapshot_profiles::Entry MeasuredEntry() {
  const char *path = getenv("SNAPSHOT_QUERY_PROFILE_FILE");
  if (!path) throw std::runtime_error("focused snapshot tests require externally measured profile");
  return Decode(Read(path)).at(0);
}
rewriter::AnalyzeSnapshotQueryRequest SnapshotRequest(
  const std::string &q, const std::string &sql) {
  rewriter::AnalyzeSnapshotQueryRequest r;
  r.set_contract_version(1);
  r.set_query_profile_id(q);
  r.set_sql(sql);
  r.set_logical_database("tenant");
  for (auto name : {"copy", "events", "ids"}) {
    auto *t = r.add_catalog();
    t->set_database("tenant");
    t->set_table(name);
    t->set_table_id("0x" + std::string(63, '0') + std::to_string(r.catalog_size()));
    t->set_schema_hash("0x" + std::string(64, 'a'));
    auto *c = t->add_columns();
    c->set_name("value");
    c->set_type("Int64");
    c->set_generation(rewriter::SNAPSHOT_QUERY_COLUMN_GENERATION_ORDINARY);
  }
  return r;
}
void EmptyPrepare(const rewriter::PrepareSnapshotQueryResponse &out, const std::string &q) {
  EXPECT_EQ(out.code(), rewriter::INVALID_INPUT);
  EXPECT_EQ(out.message(), "snapshot query execution deadline exceeded");
  EXPECT_EQ(out.contract_version(), 1);
  EXPECT_EQ(out.query_profile_id(), q);
  EXPECT_TRUE(out.select_sql().empty());
  EXPECT_TRUE(out.target_table_id().empty());
  EXPECT_TRUE(out.target_columns().empty());
  EXPECT_TRUE(out.read_table_ids().empty());
}
} // namespace
TEST(SnapshotClosed, WholePrepareGenerationBudget) {
  auto entry = MeasuredEntry();
  entry.record = Replace(entry.record, "\"max_execution_ms\":30000", "\"max_execution_ms\":1000");
  Rehash(entry);
  Temp file;
  file.Write(Bundle({entry}));
  RewriterServiceImpl service(Profiles::Load(file.name));
  rewriter::PrepareSnapshotQueryRequest r;
  *r.mutable_analysis() = SnapshotRequest(entry.id, "INSERT INTO tenant.copy SELECT 7");
  // Warm actual parsing/generation before the timing cases.
  rewriter::PrepareSnapshotQueryResponse warm;
  service.doPrepareSnapshotQuery(&r, &warm);
  ASSERT_EQ(warm.code(), rewriter::SUCCESS);
  for (const auto &mode : {"control", "caller_cancel", "profile_deadline"}) {
    SCOPED_TRACE(mode);
    bool cancelled = false;
    int generations = 0;
    snapshot_query::Execution execution;
    execution.cancelled = [&] { return cancelled; };
    execution.after_generation = [&] {
      ++generations;
      if (std::string(mode) == "caller_cancel" && generations == 2) cancelled = true;
      if (std::string(mode) == "profile_deadline")
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
    };
    rewriter::PrepareSnapshotQueryResponse out;
    service.doPrepareSnapshotQuery(&r, &out, execution);
    EXPECT_EQ(generations, 2);
    if (std::string(mode) == "control") {
      EXPECT_EQ(out.code(), rewriter::SUCCESS);
      EXPECT_EQ(out.select_sql(), "SELECT accurateCast('7', 'Int64')");
    } else
      EmptyPrepare(out, entry.id);
  }
}
TEST(SnapshotClosed, BothWithScopesAndLexicalBoundary) {
  auto entry = MeasuredEntry();
  Temp file;
  file.Write(Bundle({entry}));
  RewriterServiceImpl service(Profiles::Load(file.name));
  auto r = SnapshotRequest(entry.id,
    "WITH x AS (SELECT value FROM tenant.events), y AS (SELECT value FROM x) INSERT INTO "
    "tenant.copy WITH x AS (SELECT value FROM y) SELECT value FROM x");
  rewriter::AnalyzeSnapshotQueryResponse a;
  service.doAnalyzeSnapshotQuery(&r, &a);
  ASSERT_EQ(a.code(), rewriter::SUCCESS) << a.DebugString();
  EXPECT_EQ(a.sql_after_materialization(), r.sql());
  ASSERT_EQ(a.read_table_ids_size(), 1);
  rewriter::PrepareSnapshotQueryRequest p;
  *p.mutable_analysis() = r;
  auto *b = p.add_bindings();
  b->set_table_id(a.read_table_ids(0));
  b->set_scratch_database("snapshot_scratch");
  b->set_scratch_table("r2");
  rewriter::PrepareSnapshotQueryResponse out;
  service.doPrepareSnapshotQuery(&p, &out);
  ASSERT_EQ(out.code(), rewriter::SUCCESS) << out.DebugString();
  EXPECT_EQ(out.select_sql(),
    "WITH x AS (SELECT value FROM snapshot_scratch.r2), y AS (SELECT value FROM x) SELECT * FROM "
    "(WITH x AS (SELECT value FROM y) SELECT value FROM x) AS snapshot_output");
  for (auto sql :
    {"WITH x AS (SELECT 'INSERT INTO fake' AS value) INSERT INTO tenant.copy SELECT 7",
      "WITH x AS (SELECT $$INSERT INTO fake$$ AS value) INSERT INTO tenant.copy SELECT 7",
      "WITH x AS (SELECT 7 /* INSERT INTO fake */ AS value) INSERT INTO tenant.copy SELECT 7",
      "WITH `INSERT` AS (SELECT 7 AS value) INSERT INTO tenant.copy SELECT 7"}) {
    r.set_sql(sql);
    service.doAnalyzeSnapshotQuery(&r, &a);
    EXPECT_EQ(a.code(), rewriter::SUCCESS) << sql << a.DebugString();
  }
  for (auto sql : {"WITH x AS (SELECT 7); INSERT INTO tenant.copy SELECT 7",
         "WITH x AS (INSERT INTO tenant.copy SELECT 7) INSERT INTO tenant.copy SELECT 7",
         "WITH x AS (SELECT 7) garbage INSERT INTO tenant.copy SELECT 7",
         "WITH x AS (SELECT 7) INSERT INTO tenant.copy SELECT 7; SELECT 8"}) {
    r.set_sql(sql);
    service.doAnalyzeSnapshotQuery(&r, &a);
    EXPECT_NE(a.code(), rewriter::SUCCESS) << sql;
    EXPECT_TRUE(a.sql_after_materialization().empty());
    EXPECT_TRUE(a.target_table_id().empty());
    EXPECT_TRUE(a.read_table_ids().empty());
  }
}
TEST(SnapshotClosed, MeasuredRecordSemanticRefusals) {
  auto original = MeasuredEntry();
  const std::vector<std::pair<std::string, std::string>> changes = {
    {"\"cast_keep_nullable\",\"value\":\"0\"", "\"cast_keep_nullable\",\"value\":\"1\""},
    {"\"column-profile-v1\"", "\"column-profile-v2\""},
    {"\"output-order-v1\"", "\"output-order-v2\""},
    {"\"max_sql_bytes\":65536", "\"max_sql_bytes\":65537"},
    {"\"max_descriptor_bytes\":65536", "\"max_descriptor_bytes\":65537"},
    {"\"max_execution_ms\":30000", "\"max_execution_ms\":30001"},
    {"\"prepare-integer-literal-exact-v1\"", "\"prepare-integer-literal-exact-v2\""}};
  for (auto &change : changes) {
    auto entry = original;
    entry.record = Replace(entry.record, change.first, change.second);
    Rehash(entry);
    Temp file;
    file.Write(Bundle({entry}));
    auto profiles = Profiles::Load(file.name);
    ASSERT_EQ(profiles->CandidateCount(), 1);
    RewriterServiceImpl service(profiles);
    auto r = SnapshotRequest(entry.id, "INSERT INTO tenant.copy SELECT 7");
    rewriter::AnalyzeSnapshotQueryResponse out;
    service.doAnalyzeSnapshotQuery(&r, &out);
    EXPECT_EQ(out.code(), rewriter::PROFILE_UNAVAILABLE);
    EXPECT_EQ(out.contract_version(), 0);
    EXPECT_TRUE(out.query_profile_id().empty());
    EXPECT_TRUE(out.sql_after_materialization().empty());
  }
}

TEST(SnapshotClosed, CanonicalBooleanGrouping) {
  auto entry = MeasuredEntry();
  Temp file;
  file.Write(Bundle({entry}));
  RewriterServiceImpl service(Profiles::Load(file.name));
  const std::vector<std::string> predicates = {"(value >= 0 AND value < 9) OR NOT (value = 7)",
    "value = 1 AND (value = 2 OR value = 3)",
    "NOT ((value = 1 AND value = 2) OR value = 3)",
    "NOT (value = 1 OR (value = 2 AND NOT (value = 3)))",
    "(value = 1 OR value = 2) AND (value = 3 OR value = 4)",
    "value IN (SELECT value FROM tenant.ids) OR NOT (value = (SELECT value FROM tenant.ids))",
    "NOT (value IN (SELECT value FROM tenant.ids) AND value = (SELECT value FROM tenant.ids))"};
  for (const auto &predicate : predicates) {
    auto r = SnapshotRequest(
      entry.id, "INSERT INTO tenant.copy SELECT value FROM tenant.events WHERE " + predicate);
    rewriter::AnalyzeSnapshotQueryResponse original;
    service.doAnalyzeSnapshotQuery(&r, &original);
    ASSERT_EQ(original.code(), rewriter::SUCCESS) << r.sql() << original.DebugString();
    EXPECT_EQ(original.sql_after_materialization(), r.sql());
    r.set_sql(original.sql_after_materialization());
    rewriter::AnalyzeSnapshotQueryResponse roundtrip;
    service.doAnalyzeSnapshotQuery(&r, &roundtrip);
    EXPECT_EQ(roundtrip.SerializeAsString(), original.SerializeAsString());
  }
}

TEST(SnapshotClosed, InsertIdentifierIsNotStatementBoundary) {
  auto entry = MeasuredEntry();
  Temp file;
  file.Write(Bundle({entry}));
  RewriterServiceImpl service(Profiles::Load(file.name));
  for (const auto &name : {"insert", "INSERT", "InSeRt", "`insert`", "\"insert\""}) {
    SCOPED_TRACE(name);
    auto r = SnapshotRequest(entry.id,
      "WITH " + std::string(name) +
        " AS (SELECT value FROM tenant.events) INSERT INTO tenant.copy SELECT value FROM " + name);
    rewriter::AnalyzeSnapshotQueryResponse original;
    service.doAnalyzeSnapshotQuery(&r, &original);
    ASSERT_EQ(original.code(), rewriter::SUCCESS) << original.DebugString();
    ASSERT_EQ(original.read_table_ids_size(), 1);
    EXPECT_EQ(original.read_table_ids(0), r.catalog(1).table_id());
    EXPECT_EQ(original.target_table_id(), r.catalog(0).table_id());
    EXPECT_EQ(original.target_columns(0), "value");
    r.set_sql(original.sql_after_materialization());
    rewriter::AnalyzeSnapshotQueryResponse signed_analysis;
    service.doAnalyzeSnapshotQuery(&r, &signed_analysis);
    EXPECT_EQ(signed_analysis.SerializeAsString(), original.SerializeAsString());
    rewriter::PrepareSnapshotQueryRequest p;
    *p.mutable_analysis() = r;
    auto *binding = p.add_bindings();
    binding->set_table_id(original.read_table_ids(0));
    binding->set_scratch_database("snapshot_scratch");
    binding->set_scratch_table("r2");
    rewriter::PrepareSnapshotQueryResponse prepared;
    service.doPrepareSnapshotQuery(&p, &prepared);
    ASSERT_EQ(prepared.code(), rewriter::SUCCESS) << prepared.DebugString();
    const std::string canonical = name[0] == '`' || name[0] == '\"' ? "insert" : name;
    EXPECT_EQ(prepared.select_sql(),
      "WITH " + canonical + " AS (SELECT value FROM snapshot_scratch.r2) SELECT value FROM " +
        canonical);
    EXPECT_EQ(prepared.target_table_id(), original.target_table_id());
    EXPECT_EQ(prepared.target_columns(0), "value");
    EXPECT_EQ(prepared.read_table_ids(0), original.read_table_ids(0));
  }
  for (const auto &sql : {"WITH x AS (SELECT 7) SELECT insert FROM tenant.events",
         "WITH x AS (SELECT 7) SELECT INSERT FROM tenant.events",
         "WITH 7 AS insert SELECT insert"}) {
    auto r = SnapshotRequest(entry.id, sql);
    rewriter::AnalyzeSnapshotQueryResponse result;
    service.doAnalyzeSnapshotQuery(&r, &result);
    EXPECT_EQ(result.code(), rewriter::NOT_SNAPSHOT_QUERY) << sql << result.DebugString();
    EXPECT_EQ(result.contract_version(), 1);
    EXPECT_EQ(result.query_profile_id(), entry.id);
    EXPECT_TRUE(result.sql_after_materialization().empty());
    EXPECT_TRUE(result.target_table_id().empty());
    EXPECT_TRUE(result.target_columns().empty());
    EXPECT_TRUE(result.read_table_ids().empty());
  }
}
