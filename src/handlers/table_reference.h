// src/handlers/table_reference.h — the table-reference policy (housegate spec
// 2026-09-26 §5), mirrored from rewriter-go internal/handlers/preflight.go.
#pragma once

#include <Parsers/ASTAlterQuery.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/IAST.h>
#include <Parsers/Lexer.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "handlers/name_rewrite.h"
#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

// The cross-engine message families (rewriter-go AGENTS.md "Message
// families"). The texts are a contract with rewriter-go, byte for byte.
constexpr std::string_view kIdentifierParameterMessage =
  "query parameters are not supported in a database or table position";
constexpr std::string_view kUnsupportedStatementMessage = "statement is not supported";

// T3: a protected database is any database_map value, any
// protected_databases entry and, while the SI surface is active, any SI
// physical or reserved database (spec §5 "Names").
std::string protectedDatabaseRejectMessage(const std::string &database);
bool protectedDatabase(const std::string &name, const rewriter::RewriteTableDynamicArgs &args);
// Every protected_databases entry must be a simple identifier (spec §6).
bool validateProtectedDatabases(const rewriter::RewriteTableDynamicArgs &args, std::string *error);

// The IN family (in, notIn, nullIn, notNullIn, their global forms and every
// IgnoreSet alias), matched case-insensitively.
bool isInFamilyFunction(const std::string &name);

// True when `node` is an identifier bound to a query parameter
// (`{p:Identifier}`, `db.{p:Identifier}`, `{d:Identifier}.t`).
bool isIdentifierParameter(const DB::IAST *node);

// The (database, table) an IN-operand identifier names, from its parts: one
// part is an unqualified table (a bare quoted `db2.x` included), two or more
// split at the first part (spec 2026-09-26 T4, rewriter-go decodeInOperand).
std::pair<std::string, std::string> inOperandTarget(const DB::ASTIdentifier &identifier);

// T7 (spec §5, rewriter-go precedence step 2): the statement class has no
// handler. A session SET is modelled only while the SI surface is inactive.
bool unmodelledStatementClass(const DB::ASTPtr &ast, const TableRewriteSelection &sel);

// A read inside a statement whose expressions no rewrite reaches, re-checked
// after the handlers accepted the statement (a handler's own
// storage-integrity rejection still wins); the caller refuses it as
// unsupported. rejectUngovernedReads (R2) refuses these first, so this is
// defence in depth, except where noted. It refuses an IN operand naming a
// table (decoded AST names; an enclosing WITH alias is not exempt, since
// ClickHouse binds none there) in an ALTER action, a structured DELETE /
// UPDATE and a CREATE outside its SELECT body, and anything R2's structural
// predicate finds. It is the only guard of an IN operand inside a CREATE's
// `AS <table function>` source (no R2 rule); the ENGINE arguments are R2's
// too (storageHasRead), so this scan only backs R2 up there. A SHOW trailing
// clause is R7's (rejectShowBody).
bool inOperandInUnrewrittenPosition(const DB::ASTPtr &ast);

// A column declaration's own SETTINGS (…) clause in a CREATE column list or
// an ALTER … ADD COLUMN. rewriter-go cannot re-emit it and refuses such a
// statement (`statement is not supported`) at its last step, on a would-be
// Success, in both SI states; ALTER … MODIFY COLUMN … SETTINGS is admitted by
// both engines. The caller applies the same refusal to an accepted statement.
bool columnDeclarationHasSettings(const DB::ASTPtr &ast);

// A literal the embedded 26.3 parser folds into SQL that ClickHouse 25.8 and
// 26.2 read differently (Task I3 fix rounds 2–3, measured on both servers):
//  - N1, a literal tuple wrapped in an extra pair of grouping parentheses,
//    `((1, 2))` or `((1, 2) AS x)`: 25.8 / 26.2 read a one-element tuple of a
//    tuple (`SELECT ((1, 2))` is `((1,2))`, `1 IN ((1, 2))` is error 43), the
//    formatter re-emits the flat `(1, 2)`;
//  - N2, a float literal below the smallest normal double (`1e-400`,
//    `1e-310`, `2.2250738585072013e-308`): 25.8 rejects it (error 62), the
//    formatter re-emits `0.` or a value 25.8 accepts.
// Read from `parsed_sql` (the text the AST was parsed from) up to an INSERT's
// inline data, which is forwarded verbatim. Any such literal in the source
// refuses the statement, whatever the formatter emits (`statement is not
// supported`, both SI states, applied by the caller to an accepted
// statement). A `(` counts as a call's own parenthesis only after a
// non-keyword word naming a call in the tree. Fails closed on a text it
// cannot tokenize or bracket-match.
bool foldsMeaningChangingLiteral(const DB::ASTPtr &ast, const std::string &parsed_sql);

