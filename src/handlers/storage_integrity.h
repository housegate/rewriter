// src/handlers/storage_integrity.h — housegate Spec G policy helpers.
#pragma once

#include <Parsers/IAST.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rewriter.grpc.pb.h"

namespace DB {
class ASTFunction;
}

namespace rewriter_handlers {

constexpr std::string_view kDefaultReservedRowIdColumn = "_hg_row_id";
constexpr std::string_view kStorageIntegrityContractMessage =
  "storage-integrity contract version V1 or V2 is required";
constexpr std::string_view kStorageIntegrityWrapperMessage =
  "FINAL/SAMPLE/PREWHERE/WITH OFFSET/column aliases on storage-integrity tables are not supported";
constexpr std::string_view kStorageIntegrityUnmodelledMessage =
  "storage-integrity is configured; statement class is not modelled by the rewriter and cannot be forwarded";

struct StorageIntegrityHit {
  const rewriter::StorageIntegrityArgs::Table *table = nullptr;
  std::string logical_key;
  std::string logical_database;
};

// Validate every proof field before acknowledgement. A V1 request with an
// empty table map is legacy/no-op and is not activated by the caller; a V2
// request is activated by version (see storageIntegritySurfaceActive). Under
// V2 every reserved_databases entry must be a simple identifier; V1 ignores
// that field entirely.
bool validateStorageIntegrity(const rewriter::RewriteTableDynamicArgs &args, std::string *error);

// True when the request activates the storage-integrity surface: any V2
// request, even with an empty table map (housegate sub-project 3, H6), or any
// other request whose table map is non-empty. Mirrors rewriter-go's
// nameresolve.StorageIntegritySurfaceActive.
bool storageIntegritySurfaceActive(const rewriter::RewriteTableDynamicArgs &args);

// True when the request carries contract V2, whose only write-policy
// difference from V1 is that a plain DROP TABLE of a logical SI table is
// accepted. Mirrors rewriter-go's nameresolve.StorageIntegrityDropContract.
bool storageIntegrityDropContract(const rewriter::RewriteTableDynamicArgs &args);

std::optional<StorageIntegrityHit> lookupStorageIntegrity(const std::string &origin_db,
  const std::string &origin_table,
  const rewriter::RewriteTableDynamicArgs &args);

bool isStorageIntegrityPhysicalDatabase(
  const std::string &database, const rewriter::RewriteTableDynamicArgs &args);
bool isStorageIntegrityLogicalDatabase(
  const std::string &database, const rewriter::RewriteTableDynamicArgs &args);
std::vector<std::string> storageIntegrityPhysicalDatabases(
  const rewriter::RewriteTableDynamicArgs &args);
std::string storageIntegrityExecutionDatabase(const rewriter::RewriteTableDynamicArgs &args);

bool authorizeStorageIntegrityLogical(const std::string &origin_db,
  const rewriter::RewriteTableDynamicArgs &args,
  std::string *logical_database);

std::string reservedRowIdColumn(const rewriter::RewriteTableDynamicArgs &args);
std::string storageIntegrityWriteRejectMessage(const std::string &logical_key);
std::string storageIntegrityUnauthorizedMessage(const std::string &logical_database);
std::string storageIntegrityPhysicalRejectMessage(const std::string &physical_name);
std::string storageIntegrityPhysicalDatabaseRejectMessage(const std::string &physical_database);

std::string storageIntegritySurfaceSQL(
  const rewriter::StorageIntegrityArgs::Table &table, const rewriter::StorageIntegrityArgs &args);
DB::ASTPtr buildStorageIntegrityDerivedTable(
  const rewriter::StorageIntegrityArgs::Table &table, const rewriter::StorageIntegrityArgs &args);
std::string restoreStorageIntegrityProjectionSyntax(
  std::string sql, const std::string &reserved_column);
bool astReferencesIdentifier(const DB::ASTPtr &ast, const std::string &name);
// Scope-aware variant of astReferencesIdentifier: a reference counts only when
// the query block owning it reads a storage-integrity table, or when its
// qualifier is bound to one in that block or an enclosing one. An identically
// named column on an ordinary table is legitimate (Spec I D7a).
bool astReferencesIdentifierInScope(const DB::ASTPtr &ast,
  const std::string &name,
  const rewriter::RewriteTableDynamicArgs &args);
std::string describeMetadataSQL(const std::string &safe_table, const std::string &reserved_column);

// Response helpers keep SI classification present on every reject, including
// direct protocol-owned physical namespaces and unresolved indirect targets.
void recordStorageIntegrityAccess(rewriter::RewriteSQLResponse *response,
  const std::string &original_database,
  const std::string &original_table,
  const std::string &logical_database,
  const std::string &physical_database);
void recordLogicalStorageIntegrityAccess(rewriter::RewriteSQLResponse *response,
  const std::string &original_database,
  const std::string &original_table,
  const rewriter::RewriteTableDynamicArgs &args);
void recordPhysicalStorageIntegrityAccess(rewriter::RewriteSQLResponse *response,
  const std::string &original_database,
  const std::string &original_table,
  const std::string &physical_database);

enum class NamespaceRefSource {
  TableFunction,
  InTable,
  TableEngine,
  DictionarySource,
};

struct NamespaceRef {
  NamespaceRefSource source = NamespaceRefSource::TableFunction;
  std::string name;
  std::string database;
  std::string table;
  bool resolved = false;
  bool uses_current_database = false;
  // InTable only: the SELECT handler rewrites this operand itself (see
  // collectStorageIntegrityNamespaceRefs), so the exact-key check is skipped.
  bool rewritten_read = false;
};

// One read-bearing AST event in grammar/source order. Keeping ordinary table
// nodes and indirect namespace refs in one stream makes the first SI object
// win even when the two source kinds are interleaved (Spec I D2).
struct StorageIntegrityReadRef {
  enum class Kind { Table, Namespace } kind = Kind::Table;
  std::string database;
  std::string table;
  NamespaceRef namespace_ref;
  // Table only: database/table come from splitting a one-part quoted name
  // (`db1.t`) at its first dot. The name is one table of the current
  // database, so it never becomes the derived read of the SI table the split
  // names; that reading can only be refused, never admitted.
  bool split_one_part = false;
  // Table only: the table sits inside the arguments of a table function
  // other than view() (viewIfPermitted, viewExplain, executable, …). An
  // INSERT … SELECT / CTAS body never admits an Active table there as a
  // derived read (spec 2026-09-26 T4); T5 decides the function itself.
  bool inside_non_view_table_function = false;
};

// Compatibility shim for SI syntax accepted by the Go parser but absent from
// the pinned ClickHouse parser (WITH OFFSET, table alias column lists, and
// star RENAME). It tokenizes source SQL so literals/comments cannot trigger
// policy, rejects SI-bound wrappers, and temporarily removes ordinary-bound
// syntax so it can be restored after the AST rewrite.
struct StorageIntegritySQLCompatibility {
  struct Restore {
    enum class Kind { WithOffset, AliasColumns } kind;
    std::string database;
    std::string table;
    std::string alias;
    std::string suffix;
  };
  std::string original_sql;
  std::string parse_sql;
  std::vector<Restore> restores;
};

bool prepareStorageIntegritySQL(const std::string &sql,
  const rewriter::RewriteTableDynamicArgs &args,
  rewriter::RewriteSQLResponse *response,
  StorageIntegritySQLCompatibility *compatibility);
void restoreStorageIntegritySQL(
  rewriter::RewriteSQLResponse *response, const StorageIntegritySQLCompatibility &compatibility);

// How a one-part IN operand is read by collectStorageIntegrityNamespaceRefs.
enum class InOperandReading {
  // The conservative default, required wherever the operand reaches
  // ClickHouse verbatim (mutations): a dotted one-part name is split at its
  // first dot (`db1.t` is refused as the SI table db1.t) and an Active table
  // is refused.
  Split,
  // A write statement whose body the SELECT pipeline rewrites (INSERT …
  // SELECT, CTAS, view bodies): inside the rewritten body a one-part operand
  // is read like SelectRewrite (the whole name against the current database,
  // exempt from the exact-SI-key refusal: the rewrite turns an Active table
  // into the derived read); in an R2 position of the statement (unbound) the
  // split reading above, and also the whole name. Either refusal wins there.
  SplitAndWhole,
  // The SELECT handler, which rewrites the operand itself: the whole name
  // against the current database only, exempt from the exact-SI-key refusal
  // because the rewrite turns an Active table into the derived read.
  SelectRewrite,
};
std::vector<NamespaceRef> collectStorageIntegrityNamespaceRefs(const DB::ASTPtr &ast,
  InOperandReading reading = InOperandReading::Split);

// The refs minus the qualified (two or more parts) IN-operand targets: the
// SELECT walk collects those as read sources keyed by (database, table)
// identity, so its target check classifies them (spec 2026-09-26 T4). A
// one-part operand stays: it reads through the current database, and whether
// that database is resolvable or an SI physical one is only decided here
// (rejectStorageIntegrityNamespaces, which exempts an IN ref from the
// exact-SI-key check because the derived read handles an Active table).
std::vector<NamespaceRef> withoutQualifiedInOperandRefs(std::vector<NamespaceRef> refs);

// The (database, table) a namespace-carrying table function or table engine
// names in its literal arguments, or nullopt for any other function. Shared
// with the table-reference preflight (T3), which reads only `database`.
std::optional<NamespaceRef> decodeTableFunctionNamespace(const DB::ASTFunction &fn);
std::optional<NamespaceRef> decodeTableEngineNamespace(const DB::ASTFunction &engine);

// The static string a namespace-carrying argument spells: a string literal's
// value or an identifier's dotted name; nullopt for anything else.
std::optional<std::string> staticNamespaceArgValue(const DB::ASTPtr &arg);
// Whether a WITH alias binds a later FROM / IN name (cte_scope.h). A SELECT
// body binds its CTEs and qualifying WITH / projection aliases; ClickHouse
// binds none in a structured mutation, a column / constraint / storage
// expression or a structured ALTER action (measured on 26.2 and 25.8:
// `DELETE … WHERE a IN (WITH c AS (…) SELECT x FROM c)` reads the table c, and
// so do WITH RECURSIVE, CHECK / ASSUME, TTL and INDEX expressions), so R2
// collects those with every name read as a table (Unbound).
enum class CTEBinding { Bound, Unbound };
std::vector<StorageIntegrityReadRef> collectStorageIntegrityReadRefs(const DB::ASTPtr &ast,
  CTEBinding binding = CTEBinding::Bound, InOperandReading reading = InOperandReading::Split);
bool rejectStorageIntegrityNamespaces(rewriter::RewriteSQLResponse *response,
  const std::vector<NamespaceRef> &refs,
  const rewriter::RewriteTableDynamicArgs &args,
  rewriter::RewriteCode code);

// Refuse any statement whose root is not a SELECT-family node while the v1
// storage-integrity contract is active: reaching the SELECT dispatch tail with
// such a root means no handler modelled the statement, so nothing checked it
// against the protocol-owned namespaces (Spec I D1). Returns true when the
// response was turned into a rejection.
bool rejectUnmodelledStorageIntegrityStatement(const DB::ASTPtr &ast,
  const rewriter::RewriteTableDynamicArgs &args,
  rewriter::RewriteSQLResponse *response);

// Upgrade an already-rejected response's message so it names the
// storage-integrity object the statement addressed (Spec I D2). Message-only:
// Success is never touched, a response that already carries an SI-flagged
// accessed table keeps its own message, and a statement that names nothing
// storage-integrity keeps the caller's message. Only names `ast` proves count
// (rewriter-go AnnotateStorageIntegrityRejectAST): `ast` is the statement as
// parsed, before any handler rewrote it. Never throws: it runs from a
// destructor, and on an internal error the caller's message is kept.
void annotateStorageIntegrityReject(rewriter::RewriteSQLResponse *response,
  const DB::ASTPtr &ast,
  const rewriter::RewriteTableDynamicArgs &args);

} // namespace rewriter_handlers
