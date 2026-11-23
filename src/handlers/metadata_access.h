// src/handlers/metadata_access.h — RewriteSQLRequest.refuse_metadata_access,
// the opt-in metadata-access refusal for callers outside dynamic mode.
//
// Static-mode callers (the Sentio analytics engine) send their own table maps
// and get none of the dynamic-mode table-reference policy, so a caller-typed
// `SELECT * FROM system.query_log` or `SELECT currentDatabase()` used to pass
// through unchanged. This is the rule upstream sql-rewriter enforced
// unconditionally (#23 / #24) for that caller, extended with the dynamic-mode
// deployment-identity list and the string lookups; with the flag set it
// applies in every mode, before and after the rewrite options.
#pragma once

#include <Parsers/IAST.h>

#include <string>
#include <string_view>

namespace rewriter_handlers {

constexpr std::string_view kMetadataAccessDisabledMessage = "metadata access is disabled";

// Whether a statement reads ClickHouse metadata, as
// RewriteSQLRequest.refuse_metadata_access documents: a reference to the
// `system` / `information_schema` database (case-insensitive) in a table,
// target, USE, SHOW COLUMNS / INDEX or IN-operand position; SHOW TABLES /
// DATABASES; a table function or table engine that can reach those
// databases; a string lookup naming them; a call or bare name revealing the
// deployment (version() and the dynamic-mode identity list).
//
// `ast` is a parsed tree with its field-only members registered
// (registerFieldOnlyChildren), so walking `children` reaches every subtree;
// `sql` is the text it was parsed from (the identity-call scan reads tokens).
bool readsMetadata(const DB::ASTPtr &ast, const std::string &sql);

} // namespace rewriter_handlers
