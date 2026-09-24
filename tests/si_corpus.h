#pragma once

// The storage-integrity corpus contract, mirrored from
// rewriter-go/internal/harness/sicorpus_test.go. The JSON file is
// byte-identical in both repos; these rules and their ids (R1..R8) are
// identical too, so a case one runner rejects the other rejects as well.
//
// Spec J D3.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include "si_normalize.h"

namespace si_corpus {

// Fingerprint is FNV-1a/64 over the exact file bytes. It powers this repo's
// local exact-byte pin and is deliberately dependency-free (no OpenSSL in the
// test target); `shasum -a 256` remains the human-readable cross-check recorded
// in each PR.
inline uint64_t Fingerprint(const std::string &raw) {
  uint64_t h = 14695981039346656037ULL;
  for (const unsigned char b : raw) {
    h ^= static_cast<uint64_t>(b);
    h *= 1099511628211ULL;
  }
  return h;
}

// This repo locally pins its own exact corpus bytes, catching accidental edits
// and stale local pins. Intentional updates require paired PRs with rewriter-go,
// an explicit byte-for-byte cmp, and a SHA-256 recorded in each PR description.
constexpr uint64_t kCorpusFingerprint = 7051648083520101593ULL;
constexpr size_t kCorpusBytes = 254170;
constexpr size_t kCorpusCases = 256;

struct Accessed {
  std::string original_database;
  std::string original_table;
  std::string logical_database;
  std::string physical_database;
  bool is_remote = false;
  bool is_storage_integrity = false;
};

struct Case {
  std::string name;
  std::string sql;
  // "V1" or "V2" (rule R8): the StorageIntegrityArgs.contract_version the
  // runners send for this case.
  std::string contract_version;
  std::string want_code;
  std::string want_stmt;
  std::string want_sql;
  std::string want_sql_go;
  std::string want_sql_cpp;
  std::string want_message_contains;
  bool has_dynamic = false;
  bool reject = false;
  bool allow_sql_divergence = false;
  bool want_no_contract_ack = false;
  Poco::JSON::Object::Ptr dynamic;  // Raw JSON; bound to the proto by the test.
  std::map<std::string, std::string> want_table_rewrites;
  bool has_table_rewrites = false;
  bool has_accessed = false;
  std::vector<Accessed> want_accessed;
  std::vector<std::string> want_sql_contains;
  std::vector<std::string> want_sql_not_contains;

