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

inline std::string resolvedDelim(const rewriter::RewriteTableDynamicArgs &args) {
  return args.delim().empty() ? "_" : args.delim();
}

} // namespace

const rewriter::RewriteTableDynamicArgs* findDynamicArgs(
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options) {
  const rewriter::RewriteTableDynamicArgs *out = nullptr;
  for (const auto &option : options) {
    if (option.op() != rewriter::RewriteOp::TableNameRewrite) continue;
    if (!option.has_table_name_args()) continue;
    if (!option.table_name_args().has_dynamic_args()) continue;
    // Last-wins: keep scanning so later options override earlier ones,
    // matching the chained semantics the SELECT path already has.
    out = &option.table_name_args().dynamic_args();
  }
  return out;
}

TableRewriteSelection findActiveTableRewrite(
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options) {
  TableRewriteSelection out;
  for (const auto &option : options) {
    if (option.op() != rewriter::RewriteOp::TableNameRewrite) continue;
    if (!option.has_table_name_args()) continue;
    const auto &args = option.table_name_args();
    // Per-option: presence of `static_args` alone selects static-mode —
    // an empty static_args is an explicit "static-only, no rewrite"
    // intent, NOT a fallthrough to dynamic. Only when `static_args` is
    // absent do we fall back to `dynamic_args`. Options that contribute
    // neither sub-message don't override a prior selection (consistent
    // with findDynamicArgs).
    if (args.has_static_args()) {
      out.mode = TableRewriteMode::Static;
      out.static_args = &args.static_args();
      out.dynamic_args = nullptr;
    } else if (args.has_dynamic_args()) {
      out.mode = TableRewriteMode::Dynamic;
      out.static_args = nullptr;
      out.dynamic_args = &args.dynamic_args();
    }
  }
  return out;
}

StaticLookupOutcome lookupStaticTableRewrite(
  const std::string &origin_db,
  const std::string &origin_table,
  const rewriter::RewriteTableStaticArgs &args) {
  StaticLookupOutcome out;
  // Lookup key matches ASTReplaceTransformer's keying for the SELECT path —
  // either bare "table" or qualified "db.table". Callers that key their
  // maps the other way will land in NotMatched.
  const std::string key = origin_db.empty()
    ? origin_table
    : origin_db + "." + origin_table;

  // Precedence: table_map -> remote_table_map -> table_with_database_map.
  if (auto it = args.table_map().find(key); it != args.table_map().end()) {
    out.status = StaticLookupOutcome::Status::Matched;
    out.new_db = origin_db;  // table_map only renames; db unchanged.
    out.new_table = it->second;
    return out;
  }
  if (args.remote_table_map().count(key) > 0) {
    out.status = StaticLookupOutcome::Status::RemoteUnsupported;
    return out;
  }
  if (auto it = args.table_with_database_map().find(key);
      it != args.table_with_database_map().end()) {
    out.status = StaticLookupOutcome::Status::Matched;
    out.new_db = it->second.database();
    out.new_table = it->second.table();
    return out;
  }
  out.status = StaticLookupOutcome::Status::NotMatched;
  return out;
}

std::optional<std::string> resolvePhysicalDatabase(
  const std::string &logical_db,
  const rewriter::RewriteTableDynamicArgs &args) {
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
  const rewriter::RewriteTableDynamicArgs &args) {
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
  const rewriter::RewriteTableDynamicArgs &args) {
  // Prefix is everything buildDynamicTableName produces up to (but not
  // including) `original_table`. Equivalent to calling the builder with
  // an empty `original_table`.
  return buildDynamicTableName(logical_db, /*original_table=*/"", args);
}

DynamicRewriteOutcome applyDynamicRewrite(
  const std::string &origin_db,
  const std::string &origin_table,
  const rewriter::RewriteTableDynamicArgs &args,
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

  outcome.logical_db = logical;
  outcome.physical_db = *physical;
  outcome.new_table = origin_table.empty()
    ? ""  // USE: no table, only the DB swap matters.
    : buildDynamicTableName(logical, origin_table, args);

  // Remote-upstream check: if the logical DB is remote-mapped, swap the
  // outcome to OkRemote and surface the upstream's connection info. The
  // SELECT path uses these to wrap the identifier in `remote(...)`; the
  // non-SELECT handlers reject OkRemote with UnsupportedStatement.
  if (auto idx_it = args.logical_database_to_remote_upstream_index().find(logical);
      idx_it != args.logical_database_to_remote_upstream_index().end()) {
    const std::string &upstream_key = idx_it->second;
    auto upstream_it = args.remote_upstreams().find(upstream_key);
    if (upstream_it == args.remote_upstreams().end()) {
      std::ostringstream oss;
      oss << stmt_kind << " target's logical database '" << logical
          << "' is mapped to remote upstream key '" << upstream_key
          << "' but that key is not in remote_upstreams";
      outcome.status = DynamicRewriteStatus::InvalidRequest;
      outcome.reject_reason = oss.str();
      return outcome;
    }
    outcome.status = DynamicRewriteStatus::OkRemote;
    outcome.remote_addr = upstream_it->second.addr();
    outcome.remote_user = upstream_it->second.user();
    outcome.remote_password = upstream_it->second.password();
    return outcome;
  }

  outcome.status = DynamicRewriteStatus::OK;
  return outcome;
}

