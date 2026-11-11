// src/handlers/system_tables.cc — see system_tables.h.
#include "handlers/system_tables.h"

#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>
#include <Parsers/ASTAlterQuery.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTQueryWithTableAndOutput.h>
#include <Parsers/ASTRenameQuery.h>
#include <Parsers/ASTShowColumnsQuery.h>
#include <Parsers/ASTShowIndexesQuery.h>
#include <Parsers/ASTDescribeCacheQuery.h>
#include <Parsers/ASTShowEngineQuery.h>
#include <Parsers/ASTShowFunctionsQuery.h>
#include <Parsers/ASTShowProcesslistQuery.h>
#include <Parsers/ASTShowSettingQuery.h>
#include <Parsers/ASTViewTargets.h>
#include <Parsers/Access/ASTShowAccessEntitiesQuery.h>
#include <Parsers/Access/ASTShowAccessQuery.h>
#include <Parsers/Access/ASTShowCreateAccessEntityQuery.h>
#include <Parsers/Access/ASTShowGrantsQuery.h>
#include <Parsers/Access/ASTShowPrivilegesQuery.h>
#include <Parsers/Lexer.h>

#include <algorithm>
#include <cctype>
#include <set>

#include "handlers/common.h"
#include "handlers/cte_scope.h"
#include "handlers/sql_tokens.h"
#include "handlers/select.h"
#include "handlers/storage_integrity.h"
#include "handlers/table_reference.h"