  // ExpectedSQL is the pin this engine must match exactly, after
  // normalization. allow_sql_divergence selects the C++-specific pin.
  const std::string &ExpectedSQL() const {
    return allow_sql_divergence ? want_sql_cpp : want_sql;
  }
};

inline const std::set<std::string> &CaseKeys() {
  static const std::set<std::string> keys = {
    "name", "sql", "contract_version", "dynamic", "want_code", "want_stmt", "want_sql",
    "want_sql_go", "want_sql_cpp", "want_sql_contains", "want_sql_not_contains",
    "want_message_contains", "want_table_rewrites", "want_accessed", "reject",
    "allow_sql_divergence", "want_no_contract_ack",
  };
  return keys;
}

inline const std::set<std::string> &DynamicKeys() {
  static const std::set<std::string> keys = {
    "database_map", "known_physical_databases",
    "upstream_logical_database_in_context", "upstream_physical_database_in_context",
    "delim", "logical_database_to_remote_upstream_index", "remote_upstreams",
    "storage_integrity",
  };
  return keys;
}

inline const std::set<std::string> &StorageIntegrityKeys() {
  static const std::set<std::string> keys = {
    "tables", "read_mode", "reserved_row_id_column", "reserved_databases",
  };
  return keys;
}

inline const std::set<std::string> &StorageIntegrityTableKeys() {
  static const std::set<std::string> keys = {
    "safe_table", "unsafe_table", "excluded_unsafe_parts",
  };
  return keys;
}

inline const std::set<std::string> &RemoteUpstreamKeys() {
  static const std::set<std::string> keys = {"addr", "user", "password"};
  return keys;
}

inline const std::set<std::string> &AccessedKeys() {
  static const std::set<std::string> keys = {
    "original_database", "original_table", "logical_database", "physical_database",
    "is_remote", "is_storage_integrity",
  };
  return keys;
}

inline const std::set<std::string> &KnownCodes() {
  static const std::set<std::string> codes = {
    "Success", "SyntaxError", "RewriteError", "UnsupportedStatement",
    "InvalidRewriteRequest",
  };
  return codes;
}

inline void RejectUnknownKeys(const Poco::JSON::Object::Ptr &object,
                              const std::set<std::string> &allowed,
                              const std::string &path,
                              std::vector<std::string> *violations) {
  if (!object || !violations) return;
  for (const auto &name : object->getNames()) {
    if (allowed.count(name) == 0) {
      violations->push_back(path + ": R7: unknown key \"" + name
                            + "\" (the corpus schema is frozen; `sql_exact` is deleted)");
    }
  }
}

inline std::runtime_error TypeError(const std::string &path, const char *expected) {
  return std::runtime_error(path + ": expected " + expected);
}

inline std::string JsonString(const Poco::JSON::Object::Ptr &object,
                              const std::string &key,
                              const std::string &path) {
  if (!object || !object->has(key) || object->isNull(key)) return {};
  const auto value = object->get(key);
  if (!value.isString()) throw TypeError(path + "." + key, "string");
  return value.extract<std::string>();
}

inline bool JsonBool(const Poco::JSON::Object::Ptr &object,
                     const std::string &key,
                     const std::string &path) {
  if (!object || !object->has(key) || object->isNull(key)) return false;
  const auto value = object->get(key);
  if (!value.isBoolean()) throw TypeError(path + "." + key, "boolean");
  return value.extract<bool>();
}

inline Poco::JSON::Object::Ptr JsonObject(const Poco::JSON::Object::Ptr &object,
                                          const std::string &key,
                                          const std::string &path) {
  if (!object || !object->has(key) || object->isNull(key)) return {};
  auto value = object->getObject(key);
  if (!value) throw TypeError(path + "." + key, "object");
  return value;
}

inline Poco::JSON::Array::Ptr JsonArray(const Poco::JSON::Object::Ptr &object,
                                        const std::string &key,
                                        const std::string &path) {
  if (!object || !object->has(key) || object->isNull(key)) return {};
  auto value = object->getArray(key);
  if (!value) throw TypeError(path + "." + key, "array");
  return value;
}

inline std::vector<std::string> JsonStrings(const Poco::JSON::Object::Ptr &object,
                                            const std::string &key,
                                            const std::string &path = "json") {
  std::vector<std::string> out;
  const auto array = JsonArray(object, key, path);
  if (!array) return out;
  out.reserve(array->size());
  for (size_t i = 0; i < array->size(); ++i) {
    const auto value = array->get(static_cast<unsigned int>(i));
    if (!value.isString()) {
      throw TypeError(path + "." + key + "[" + std::to_string(i) + "]", "string");
    }
    out.push_back(value.extract<std::string>());
  }
  return out;
}

inline void ValidateStringMap(const Poco::JSON::Object::Ptr &object,
                              const std::string &path) {
  if (!object) return;
  for (const auto &entry : *object) {
    if (!entry.second.isString()) throw TypeError(path + "." + entry.first, "string");
  }
}

inline void ValidateDynamicSchema(const Poco::JSON::Object::Ptr &dynamic,
                                  const std::string &path,
                                  std::vector<std::string> *violations) {
  RejectUnknownKeys(dynamic, DynamicKeys(), path, violations);

  ValidateStringMap(JsonObject(dynamic, "database_map", path), path + ".database_map");
  ValidateStringMap(JsonObject(dynamic, "logical_database_to_remote_upstream_index", path),
                    path + ".logical_database_to_remote_upstream_index");
  JsonStrings(dynamic, "known_physical_databases", path);
  JsonString(dynamic, "upstream_logical_database_in_context", path);
  JsonString(dynamic, "upstream_physical_database_in_context", path);
  JsonString(dynamic, "delim", path);

  const auto remote_upstreams = JsonObject(dynamic, "remote_upstreams", path);
  if (remote_upstreams) {
    for (const auto &entry : *remote_upstreams) {
      const std::string entry_path = path + ".remote_upstreams." + entry.first;
      const auto upstream = remote_upstreams->getObject(entry.first);
      if (!upstream) throw TypeError(entry_path, "object");
      RejectUnknownKeys(upstream, RemoteUpstreamKeys(), entry_path, violations);
      JsonString(upstream, "addr", entry_path);
      JsonString(upstream, "user", entry_path);
      JsonString(upstream, "password", entry_path);
    }
  }

  const auto storage_integrity = JsonObject(dynamic, "storage_integrity", path);
  if (!storage_integrity) return;
  const std::string si_path = path + ".storage_integrity";
  RejectUnknownKeys(storage_integrity, StorageIntegrityKeys(), si_path, violations);
  JsonString(storage_integrity, "read_mode", si_path);
  JsonString(storage_integrity, "reserved_row_id_column", si_path);
  JsonStrings(storage_integrity, "reserved_databases", si_path);
  const auto tables = JsonObject(storage_integrity, "tables", si_path);
  if (!tables) return;
  for (const auto &entry : *tables) {
    const std::string table_path = si_path + ".tables." + entry.first;
    const auto table = tables->getObject(entry.first);
    if (!table) throw TypeError(table_path, "object");
    RejectUnknownKeys(table, StorageIntegrityTableKeys(), table_path, violations);
    JsonString(table, "safe_table", table_path);
    JsonString(table, "unsafe_table", table_path);
    JsonStrings(table, "excluded_unsafe_parts", table_path);
  }
}

// LoadCases parses exactly one JSON value. Poco's non-streaming parser rejects
// trailing non-whitespace bytes; the explicit schema walk mirrors Go's
// DisallowUnknownFields recursively, including nested maps of structured
// values. Unknown keys are appended to *violations so the meta-test can report
// all R7 failures at once; malformed JSON or wrong value types throw.
inline std::vector<Case> LoadCases(const std::string &path,
                                   std::vector<std::string> *violations) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("open " + path + ": failed");
  std::stringstream buffer;
  buffer << input.rdbuf();

