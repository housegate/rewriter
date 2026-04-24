// src/handlers/name_rewrite.cc
#include "handlers/name_rewrite.h"

#include <sstream>
#include <unordered_map>

#include "ASTTransformers.h"

namespace rewriter_handlers {

namespace {

// Must match the "db.table" (or bare "table") format used throughout the
// rewrite pipeline — see planTableRewrite()'s contract and the
// RewriteSQLResponse.table_rewrites documentation.
inline std::string qualify(const std::string &db, const std::string &table) {
  return db.empty() ? table : db + "." + table;
}

} // namespace

SingleTableRewriteOutcome applySingleTableRewrite(
  const std::string &origin_db,
  const std::string &origin_table,
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options,
  std::string *cur_db,
  std::string *cur_table,
  std::string_view stmt_kind) {
  SingleTableRewriteOutcome outcome;
  *cur_db = origin_db;
  *cur_table = origin_table;

  bool matched_any = false;
  for (const auto &option : options) {
    if (option.op() != rewriter::RewriteOp::TableNameRewrite) continue;
    const auto &args = option.table_name_args();

    // Build per-option maps (matches SELECT's semantics: each option brings
    // its own fresh maps; it does not merge with earlier options).
    std::unordered_map<std::string, std::string> table_name_map;
    table_name_map.reserve(args.table_map_size());
    for (const auto &kv : args.table_map()) { table_name_map[kv.first] = kv.second; }

    std::unordered_map<std::string, DB::RemoteTable> remote_table_map;
    remote_table_map.reserve(args.remote_table_map_size());
    for (const auto &kv : args.remote_table_map()) {
      DB::RemoteTable r;
      r.addr = kv.second.addr();
      r.database_name = kv.second.database();
      r.table_name = kv.second.table();
      r.user_name = kv.second.user();
      r.password = kv.second.password();
      remote_table_map[kv.first] = r;
    }

    std::unordered_map<std::string, DB::TableWithDatabase> table_with_database_map;
    table_with_database_map.reserve(args.table_with_database_map_size());
    for (const auto &kv : args.table_with_database_map()) {
      DB::TableWithDatabase t;
      t.database_name = kv.second.database();
      t.table_name = kv.second.table();
      table_with_database_map[kv.first] = t;
    }

    const std::string lookup_key = qualify(*cur_db, *cur_table);
    const auto plan = DB::planTableRewrite(
      lookup_key, table_name_map, remote_table_map, table_with_database_map);

    switch (plan.kind) {
    case DB::TableRewriteKind::None:
      break;
    case DB::TableRewriteKind::Rename:
      *cur_table = plan.table;
      matched_any = true;
      break;
    case DB::TableRewriteKind::WithDatabase:
      if (!plan.db.empty()) *cur_db = plan.db;
      *cur_table = plan.table;
      matched_any = true;
      break;
    case DB::TableRewriteKind::Remote: {
      std::ostringstream oss;
      oss << stmt_kind << " target '" << lookup_key
          << "' matches remote_table_map; remote_table_map only applies to SELECT";
      outcome.status = SingleTableRewriteStatus::RejectRemote;
      outcome.reject_reason = oss.str();
      return outcome;
    }
    }
  }

  outcome.status = matched_any
    ? SingleTableRewriteStatus::Rewritten
    : SingleTableRewriteStatus::NoChange;
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
  if (key == val) return;
  (*response->mutable_table_rewrites())[key] = val;
}

} // namespace rewriter_handlers
