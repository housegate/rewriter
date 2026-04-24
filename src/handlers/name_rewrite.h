// src/handlers/name_rewrite.h
//
// Shared helper for applying TableNameRewrite to statements that touch a
// single named table slot (INSERT/CREATE/DROP/ALTER/UPDATE/DELETE target,
// EXISTS TABLE, SHOW CREATE TABLE, each leg of RENAME).
//
// SELECT uses ASTReplaceTransformer directly to walk the AST tree; these
// single-target paths don't need traversal, just a lookup on one (db, table).
#pragma once

#include <google/protobuf/repeated_ptr_field.h>
#include <string>

#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

enum class SingleTableRewriteStatus {
  NoChange,      // No TableNameRewrite option matched. (*cur_db, *cur_table) untouched.
  Rewritten,     // Matched table_name_map or table_with_database_map. (*cur_db, *cur_table)
                 // updated to post-rewrite form.
  RejectRemote,  // Matched remote_table_map. Caller must reject the whole statement —
                 // `reject_reason` carries a human-readable explanation.
};

struct SingleTableRewriteOutcome {
  SingleTableRewriteStatus status = SingleTableRewriteStatus::NoChange;
  std::string reject_reason;  // populated when status == RejectRemote
};

// Walks every TableNameRewrite option in `options` and applies its maps to
// (*cur_db, *cur_table). The lookup key is qualifyTable(origin_db, origin_table)
// for the first pass, and qualifyTable(*cur_db, *cur_table) for subsequent
// passes (so chained rewrites like `foo -> bar -> baz` work).
//
// On match:
//   - Rename: short-table-name changes, db preserved.
//   - WithDatabase: both (or short-only if the map's db field is empty) change.
//   - Remote: returns RejectRemote immediately; `stmt_kind` is embedded in the
//     reject_reason (e.g. "INSERT target cannot be rewritten to remote()").
//
// Statements that reach a Remote match MUST NOT be accepted; they are not
// expressible as remote() (INSERT/ALTER/etc. can't target a remote function
// meaningfully, and we never reshape the target AST for writes).
SingleTableRewriteOutcome applySingleTableRewrite(
  const std::string &origin_db,
  const std::string &origin_table,
  const google::protobuf::RepeatedPtrField<rewriter::RewriteOption> &options,
  std::string *cur_db,
  std::string *cur_table,
  std::string_view stmt_kind);

// Append an {origin -> rewritten} entry to response.table_rewrites. No-op when
// origin == rewritten (the contract is: only actual rewrites appear in the map).
void recordTableRewrite(
  rewriter::RewriteSQLResponse *response,
  const std::string &origin_db,
  const std::string &origin_table,
  const std::string &new_db,
  const std::string &new_table);

} // namespace rewriter_handlers