  Poco::JSON::Parser parser;
  const auto root = parser.parse(buffer.str());
  if (root.type() != typeid(Poco::JSON::Array::Ptr)) {
    throw TypeError(path, "one JSON array");
  }
  const auto array = root.extract<Poco::JSON::Array::Ptr>();

  std::vector<Case> out;
  out.reserve(array->size());
  for (size_t i = 0; i < array->size(); ++i) {
    const auto object = array->getObject(static_cast<unsigned int>(i));
    if (!object) {
      throw TypeError("case[" + std::to_string(i) + "]", "object");
    }

    Case c;
    c.name = JsonString(object, "name", "case[" + std::to_string(i) + "]");
    const std::string case_path = c.name.empty()
      ? "case[" + std::to_string(i) + "]"
      : c.name;
    c.sql = JsonString(object, "sql", case_path);
    c.contract_version = JsonString(object, "contract_version", case_path);
    RejectUnknownKeys(object, CaseKeys(), case_path, violations);
    c.want_code = JsonString(object, "want_code", case_path);
    c.want_stmt = JsonString(object, "want_stmt", case_path);
    c.want_sql = JsonString(object, "want_sql", case_path);
    c.want_sql_go = JsonString(object, "want_sql_go", case_path);
    c.want_sql_cpp = JsonString(object, "want_sql_cpp", case_path);
    c.want_message_contains = JsonString(object, "want_message_contains", case_path);
    c.reject = JsonBool(object, "reject", case_path);
    c.allow_sql_divergence = JsonBool(object, "allow_sql_divergence", case_path);
    c.want_no_contract_ack = JsonBool(object, "want_no_contract_ack", case_path);

    c.dynamic = JsonObject(object, "dynamic", case_path);
    c.has_dynamic = static_cast<bool>(c.dynamic);
    if (c.dynamic) ValidateDynamicSchema(c.dynamic, case_path + ".dynamic", violations);

    const auto rewrites = JsonObject(object, "want_table_rewrites", case_path);
    c.has_table_rewrites = static_cast<bool>(rewrites);
    if (rewrites) {
      ValidateStringMap(rewrites, case_path + ".want_table_rewrites");
      for (const auto &entry : *rewrites) {
        c.want_table_rewrites[entry.first] = entry.second.extract<std::string>();
      }
    }

    const auto accessed = JsonArray(object, "want_accessed", case_path);
    c.has_accessed = static_cast<bool>(accessed);
    if (accessed) {
      c.want_accessed.reserve(accessed->size());
      for (size_t j = 0; j < accessed->size(); ++j) {
        const auto entry = accessed->getObject(static_cast<unsigned int>(j));
        const std::string entry_path = case_path + ".want_accessed[" + std::to_string(j) + "]";
        if (!entry) throw TypeError(entry_path, "object");
        RejectUnknownKeys(entry, AccessedKeys(), entry_path, violations);
        Accessed expected;
        expected.original_database = JsonString(entry, "original_database", entry_path);
        expected.original_table = JsonString(entry, "original_table", entry_path);
        expected.logical_database = JsonString(entry, "logical_database", entry_path);
        expected.physical_database = JsonString(entry, "physical_database", entry_path);
        expected.is_remote = JsonBool(entry, "is_remote", entry_path);
        expected.is_storage_integrity = JsonBool(entry, "is_storage_integrity", entry_path);
        c.want_accessed.push_back(std::move(expected));
      }
    }

    c.want_sql_contains = JsonStrings(object, "want_sql_contains", case_path);
    c.want_sql_not_contains = JsonStrings(object, "want_sql_not_contains", case_path);
    out.push_back(std::move(c));
  }
  return out;
}