// T5 (spec §5 "Table functions" / "Table engines", rewriter-go
// internal/engine/allowlists.go).
std::string tableFunctionRefusedMessage(const std::string &name);
std::string tableFunctionUnknownMessage(const std::string &name);
enum class TableFunctionClass { Unknown, Refused, DataOnly, External };
TableFunctionClass classifyTableFunction(const std::string &name);
std::string tableEngineRefusedMessage(const std::string &name);
std::string tableEngineUnknownMessage(const std::string &name);
std::string tableSettingRefusedMessage(const std::string &name);
enum class TableEngineClass { Refused, Allowed, Unknown };
TableEngineClass classifyTableEngine(const std::string &name, size_t argument_count);
bool refusedTableSetting(const std::string &name);

// T5: refuse a disallowed source table function, CREATE / materialized-view
// engine, or disk / storage_policy table setting. Returns true when it
// populated a rejection. The preflight runs it while the SI surface is
// inactive; the SI handler paths run it after their own checks while it is
// active, so an SI-owned message wins (rewriter-go rejectDisallowedCarriers).
bool rejectDisallowedCarriers(const DB::ASTPtr &ast, rewriter::RewriteSQLResponse *response);

// T6 (rewriter-go handlers/lookups.go): the refusal of a joinGet /
// joinGetOrNull / dictGet-family call, or of a hasColumnInTable call that is
// not rewritten: `<fn> target "<arg>" does not resolve through the caller's
// databases`; <fn> as written, <arg> "" when not a literal. The preflight
// refuses every lookup call except hasColumnInTable in a rewritten SELECT
// body, which the SELECT path resolves.
std::string stringLookupUnresolvedMessage(const std::string &function, const std::string &arg);

// Spec 2026-09-26 §5 "Unqualified names that do not resolve" (rewriter-go
// nameresolve.UnresolvedUnqualifiedTableMessage): `unqualified table "<t>"
// does not resolve through the session's logical database`, the name inserted
// verbatim like T6.
std::string unresolvedUnqualifiedTableMessage(const std::string &table);

// The name a ClickHouse Lexer BareWord / QuotedIdentifier token denotes,
// decoded exactly as ClickHouse's ParserIdentifier decodes it (backslash
// escapes such as \N and \xHH, doubled quotes, Unicode “…” quotes);
// std::nullopt for any other token and for a name ClickHouse would not
// accept.
std::optional<std::string> decodeIdentifierToken(const DB::Token &token);

// R5 (rewriter-go internal/engine/settings.go): a SQL-bearing or dialect
// setting in a query-level SETTINGS clause or a session SET is a T5
// table-setting refusal; a value that is not a numeric, string or boolean
// literal is `statement is not supported`.
bool sqlBearingSetting(const std::string &name);
// `sql` is the statement text: the AST folds a hex or binary literal into a
// plain integer and a signed non-finite float into a bare one, so those
// literal forms are read from the tokens (rewriter-go refuses them in the same
// positions); null skips that check.
bool rejectQuerySettings(const DB::ASTPtr &ast, rewriter::RewriteSQLResponse *response,
                         const std::string *sql = nullptr);

// R2 (rewriter-go rejectUngovernedReads): a read in a position no rewrite
// reaches is `statement is not supported`.
//   - ALTER (OpaqueTextIsUngoverned): an action is opaque text to the
//     reference engine, so a subquery, a table, a cross-table partition action
//     (FETCH, ATTACH | REPLACE PARTITION … FROM, MOVE PARTITION … TO TABLE),
//     MODIFY QUERY, or an IN-family occurrence whose operand region in `sql`
//     is not literal-only is refused. A quoted IN-family name is matched as
//     ClickHouse decodes it (escapes, Unicode quotes). An ALTER made only of
//     the actions rewriter-go structures (ADD COLUMN, ADD CONSTRAINT, DROP
//     COLUMN) follows the structural rule below instead.
//   - structured UPDATE / DELETE (assignments, predicate, partition) and
//     CREATE column, index, constraint, projection and non-engine storage
//     expressions, a materialized view's inner engines included
//     (ExpressionPositionHasReads): a table, a table function (a `values`
//     list of literals excepted), an IN operand naming a table, or a
//     table-position parameter is refused; a table-free subquery and a tuple
//     IN operand are not reads. No WITH alias binds a name in these positions
//     (measured on ClickHouse 26.2 and 25.8), so a name that matches an
//     enclosing or recursive CTE is still a table.
//   - a REFRESH materialized view is always refused: it reruns its query on a
//     schedule, outside any session.
//   - INSERT: FROM INFILE is always refused. The VALUES rows are structured
//     positions (the rule above), except that rows which do not parse as
//     expressions take the lexical rule below with no SELECT allowed. An
//     INSERT whose column list is followed by a SETTINGS clause keeps its rows
//     or query as opaque text (OpaqueInsertQueryIsUngoverned): a FROM / JOIN /
//     WITH keyword, a second SELECT, an IN-family occurrence whose operand
//     region is not literal-only, a lookup-family call or an Identifier
//     parameter in that text is refused.
// The preflight runs it while the SI surface is inactive; the SI write path
// runs it after the SI write preflight while it is active, so an SI object
// keeps its SI message; `storage_integrity` (the SI write path's selection)
// also names an SI physical or reserved database qualifying a name in opaque
// INSERT text, which no SI handler reads. inOperandInUnrewrittenPosition
// re-checks after the handlers.
bool rejectUngovernedReads(const DB::ASTPtr &ast, const std::string &sql, rewriter::RewriteSQLResponse *response,
                           const TableRewriteSelection *storage_integrity = nullptr);

