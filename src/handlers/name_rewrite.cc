// src/handlers/name_rewrite.cc
#include "handlers/name_rewrite.h"

#include <sstream>

namespace rewriter_handlers {

namespace {

// "db.table" (or bare "table" / "db" when the other side is empty).
inline std::string qualify(const std::string &db, const std::string &table) {
  if (db.empty()) return table;
  if (table.empty()) return db;
  return db + "." + table;
}

inline std::string resolvedDelim(const rewriter::RewriteTableForDynamicArgs &args) {
  return args.delim().empty() ? "_" : args.delim();
}

} // namespace

const rewriter::RewriteTableForDynamicArgs* findGeneralArgs(
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options) {
  const rewriter::RewriteTableForDynamicArgs *out = nullptr;
  for (const auto &option : options) {
    if (option.op() != rewriter::RewriteOp::TableNameRewrite) continue;
    if (!option.has_table_name_args()) continue;
    if (!option.table_name_args().has_general_args()) continue;
    // Last-wins: keep scanning so later options override earlier ones,
    // matching the chained semantics the SELECT path already has.
    out = &option.table_name_args().general_args();
  }
  return out;
}

std::optional<std::string> resolvePhysicalDatabase(
  const std::string &logical_db,
  const rewriter::RewriteTableForDynamicArgs &args) {
  if (logical_db.empty()) return std::nullopt;
  // 1) database_map: virtual/logical -> physical.
  if (auto it = args.database_map().find(logical_db); it != args.database_map().end()) {
    return it->second;
  }
  // 2) Already a physical DB the user knows about -> passthrough.
  for (const auto &p : args.known_physical_databases()) {
    if (p == logical_db) return logical_db;
  }
  return std::nullopt;
}

std::string buildDynamicTableName(
  const std::string &logical_db,
  const std::string &original_table,
  const rewriter::RewriteTableForDynamicArgs &args) {
  const std::string delim = resolvedDelim(args);
  std::string out = logical_db;
  out += delim;
  // Insert the extra_arguments block + its trailing delim ONLY when the
  // block is non-empty — otherwise we'd emit a double delim between
  // `logical_db` and `original_table`.
  if (args.extra_arguments_size() > 0) {
    bool first = true;
    for (const auto &a : args.extra_arguments()) {
      if (!first) out += delim;
      out += a;
      first = false;
    }
    out += delim;
  }
  out += original_table;
  return out;
}

std::string buildDynamicTablePrefix(
  const std::string &logical_db,
  const rewriter::RewriteTableForDynamicArgs &args) {
  // Prefix is everything buildDynamicTableName produces up to (but not
  // including) `original_table`. Equivalent to calling the builder with
  // an empty `original_table`.
  return buildDynamicTableName(logical_db, /*original_table=*/"", args);
}

DynamicRewriteOutcome applyDynamicRewrite(
  const std::string &origin_db,
  const std::string &origin_table,
  const rewriter::RewriteTableForDynamicArgs &args,
  std::string_view stmt_kind) {
  DynamicRewriteOutcome outcome;

  // Resolve logical: SQL qualifier wins, else upstream context. Empty →
  // reject (caller has no routing info).
  std::string logical = origin_db.empty() ? args.upstream_logical_database_in_context() : origin_db;
  if (logical.empty()) {
    std::ostringstream oss;
    oss << stmt_kind << " target '" << origin_table
        << "' is unqualified and no upstream_logical_database_in_context is set; "
           "caller must send `USE <db>` or qualify the target";
    outcome.status = DynamicRewriteStatus::InvalidRequest;
    outcome.reject_reason = oss.str();
    return outcome;
  }

  auto physical = resolvePhysicalDatabase(logical, args);
  if (!physical) {
    std::ostringstream oss;
    oss << stmt_kind << " target references logical database '" << logical
        << "' which is not in database_map and not a known physical database; "
           "user does not have this database";
    outcome.status = DynamicRewriteStatus::InvalidRequest;
    outcome.reject_reason = oss.str();
    return outcome;
  }

  outcome.status = DynamicRewriteStatus::OK;
  outcome.logical_db = logical;
  outcome.physical_db = *physical;
  outcome.new_table = origin_table.empty()
    ? ""  // USE: no table, only the DB swap matters.
    : buildDynamicTableName(logical, origin_table, args);
  return outcome;
}

void recordTableRewrite(
  rewriter::RewriteSQLResponse *response,
  const std::string &origin_db,
  const std::string &origin_table,
  const std::string &new_db,
  const std::string &new_table) {
  const std::string key = qualify(origin_db, origin_table);
  const std::string val = qualify(new_db, new_table);
  if (key.empty() || key == val) return;
  (*response->mutable_table_rewrites())[key] = val;
}

} // namespace rewriter_handlers