inline bool IsBlank(const std::string &value) {
  return std::all_of(value.begin(), value.end(), [](const unsigned char c) {
    return std::isspace(c) != 0;
  });
}

inline bool ContainsEquivalent(const std::string &sql, const std::string &substring) {
  return sql.find(substring) != std::string::npos
    || NormalizeSIIdentifierQuotes(sql).find(NormalizeSIIdentifierQuotes(substring))
         != std::string::npos;
}

// ValidateCorpus mirrors ValidateSICorpus in rewriter-go rule for rule.
inline std::vector<std::string> ValidateCorpus(const std::vector<Case> &cases) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (const auto &c : cases) {
    const auto add = [&](const char *rule, const std::string &detail) {
      out.push_back(c.name + ": " + rule + ": " + detail);
    };
    if (IsBlank(c.name)) {
      out.push_back("<unnamed>: R1: name must be non-empty");
      continue;
    }
    if (!seen.insert(c.name).second) add("R1", "duplicate case name");
    if (IsBlank(c.sql)) add("R2", "sql must be non-empty");
    if (c.contract_version != "V1" && c.contract_version != "V2") {
      add("R8", "contract_version must be \"V1\" or \"V2\", got \""
                + c.contract_version + "\"");
    }

    if (c.want_code.empty()) {
      add("R6", "want_code must be non-empty");
    } else if (KnownCodes().count(c.want_code) == 0) {
      add("R6", "unknown want_code " + c.want_code);
    }

    const bool is_reject = !c.want_code.empty() && c.want_code != "Success";
    const bool is_success = c.want_code == "Success";
    if (is_reject) {
      if (!c.reject) add("R3", "want_code != Success must set reject: true");
      if (c.want_message_contains.empty()) {
        add("R3", "a reject case must carry want_message_contains");
      }
      if (c.allow_sql_divergence) {
        add("R3", "a reject case forbids allow_sql_divergence");
      }
      if (!c.want_sql.empty() || !c.want_sql_go.empty() || !c.want_sql_cpp.empty()
          || !c.want_sql_contains.empty() || !c.want_sql_not_contains.empty()) {
        add("R3", "a reject case must not pin SQL (it echoes the input)");
      }
    } else if (is_success) {
      if (c.reject) add("R3", "reject: true requires want_code != Success");
      if (c.allow_sql_divergence) {
        if (c.want_sql_go.empty() || c.want_sql_cpp.empty()) {
          add("R3", "allow_sql_divergence requires both want_sql_go and want_sql_cpp");
        }
        if (!c.want_sql.empty()) {
          add("R3", "allow_sql_divergence forbids want_sql; pin each engine separately");
        }
      } else {
        if (c.want_sql.empty()) add("R3", "a success case must carry want_sql");
        if (!c.want_sql_go.empty() || !c.want_sql_cpp.empty()) {
          add("R3", "per-engine pins require allow_sql_divergence: true");
        }
      }
    } else if (c.reject) {
      add("R3", "reject: true requires an explicit non-Success want_code");
    }

    if (is_success) {
      for (const auto &substring : c.want_sql_contains) {
        if (substring.empty()) {
          add("R4", "want_sql_contains entry must be non-empty");
          continue;
        }
        if (ContainsEquivalent(c.sql, substring)) {
          add("R4", "vacuous want_sql_contains \"" + substring
                    + "\": already a substring of the input SQL, so a no-op rewriter passes");
        }
      }
      for (const auto &substring : c.want_sql_not_contains) {
        if (substring.empty()) {
          add("R5", "want_sql_not_contains entry must be non-empty");
        }
      }
    }
  }
  return out;
}

}  // namespace si_corpus