// The VALUES rows of an INSERT parsed as rewriter-go's parser structures them
// (spec 2026-09-26 R2), for the SI write preflight: empty for a literal-only,
// absent or unparseable text, and for the opaque text that follows a column
// list and a SETTINGS clause.
DB::ASTs insertValuesRows(const DB::ASTPtr &ast);

// R7 (spec 2026-09-26, rewriter-go ShowBodyIsUngoverned): a SHOW statement's
// trailing clauses (WHERE / LIMIT, which ClickHouse keeps outside `children`)
// carry a subquery, a function call, an IN-family operand that is not
// literal-only (read from the AST and, as rewriter-go reads it, from the text
// after the first trailing-clause keyword of `sql`), a query parameter or a
// quoted dotted name. Every SHOW family is covered, the ones rewritten into a
// synthetic SELECT (TABLES, DATABASES) included.
bool showBodyIsUngoverned(const DB::IAST &show, const std::string &sql);
// The SHOW trailing-clause qualified name whose database is an SI physical
// database, for the SI-active message; empty when none.
std::pair<std::string, std::string> showBodyStorageIntegrityName(
  const DB::IAST &show, const rewriter::RewriteTableDynamicArgs &args);
// The SHOW handlers' R7 refusal in dynamic mode (`dynamic` null: no-op): with
// the SI surface active an SI physical name in a trailing clause takes the SI
// message first; then an ungoverned trailing clause is `statement is not
// supported`. Returns true when it populated a rejection. The handlers run it
// after their own target and namespace checks.
bool rejectShowBody(const DB::IAST &show, const std::string &sql,
                    const rewriter::RewriteTableDynamicArgs *dynamic,
                    rewriter::RewriteSQLResponse *response);

// joinGet / joinGetOrNull and the dictGet / dictHas / … / hasColumnInTable
// families (rewriter-go IsStringLookup), matched case-insensitively.
bool isStringLookupFunction(const std::string &name);

// rewriter-go OpaqueInsertQueryText: for an INSERT whose column list is
// followed by a SETTINGS clause, the text from that keyword to the end of
// the statement; std::nullopt for every other INSERT.
std::optional<std::string_view> insertOpaqueQueryText(const DB::ASTInsertQuery &insert, std::string_view sql);

// An ALTER made only of the actions rewriter-go structures (ADD COLUMN, ADD
// CONSTRAINT, DROP COLUMN); every other ALTER's actions are opaque text there.
bool alterIsStructured(const DB::ASTAlterQuery &alter);

// Dynamic-mode table-reference preflight, run right after parsing, in the
// spec's precedence order. Returns true when it populated a rejection; the
// caller returns. Static and no-rewrite requests pass untouched.
//
// The system-table allowlist (spec §5 "The system database") runs right
// after T3, in both SI states. While the SI surface is active and the
// statement also names a storage-integrity object, the SI pipeline answers
// first: the refusal is not issued here but returned in
// `deferred_system_table`, and the caller applies it to the pipeline's
// would-be Success (rejectDeferredSystemTable), so an SI message keeps
// precedence (rewriter-go RejectSystemTablesOnSuccess).
bool preflightTableReferences(const DB::ASTPtr &ast,
  const rewriter::RewriteSQLRequest *request,
  rewriter::RewriteSQLResponse *response,
  std::optional<std::string> *deferred_system_table = nullptr);

} // namespace rewriter_handlers