bool isLogicalRemoteMapped(
  const std::string &logical,
  const rewriter::RewriteTableDynamicArgs &args) {
  if (logical.empty()) return false;
  return args.logical_database_to_remote_upstream_index().count(logical) > 0;
}

AccessedTableResolution resolveAccessedTable(
  const std::string &origin_db,
  const std::string &origin_table,
  const TableRewriteSelection &selection) {
  AccessedTableResolution out;
  if (selection.mode == TableRewriteMode::None) return out;

  if (selection.mode == TableRewriteMode::Static) {
    // SELECT-side three-map lookup. logical_database stays empty: static
    // mode doesn't carry a logical/physical distinction beyond what the
    // caller already encoded into the maps. Lookup key matches
    // ASTReplaceTransformer's keying — qualified "db.table" or bare
    // "table" when origin_db is empty.
    const auto &args = *selection.static_args;
    const std::string key = qualify(origin_db, origin_table);
    if (auto it = args.table_map().find(key); it != args.table_map().end()) {
      // Rename only — db unchanged. When origin_db was empty (bare table),
      // physical_database stays empty: ClickHouse will resolve it against
      // the session default at query time, which the rewriter doesn't track.
      out.physical_database = origin_db;
      return out;
    }
    if (auto it = args.remote_table_map().find(key);
        it != args.remote_table_map().end()) {
      out.physical_database = it->second.database();
      out.is_remote = true;
      return out;
    }
    if (auto it = args.table_with_database_map().find(key);
        it != args.table_with_database_map().end()) {
      // Empty `database` in the entry means "preserve origin db" (per the
      // proto contract for table_with_database_map).
      out.physical_database = it->second.database().empty()
        ? origin_db
        : it->second.database();
      return out;
    }
    return out;  // Unmatched: leave physical_database empty (passthrough).
  }

  // Dynamic mode.
  const auto &args = *selection.dynamic_args;
  const std::string logical = origin_db.empty()
    ? args.upstream_logical_database_in_context()
    : origin_db;
  if (logical.empty()) return out;
  out.logical_database = logical;
  if (auto p = resolvePhysicalDatabase(logical, args)) {
    out.physical_database = *p;
  }
  // SELECT is lenient on dynamic-mode misses (e.g. system.numbers / CTE
  // aliases), so an unresolvable logical lands here with logical_database
  // populated but physical_database empty. is_remote still reflects the
  // index lookup so callers can tell remote-targeted accesses apart even
  // when physical resolution failed elsewhere.
  if (args.logical_database_to_remote_upstream_index().count(logical) > 0) {
    out.is_remote = true;
  }
  return out;
}

void recordAccessedTable(
  rewriter::RewriteSQLResponse *response,
  const std::string &origin_db,
  const std::string &origin_table,
  const TableRewriteSelection &selection) {
  if (origin_table.empty()) return;
  auto *entry = response->add_original_accessed_tables();
  entry->set_original_database(origin_db);
  entry->set_original_table(origin_table);
  const auto resolution = resolveAccessedTable(origin_db, origin_table, selection);
  entry->set_logical_database(resolution.logical_database);
  entry->set_physical_database(resolution.physical_database);
  entry->set_is_remote(resolution.is_remote);
}

void recordTableRewrite(
  rewriter::RewriteSQLResponse *response,
  const std::string &origin_db,
  const std::string &origin_table,
  const std::string &new_db,
  const std::string &new_table) {
  // DB-only rewrites belong in `database_rewrites`; route them out so we
  // don't accidentally produce table_rewrites entries with no table part
  // (which would be ambiguous to parse from the caller's side).
  if (origin_table.empty() && new_table.empty()) {
    recordDatabaseRewrite(response, origin_db, new_db);
    return;
  }
  const std::string key = qualify(origin_db, origin_table);
  const std::string val = qualify(new_db, new_table);
  if (key.empty() || key == val) return;
  (*response->mutable_table_rewrites())[key] = val;
}

void recordDatabaseRewrite(
  rewriter::RewriteSQLResponse *response,
  const std::string &origin_db,
  const std::string &new_db) {
  if (origin_db.empty() || origin_db == new_db) return;
  (*response->mutable_database_rewrites())[origin_db] = new_db;
}

} // namespace rewriter_handlers