namespace rewriter_handlers {

namespace {

const std::set<std::string> &systemTableAllowlist() {
  static const std::set<std::string> allowlist = {
    // class (b): static or server-global information that shows nothing of
    // another tenant
    "aggregate_function_combinators", "azure_queue_settings", "build_options", "codecs", "collations",
    "contributors", "dashboards", "data_type_families", "database_engines", "formats", "functions",
    "jemalloc_bins", "jemalloc_stats", "keywords", "licenses", "merge_tree_settings", "numbers",
    "numbers_mt", "one", "primes", "privileges", "replicated_merge_tree_settings", "s3_queue_settings",
    "settings", "settings_changes", "table_engines", "table_functions", "time_zones", "tokenizers",
    "unicode", "warnings", "zeros", "zeros_mt",
    // class (b*): global topology, read by the Sentio driver on every connect
    "clusters",
    // class (c): per-database metadata the Sentio driver reads by physical
    // name. They still show every tenant's names (and parts partition
    // values, mutations command text): a known residual that step 2
    // (housegate/housegate#218) closes by serving them as a filtered logical
    // rewrite.
    "tables", "columns", "data_skipping_indices", "projections", "parts", "databases", "mutations"};
  return allowlist;
}

// rewriter-go systemTableAwaitingFilteredRead: the class (c) tables no known
// client reads through housegate, refused until step 2 serves them as
// filtered logical reads; their refusal names that path.
const std::set<std::string> &systemTableAwaitingFilteredRead() {
  static const std::set<std::string> tables = {
    "completions", "parts_columns", "projection_parts", "projection_parts_columns"};
  return tables;
}

// rewriter-go nameresolve.FilteredReadIssue.
constexpr std::string_view kFilteredReadIssue = "housegate/housegate#218";

using Refs = std::vector<ObjectRef>;

// rewriter-go ClickHouseIdentifierValue: a name that still carries a
// backslash is decoded again with ClickHouse's identifier rules. ClickHouse's
// parser already decoded every AST name once, so a second decode only turns
// a name spelled with an escaped backslash into another name; the check only
// refuses, so it errs toward refusing, as rewriter-go does.
std::string clickHouseIdentifierValue(const std::string &name) {
  if (name.find('\\') == std::string::npos) return name;
  std::string quoted = "`";
  for (const char c : name) {
    if (c == '`') quoted += '`';
    quoted += c;
  }
  quoted += '`';
  try {
    DB::ReadBufferFromMemory buf(quoted.data(), quoted.size());
    std::string value;
    DB::readBackQuotedStringWithSQLStyle(value, buf);
    if (!buf.eof()) return name;
    return value;
  } catch (...) {
    return name;
  }
}

void addTarget(Refs &out, const std::string &database, const std::string &table) {
  if (table.empty()) return;
  out.push_back({clickHouseIdentifierValue(database), clickHouseIdentifierValue(table), true});
}

// rewriter-go sourceNameValue: a token read as a name the way ClickHouse's
// lexer reads an identifier — a bare word as spelled, a quoted identifier
// decoded (escapes, doubled quotes, Unicode “…” verbatim). A string, number
// or operator is not a name.
std::optional<std::string> tokenName(const DB::Token &token) {
  if (token.type != DB::TokenType::BareWord && token.type != DB::TokenType::QuotedIdentifier) return std::nullopt;
  return decodeIdentifierToken(token);
}

// rewriter-go qualifiedSourceRuns: every `<db> . <name>` run whose first part
// is not itself the table half of a longer run. `<db> .` followed by
// anything that is not a name (`system.*`) names no static object. A run of
// three or more parts is reported as (first, last) — ClickHouse's SHOW
// COLUMNS / INDEX reading — before (first, second) — an expression's
// reading — so that either refused name refuses.
bool qualifiedSourceRuns(std::string_view text, Refs &out) {
  std::vector<DB::Token> tokens;
  if (!significantTokens(text, tokens)) return false;
  const size_t n = tokens.size();
  for (size_t i = 0; i + 1 < n; ++i) {
    if (tokens[i + 1].type != DB::TokenType::Dot) continue;
    if (i > 0 && tokens[i - 1].type == DB::TokenType::Dot) continue;
    const auto database = tokenName(tokens[i]);
    if (!database) continue;
    ObjectRef ref{*database, "", false};
    if (i + 2 < n) {
      if (const auto table = tokenName(tokens[i + 2])) {
        ref.table = *table;
        ref.exact = true;
      } else {
        ref.table = std::string(tokens[i + 2].begin, tokens[i + 2].end);
      }
    }
    if (ref.exact) {
      size_t j = i + 2;
      while (j + 2 < n && tokens[j + 1].type == DB::TokenType::Dot) {
        const auto last = tokenName(tokens[j + 2]);
        if (!last) {
          out.push_back({*database, std::string(tokens[j + 2].begin, tokens[j + 2].end), false});
          break;
        }
        j += 2;
        if (j + 1 >= n || tokens[j + 1].type != DB::TokenType::Dot) out.push_back({*database, *last, true});
      }
    }
    out.push_back(std::move(ref));
  }
  return true;
}

// A statement rewriter-go's parser keeps as a command node (measured on the
// pinned Polyglot): its text is read as qualified runs before anything else.
bool commandStatement(const DB::IAST &ast) {
  using Kind = DB::IAST::QueryKind;
  switch (ast.getQueryKind()) {
  case Kind::Show:
  case Kind::Exists:
  case Kind::Describe:
  case Kind::Rename:
  case Kind::Grant:
  case Kind::Revoke:
  case Kind::Use:
    return true;
  default:
    break;
  }
  if (const auto *alter = ast.as<DB::ASTAlterQuery>(); alter && alter->command_list) {
    for (const auto &child : alter->command_list->children) {
      const auto *command = child ? child->as<DB::ASTAlterCommand>() : nullptr;
      if (command && command->type == DB::ASTAlterCommand::UPDATE) return true;
    }
  }
  return false;
}

// rewriter-go AllWriteTargets, plus the targets a command statement names
// (EXISTS / SHOW CREATE, SHOW COLUMNS / INDEX, RENAME sides).
void collectWriteTargets(const DB::IAST &ast, Refs &out) {
  if (const auto *insert = ast.as<DB::ASTInsertQuery>()) addTarget(out, insert->getDatabase(), insert->getTable());
  if (const auto *drop = ast.as<DB::ASTDropQuery>(); drop && drop->database_and_tables) {
    for (const auto &target : drop->database_and_tables->children) {
      const auto *id = target ? dynamic_cast<const DB::ASTIdentifier *>(target.get()) : nullptr;
      if (!id || id->isParam()) continue;
      if (id->name_parts.size() == 1) addTarget(out, "", id->name_parts[0]);
      else if (id->name_parts.size() >= 2) addTarget(out, id->name_parts[0], id->name_parts[1]);
    }
  } else if (const auto *q = dynamic_cast<const DB::ASTQueryWithTableAndOutput *>(&ast)) {
    addTarget(out, q->getDatabase(), q->getTable());
  }
  if (const auto *create = ast.as<DB::ASTCreateQuery>()) {
    // Every named target, of every kind: an MV's TO table and a TimeSeries
    // table's SAMPLES / TAGS / METRICS / RECENT SAMPLES tables (the latter
    // are refused earlier as an unmodelled CREATE shape, T7; read here too so
    // no target escapes this check).
    if (create->targets) {
      for (const auto &target : create->targets->targets) {
        if (!create->targets->hasTableID(target.kind)) continue;
        const auto id = create->targets->getTableID(target.kind);
        addTarget(out, id.database_name, id.table_name);
      }
    }
    addTarget(out, create->as_database, create->as_table);
  }
  if (const auto *alter = ast.as<DB::ASTAlterQuery>(); alter && alter->command_list) {
    for (const auto &child : alter->command_list->children) {
      const auto *command = child ? child->as<DB::ASTAlterCommand>() : nullptr;
      if (!command) continue;
      addTarget(out, command->from_database, command->from_table);
      addTarget(out, command->to_database, command->to_table);
    }
  }
  if (const auto *rename = ast.as<DB::ASTRenameQuery>()) {
    for (const auto &element : rename->getElements()) {
      addTarget(out, element.from.getDatabase(), element.from.getTable());
      addTarget(out, element.to.getDatabase(), element.to.getTable());
    }
  }
  if (const auto *columns = ast.as<DB::ASTShowColumnsQuery>()) addTarget(out, columns->database, columns->table);
  if (const auto *indexes = ast.as<DB::ASTShowIndexesQuery>()) addTarget(out, indexes->database, indexes->table);
}

// rewriter-go's namespace visitor over a table function's or table engine's
// arguments: a merge() / Merge pattern, and any non-static argument, is
// never an allowlist match.
void addNamespace(Refs &out, const NamespaceRef &ref) {
  if (ref.database.empty() && !ref.uses_current_database) return;
  const std::string lname = lowerName(ref.name);
  const bool exact = ref.resolved && !ref.table.empty() && lname != "merge";
  out.push_back({clickHouseIdentifierValue(ref.database), exact ? clickHouseIdentifierValue(ref.table) : ref.table,
                 exact});
}

// The read sources and carriers, in walker order (ClickHouse keeps a SELECT's
// WITH, select list, FROM, WHERE … in that order in `children`; Task I1's
// registerFieldOnlyChildren adopted the members kept outside it). A name a
// WITH alias in scope binds is the CTE, not a table.
void collectReadSources(const DB::ASTPtr &node, const CTEAliasScope &scope, Refs &out) {
  if (!node) return;
  if (const auto *id = node->as<DB::ASTTableIdentifier>(); id && !id->isParam()) {
    const auto &parts = id->name_parts;
    if (parts.size() == 1) {
      if (!scope.bindsTable(parts[0])) addTarget(out, "", parts[0]);
    } else if (parts.size() >= 2) {
      addTarget(out, parts[0], parts[1]);
    }
  }
  if (const auto *storage = node->as<DB::ASTStorage>(); storage && storage->engine) {
    if (const auto *engine = storage->engine->as<DB::ASTFunction>()) {
      if (auto ref = decodeTableEngineNamespace(*engine)) addNamespace(out, *ref);
    }
  }
  if (const auto *fn = node->as<DB::ASTFunction>()) {
    if (auto ref = decodeTableFunctionNamespace(*fn)) addNamespace(out, *ref);
    if (isInFamilyFunction(fn->name) && fn->arguments && fn->arguments->children.size() == 2) {
      // An IN operand is a table: unqualified in the session database, or
      // split at its first part (rewriter-go decodeInOperand).
      const auto &operand = fn->arguments->children[1];
      const auto *id = operand ? dynamic_cast<const DB::ASTIdentifier *>(operand.get()) : nullptr;
      if (id && !id->isParam() && !id->name_parts.empty()) {
        const auto [database, table] = inOperandTarget(*id);
        if (!(database.empty() && scope.bindsInOperand(table))) addTarget(out, database, table);
      }
    }
  }
  visitChildrenWithCTEVisibility(*node, scope, [&](const DB::ASTPtr &child, const CTEAliasScope &child_scope) {
    collectReadSources(child, child_scope, out);
  });
}

// The texts rewriter-go keeps opaque in a structured statement: the actions
// of an ALTER that is not structured throughout (the whole statement text
// here: its target is a write target already), the query text after an
// INSERT column list and SETTINGS, and a view column list's INDEX /
// PROJECTION / PRIMARY KEY items (rewriter-go reads their source text; the
// formatted item names the same identifiers).
std::vector<std::string> opaqueTexts(const DB::IAST &ast, const std::string &sql) {
  std::vector<std::string> texts;
  if (const auto *alter = ast.as<DB::ASTAlterQuery>(); alter && !alterIsStructured(*alter)) texts.push_back(sql);
  if (const auto *insert = ast.as<DB::ASTInsertQuery>()) {
    if (auto text = insertOpaqueQueryText(*insert, sql)) texts.emplace_back(*text);
  }
  if (const auto *create = ast.as<DB::ASTCreateQuery>();
      create && (create->is_ordinary_view || create->is_materialized_view) && create->columns_list) {
    // Formatted in place: a registered tree is never cloned (common.h).
    const auto format = [](const DB::IAST &item) {
      return item.formatWithPossiblyHidingSensitiveData(0, true, true, false, kQuotingRule, kQuotingStyle, true);
    };
    const auto *columns = create->columns_list;
    for (const DB::IAST *list : {static_cast<const DB::IAST *>(columns->indices),
           static_cast<const DB::IAST *>(columns->projections)}) {
      if (!list) continue;
      for (const auto &item : list->children)
        if (item) texts.push_back(format(*item));
    }
    for (const DB::IAST *key : {columns->primary_key, columns->primary_key_from_columns})
      if (key) texts.push_back(format(*key));
  }
  return texts;
}

std::optional<std::string> stringLiteralArg(const DB::ASTPtr &node) {
  const auto *literal = node ? node->as<DB::ASTLiteral>() : nullptr;
  if (!literal || literal->value.getType() != DB::Field::Types::String) return std::nullopt;
  return literal->value.safeGet<String>();
}

std::string unwrapQuoted(const std::string &value) {
  if (value.size() >= 2 && (value.front() == '`' || value.front() == '"') && value.back() == value.front())
    return value.substr(1, value.size() - 2);
  return value;
}

// rewriter-go collectStringLookupOccurrences as CollectObjectRefs reads it:
// hasColumnInTable's database and table literals, or the joinGet / dictGet
// families' first argument split at its first dot.
void collectLookupTargets(const DB::IAST &node, Refs &out) {
  if (const auto *fn = node.as<DB::ASTFunction>();
      fn && isStringLookupFunction(fn->name) && fn->arguments && !fn->arguments->children.empty()) {
    const auto &args = fn->arguments->children;
    if (lowerName(fn->name).rfind("hascolumnintable", 0) == 0) {
      if (args.size() >= 3) {
        const auto database = stringLiteralArg(args[args.size() - 3]);
        const auto table = stringLiteralArg(args[args.size() - 2]);
        if (database && table) addTarget(out, *database, *table);
      }
    } else if (const auto value = staticNamespaceArgValue(args[0])) {
      if (const auto dot = value->find('.'); dot != std::string::npos && dot > 0)
        addTarget(out, unwrapQuoted(value->substr(0, dot)), unwrapQuoted(value->substr(dot + 1)));
    }
  }
  for (const auto &child : node.children)
    if (child) collectLookupTargets(*child, out);
}

bool isSystemDatabase(const std::string &database, const std::string &context) {
  return database.empty() ? context == kSystemDatabase : database == kSystemDatabase;
}

void clearRewriteMaps(rewriter::RewriteSQLResponse *response) {
  response->clear_table_rewrites();
  response->clear_database_rewrites();
  response->clear_privileges_deltas();
  response->clear_failed_cte_aliases();
}

void recordSystemTable(rewriter::RewriteSQLResponse *response, const SystemTableRefusal &refusal,
                       const TableRewriteSelection &selection) {
  if (refusal.table.empty()) return;
  for (const auto &entry : response->original_accessed_tables())
    if (entry.original_database() == refusal.database && entry.original_table() == refusal.table) return;
  recordAccessedTable(response, refusal.database, refusal.table, selection);
}

// The word after SHOW and its prefixes (rewriter-go DBLevelInfo.ShowWhat /
// ShowWhatNext), uppercased; nullopt when the text does not tokenize (the
// caller then fails closed, as for an unknown kind).
std::optional<std::pair<std::string, std::string>> showKind(const std::string &sql) {
  std::vector<DB::Token> tokens;
  if (!significantTokens(sql, tokens)) return std::nullopt;
  size_t i = 0;
  if (bareWordUpper(tokens, i) != "SHOW") return std::pair<std::string, std::string>{};
  ++i;
  for (const char *prefix : {"EXTENDED", "FULL", "TEMPORARY"})
    if (bareWordUpper(tokens, i) == prefix) ++i;
  const std::string what = i < tokens.size() ? upperWord(std::string(tokens[i].begin, tokens[i].end)) : std::string();
  const std::string next =
    i + 1 < tokens.size() ? upperWord(std::string(tokens[i + 1].begin, tokens[i + 1].end)) : std::string();
  return std::pair<std::string, std::string>{what, next};
}

// rewriter-go systemTableShowRefusal: the table a SHOW kind reads when it is
// outside the allowlist, or nullopt for a kind that reads an allowed table
// (CLUSTER[S], SETTINGS, CHANGED SETTINGS, SETTING, FUNCTIONS, ENGINES,
// PRIVILEGES). `unknown` reports a kind in neither list.
std::optional<std::string> showSystemTable(const std::string &what, const std::string &next, bool *unknown) {
  *unknown = false;
  if (what == "PROCESSLIST") return "processes";
  if (what == "MERGES") return "merges";
  if (what == "DICTIONARIES") return "dictionaries";
  if (what == "USERS" || what == "ACCESS") return "users";
  if (what == "ROLES") return "roles";
  if (what == "GRANTS") return "grants";
  if (what == "PROFILES") return "settings_profiles";
  if (what == "ROW" || what == "POLICIES") return "row_policies";
  if (what == "QUOTA") return "quota_usage";
  if (what == "QUOTAS") return "quotas";
  if (what == "FILESYSTEM" || what == "CACHES") return "filesystem_cache_settings";
  if (what == "CURRENT") return next == "QUOTA" ? "quota_usage" : "current_roles";
  if (what == "ENABLED") return "enabled_roles";
  if (what == "SETTINGS") {
    if (next == "PROFILES") return "settings_profiles";
    return std::nullopt;
  }
  static const std::set<std::string> kept = {"CLUSTER", "CLUSTERS", "FUNCTIONS", "ENGINES", "CHANGED", "SETTING",
    "PRIVILEGES", "TABLES", "DATABASES", "CREATE", "COLUMNS", "FIELDS", "INDEX", "INDEXES", "INDICES", "KEYS"};
  if (!kept.count(what)) *unknown = true;
  return std::nullopt;
}

// rewriter-go showKindClass: the kinds the SHOW dispatch models without a
// target (answered in both SI states).
bool showTargetLess(const std::string &what) {
  static const std::set<std::string> kinds = {"CLUSTER", "CLUSTERS", "SETTINGS", "MERGES", "CACHES", "PROCESSLIST",
    "FUNCTIONS", "GRANTS", "USERS", "ROLES", "ROW", "QUOTA", "QUOTAS", "PROFILES", "POLICIES", "ACCESS", "ENGINES",
    "FILESYSTEM"};
  return kinds.count(what) > 0;
}

void rejectStatement(rewriter::RewriteSQLResponse *response, rewriter::RewriteCode code, const std::string &message) {
  response->clear_statement_type();
  response->clear_original_accessed_tables();
  clearRewriteMaps(response);
  response->set_code(code);
  response->set_message(message);
}

} // namespace

bool systemTableAllowed(const std::string &table) {
  return systemTableAllowlist().count(table) > 0;
}

std::string systemTableRefusedMessage(const std::string &table) {
  std::string message = "system table " + std::string(kSystemDatabase) + "." + table + " is not accessible";
  if (systemTableAwaitingFilteredRead().count(table))
    message += "; tenant-filtered metadata reads are tracked in " + std::string(kFilteredReadIssue);
  return message;
}

bool metadataDatabaseRefused(const std::string &database) {
  return database == "information_schema" || database == "INFORMATION_SCHEMA";
}

std::string metadataDatabaseRefusedMessage(const std::string &database) {
  return "database " + database + " is not accessible; tenant-filtered metadata reads are tracked in " +
         std::string(kFilteredReadIssue);
}

std::optional<std::vector<ObjectRef>> collectObjectRefs(const DB::ASTPtr &ast, const std::string &sql,
                                                        const DB::ASTs *values_rows) {
  Refs out;
  if (!ast) return out;
  const bool command = commandStatement(*ast);
  if (command && !qualifiedSourceRuns(sql, out)) return std::nullopt;
  collectWriteTargets(*ast, out);
  collectReadSources(ast, CTEAliasScope{}, out);
  // An INSERT's VALUES rows are kept outside the AST (ClickHouse reads them
  // from the data); rewriter-go structures them. They are an R2 position: no
  // WITH binds there.
  const DB::ASTs parsed_rows = values_rows ? DB::ASTs{} : insertValuesRows(ast);
  const DB::ASTs &rows = values_rows ? *values_rows : parsed_rows;
  for (const auto &row : rows) collectReadSources(row, unboundScope(), out);
  if (!command) {
    for (const auto &text : opaqueTexts(*ast, sql))
      if (!qualifiedSourceRuns(text, out)) return std::nullopt;
  }
  collectLookupTargets(*ast, out);
  for (const auto &row : rows) collectLookupTargets(*row, out);
  return out;
}

SystemTableScan scanSystemTables(const DB::ASTPtr &ast, const std::string &sql,
                                 const rewriter::RewriteTableDynamicArgs &args, const DB::ASTs *values_rows) {
  SystemTableScan scan;
  const auto refs = collectObjectRefs(ast, sql, values_rows);
  if (!refs) {
    scan.failed = true;
    return scan;
  }
  const std::string &context = args.upstream_logical_database_in_context();
  const bool active = storageIntegritySurfaceActive(args);
  for (const auto &ref : *refs) {
    if (isSystemDatabase(ref.database, context)) {
      if (!scan.refused && !(ref.exact && systemTableAllowed(ref.table))) {
        scan.refused = true;
        scan.refusal = {std::string(kSystemDatabase), ref.table};
      }
      continue;
    }
    const std::string database = ref.database.empty() ? context : ref.database;
    if (metadataDatabaseRefused(database)) {
      // information_schema: every table, pattern or name is refused.
      if (!scan.refused) {
        scan.refused = true;
        scan.refusal = {database, ref.table};
      }
      continue;
    }
    if (!active || scan.storage_integrity_object) continue;
    if (isStorageIntegrityPhysicalDatabase(database, args)
        || (!ref.exact && isStorageIntegrityLogicalDatabase(database, args))
        || lookupStorageIntegrity(ref.database, ref.table, args))
      scan.storage_integrity_object = true;
  }
  return scan;
}

void rejectSystemTable(rewriter::RewriteSQLResponse *response, const SystemTableRefusal &refusal,
                       const std::string &sql, const TableRewriteSelection &selection) {
  response->clear_statement_type();
  clearRewriteMaps(response);
  recordSystemTable(response, refusal, selection);
  response->set_sql_after_rewrite(sql);
  response->set_code(rewriter::RewriteCode::UnsupportedStatement);
  response->set_message(refusal.database == kSystemDatabase ? systemTableRefusedMessage(refusal.table)
                                                            : metadataDatabaseRefusedMessage(refusal.database));
}

void rejectDeferredSystemTable(rewriter::RewriteSQLResponse *response, const SystemTableRefusal &refusal,
                               const std::string &sql, const TableRewriteSelection &selection) {
  if (response->code() != rewriter::RewriteCode::Success) return;
  rejectSystemTable(response, refusal, sql, selection);
}

void rejectShowSystemTable(rewriter::RewriteSQLResponse *response, const std::string &table) {
  rejectStatement(response, rewriter::RewriteCode::UnsupportedStatement, systemTableRefusedMessage(table));
}

bool showTargetIsMultiPart(const std::string &sql) {
  std::vector<DB::Token> tokens;
  // A text the lexer cannot tokenize is refused like a multi-part target
  // (fail closed; the parser shares the lexer, so a parsed statement always
  // tokenizes).
  if (!significantTokens(sql, tokens)) return true;
  const auto word = [&](size_t at) { return bareWordUpper(tokens, at); };
  size_t i = 0;
  if (word(i++) != "SHOW") return false;
  for (const char *prefix : {"EXTENDED", "FULL"})
    if (word(i) == prefix) ++i;
  static const std::set<std::string> kinds = {"COLUMNS", "FIELDS", "INDEX", "INDEXES", "INDICES", "KEYS"};
  if (!kinds.count(word(i++))) return false;
  if (const auto clause = word(i++); clause != "FROM" && clause != "IN") return false;
  size_t parts = 0;
  while (i < tokens.size() && tokenName(tokens[i])) {
    ++parts;
    if (i + 1 < tokens.size() && tokens[i + 1].type == DB::TokenType::Dot) i += 2;
    else break;
  }
  return parts >= 3;
}

bool handleShowSystemFormQuery(const DB::ASTPtr &ast, const rewriter::RewriteSQLRequest *request,
                               rewriter::RewriteSQLResponse *response) {
  if (!(ast->as<DB::ASTShowProcesslistQuery>() || ast->as<DB::ASTShowAccessEntitiesQuery>()
        || ast->as<DB::ASTShowGrantsQuery>() || ast->as<DB::ASTShowAccessQuery>()
        || ast->as<DB::ASTShowCreateAccessEntityQuery>() || ast->as<DB::ASTShowFunctionsQuery>()
        || ast->as<DB::ASTShowEnginesQuery>() || ast->as<DB::ASTShowSettingQuery>()
        || ast->as<DB::ASTShowPrivilegesQuery>() || ast->as<DB::ASTDescribeCacheQuery>()))
    return false;
  const auto selection = findActiveTableRewrite(request->options());
  if (selection.mode != TableRewriteMode::Dynamic) return false;
  const bool active = storageIntegritySurfaceActive(*selection.dynamic_args);
  // R5 outranks every refusal below, in both SI states (rewriter-go; with the
  // surface inactive the preflight already ran it).
  if (rejectQuerySettings(ast, response, &request->sql())) return true;
  // SHOW CREATE of an access entity and DESCRIBE FILESYSTEM CACHE read access
  // storage and the cache registry; rewriter-go refuses both in both states.
  if (ast->as<DB::ASTShowCreateAccessEntityQuery>() || ast->as<DB::ASTDescribeCacheQuery>()) {
    rejectStatement(response, rewriter::RewriteCode::UnsupportedStatement, std::string(kUnsupportedStatementMessage));
    return true;
  }
  const auto kind = showKind(request->sql());
  if (!kind) {
    // A text the lexer cannot tokenize names no kind: an unknown kind, refused
    // in both SI states (with the SI catch-all while the surface is active).
    rejectStatement(response, rewriter::RewriteCode::UnsupportedStatement,
                    std::string(active ? kStorageIntegrityUnmodelledMessage : kUnsupportedStatementMessage));
    return true;
  }
  const auto &[what, next] = *kind;
  bool unknown = false;
  const auto table = showSystemTable(what, next, &unknown);
  if (showTargetLess(what)) {
    if (table) {
      rejectShowSystemTable(response, *table);
      return true;
    }
    // An allowed kind (FUNCTIONS, ENGINES) passes in both SI states.
    handleSelectQuery(ast, request, response);
    return true;
  }
  if (active) {
    // A kind the SI surface does not model: the SI catch-all answers.
    rejectStatement(response, rewriter::RewriteCode::UnsupportedStatement,
                    std::string(kStorageIntegrityUnmodelledMessage));
    return true;
  }
  if (table) {
    rejectShowSystemTable(response, *table);
    return true;
  }
  if (unknown) {
    rejectStatement(response, rewriter::RewriteCode::UnsupportedStatement, std::string(kUnsupportedStatementMessage));
    return true;
  }
  return false;  // SETTING, PRIVILEGES: the ordinary pass-through
}

} // namespace rewriter_handlers
