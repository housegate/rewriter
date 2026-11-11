// src/handlers/table_reference.cc — see table_reference.h.
#include "handlers/table_reference.h"
#include "handlers/common.h"
#include "handlers/sql_tokens.h"

#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>
#include <Parsers/ASTAlterQuery.h>
#include <Parsers/ASTColumnDeclaration.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTDeleteQuery.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTColumnsTransformers.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTWithAlias.h>
#include <Parsers/CommonParsers.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTQueryWithTableAndOutput.h>
#include <Parsers/ASTRenameQuery.h>
#include <Parsers/ASTProjectionSelectQuery.h>
#include <Parsers/ASTQueryParameter.h>
#include <Parsers/ASTSelectIntersectExceptQuery.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTSetQuery.h>
#include <Parsers/ASTDescribeCacheQuery.h>
#include <Parsers/ASTShowColumnsQuery.h>
#include <Parsers/ASTShowEngineQuery.h>
#include <Parsers/ASTShowIndexesQuery.h>
#include <Parsers/ASTShowProcesslistQuery.h>
#include <Parsers/ASTShowTablesQuery.h>
#include <Parsers/ASTSubquery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ASTUpdateQuery.h>
#include <Parsers/ASTUseQuery.h>
#include <Parsers/ASTViewTargets.h>
#include <Parsers/Access/ASTShowAccessQuery.h>
#include <Parsers/Access/ASTShowPrivilegesQuery.h>
#include <Parsers/ExpressionListParsers.h>
#include <Parsers/Lexer.h>
#include <Parsers/parseQuery.h>
#include <Parsers/TablePropertiesQueriesASTs.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "handlers/storage_integrity.h"
#include "handlers/system_tables.h"
#include "handlers/writes.h"

namespace rewriter_handlers {

namespace {

bool simpleIdentifierName(const std::string &value) {
  if (value.empty()) return false;
  for (size_t i = 0; i < value.size(); ++i) {
    const auto c = static_cast<unsigned char>(value[i]);
    if (!(std::isalpha(c) || c == '_' || (i > 0 && std::isdigit(c)))) return false;
  }
  return true;
}

bool surfaceActive(const TableRewriteSelection &sel) {
  return sel.mode == TableRewriteMode::Dynamic && storageIntegritySurfaceActive(*sel.dynamic_args);
}

// Visits every child of `node`, and the AST-valued fields ClickHouse keeps
// outside `children` (the SHOW trailing clauses).
void forEachChild(const DB::IAST &node, const std::function<void(const DB::ASTPtr &)> &visit) {
  for (const auto &child : node.children) visit(child);
  if (const auto *show = node.as<DB::ASTShowTablesQuery>()) {
    if (show->where_expression) visit(show->where_expression);
    if (show->limit_length) visit(show->limit_length);
  } else if (const auto *columns = node.as<DB::ASTShowColumnsQuery>()) {
    if (columns->where_expression) visit(columns->where_expression);
    if (columns->limit_length) visit(columns->limit_length);
  } else if (const auto *indexes = node.as<DB::ASTShowIndexesQuery>()) {
    if (indexes->where_expression) visit(indexes->where_expression);
  }
}

// An alias written as a query parameter (`AS {name:Identifier}`). ClickHouse's
// formatter never prints ASTWithAlias::parametrised_alias, so every rewrite of
// such a statement would silently drop the alias.
bool anyParametrisedAlias(const DB::IAST &node) {
  if (const auto *with_alias = dynamic_cast<const DB::ASTWithAlias *>(&node);
      with_alias && with_alias->parametrised_alias)
    return true;
  bool found = false;
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!found && child) found = anyParametrisedAlias(*child);
  });
  return found;
}

// A `STREAM [BOUNDED] [UNORDERED] [CURSOR …] [WATERMARK …]` table expression
// (ClickHouse 26.8): a streaming read no handler models. The SI rewrite
// replaces the table expression it is bound to, and no supported backend
// before 26.8 parses it.
bool anyStreamTableExpression(const DB::IAST &node) {
  if (const auto *te = node.as<DB::ASTTableExpression>(); te && te->stream_settings) return true;
  bool found = false;
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!found && child) found = anyStreamTableExpression(*child);
  });
  return found;
}

// A `LOCAL` or `NATURAL` join. ClickHouse 26.8 reads `LOCAL` / `NATURAL`
// after a table expression as join keywords and prints them, where 25.8 and
// 26.3 read a bare `local` / `natural` there as the table's alias (and 26.3
// printed no LOCAL): `FROM numbers(3) local JOIN t` would reach such a backend
// as `numbers(3) AS local` and a plain join. A NATURAL join has no ON / USING
// those servers accept.
bool anyLocalOrNaturalJoin(const DB::IAST &node) {
  if (const auto *join = node.as<DB::ASTTableJoin>();
      join && (join->locality == DB::JoinLocality::Local || join->is_natural))
    return true;
  bool found = false;
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!found && child) found = anyLocalOrNaturalJoin(*child);
  });
  return found;
}

// A MergeTree `UNIQUE KEY` (ClickHouse 26.8): a storage expression neither
// R2 nor the storage-integrity write path inspects (the AST member
// enumeration found a read of an SI physical table there answered Success
// under contract V2), and no backend before 26.8 parses it.
bool anyStorageUniqueKey(const DB::IAST &node) {
  if (const auto *storage = node.as<DB::ASTStorage>(); storage && storage->unique_key) return true;
  bool found = false;
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!found && child) found = anyStorageUniqueKey(*child);
  });
  return found;
}

// ClickHouse 26.8 shapes of otherwise admitted statement classes that no
// handler models (see preflightTableReferences).
bool unmodelledClickHouse268Shape(const DB::IAST &ast) {
  // A view-target clause other than a materialized view's TO target: a
  // TimeSeries table's SAMPLES / TAGS / METRICS / RECENT SAMPLES target (a
  // named table, an INNER UUID, INNER COLUMNS or an inner ENGINE), or INNER
  // COLUMNS on any target. No handler reads or rewrites those targets, and
  // the inner column declarations are outside every R2 position.
  if (const auto *create = ast.as<DB::ASTCreateQuery>(); create && create->targets) {
    for (const auto &target : create->targets->targets) {
      if ((target.kind != DB::ViewTarget::To && target.kind != DB::ViewTarget::Inner) || target.inner_columns)
        return true;
    }
  }
  // `ALTER TABLE … MODIFY SENTIO BINARY TYPE ORIGIN '…'`: the fork's
  // privileged one-time registration of how a table's stored binary type
  // headers are read. Every tenant reaches ClickHouse as one shared user, so
  // ClickHouse's privilege check separates nothing; a tenant must never
  // issue it.
  if (const auto *alter = ast.as<DB::ASTAlterQuery>(); alter && alter->command_list) {
    for (const auto &child : alter->command_list->children) {
      const auto *command = child ? child->as<DB::ASTAlterCommand>() : nullptr;
      if (command && command->type == DB::ASTAlterCommand::MODIFY_SENTIO_BINARY_TYPE_ORIGIN) return true;
    }
  }
  // `DESCRIBE TEMPORARY TABLE t` names a session temporary table, which the
  // DESCRIBE handler would resolve as a table of the session database.
  if (const auto *describe = ast.as<DB::ASTDescribeQuery>(); describe && describe->temporary) return true;
  return anyStreamTableExpression(ast) || anyLocalOrNaturalJoin(ast) || anyStorageUniqueKey(ast);
}

bool anyIdentifierParameter(const DB::IAST &node) {
  if (isIdentifierParameter(&node)) return true;
  bool found = false;
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!found && child) found = anyIdentifierParameter(*child);
  });
  return found;
}

// The trailing clauses (WHERE / LIMIT) ClickHouse keeps outside `children`
// for every SHOW family: SHOW TABLES / DATABASES / DICTIONARIES / CLUSTERS /
// MERGES / SETTINGS / CACHES (ASTShowTablesQuery) and SHOW COLUMNS / INDEX.
std::vector<const DB::IAST *> showBodies(const DB::IAST &show) {
  std::vector<const DB::IAST *> out;
  if (const auto *tables = show.as<DB::ASTShowTablesQuery>()) {
    out = {tables->where_expression.get(), tables->limit_length.get()};
  } else if (const auto *columns = show.as<DB::ASTShowColumnsQuery>()) {
    out = {columns->where_expression.get(), columns->limit_length.get()};
  } else if (const auto *indexes = show.as<DB::ASTShowIndexesQuery>()) {
    out = {indexes->where_expression.get()};
  }
  out.erase(std::remove(out.begin(), out.end(), nullptr), out.end());
  return out;
}

// An Identifier query parameter in any position of `node`: bound to a name
// (`{p:Identifier}`, `db.{p:Identifier}`) or held as a value substitution
// whose type is Identifier.
bool anyIdentifierSubstitution(const DB::IAST &node) {
  if (isIdentifierParameter(&node)) return true;
  if (const auto *param = node.as<DB::ASTQueryParameter>(); param && param->type == "Identifier") return true;
  bool found = false;
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!found && child) found = anyIdentifierSubstitution(*child);
  });
  return found;
}

// An IN-family call with its two arguments. The parser unwraps an operand's
// parentheses and drops a unary plus, so arguments[1] is the operand at
// every depth (`a IN ((x))`, `a IN +x`).
const DB::ASTFunction *inFamilyCall(const DB::IAST &node) {
  const auto *fn = node.as<DB::ASTFunction>();
  if (!fn || !isInFamilyFunction(fn->name) || !fn->arguments
      || fn->arguments->children.size() != 2)
    return nullptr;
  return fn;
}

// T2 over every database or table position (rewriter-go
// TablePositionParameter). A column-position parameter is not refused.
bool tablePositionParameter(const DB::IAST &node) {
  if (const auto *te = node.as<DB::ASTTableExpression>()) {
    if (isIdentifierParameter(te->database_and_table_name.get())) return true;
  }
  if (const auto *fn = inFamilyCall(node)) {
    if (isIdentifierParameter(fn->arguments->children[1].get())) return true;
  }
  if (const auto *q = dynamic_cast<const DB::ASTQueryWithTableAndOutput *>(&node)) {
    if (isIdentifierParameter(q->database.get()) || isIdentifierParameter(q->table.get())) return true;
  }
  if (const auto *drop = node.as<DB::ASTDropQuery>()) {
    // `DROP TABLE a, b` keeps its targets in database_and_tables (database and
    // table stay null); only a single-table list is folded into them.
    if (drop->database_and_tables) {
      for (const auto &target : drop->database_and_tables->children)
        if (isIdentifierParameter(target.get())) return true;
    }
  }
  if (const auto *insert = node.as<DB::ASTInsertQuery>()) {
    if (isIdentifierParameter(insert->database.get()) || isIdentifierParameter(insert->table.get()))
      return true;
  }
  if (const auto *rename = node.as<DB::ASTRenameQuery>()) {
    for (const auto &e : rename->getElements()) {
      if (isIdentifierParameter(e.from.database.get()) || isIdentifierParameter(e.from.table.get())
          || isIdentifierParameter(e.to.database.get()) || isIdentifierParameter(e.to.table.get()))
        return true;
    }
  }
  if (const auto *use = node.as<DB::ASTUseQuery>()) {
    if (isIdentifierParameter(use->database)) return true;
  }
  if (const auto *show = node.as<DB::ASTShowTablesQuery>()) {
    if (isIdentifierParameter(show->from)) return true;
  }
  // A SHOW trailing clause is command text to the reference engine, like an
  // ALTER action: an Identifier parameter anywhere in one is refused
  // (rewriter-go refuses `SHOW … WHERE name = {p:Identifier}` with T2).
  for (const auto *body : showBodies(node))
    if (anyIdentifierSubstitution(*body)) return true;
  if (const auto *create = node.as<DB::ASTCreateQuery>()) {
    // `MV … TO db.{p:Identifier}` keeps the target as a parameterised AST.
    if (create->targets) {
      for (const auto kind : create->targets->getKinds())
        if (create->targets->hasTableASTWithQueryParams(kind)) return true;
    }
  }
  if (const auto *alter = node.as<DB::ASTAlterQuery>()) {
    // Every ALTER action is opaque text to the reference engine: a parameter
    // anywhere in one is refused (rewriter-go AGENTS precedence step 1).
    if (alter->command_list && anyIdentifierParameter(*alter->command_list)) return true;
  }
  bool found = false;
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!found && child) found = tablePositionParameter(*child);
  });
  return found;
}

std::string stripIdentifierQuotes(std::string value) {
  if (value.size() >= 2 && (value.front() == '`' || value.front() == '"') && value.back() == value.front())
    return value.substr(1, value.size() - 2);
  return value;
}

// The name a BareWord or QuotedIdentifier token denotes, decoded exactly as
// ClickHouse's ParserIdentifier decodes it: a backquoted or double-quoted
// name through read{Back,Double}QuotedStringWithSQLStyle (backslash escapes
// such as \xHH, doubled quote characters), an English Unicode double-quoted
// name verbatim between its quotes. Every lexical scanner here matches a
// function name through it, since ClickHouse resolves the decoded name
// (`\x69n`(a, t) is IN). std::nullopt for any other token, and for a quoted
// name ClickHouse would not accept (empty, or an undecodable escape).
std::optional<std::string> identifierTokenName(const DB::Token &token) {
  if (token.type == DB::TokenType::BareWord) return std::string(token.begin, token.end);
  if (token.type != DB::TokenType::QuotedIdentifier || token.size() < 2) return std::nullopt;
  if (*token.begin == '\xE2') {
    if (token.size() <= 6) return std::nullopt;
    return std::string(token.begin + 3, token.end - 3);
  }
  try {
    DB::ReadBufferFromMemory buf(token.begin, token.size());
    std::string name;
    if (*token.begin == '`')
      DB::readBackQuotedStringWithSQLStyle(name, buf);
    else
      DB::readDoubleQuotedStringWithSQLStyle(name, buf);
    if (name.empty()) return std::nullopt;
    return name;
  } catch (...) {
    return std::nullopt;
  }
}

// The database a string-lookup literal names: "db.table" / "db.`t.x`" → db.
std::string lookupLiteralDatabase(const std::string &literal) {
  const auto dot = literal.find('.');
  if (dot == std::string::npos) return {};
  return stripIdentifierQuotes(literal.substr(0, dot));
}

// The database an unquoted qualified argument (`joinGet(db.t, …)`) names.
std::optional<std::string> qualifiedIdentifierDatabase(const DB::ASTPtr &node) {
  const auto *id = node ? dynamic_cast<const DB::ASTIdentifier *>(node.get()) : nullptr;
  if (!id || id->isParam() || id->name_parts.size() != 2) return std::nullopt;
  return id->name_parts.front();
}

// The value of a plain string-literal argument (an identifier is decoded
// structurally by qualifiedIdentifierDatabase instead).
std::optional<std::string> stringLiteral(const DB::ASTPtr &node) {
  if (!node || !node->as<DB::ASTLiteral>()) return std::nullopt;
  return staticNamespaceArgValue(node);
}

// joinGet / joinGetOrNull and the dictGet / dictHas / … / hasColumnInTable
// families (rewriter-go IsStringLookup), matched case-insensitively.
bool isStringLookupName(const std::string &name) {
  const std::string lower = lowerName(name);
  if (lower == "joinget" || lower == "joingetornull") return true;
  for (const char *prefix : {"dictget", "dicthas", "dictgethierarchy", "dictisin",
         "dictgetchildren", "dictgetdescendants", "hascolumnintable"})
    if (lower.rfind(prefix, 0) == 0) return true;
  return false;
}

bool isShowBody(const DB::IAST &node, const DB::ASTPtr &child) {
  if (const auto *show = node.as<DB::ASTShowTablesQuery>())
    return child == show->where_expression || child == show->limit_length;
  if (const auto *columns = node.as<DB::ASTShowColumnsQuery>())
    return child == columns->where_expression || child == columns->limit_length;
  if (const auto *indexes = node.as<DB::ASTShowIndexesQuery>())
    return child == indexes->where_expression;
  return false;
}

struct DatabaseRef {
  std::string database;
  bool blind = false;  // a string-lookup argument: no SI handler classifies it
};

// T3 collection (rewriter-go CollectDatabaseReferenceSets). `opaque` marks an
// ALTER action or a SHOW trailing clause: rewriter-go reads those as text, so
// only qualified names count there, never table-function or lookup literals.
void collectDatabases(const DB::IAST &node, bool opaque, std::vector<DatabaseRef> &out) {
  auto add = [&](const std::string &db, bool blind = false) {
    if (!db.empty()) out.push_back({db, blind});
  };
  // IAST::as<T>() matches the exact type; ASTTableIdentifier derives from
  // ASTIdentifier, so the identifier check needs a dynamic_cast.
  if (const auto *id = dynamic_cast<const DB::ASTIdentifier *>(&node)) {
    const bool table_identifier = node.as<DB::ASTTableIdentifier>() != nullptr;
    if ((table_identifier || opaque) && id->name_parts.size() >= 2) add(id->name_parts.front());
  }
  if (const auto *fn = inFamilyCall(node)) {
    const auto *operand = fn->arguments->children[1]->as<DB::ASTIdentifier>();
    if (operand && operand->name_parts.size() >= 2) add(operand->name_parts.front());
  }
  // Statement targets: write / DDL targets, EXISTS / SHOW CREATE, CREATE /
  // DROP DATABASE, RENAME sides, USE, SHOW … FROM, CREATE … AS, MV TO, and an
  // ALTER partition action's FROM / TO table.
  if (const auto *q = dynamic_cast<const DB::ASTQueryWithTableAndOutput *>(&node)) add(q->getDatabase());
  if (const auto *command = node.as<DB::ASTAlterCommand>()) {
    add(command->from_database);
    add(command->to_database);
  }
  if (const auto *insert = node.as<DB::ASTInsertQuery>()) add(insert->getDatabase());
  if (const auto *rename = node.as<DB::ASTRenameQuery>()) {
    for (const auto &e : rename->getElements()) {
      add(e.from.getDatabase());
      add(e.to.getDatabase());
    }
  }
  if (const auto *use = node.as<DB::ASTUseQuery>()) add(use->getDatabase());
  if (const auto *show = node.as<DB::ASTShowTablesQuery>()) add(show->getFrom());
  if (const auto *columns = node.as<DB::ASTShowColumnsQuery>()) add(columns->database);
  if (const auto *indexes = node.as<DB::ASTShowIndexesQuery>()) add(indexes->database);
  if (const auto *create = node.as<DB::ASTCreateQuery>()) {
    add(create->as_database);
    if (create->targets && create->hasTargetTableID(DB::ViewTarget::To))
      add(create->getTargetTableID(DB::ViewTarget::To).database_name);
  }
  // Carrier arguments: a table function's or table engine's literal
  // database, and a string-lookup literal (SI-handler-blind).
  if (!opaque) {
    if (const auto *te = node.as<DB::ASTTableExpression>()) {
      if (const auto *fn = te->table_function ? te->table_function->as<DB::ASTFunction>() : nullptr) {
        if (auto ref = decodeTableFunctionNamespace(*fn)) add(ref->database);
      }
    }
    // The function an INSERT INTO FUNCTION writes through and the one a
    // CREATE TABLE … AS fn() copies: T3 precedes the later T5 / class refusal
    // of these statements (spec 2026-09-26 §5; rewriter-go parity).
    const DB::IAST *statement_function = nullptr;
    if (const auto *insert = node.as<DB::ASTInsertQuery>()) statement_function = insert->table_function.get();
    if (const auto *create = node.as<DB::ASTCreateQuery>()) statement_function = create->as_table_function;
    if (const auto *fn = statement_function ? statement_function->as<DB::ASTFunction>() : nullptr) {
      if (auto ref = decodeTableFunctionNamespace(*fn)) add(ref->database);
    }
    if (const auto *create = node.as<DB::ASTCreateQuery>(); create && create->storage && create->storage->engine) {
      if (auto ref = decodeTableEngineNamespace(*create->storage->engine)) add(ref->database);
    }
    // A MATERIALIZED VIEW's ENGINE lives in the view targets.
    if (const auto *create = node.as<DB::ASTCreateQuery>(); create && create->targets) {
      for (const auto *inner : create->targets->getInnerEngines()) {
        if (!inner || !inner->engine) continue;
        if (auto ref = decodeTableEngineNamespace(*inner->engine)) add(ref->database);
      }
    }
    if (const auto *fn = node.as<DB::ASTFunction>(); fn && isStringLookupName(fn->name) && fn->arguments) {
      const auto &args = fn->arguments->children;
      if (lowerName(fn->name).rfind("hascolumnintable", 0) == 0) {
        if (args.size() >= 3) {
          const auto &arg = args[args.size() - 3];
          if (auto db = qualifiedIdentifierDatabase(arg)) add(*db, true);
          else if (auto literal = stringLiteral(arg)) add(stripIdentifierQuotes(*literal), true);
        }
      } else if (!args.empty()) {
        if (auto db = qualifiedIdentifierDatabase(args[0])) add(*db, true);
        else if (auto literal = stringLiteral(args[0])) add(lookupLiteralDatabase(*literal), true);
      }
    }
  }
  const auto *alter = node.as<DB::ASTAlterQuery>();
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!child) return;
    const bool in_commands = alter && child.get() == alter->command_list;
    collectDatabases(*child, opaque || in_commands || isShowBody(node, child), out);
  });
}

} // namespace

std::string protectedDatabaseRejectMessage(const std::string &database) {
  return "protected database " + database + " is not addressable";
}

bool protectedDatabase(const std::string &name, const rewriter::RewriteTableDynamicArgs &args) {
  if (name.empty()) return false;
  for (const auto &[_, physical] : args.database_map())
    if (physical == name) return true;
  for (const auto &entry : args.protected_databases())
    if (entry == name) return true;
  return storageIntegritySurfaceActive(args) && isStorageIntegrityPhysicalDatabase(name, args);
}

bool validateProtectedDatabases(const rewriter::RewriteTableDynamicArgs &args, std::string *error) {
  for (const auto &entry : args.protected_databases()) {
    if (!simpleIdentifierName(entry)) {
      if (error) *error = "protected_databases entry \"" + entry + "\" must be a simple identifier";
      return false;
    }
  }
  // A logical database routed to a ClickHouse metadata database would turn
  // every table of the logical database into a metadata table: the
  // system-table allowlist (spec 2026-09-26 §5) only sees `system.<name>`
  // when the caller writes it. Checked in key order so the message is stable.
  std::vector<std::pair<std::string, std::string>> mappings(args.database_map().begin(), args.database_map().end());
  std::sort(mappings.begin(), mappings.end());
  for (const auto &[logical, physical] : mappings) {
    if (physical == "system" || physical == "information_schema" || physical == "INFORMATION_SCHEMA") {
      if (error) *error = "database_map entry \"" + logical + "\" maps to the metadata database \"" + physical + "\"";
      return false;
    }
  }
  return true;
}

bool isInFamilyFunction(const std::string &name) {
  std::string lower = lowerName(name);
  constexpr std::string_view ignore_set = "ignoreset";
  if (lower.ends_with(ignore_set)) lower.resize(lower.size() - ignore_set.size());
  static const std::set<std::string> family = {"in", "notin", "nullin", "notnullin",
    "globalin", "globalnotin", "globalnullin", "globalnotnullin"};
  return family.count(lower) > 0;
}

bool isIdentifierParameter(const DB::IAST *node) {
  const auto *id = node ? dynamic_cast<const DB::ASTIdentifier *>(node) : nullptr;
  return id && id->isParam();
}

// Classes whose AST reports QueryKind::None although rewriter-go models them:
// the target-less SHOW forms and DESCRIBE FILESYSTEM CACHE name no table, so
// they are not T7. The set is explicit (every other None-kind class stays
// unmodelled). handleShowSystemFormQuery answers them in dynamic mode (the
// system-table allowlist refuses PROCESSLIST, ACCESS and DESCRIBE
// FILESYSTEM CACHE).
bool admittedKindNoneClass(const DB::ASTPtr &ast) {
  return ast->as<DB::ASTShowProcesslistQuery>() || ast->as<DB::ASTShowEnginesQuery>()
    || ast->as<DB::ASTShowPrivilegesQuery>() || ast->as<DB::ASTShowAccessQuery>()
    || ast->as<DB::ASTDescribeCacheQuery>();
}

std::pair<std::string, std::string> inOperandTarget(const DB::ASTIdentifier &identifier) {
  const auto &parts = identifier.name_parts;
  if (parts.size() == 1) return {"", parts.front()};
  std::string table = parts[1];
  for (size_t i = 2; i < parts.size(); ++i) table += "." + parts[i];
  return {parts.front(), table};
}

namespace {

// The table function a DESCRIBE names as its target (`DESCRIBE TABLE
// numbers(10)`), or nullptr.
const DB::ASTFunction *describeTargetFunction(const DB::IAST &ast) {
  const auto *describe = ast.as<DB::ASTDescribeQuery>();
  const auto *te = describe && describe->table_expression
    ? describe->table_expression->as<DB::ASTTableExpression>() : nullptr;
  return te && te->table_function ? te->table_function->as<DB::ASTFunction>() : nullptr;
}

bool containsQuery(const DB::IAST &node) {
  if (node.as<DB::ASTSelectWithUnionQuery>() || node.as<DB::ASTSelectQuery>()
      || node.as<DB::ASTSelectIntersectExceptQuery>() || node.as<DB::ASTSubquery>())
    return true;
  for (const auto &child : node.children)
    if (child && containsQuery(*child)) return true;
  return false;
}

} // namespace

bool unmodelledStatementClass(const DB::ASTPtr &ast, const TableRewriteSelection &sel) {
  using Kind = DB::IAST::QueryKind;
  if (admittedKindNoneClass(ast)) return false;
  switch (ast->getQueryKind()) {
  case Kind::Select:
  case Kind::Insert:
  case Kind::Delete:
  case Kind::Update:
  case Kind::Rename:
  case Kind::Grant:
  case Kind::Revoke:
  case Kind::Use:
  case Kind::Show:
  case Kind::Exists:
    return false;
  case Kind::Describe: {
    // DESCRIBE (SELECT …) is an unmodelled shape, refused before its names
    // are examined (rewriter-go AGENTS precedence step 8).
    const auto *describe = ast->as<DB::ASTDescribeQuery>();
    const auto *te = describe && describe->table_expression
      ? describe->table_expression->as<DB::ASTTableExpression>() : nullptr;
    if (te && te->subquery) return true;
    // So is a DESCRIBE of an admitted table function whose arguments carry a
    // query (`DESCRIBE TABLE view(SELECT …)`): the DESCRIBE handler forwards
    // the statement verbatim, so the query would reach ClickHouse unrewritten
    // (rewriter-go refuses it the same way). A refused or unknown function
    // is left to T5, which names it.
    const auto *fn = describeTargetFunction(*ast);
    if (!fn) return false;
    const auto cls = classifyTableFunction(fn->name);
    return (cls == TableFunctionClass::DataOnly || cls == TableFunctionClass::External) && containsQuery(*fn);
  }
  case Kind::Alter: {
    const auto *alter = ast->as<DB::ASTAlterQuery>();
    return !alter || alter->alter_object != DB::ASTAlterQuery::AlterObjectType::TABLE;
  }
  case Kind::Create: {
    // ATTACH, CREATE DICTIONARY / WINDOW VIEW and every access-entity or
    // function CREATE (their own AST classes) are unmodelled.
    const auto *create = ast->as<DB::ASTCreateQuery>();
    return !create || create->attach || create->is_dictionary || create->is_window_view;
  }
  case Kind::Drop: {
    const auto *drop = ast->as<DB::ASTDropQuery>();
    return !drop || drop->kind == DB::ASTDropQuery::Kind::Detach;
  }
  case Kind::Set:
    return !ast->as<DB::ASTSetQuery>() || surfaceActive(sel);
  default:
    return true;
  }
}

namespace {

// A table-function source, except a `values` whose arguments carry no query:
// it only spells literal rows (rewriter-go reads it as a VALUES list and
// refuses a query inside one; a table in its arguments is still found by the
// caller's walk).
bool hasTableFunctionSource(const DB::IAST &node) {
  if (const auto *te = node.as<DB::ASTTableExpression>(); te && te->table_function) {
    const auto *fn = te->table_function->as<DB::ASTFunction>();
    if (!fn || lowerName(fn->name) != "values" || containsQuery(*fn)) return true;
  }
  bool found = false;
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!found && child) found = hasTableFunctionSource(*child);
  });
  return found;
}

// rewriter-go ExpressionPositionHasReads, for the positions both parsers
// structure: a table, a namespace-bearing table function or IN-operand table
// (the SI read walker's events), any other table-function source, or a
// parameter in a table or IN-operand position (T2 names that one first; a
// column-position parameter reads nothing and rewriter-go admits it). A
// subquery that reads no table — `(SELECT 1)` — and an IN operand that is a
// tuple of expressions — `a IN (1, b)` — are not reads, unlike under the
// opaque ALTER text rule. No WITH alias binds here: ClickHouse reads every
// FROM / IN name in these positions as a table, a name declared by an
// enclosing or recursive WITH included (CTEBinding::Unbound).
bool structuredExpressionHasRead(const DB::ASTPtr &node) {
  if (!node) return false;
  return !collectStorageIntegrityReadRefs(node, CTEBinding::Unbound).empty() || hasTableFunctionSource(*node)
    || tablePositionParameter(*node);
}

// A raw AST field held in `parent.children`, as the owning pointer. A field the
// parent does not own cannot be walked, so it is reported as a read.
bool ownedExpressionHasRead(const DB::IAST &parent, const DB::IAST *field) {
  if (!field) return false;
  for (const auto &child : parent.children)
    if (child.get() == field) return structuredExpressionHasRead(child);
  return true;
}

// The expressions of a table's storage clause: the ENGINE arguments (T5 and
// the namespace policy decide the engine itself; an argument that reads a
// table, calls a table function or carries a parameter is refused like any
// other storage expression, in both SI states), PARTITION BY, PRIMARY KEY,
// ORDER BY, SAMPLE BY and TTL (its GROUP BY … SET and RECOMPRESS CODEC
// included, registerFieldOnlyChildren). The SETTINGS are R5's.
bool storageHasRead(const DB::ASTStorage *storage) {
  if (!storage) return false;
  if (storage->engine && storage->engine->arguments
      && ownedExpressionHasRead(*storage->engine, storage->engine->arguments.get()))
    return true;
  for (const DB::IAST *expr : {storage->partition_by, storage->primary_key, storage->order_by,
         storage->sample_by, storage->ttl_table})
    if (ownedExpressionHasRead(*storage, expr)) return true;
  return false;
}

// The ALTER actions rewriter-go's parser structures and walks like a CREATE
// column or constraint (measured; MODIFY COLUMN, ADD INDEX and the mutation
// actions stay opaque text there).
bool structuredAlterCommand(const DB::IAST &node) {
  const auto *command = node.as<DB::ASTAlterCommand>();
  return command
    && (command->type == DB::ASTAlterCommand::ADD_COLUMN || command->type == DB::ASTAlterCommand::ADD_CONSTRAINT
        || command->type == DB::ASTAlterCommand::DROP_COLUMN);
}

// An ALTER whose every action is structured is judged by the structural rule
// alone; one opaque action makes the whole statement text opaque (the
// lexical IN rule cannot tell which action a region belongs to).
bool structuredAlter(const DB::ASTAlterQuery &alter) {
  if (!alter.command_list || alter.command_list->children.empty()) return false;
  const auto &commands = alter.command_list->children;
  return std::all_of(commands.begin(), commands.end(),
    [](const DB::ASTPtr &command) { return command && structuredAlterCommand(*command); });
}

// R2 over the positions no rewrite reaches in a structured statement: the
// assignments, predicate and partition of an UPDATE / DELETE, the column,
// index, constraint, projection and storage expressions of a CREATE (a
// materialized view's inner engines included; its SELECT body is rewritten),
// and the actions of an ALTER that is structured throughout.
bool structuredStatementHasRead(const DB::IAST &ast) {
  if (const auto *alter = ast.as<DB::ASTAlterQuery>()) {
    if (!structuredAlter(*alter)) return false;
    for (const auto &command : alter->command_list->children)
      if (structuredExpressionHasRead(command)) return true;
    return false;
  }
  if (const auto *update = ast.as<DB::ASTUpdateQuery>()) {
    return structuredExpressionHasRead(update->assignments) || structuredExpressionHasRead(update->predicate)
      || structuredExpressionHasRead(update->partition);
  }
  if (const auto *del = ast.as<DB::ASTDeleteQuery>()) {
    return structuredExpressionHasRead(del->predicate) || structuredExpressionHasRead(del->partition);
  }
  if (const auto *create = ast.as<DB::ASTCreateQuery>()) {
    if (ownedExpressionHasRead(*create, create->columns_list) || storageHasRead(create->storage)) return true;
    if (create->targets) {
      for (const auto *inner : create->targets->getInnerEngines())
        if (storageHasRead(inner)) return true;
    }
  }
  return false;
}

bool hasInOperandTable(const DB::IAST &node) {
  if (const auto *fn = inFamilyCall(node)) {
    const auto *operand = fn->arguments->children[1]->as<DB::ASTIdentifier>();
    if (operand && !operand->isParam()) return true;
  }
  bool found = false;
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!found && child) found = hasInOperandTable(*child);
  });
  return found;
}

} // namespace

bool inOperandInUnrewrittenPosition(const DB::ASTPtr &ast) {
  using Kind = DB::IAST::QueryKind;
  if (!ast) return false;
  switch (ast->getQueryKind()) {
  // Every refusal of the IN-operand scan (decoded AST names; no WITH alias is
  // exempt, as none binds in these positions) is kept as defence in depth
  // behind R2, and R2's own predicate re-checks the positions it owns.
  case Kind::Alter:
  case Kind::Delete:
  case Kind::Update:
    return hasInOperandTable(*ast) || structuredStatementHasRead(*ast);
  // A SHOW trailing clause is R7's (rejectShowBody), which every SHOW
  // handler runs in dynamic mode and which refuses a superset of this scan.
  case Kind::Create: {
    // The SELECT body of a CTAS / VIEW / MATERIALIZED VIEW is a rewritten
    // position. The column, constraint and storage expressions, ENGINE
    // arguments included (storageHasRead), are R2's; the `AS <table
    // function>` source has no R2 rule, so this scan is its only guard.
    const auto *create = ast->as<DB::ASTCreateQuery>();
    if (!create) return false;
    if (structuredStatementHasRead(*ast)) return true;
    if (!create->select) return hasInOperandTable(*ast);
    for (const auto &child : create->children)
      if (child && child.get() != create->select && hasInOperandTable(*child)) return true;
    if (create->targets) {
      for (const auto *inner : create->targets->getInnerEngines())
        if (inner && hasInOperandTable(*inner)) return true;
    }
    return false;
  }
  default:
    return false;
  }
}

namespace {

bool holdsColumnDeclarationSettings(const DB::IAST &node) {
  if (const auto *column = node.as<DB::ASTColumnDeclaration>(); column && column->getSettings()) return true;
  bool found = false;
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!found && child) found = holdsColumnDeclarationSettings(*child);
  });
  return found;
}

} // namespace

bool columnDeclarationHasSettings(const DB::ASTPtr &ast) {
  if (!ast) return false;
  if (const auto *create = ast->as<DB::ASTCreateQuery>())
    return create->columns_list && holdsColumnDeclarationSettings(*create->columns_list);
  if (const auto *alter = ast->as<DB::ASTAlterQuery>(); alter && alter->command_list) {
    for (const auto &child : alter->command_list->children) {
      const auto *command = child ? child->as<DB::ASTAlterCommand>() : nullptr;
      if (command && command->type == DB::ASTAlterCommand::ADD_COLUMN && command->col_decl
          && holdsColumnDeclarationSettings(*command->col_decl))
        return true;
    }
  }
  return false;
}

namespace {

// Every word of ClickHouse's parser keywords (upper case). A `(` after one of
// them opens a parenthesised expression, not a call.
const std::set<std::string> &keywordWords() {
  static const std::set<std::string> words = [] {
    std::set<std::string> out;
    for (const auto &keyword : DB::getKeyWordSet()) {
      size_t start = 0;
      while (start <= keyword.size()) {
        const size_t space = keyword.find(' ', start);
        const size_t stop = space == std::string::npos ? keyword.size() : space;
        if (stop > start) out.insert(upperWord(keyword.substr(start, stop - start)));
        if (space == std::string::npos) break;
        start = space + 1;
      }
    }
    return out;
  }();
  return words;
}

void collectFunctionNames(const DB::IAST &node, std::set<std::string> &out) {
  if (const auto *function = node.as<DB::ASTFunction>()) out.insert(lowerName(function->name));
  for (const auto &child : node.children)
    if (child) collectFunctionNames(*child, out);
}

// A token ClickHouse 25.8 / 26.2 accept inside a literal tuple (measured):
// numbers, strings (x'…', b'…', heredocs), NULL / TRUE / FALSE / inf /
// infinity / nan in any case, leading signs, commas and nested tuples. An
// array, an identifier, a call, `::` or an operator makes the tuple an
// expression.
bool literalTupleToken(const DB::Token &token) {
  switch (token.type) {
  case DB::TokenType::Number: case DB::TokenType::StringLiteral: case DB::TokenType::HereDoc:
  case DB::TokenType::Minus: case DB::TokenType::Plus: case DB::TokenType::Comma:
  case DB::TokenType::OpeningRoundBracket: case DB::TokenType::ClosingRoundBracket:
    return true;
  case DB::TokenType::BareWord: {
    const auto word = lowerName(std::string(token.begin, token.end));
    return word == "null" || word == "true" || word == "false" || word == "inf" || word == "infinity"
      || word == "nan";
  }
  default:
    return false;
  }
}

// Whether tokens[open..close] (a round-bracket group) has the shape rule N1
// was first written against: at least one top-level comma, no trailing comma
// and only literal tokens, a sign opening an element. Brackets inside it are
// not examined, so it also holds for `((1, (2)))`: v0.16.0 refused every such
// group, and a group of this shape that is not a literal tuple (below) still
// is refused.
bool literalTupleShape(const std::vector<DB::Token> &tokens, size_t open, size_t close) {
  if (close < open + 2 || tokens[close - 1].type == DB::TokenType::Comma) return false;
  bool comma = false;
  int depth = 0;
  for (size_t i = open + 1; i < close; ++i) {
    const auto &token = tokens[i];
    if (!literalTupleToken(token)) return false;
    // A sign only opens an element (`(-1, +2)`); `1 + 1` and `- -1` are
    // expressions there (measured).
    if ((token.type == DB::TokenType::Minus || token.type == DB::TokenType::Plus)
        && tokens[i - 1].type != DB::TokenType::OpeningRoundBracket && tokens[i - 1].type != DB::TokenType::Comma)
      return false;
    if (token.type == DB::TokenType::OpeningRoundBracket) ++depth;
    else if (token.type == DB::TokenType::ClosingRoundBracket) --depth;
    else if (token.type == DB::TokenType::Comma && depth == 0) comma = true;
  }
  return comma;
}

// Whether tokens[open..close] is a literal tuple for ClickHouse 25.8 / 26.2:
// literalTupleShape, every nested round-bracket group a literal tuple in turn,
// and no sign before a bracket. Measured on 25.8, 26.2 and 26.3
// (`toTypeName(((1, E)))`): the outer pair is the N1 grouping only when E is
// a scalar literal or such a tuple (`(2, 3)`, `(-2, +3)`, `('a', NULL)`,
// `(2, (3, 4))`, `((2, 3), (4, 5))`); a parenthesised scalar (`(2)`, `(-2)`,
// `('a')`, `(NULL)`, `(inf)`, `((2))`), a trailing comma (`(2, 3,)`), `()`,
// a signed bracket (`+(1)`, `-(2, 3)`) or a nested group of one of those
// (`(2, (3))`, `(2, ((3, 4)))`) makes the tuple an expression there, and the
// outer pair plain grouping.
bool literalTupleGroup(const std::vector<DB::Token> &tokens, const std::vector<size_t> &match, size_t open,
                       size_t close) {
  if (!literalTupleShape(tokens, open, close)) return false;
  for (size_t i = open + 1; i < close; ++i) {
    const auto &token = tokens[i];
    // A sign keeps the tuple literal only before a number or inf / infinity /
    // nan (any case). Before a string, `x'…'`, `b'…'`, a heredoc, NULL,
    // TRUE / FALSE or a bracket it makes the tuple an expression on 25.8 /
    // 26.2 (fix round 2, measured on 25.8, 26.2 and 26.3: `((1, +'a'))` is
    // `Tuple(UInt8, String)` there; `((1, +1))`, `((1, -inf))`,
    // `((1, +NaN))` are `Tuple(Tuple(…))`).
    if (token.type == DB::TokenType::Minus || token.type == DB::TokenType::Plus) {
      const auto &next = tokens[i + 1];
      if (next.type != DB::TokenType::Number && !wordIs(next, "inf") && !wordIs(next, "infinity")
          && !wordIs(next, "nan"))
        return false;
    }
    if (token.type == DB::TokenType::OpeningRoundBracket) {
      if (!literalTupleGroup(tokens, match, i, match[i])) return false;
      i = match[i];
    }
  }
  return true;
}

// A float literal below the smallest normal double: one that underflows to
// zero (`1e-400`), a non-zero subnormal (`1e-310`) or one that rounds up to
// DBL_MIN (`2.2250738585072013e-308`, `0x1.fffffffffffffp-1023`). 25.8
// rejects every such literal (error 62) while the formatter re-emits a value
// it may accept. Conservative: a range error, or a non-zero value at or below
// DBL_MIN (DBL_MIN's own spelling included).
bool subnormalNumber(const DB::Token &token) {
  std::string text;
  for (const char *c = token.begin; c != token.end; ++c)
    if (*c != '_') text.push_back(*c);
  if (text.size() > 1 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) return false;
  errno = 0;
  char *stop = nullptr;
  const double value = std::strtod(text.c_str(), &stop);
  if (errno == ERANGE) return true;
  return value != 0 && std::fabs(value) <= std::numeric_limits<double>::min();
}

// The literal-fold scan of `text`: `refuse` for a subnormal number or a text
// that does not tokenize or bracket-match; otherwise the byte offset of the
// outer `(` of every literal-tuple group wrapped in an extra pair of grouping
// parentheses (with an optional alias). `is_call` decides whether the `(` at
// an index opens a call's own argument list.
LiteralFoldPlan scanLiteralFolds(std::string_view text,
                                 const std::function<bool(const std::vector<DB::Token> &, size_t)> &is_call) {
  LiteralFoldPlan out;
  std::vector<DB::Token> tokens;
  if (!significantTokens(text, tokens)) {
    out.refuse = true;
    return out;
  }
  std::vector<size_t> match(tokens.size(), 0);
  std::vector<size_t> open;
  // A `(` that opens one set of a GROUPING SETS list is syntax, like a call's
  // own parenthesis: `GROUPING SETS (((1, 2)))` is the set `((1, 2))`, whose
  // key 25.8 / 26.2 read as the literal `(1, 2)` (EXPLAIN AST, measured).
  std::vector<bool> set_opener(tokens.size(), false);
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].type == DB::TokenType::Number && subnormalNumber(tokens[i])) {
      out.refuse = true;
      return out;
    }
    if (tokens[i].type == DB::TokenType::OpeningRoundBracket) {
      if (!open.empty()) {
        const size_t parent = open.back();
        set_opener[i] = parent >= 2 && wordIs(tokens[parent - 1], "sets") && wordIs(tokens[parent - 2], "grouping");
      }
      open.push_back(i);
    } else if (tokens[i].type == DB::TokenType::ClosingRoundBracket) {
      if (open.empty()) {
        out.refuse = true;
        return out;
      }
      match[open.back()] = i;
      match[i] = open.back();
      open.pop_back();
    }
  }
  if (!open.empty()) {
    out.refuse = true;
    return out;
  }

  for (size_t p = 0; p + 1 < tokens.size(); ++p) {
    if (tokens[p].type != DB::TokenType::OpeningRoundBracket
        || tokens[p + 1].type != DB::TokenType::OpeningRoundBracket || set_opener[p] || is_call(tokens, p))
      continue;
    const size_t q = p + 1, q_close = match[q], p_close = match[p];
    // The group's sole content is the inner tuple, with an optional alias.
    size_t next = q_close + 1;
    if (next < p_close && wordIs(tokens[next], "as")) ++next;
    if (next < p_close && (tokens[next].type == DB::TokenType::BareWord
                           || tokens[next].type == DB::TokenType::QuotedIdentifier))
      ++next;
    if (next != p_close) continue;
    // `NOT (` is a call to 25.8 / 26.2 and an operator with a grouping to
    // 26.3 (fix round 2, review R1-M-1): a fold right after NOT is refused,
    // as v0.16.0 refused it.
    if (p > 0 && wordIs(tokens[p - 1], "not")
        && (literalTupleGroup(tokens, match, q, q_close) || literalTupleShape(tokens, q, q_close))) {
      out.refuse = true;
      return out;
    }
    if (literalTupleGroup(tokens, match, q, q_close)) {
      out.folds.push_back(static_cast<size_t>(tokens[p].begin - text.data()));
    } else if (literalTupleShape(tokens, q, q_close)) {
      // Not a literal tuple for 25.8 / 26.2 (`((1, (2)))`), but one the
      // embedded parser may still fold into a literal: preserving it would
      // print a grouping 25.8 / 26.2 do not read in the source. Refused, as
      // v0.16.0 refused it.
      out.refuse = true;
      return out;
    }
  }
  // A cap on the folds one statement may preserve: each costs a marker, a
  // second parse and a tree hash. Far above any realistic statement; beyond
  // it the statement is refused (fail closed, review R1-N-1).
  if (out.folds.size() > kMaxLiteralFolds) out.refuse = true;
  return out;
}

constexpr std::string_view kLiteralFoldMarker = "__housegate_grouped_literal_tuple";

void collectLiteralFoldMarkers(const DB::ASTPtr &node, std::vector<DB::ASTPtr *> &out, DB::ASTPtr *slot) {
  if (!node) return;
  if (const auto *fn = node->as<DB::ASTFunction>(); fn && fn->name == kLiteralFoldMarker && slot) out.push_back(slot);
  for (auto &child : node->children) collectLiteralFoldMarkers(child, out, &child);
}

} // namespace

bool notCallPrecedenceDiverges(const DB::ASTPtr &ast, const std::string &parsed_sql) {
  if (!ast) return false;
  std::string_view text(parsed_sql);
  if (const auto *insert = ast->as<DB::ASTInsertQuery>(); insert && insert->data
      && insert->data >= parsed_sql.data() && insert->data <= parsed_sql.data() + parsed_sql.size())
    text = text.substr(0, static_cast<size_t>(insert->data - parsed_sql.data()));
  std::vector<DB::Token> tokens;
  if (!significantTokens(text, tokens)) return true;
  std::vector<size_t> match(tokens.size(), 0);
  std::vector<size_t> open;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].type == DB::TokenType::OpeningRoundBracket) {
      open.push_back(i);
    } else if (tokens[i].type == DB::TokenType::ClosingRoundBracket) {
      if (open.empty()) return true;
      match[open.back()] = i;
      open.pop_back();
    }
  }
  if (!open.empty()) return true;
  // What may follow `NOT (…)` with the same reading on every server: the end
  // of the expression (a closing bracket, a separator, the ternary, an alias,
  // AND / OR, which bind looser than NOT everywhere, or a clause keyword).
  static const std::set<std::string> ends = {"AND", "OR", "AS", "THEN", "ELSE", "END", "WHEN", "FROM", "WHERE",
    "PREWHERE", "GROUP", "ORDER", "HAVING", "LIMIT", "OFFSET", "SETTINGS", "FORMAT", "UNION", "EXCEPT",
    "INTERSECT", "WINDOW", "QUALIFY", "ASC", "DESC", "ASCENDING", "DESCENDING", "WITH", "INTO"};
  for (size_t i = 0; i + 1 < tokens.size(); ++i) {
    if (!wordIs(tokens[i], "not") || tokens[i + 1].type != DB::TokenType::OpeningRoundBracket) continue;
    const size_t after = match[i + 1] + 1;
    if (after >= tokens.size()) continue;
    const auto &next = tokens[after];
    switch (next.type) {
    case DB::TokenType::ClosingRoundBracket: case DB::TokenType::ClosingSquareBracket:
    case DB::TokenType::Comma: case DB::TokenType::Semicolon: case DB::TokenType::QuestionMark:
    case DB::TokenType::Colon: case DB::TokenType::QuotedIdentifier:
      continue;
    case DB::TokenType::BareWord:
      if (ends.contains(upperWord(std::string(next.begin, next.end)))) continue;
      return true;
    default:
      return true;
    }
  }
  return false;
}

LiteralFoldPlan planLiteralFolds(const DB::ASTPtr &ast, const std::string &parsed_sql) {
  if (!ast) return {};
  std::string_view text(parsed_sql);
  // An INSERT's inline data is forwarded verbatim; only the statement before
  // it is re-emitted.
  if (const auto *insert = ast->as<DB::ASTInsertQuery>(); insert && insert->data
      && insert->data >= parsed_sql.data() && insert->data <= parsed_sql.data() + parsed_sql.size())
    text = text.substr(0, static_cast<size_t>(insert->data - parsed_sql.data()));
  std::set<std::string> functions;
  collectFunctionNames(*ast, functions);
  // A call's own parentheses, `f((1, 2))`, pass the tuple as written. Only a
  // non-keyword word naming a call in the tree counts; anything else (a
  // keyword word, even one naming a call such as `array` or `any`, a quoted
  // name, `)`) is read as grouping. A grouping misread is harmless: its marker
  // changes the parse, and adoptLiteralFolds then refuses the statement.
  LiteralFoldPlan plan = scanLiteralFolds(text, [&](const std::vector<DB::Token> &tokens, size_t p) {
    if (p == 0 || tokens[p - 1].type != DB::TokenType::BareWord) return false;
    const std::string word(tokens[p - 1].begin, tokens[p - 1].end);
    return !keywordWords().contains(upperWord(word)) && functions.contains(lowerName(word));
  });
  // A source already spelling the marker cannot be marked unambiguously.
  if (!plan.refuse && !plan.folds.empty()) {
    std::vector<DB::Token> tokens;
    significantTokens(text, tokens);
    for (const auto &token : tokens)
      if (token.type == DB::TokenType::BareWord
          && lowerName(std::string(token.begin, token.end)) == kLiteralFoldMarker)
        plan.refuse = true;
  }
  return plan;
}

std::string markLiteralFolds(const std::string &parsed_sql, const LiteralFoldPlan &plan) {
  // One pass over the text (the offsets ascend): linear in its length.
  std::string out;
  out.reserve(parsed_sql.size() + plan.folds.size() * (kLiteralFoldMarker.size() + 1));
  size_t from = 0;
  for (const size_t at : plan.folds) {
    out.append(parsed_sql, from, at - from);
    out.push_back(' ');
    out.append(kLiteralFoldMarker);
    from = at;
  }
  out.append(parsed_sql, from, std::string::npos);
  return out;
}

bool adoptLiteralFolds(DB::ASTPtr &marked, const DB::ASTPtr &original, size_t folds) {
  if (!marked || !original) return false;
  std::vector<DB::ASTPtr *> slots;
  collectLiteralFoldMarkers(marked, slots, &marked);
  if (slots.size() != folds) return false;
  for (auto *slot : slots) {
    const auto *fn = (*slot)->as<DB::ASTFunction>();
    if (fn->parameters || fn->isWindowFunction() || fn->getNullsAction() != DB::NullsAction::EMPTY || !fn->arguments
        || fn->arguments->children.size() != 1)
      return false;
    const auto *literal = fn->arguments->children[0]->as<DB::ASTLiteral>();
    if (!literal || literal->value.getType() != DB::Field::Types::Tuple) return false;
    if (!fn->alias.empty() && !literal->alias.empty()) return false;
  }
  // The marked tree with every marker replaced by its tuple is what the
  // embedded parser built from the source: the markers moved nothing else.
  // Both sides are compared as clones: a clone lays some statements'
  // children out differently from the parser (ALTER and CREATE drop their
  // name identifiers from `children`), so only like is compared with like.
  DB::ASTPtr flat = marked->clone();
  std::vector<DB::ASTPtr *> flat_slots;
  collectLiteralFoldMarkers(flat, flat_slots, &flat);
  if (flat_slots.size() != folds) return false;
  for (auto *slot : flat_slots) {
    const auto *fn = (*slot)->as<DB::ASTFunction>();
    DB::ASTPtr tuple = fn->arguments->children[0];
    if (!fn->alias.empty()) tuple->as<DB::ASTLiteral &>().alias = fn->alias;
    *slot = tuple;
  }
  if (flat->getTreeHash(/*ignore_aliases=*/false) != original->clone()->getTreeHash(/*ignore_aliases=*/false))
    return false;
  // ClickHouse's formatter prints a function without a name as its
  // parenthesised arguments: `((1, 2))` again.
  for (auto *slot : slots) (*slot)->as<DB::ASTFunction &>().name.clear();
  return true;
}

std::string tableFunctionRefusedMessage(const std::string &name) {
  return "table function " + name + " is not accepted";
}

std::string tableFunctionUnknownMessage(const std::string &name) {
  return "table function " + name + " is not recognised";
}

TableFunctionClass classifyTableFunction(const std::string &name) {
  static const std::set<std::string> refused = {"merge", "remote", "remotesecure", "cluster",
    "clusterallreplicas", "loop", "dictionary", "mergetreeindex", "mergetreeprojection",
    "timeseriesdata", "timeseriestags", "timeseriesmetrics", "timeseriesselector",
    "prometheusquery", "prometheusqueryrange", "clickhouse", "mysql", "postgresql", "mongodb",
    "jdbc", "odbc", "executable", "fuzzquery", "fuzzjson"};
  static const std::set<std::string> data_only = {"numbers", "numbers_mt", "generate_series",
    "generateseries", "generaterandom", "zeros", "zeros_mt", "null", "values", "format", "input",
    "view"};
  static const std::set<std::string> external = {"url", "s3", "gcs", "oss", "cosn", "file",
    "hdfs", "azureblobstorage", "iceberg", "deltalake", "hudi", "urlcluster", "s3cluster",
    "filecluster", "hdfscluster", "azureblobstoragecluster", "icebergcluster",
    "deltalakecluster", "hudicluster", "sqlite", "redis"};
  const std::string lower = lowerName(name);
  if (refused.count(lower) || lower.rfind("mergetree", 0) == 0) return TableFunctionClass::Refused;
  if (data_only.count(lower)) return TableFunctionClass::DataOnly;
  if (external.count(lower)) return TableFunctionClass::External;
  return TableFunctionClass::Unknown;
}

std::string tableEngineRefusedMessage(const std::string &name) {
  return "table engine " + name + " is not accepted";
}

std::string tableEngineUnknownMessage(const std::string &name) {
  return "table engine " + name + " is not recognised";
}

std::string tableSettingRefusedMessage(const std::string &name) {
  return "table setting " + name + " is not accepted";
}

// Engine names are case-sensitive in ClickHouse (`ENGINE = memory` is an
// unknown engine), so a name is matched exactly; a list name in another case
// is Unknown (rewriter-go R12). Replicated* is allowed only without arguments.
TableEngineClass classifyTableEngine(const std::string &name, size_t argument_count) {
  static const std::set<std::string> allowed = {"MergeTree", "ReplacingMergeTree",
    "SummingMergeTree", "AggregatingMergeTree", "CollapsingMergeTree",
    "VersionedCollapsingMergeTree", "GraphiteMergeTree", "Memory", "Log", "TinyLog", "StripeLog",
    "Null", "Set", "Join", "View", "MaterializedView", "LiveView"};
  constexpr std::string_view replicated = "Replicated";
  if (name.rfind(replicated, 0) == 0 && allowed.count(name.substr(replicated.size())))
    return argument_count == 0 ? TableEngineClass::Allowed : TableEngineClass::Refused;
  if (allowed.count(name)) return TableEngineClass::Allowed;
  const std::string lower = lowerName(name);
  for (const auto &candidate : allowed) {
    if (lowerName(candidate) == lower || "replicated" + lowerName(candidate) == lower)
      return TableEngineClass::Unknown;
  }
  return TableEngineClass::Refused;
}

bool refusedTableSetting(const std::string &name) {
  const std::string lower = lowerName(name);
  return lower == "disk" || lower == "storage_policy";
}

namespace {

bool rejectStorageEngine(const DB::ASTStorage *storage, rewriter::RewriteSQLResponse *response) {
  if (!storage || !storage->engine) return false;
  const auto &engine = *storage->engine;
  const size_t argc = engine.arguments ? engine.arguments->children.size() : 0;
  switch (classifyTableEngine(engine.name, argc)) {
  case TableEngineClass::Refused:
    response->set_code(rewriter::RewriteCode::UnsupportedStatement);
    response->set_message(tableEngineRefusedMessage(engine.name));
    return true;
  case TableEngineClass::Unknown:
    response->set_code(rewriter::RewriteCode::UnsupportedStatement);
    response->set_message(tableEngineUnknownMessage(engine.name));
    return true;
  case TableEngineClass::Allowed:
    break;
  }
  return false;
}

// `sql_bearing` also refuses the R5 SQL-bearing / dialect names: a CREATE's own
// storage settings refuse them, ALTER … MODIFY SETTING does not (rewriter-go).
// One table-setting name (a CREATE storage SETTINGS, ALTER … MODIFY / RESET
// SETTING key): a refused name gets the T5 message; a compound (dotted) name,
// which no table setting has, is refused as `statement is not supported`
// (rewriter-go refuses it; its last part names a refused setting there).
bool rejectTableSettingName(const std::string &name, rewriter::RewriteSQLResponse *response, bool sql_bearing) {
  const auto dot = name.rfind('.');
  const std::string last = dot == std::string::npos ? name : name.substr(dot + 1);
  if (refusedTableSetting(last) || (sql_bearing && sqlBearingSetting(last))) {
    response->set_code(rewriter::RewriteCode::UnsupportedStatement);
    response->set_message(tableSettingRefusedMessage(last));
    return true;
  }
  if (dot != std::string::npos) {
    response->set_code(rewriter::RewriteCode::UnsupportedStatement);
    response->set_message(std::string(kUnsupportedStatementMessage));
    return true;
  }
  return false;
}

bool rejectSettingChanges(const DB::ASTPtr &settings_ast, rewriter::RewriteSQLResponse *response,
                          bool sql_bearing) {
  const auto *settings = settings_ast ? settings_ast->as<DB::ASTSetQuery>() : nullptr;
  if (!settings) return false;
  for (const auto &change : settings->changes)
    if (rejectTableSettingName(change.name, response, sql_bearing)) return true;
  // `name = DEFAULT` is kept apart from `changes`.
  for (const auto &name : settings->default_settings)
    if (rejectTableSettingName(name, response, sql_bearing)) return true;
  return false;
}

bool createHasInnerStorageClause(const DB::ASTCreateQuery &create) {
  if (!create.targets) return false;
  for (const auto kind : create.targets->getKinds())
    if (create.targets->hasInnerUUID(kind)) return true;
  return false;
}

// Source-role table functions (rewriter-go CollectSourceFunctionNames): every
// FROM / JOIN table function at any depth, INSERT INTO FUNCTION and CREATE
// TABLE … AS function. An ALTER action is opaque text there, so its subquery
// is left to R2.
void collectSourceFunctions(const DB::IAST &node, bool in_alter, std::vector<std::string> &out) {
  if (!in_alter) {
    if (const auto *te = node.as<DB::ASTTableExpression>()) {
      if (const auto *fn = te->table_function ? te->table_function->as<DB::ASTFunction>() : nullptr)
        out.push_back(fn->name);
    }
    if (const auto *insert = node.as<DB::ASTInsertQuery>()) {
      if (const auto *fn = insert->table_function ? insert->table_function->as<DB::ASTFunction>() : nullptr)
        out.push_back(fn->name);
    }
    if (const auto *create = node.as<DB::ASTCreateQuery>()) {
      if (const auto *fn = create->as_table_function ? create->as_table_function->as<DB::ASTFunction>() : nullptr)
        out.push_back(fn->name);
    }
  }
  const auto *alter = node.as<DB::ASTAlterQuery>();
  for (const auto &child : node.children) {
    if (!child) continue;
    collectSourceFunctions(*child, in_alter || (alter && child.get() == alter->command_list), out);
  }
}

// A SETTINGS clause on a top-level SELECT of an INSERT's query, found the way
// ClickHouse's InsertQuerySettingsPushDownVisitor finds the clauses it copies
// into the INSERT's own settings.
bool querySelectHasSettings(const DB::IAST &node) {
  if (const auto *select = node.as<DB::ASTSelectQuery>()) return select->settings() != nullptr;
  const bool descend = node.as<DB::ASTSelectWithUnionQuery>() || node.as<DB::ASTSubquery>();
  for (const auto &child : node.children)
    if (child && (descend || child->as<DB::ASTSelectQuery>()) && querySelectHasSettings(*child)) return true;
  return false;
}

// rewriter-go measured: an INSERT whose column list is followed by a SETTINGS
// clause keeps the rest of the statement (its VALUES rows or its query) as
// opaque text (spec 2026-09-26 R2 / R5), which only the lexical rule reads
// (opaqueInsertText). Its inline rows are never parsed.
bool insertHasOpaqueData(const DB::ASTInsertQuery &insert) {
  return insert.columns && insert.settings_ast && !insert.select;
}

// The inline Values text of an INSERT (`VALUES …` / `FORMAT Values …`), which
// ClickHouse's parser keeps as raw bytes. `FORMAT <name>` keeps the name as
// written and ClickHouse resolves format names case-insensitively, so
// `FORMAT values` runs as Values too.
bool insertValuesText(const DB::ASTInsertQuery &insert, const char **begin, const char **end) {
  if (!insert.data || insert.end <= insert.data || lowerName(insert.format) != "values") return false;
  *begin = insert.data;
  *end = insert.end;
  return true;
}

// Whether Values text holds only literal tokens (numbers, strings, NULL / TRUE
// / FALSE, signs, brackets, commas, semicolons). Such rows read nothing, so a
// bulk literal payload is never parsed.
bool valuesTextIsLiteral(const char *begin, const char *end) {
  DB::Lexer lexer(begin, end);
  for (DB::Token tok = lexer.nextToken(); !tok.isEnd(); tok = lexer.nextToken()) {
    switch (tok.type) {
    case DB::TokenType::Whitespace:
    case DB::TokenType::Number: case DB::TokenType::StringLiteral:
    case DB::TokenType::Comma: case DB::TokenType::Semicolon:
    case DB::TokenType::Minus: case DB::TokenType::Plus:
    case DB::TokenType::OpeningRoundBracket: case DB::TokenType::ClosingRoundBracket:
    case DB::TokenType::OpeningSquareBracket: case DB::TokenType::ClosingSquareBracket:
      continue;
    case DB::TokenType::BareWord: {
      const auto word = lowerName(std::string(tok.begin, tok.end));
      if (word == "null" || word == "true" || word == "false") continue;
      return false;
    }
    default:
      // A comment or a lexer error: the SQL lexer's view of the bytes can
      // diverge from ClickHouse's Values reader (an unfinished `\x` escape
      // makes the reader consume the closing quote and keep reading), so the
      // rows must be parsed or read lexically, never skipped as literal.
      return false;
    }
  }
  return true;
}

// The Values rows as the expressions rewriter-go's parser builds for them:
// every top-level `( … )` row (commas, whitespace, comments and a trailing
// semicolon between rows) parsed with ParserExpressionList. std::nullopt when
// the text is anything else.
std::optional<DB::ASTs> parseValuesRows(const char *begin, const char *end) {
  DB::ASTs rows;
  DB::Lexer lexer(begin, end);
  int depth = 0;
  const char *row_begin = nullptr;
  try {
    for (DB::Token tok = lexer.nextToken(); !tok.isEnd(); tok = lexer.nextToken()) {
      if (tok.isError()) return std::nullopt;
      if (tok.type == DB::TokenType::OpeningRoundBracket) {
        if (depth++ == 0) row_begin = tok.end;
      } else if (tok.type == DB::TokenType::ClosingRoundBracket) {
        if (depth == 0) return std::nullopt;
        if (--depth == 0) {
          DB::ParserExpressionList parser(/*allow_alias_without_as_keyword=*/false);
          rows.push_back(parseBounded(parser, row_begin, tok.begin));
        }
      } else if (depth == 0 && tok.type != DB::TokenType::Whitespace
                 && tok.type != DB::TokenType::Comment && tok.type != DB::TokenType::Comma
                 && tok.type != DB::TokenType::Semicolon) {
        return std::nullopt;
      }
    }
  } catch (...) {
    return std::nullopt;
  }
  if (depth != 0 || rows.empty()) return std::nullopt;
  return rows;
}

// An INSERT's inline Values rows as rewriter-go reads them (spec 2026-09-26
// R2): `rows` holds one parsed expression list per row; `unparsed` holds a
// text that is not literal-only and does not parse as rows (the lexical rule
// reads it, failing closed). Both stay empty for a literal-only or absent
// text, and for an opaque INSERT (insertHasOpaqueData), whose text the
// lexical rule reads from the statement instead.
struct InsertValues {
  DB::ASTs rows;
  std::optional<std::string_view> unparsed;
};

InsertValues insertValues(const DB::IAST &ast) {
  InsertValues out;
  const auto *insert = ast.as<DB::ASTInsertQuery>();
  const char *begin = nullptr;
  const char *end = nullptr;
  if (!insert || insertHasOpaqueData(*insert) || !insertValuesText(*insert, &begin, &end)
      || valuesTextIsLiteral(begin, end))
    return out;
  if (auto rows = parseValuesRows(begin, end))
    out.rows = std::move(*rows);
  else
    out.unparsed = std::string_view(begin, static_cast<size_t>(end - begin));
  return out;
}

// T5 for one table function name: a refused or unknown one populates the
// rejection.
bool rejectTableFunction(const std::string &name, rewriter::RewriteSQLResponse *response) {
  switch (classifyTableFunction(name)) {
  case TableFunctionClass::Refused:
    response->set_code(rewriter::RewriteCode::UnsupportedStatement);
    response->set_message(tableFunctionRefusedMessage(name));
    return true;
  case TableFunctionClass::Unknown:
    response->set_code(rewriter::RewriteCode::UnsupportedStatement);
    response->set_message(tableFunctionUnknownMessage(name));
    return true;
  default:
    return false;
  }
}

} // namespace

bool isStringLookupFunction(const std::string &name) {
  return isStringLookupName(name);
}

bool alterIsStructured(const DB::ASTAlterQuery &alter) {
  return structuredAlter(alter);
}

DB::ASTs insertValuesRows(const DB::ASTPtr &ast) {
  return ast ? insertValues(*ast).rows : DB::ASTs{};
}

bool rejectDisallowedCarriers(const DB::ASTPtr &ast, rewriter::RewriteSQLResponse *response) {
  std::vector<std::string> functions;
  collectSourceFunctions(*ast, false, functions);
  // A VALUES row is an expression rewriter-go's parser structures: its
  // subqueries' table functions are sources too.
  for (const auto &row : insertValues(*ast).rows) collectSourceFunctions(*row, false, functions);
  for (const auto &name : functions)
    if (rejectTableFunction(name, response)) return true;
  if (const auto *create = ast->as<DB::ASTCreateQuery>()) {
    // Two shapes rewriter-go cannot inspect are refused there as "statement is
    // not supported" (CreateHeaderHasInnerStorage; its Polyglot has no grammar
    // for them), so the answer is kept level: an inner-table UUID clause
    // (`TO INNER UUID '…'`, a TimeSeries `DATA | TAGS | METRICS INNER UUID`)
    // and a database ENGINE clause, which is not a table engine.
    if (createHasInnerStorageClause(*create)
        || (create->getTable().empty() && create->storage && create->storage->engine)) {
      response->set_code(rewriter::RewriteCode::UnsupportedStatement);
      response->set_message(std::string(kUnsupportedStatementMessage));
      return true;
    }
    // A materialized view's own ENGINE / SETTINGS (`ENGINE = X` is stored as
    // the TO target's inner engine) follow the same allowlist. Every engine
    // is checked first, then every setting (rewriter-go's order).
    std::vector<const DB::ASTStorage *> storages{create->storage};
    if (create->targets)
      for (const auto *inner : create->targets->getInnerEngines()) storages.push_back(inner);
    for (const auto *storage : storages)
      if (rejectStorageEngine(storage, response)) return true;
    for (const auto *storage : storages)
      if (storage && rejectSettingChanges(storage->settings, response, /*sql_bearing=*/true)) return true;
  }
  if (const auto *alter = ast->as<DB::ASTAlterQuery>(); alter && alter->command_list) {
    for (const auto &raw : alter->command_list->children) {
      const auto *command = raw->as<DB::ASTAlterCommand>();
      if (command && command->type == DB::ASTAlterCommand::RESET_SETTING && command->settings_resets) {
        // The RESET SETTING key list (rewriter-go modifySettingKeys).
        for (const auto &key : command->settings_resets->children) {
          const auto *id = key ? key->as<DB::ASTIdentifier>() : nullptr;
          if (!id) {
            response->set_code(rewriter::RewriteCode::UnsupportedStatement);
            response->set_message(std::string(kUnsupportedStatementMessage));
            return true;
          }
          if (rejectTableSettingName(id->name(), response, /*sql_bearing=*/false)) return true;
        }
        continue;
      }
      if (!command || command->type != DB::ASTAlterCommand::MODIFY_SETTING) continue;
      if (rejectSettingChanges(command->settings_changes, response, /*sql_bearing=*/false)) return true;
    }
  }
  return false;
}

std::string stringLookupUnresolvedMessage(const std::string &function, const std::string &arg) {
  return function + " target \"" + arg + "\" does not resolve through the caller's databases";
}

std::optional<std::string> firstDeploymentIdentityCall(const std::string &sql) {
  // ClickHouse 26.3 / 26.8 registrations and aliases: DATABASE, SCHEMA and
  // current_database for currentDatabase; currentSchemas and current_schemas;
  // user and current_user for currentUser; fullHostName for FQDN.
  static const std::set<std::string> identity = {"currentdatabase", "database", "schema", "current_database",
    "currentschemas", "current_schemas", "currentuser", "user", "current_user", "session_user", "hostname",
    "fqdn", "fullhostname", "serveruuid", "getmacro",
    // authenticatedUser (alias authUser) returns the physical user too.
    "authenticateduser", "authuser",
    // ClickHouse 26.8: pgGetUserById returns the session's initial user;
    // currentRequestURL / currentHandler name the gateway's HTTP request.
    "pggetuserbyid", "pg_get_userbyid", "currentrequesturl", "currenthandler"};
  std::vector<DB::Token> tokens;
  significantTokens(sql, tokens);  // a lexer error leaves the tokens before it
  for (size_t i = 0; i + 1 < tokens.size(); ++i) {
    if (tokens[i + 1].type != DB::TokenType::OpeningRoundBracket) continue;
    if (i > 0 && tokens[i - 1].type == DB::TokenType::Dot) continue;
    // A table name before its column list (`INSERT INTO user (a)`, `CREATE
    // TABLE IF NOT EXISTS schema (…)`, an MV's `TO database (…)`) is not a call.
    if (i > 0 && (wordIs(tokens[i - 1], "into") || wordIs(tokens[i - 1], "table") || wordIs(tokens[i - 1], "exists")
                  || wordIs(tokens[i - 1], "view") || wordIs(tokens[i - 1], "dictionary") || wordIs(tokens[i - 1], "to")))
      continue;
    std::string name;
    if (tokens[i].type == DB::TokenType::BareWord) {
      name.assign(tokens[i].begin, tokens[i].end);
    } else if (tokens[i].type == DB::TokenType::QuotedIdentifier) {
      const auto decoded = decodeIdentifierToken(tokens[i]);
      if (!decoded) continue;
      name = *decoded;
    } else {
      continue;
    }
    if (identity.contains(lowerName(name))) return name;
  }
  return std::nullopt;
}

std::optional<std::string> firstBareDeploymentIdentityName(const DB::ASTPtr &ast) {
  static const std::set<std::string> bare = {"currentdatabase", "current_database", "currentuser", "current_user"};
  if (!ast) return std::nullopt;
  std::vector<const DB::IAST *> stack{ast.get()};
  while (!stack.empty()) {
    const DB::IAST *node = stack.back();
    stack.pop_back();
    if (const auto *id = dynamic_cast<const DB::ASTIdentifier *>(node);
        id && !dynamic_cast<const DB::ASTTableIdentifier *>(node) && !id->isParam() && id->name_parts.size() == 1
        && bare.contains(lowerName(id->name())))
      return id->name();
    for (auto it = node->children.rbegin(); it != node->children.rend(); ++it)
      if (*it) stack.push_back(it->get());
  }
  return std::nullopt;
}

std::string deploymentIdentityCallMessage(const std::string &name) {
  return "function " + name + " is not accessible";
}

std::optional<std::string> firstDeploymentIdentityColumn(const DB::ASTPtr &ast) {
  if (!ast) return std::nullopt;
  std::vector<const DB::IAST *> stack{ast.get()};
  while (!stack.empty()) {
    const DB::IAST *node = stack.back();
    stack.pop_back();
    if (const auto *id = dynamic_cast<const DB::ASTIdentifier *>(node);
        id && !dynamic_cast<const DB::ASTTableIdentifier *>(node) && !id->isParam() && !id->name_parts.empty()
        && id->name_parts.back() == "_database")
      return id->name_parts.back();
    for (auto it = node->children.rbegin(); it != node->children.rend(); ++it)
      if (*it) stack.push_back(it->get());
  }
  return std::nullopt;
}

std::string deploymentIdentityColumnMessage(const std::string &name) {
  return "virtual column " + name + " is not accessible";
}

std::optional<std::string> decodeIdentifierToken(const DB::Token &token) {
  return identifierTokenName(token);
}

std::string unresolvedUnqualifiedTableMessage(const std::string &table) {
  return "unqualified table \"" + table + "\" does not resolve through the session's logical database";
}

std::string unmappedDatabaseMessage(const std::string &database) {
  return "database \"" + database
    + "\" does not resolve through the caller's databases (it is not mapped for this account, or the account has no access)";
}

namespace {

bool isSelectFamily(const DB::IAST &node) {
  return node.as<DB::ASTSelectWithUnionQuery>() || node.as<DB::ASTSelectQuery>()
    || node.as<DB::ASTSelectIntersectExceptQuery>();
}

bool isHasColumnInTable(const std::string &name) {
  return lowerName(name).rfind("hascolumnintable", 0) == 0;
}

// The table a lookup call names, as rewriter-go's StringLookup reports it:
// the first argument's literal or identifier text; hasColumnInTable's
// "<db>.<table>" pair; "" when the argument is not a literal.
std::string lookupTarget(const DB::ASTFunction &fn) {
  const auto &args = fn.arguments ? fn.arguments->children : DB::ASTs{};
  if (isHasColumnInTable(fn.name)) {
    if (args.size() < 3) return {};
    const auto db = stringLiteral(args[args.size() - 3]);
    const auto table = stringLiteral(args[args.size() - 2]);
    return db && table ? *db + "." + *table : std::string();
  }
  if (args.empty()) return {};
  if (auto literal = stringLiteral(args[0])) return *literal;
  if (const auto *id = args[0]->as<DB::ASTIdentifier>(); id && !id->isParam()) return id->name();
  return {};
}

// Every lookup call with whether it sits in a SELECT body the rewrite
// pipeline processes (a SELECT root, an INSERT … SELECT / CTAS / view body;
// sticky for every descendant). A structured call must name an argument (a
// bare `joinGet()` names nothing, as in rewriter-go); in an ALTER action or a
// SHOW trailing clause, which rewriter-go reads as command text, any call
// counts.
void collectLookupCalls(const DB::IAST &node, bool in_select_body, bool opaque,
                        std::vector<std::pair<const DB::ASTFunction *, bool>> &out) {
  if (const auto *fn = node.as<DB::ASTFunction>(); fn && isStringLookupName(fn->name)
      && (opaque || (fn->arguments && !fn->arguments->children.empty())))
    out.emplace_back(fn, in_select_body);
  const auto *insert = node.as<DB::ASTInsertQuery>();
  const auto *create = node.as<DB::ASTCreateQuery>();
  const auto *alter = node.as<DB::ASTAlterQuery>();
  const bool mutation = node.as<DB::ASTUpdateQuery>() || node.as<DB::ASTDeleteQuery>();
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!child) return;
    bool body = in_select_body && !mutation;
    if (insert && child == insert->select) body = true;
    if (create && child.get() == create->select) body = true;
    const bool in_commands = alter && child.get() == alter->command_list;
    if (in_commands) body = false;
    collectLookupCalls(*child, body, opaque || in_commands || isShowBody(node, child), out);
  });
}

// T6 (rewriter-go handlers/lookups.go): every joinGet / joinGetOrNull /
// dictGet-family call, and every hasColumnInTable call outside a rewritten
// SELECT body, is refused; `rows` are the INSERT's parsed VALUES rows. A
// lookup call in VALUES text that is not read structurally (opaque, or not
// rows of expressions) is refused by R2's lexical rule.
// A lookup-family function named by an APPLY column transformer
// (`* APPLY(dictGet('db2.d', 'v'))`): ClickHouse keeps the name as a string
// (ASTColumnsApplyTransformer::func_name) and its literal arguments as
// parameters, so no lookup call node exists. rewriter-go refuses it as T6.
const DB::ASTColumnsApplyTransformer *findApplyLookup(const DB::IAST &node) {
  if (const auto *apply = node.as<DB::ASTColumnsApplyTransformer>(); apply && !apply->lambda
      && isStringLookupName(apply->func_name))
    return apply;
  const DB::ASTColumnsApplyTransformer *found = nullptr;
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!found && child) found = findApplyLookup(*child);
  });
  return found;
}

bool rejectStringLookups(const DB::ASTPtr &ast, const DB::ASTs &rows,
                         rewriter::RewriteSQLResponse *response) {
  std::vector<std::pair<const DB::ASTFunction *, bool>> calls;
  collectLookupCalls(*ast, isSelectFamily(*ast), /*opaque=*/false, calls);
  // A lookup call in a VALUES row is a T6 refusal too (rewriter-go parses the
  // rows); no rewrite pipeline reaches a row, so hasColumnInTable is refused
  // there as well.
  for (const auto &row : rows)
    collectLookupCalls(*row, /*in_select_body=*/false, /*opaque=*/false, calls);
  for (const auto &[fn, in_select_body] : calls) {
    if (isHasColumnInTable(fn->name) && in_select_body) continue;  // the SELECT path rewrites it
    response->set_code(rewriter::RewriteCode::InvalidRewriteRequest);
    response->set_message(stringLookupUnresolvedMessage(fn->name, lookupTarget(*fn)));
    return true;
  }
  if (const auto *apply = findApplyLookup(*ast)) {
    std::string target;
    if (apply->parameters && !apply->parameters->children.empty())
      target = stringLiteral(apply->parameters->children.front()).value_or("");
    response->set_code(rewriter::RewriteCode::InvalidRewriteRequest);
    response->set_message(stringLookupUnresolvedMessage(apply->func_name, target));
    return true;
  }
  return false;
}

} // namespace

bool sqlBearingSetting(const std::string &name) {
  const std::string lower = lowerName(name);
  // Name-binding settings (spec 2026-09-26 R5; rewriter-go #51 round 6/7):
  // enable_global_with_statement = 0 stops a WITH alias / CTE name reaching
  // nested queries and later set arms, so ClickHouse reads the table of that
  // name; compatibility 20.1–21.1 restores that default (and any version
  // changes defaults as a group); implicit_table_at_top_level names the table
  // a FROM-less SELECT reads; promql_table / promql_database, defence in
  // depth behind the refused promql dialect. Any value, any case.
  return lower == "additional_table_filters" || lower == "additional_result_filter"
    || lower == "parallel_replicas_custom_key" || lower == "dialect" || lower.ends_with("_dialect")
    || lower == "enable_global_with_statement" || lower == "compatibility"
    || lower == "implicit_table_at_top_level" || lower == "promql_table" || lower == "promql_database"
    // The old analyzer is refused (user ruling 2026-10-01; rewriter-go
    // fix/engine-argument-reads 5957ee1, "The old analyzer is refused"): its
    // name resolution turns an operand the binding rules trust into a table
    // read. legacy_column_name_of_tuple_literal defeats the old analyzer's
    // name-based WITH propagation; profile applies a settings profile as a
    // group (one carrying enable_analyzer = 0 would bypass the list). Any
    // value, any case. The analyzer switch itself is value-dependent
    // (isAnalyzerSetting / trueLiteralSpelling), handled in scanSettingsText.
    || lower == "legacy_column_name_of_tuple_literal" || lower == "profile"
    // ClickHouse 26.8 settings. select / filter / order / sort are SQL text
    // the server parses and wraps around the query (`SELECT <select> FROM
    // (<query>) WHERE <filter> ORDER BY <order>`), outside every walker;
    // database sets the current database like USE, moving every position the
    // rewriter does not qualify; format / input_format override the format an
    // INSERT's inline data is read with (`input_format = 'Values'` over
    // `FORMAT CSV` data the rewriter left opaque), and output_format /
    // default_format the result's; asterisk_include_virtual_columns puts
    // `_database` into `SELECT *`; the cloud-credential and dynamic-disk
    // switches hand the server's own credentials, environment, Keeper and
    // config includes to s3() / disk(); url_base / s3_base relocate a relative
    // url() / s3() target; the analyzer compatibility switches and view
    // inlining change what a name binds; enable_streaming_queries admits the
    // STREAM table expression; the AI functions' default credentials name a
    // server named collection.
    || lower == "select" || lower == "filter" || lower == "order" || lower == "sort"
    || lower == "database" || lower == "format" || lower == "input_format" || lower == "output_format"
    || lower == "default_format" || lower == "asterisk_include_virtual_columns"
    || lower == "s3_allow_server_credentials_in_user_queries" || lower.starts_with("dynamic_disk_allow_")
    || lower == "url_base" || lower == "s3_base"
    || lower == "analyzer_compatibility_allow_cte_redefinition"
    || lower == "analyzer_compatibility_prefer_alias_over_subcolumn"
    || lower == "analyzer_compatibility_multiple_joins_qualify_column_names" || lower == "analyzer_inline_views"
    || lower == "enable_streaming_queries" || lower == "ai_function_text_default_credentials"
    || lower == "ai_function_embedding_default_credentials";
}

// The query-analyzer switch (`allow_experimental_analyzer` is its alias): it is
// refused unless its value keeps the new analyzer on (trueLiteralSpelling).
bool isAnalyzerSetting(const std::string &name) {
  const std::string lower = lowerName(name);
  return lower == "enable_analyzer" || lower == "allow_experimental_analyzer";
}

// rewriter-go TrueLiteralSpelling: the closed set of single-token raw lexemes
// measured on ClickHouse 25.8 / 26.2 to set a Bool setting to true — `1`,
// `true`, `'1'`, `'true'`, the words in any case. Other values ClickHouse also
// reads as true (`0x1`, `+1`, `1.0`, `1e0`, `x'31'`, `$$true$$`, an escaped
// string) are refused: the list is closed and compared on the raw lexeme, so
// no decoding difference can admit a false value.
bool trueLiteralSpelling(std::string_view source) {
  const std::string lower = lowerName(std::string(source));
  return lower == "1" || lower == "true" || lower == "'1'" || lower == "'true'";
}

namespace {

// The only values a setting may carry in dynamic mode: a numeric, string or
// boolean literal (NULL included); a map, array, tuple or object is not.
bool plainSettingValue(const DB::Field &value) {
  switch (value.getType()) {
  case DB::Field::Types::Null:
  case DB::Field::Types::UInt64:
  case DB::Field::Types::Int64:
  case DB::Field::Types::Float64:
  case DB::Field::Types::UInt128:
  case DB::Field::Types::Int128:
  case DB::Field::Types::UInt256:
  case DB::Field::Types::Int256:
  case DB::Field::Types::String:
  case DB::Field::Types::Bool:
    return true;
  default:
    return false;
  }
}

// Query-level SETTINGS clauses and a session SET: every ASTSetQuery except a
// table's own storage settings and ALTER … MODIFY SETTING (T5 table settings).
void collectQuerySettings(const DB::IAST &node, std::vector<const DB::ASTSetQuery *> &out) {
  if (node.as<DB::ASTStorage>()) return;
  if (const auto *command = node.as<DB::ASTAlterCommand>();
      command && command->type == DB::ASTAlterCommand::MODIFY_SETTING) return;
  if (const auto *set = node.as<DB::ASTSetQuery>()) out.push_back(set);
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (child) collectQuerySettings(*child, out);
  });
}

} // namespace

namespace {

// The names of the settings in a SETTINGS list or a session SET whose value
// text is a literal form rewriter-go refuses: a negative hex / binary / inf /
// nan, and, in a session SET, in an INSERT header that follows a column list
// and in a statement other than a SELECT / WITH / CREATE / INSERT / EXPLAIN
// (SHOW, DESCRIBE, EXISTS, ALTER, DELETE, UPDATE, DROP, …), any hex / binary
// literal and any signed inf / nan.
struct SettingsText {
  std::vector<std::string> names;  // every listed name, in text order
  // Parallel to `names`: an analyzer switch (isAnalyzerSetting) whose value is
  // not a single true-literal token (trueLiteralSpelling). Refused with the
  // table-setting message, in `names` order, like a sqlBearingSetting.
  std::vector<bool> analyzer_bad;
  std::set<std::string> exotic;    // lowercased names with such a value
  // A quoted name followed by `=` after a SETTINGS / SETTING keyword whose
  // source spelling is not its plain text (settingNameTokenVerbatim).
  bool escaped_name = false;
  // A statement-initial SET list with a name not followed by `=` (the
  // boolean shorthand `SET SQL_x`): the session-SET carve-out admits only
  // `<name> = <value>` assignments (spec 2026-09-26 T7 / R5).
  bool set_shorthand = false;
};

// rewriter-go settingNameVerbatim: a bare word, or one pair of backticks or
// double quotes around the name with no backslash and no doubled quote. A
// Unicode-quoted name is not verbatim either.
bool settingNameTokenVerbatim(const DB::Token &token) {
  const std::string_view src(token.begin, token.size());
  if (src.find('\\') != std::string_view::npos) return false;
  if (token.type == DB::TokenType::BareWord) return true;
  if (token.type != DB::TokenType::QuotedIdentifier || src.size() < 2) return false;
  const char quote = src.front();
  if ((quote != '`' && quote != '"') || src.back() != quote) return false;
  return src.substr(1, src.size() - 2).find(quote) == std::string_view::npos;
}

SettingsText scanSettingsText(const std::string &sql) {
  std::vector<DB::Token> tokens;
  SettingsText found;
  DB::Lexer lexer(sql.data(), sql.data() + sql.size());
  for (;;) {
    const DB::Token token = lexer.nextToken();
    if (token.isEnd()) break;
    if (token.isError()) {
      // Fail closed: a lexer error means this text does not tokenize, so the
      // scan cannot prove it carries no refused setting (m6). The statement
      // the parser already accepted is near-impossible to re-lex with an
      // error, but if it happens the spelling refusal fires.
      found.escaped_name = true;
      return found;
    }
    if (token.type == DB::TokenType::Whitespace || token.type == DB::TokenType::Comment) continue;
    tokens.push_back(token);
  }
  if (tokens.empty()) return found;
  const bool is_insert = wordIs(tokens[0], "insert");
  const bool tail_strict = !(is_insert || wordIs(tokens[0], "select") || wordIs(tokens[0], "with")
                             || wordIs(tokens[0], "create") || wordIs(tokens[0], "explain")
                             || tokens[0].type == DB::TokenType::OpeningRoundBracket);
  bool seen_body = false;
  auto scanList = [&](size_t j, bool strict, bool *shorthand = nullptr) {
    for (;;) {
      if (j >= tokens.size()) return;
      // A name that is not its own plain text in a `name =` position
      // (rewriter-go settingNameVerbatim; a SET list is scanned here too).
      if (j + 1 < tokens.size() && tokens[j + 1].type == DB::TokenType::Equals
          && !settingNameTokenVerbatim(tokens[j]))
        found.escaped_name = true;
      // The setting name as ClickHouse decodes it, so the text order and the
      // exotic-value keys match the AST's names.
      const auto decoded = identifierTokenName(tokens[j]);
      if (!decoded) return;
      const std::string name = *decoded;
      ++j;
      // A name followed by a comma is a boolean shorthand (`SETTINGS x, …`),
      // which no SQL-bearing setting admits. A name is recorded only when `=`
      // follows it: `SELECT settings dialect FROM …` is a column and its alias.
      if (j < tokens.size() && tokens[j].type == DB::TokenType::Comma) {
        if (shorthand) *shorthand = true;
        ++j;
        continue;
      }
      if (j >= tokens.size() || tokens[j].type != DB::TokenType::Equals) {
        if (shorthand && (j >= tokens.size() || tokens[j].type == DB::TokenType::Semicolon)) *shorthand = true;
        return;
      }
      found.names.push_back(name);
      found.analyzer_bad.push_back(false);
      const size_t name_idx = found.names.size() - 1;
      const bool is_analyzer = isAnalyzerSetting(name);
      // An analyzer switch is refused unless its value is a single true-literal
      // token; mark it bad now and clear it only on that exact shape below.
      if (is_analyzer) found.analyzer_bad[name_idx] = true;
      ++j;
      bool negative = false, signed_value = false;
      while (j < tokens.size()
             && (tokens[j].type == DB::TokenType::Minus || tokens[j].type == DB::TokenType::Plus)) {
        negative = negative || tokens[j].type == DB::TokenType::Minus;
        signed_value = true;
        ++j;
      }
      if (j >= tokens.size()) return;
      const auto &value = tokens[j];
      if (value.type != DB::TokenType::Number && value.type != DB::TokenType::StringLiteral
          && value.type != DB::TokenType::BareWord) {
        // A map, array, tuple, call or parameter: not a plain literal (and so
        // never a true literal for an analyzer switch).
        found.exotic.insert(lowerName(name));
        return;
      }
      // An analyzer switch: admit only `1` / `true` / `'1'` / `'true'` as a
      // single unsigned token followed by a clause boundary (end / `,` / `)` /
      // `;` / a clause keyword). `1 + 1`, `1.0`, `0x1`, `+1`, `$$true$$` etc.
      // stay refused.
      if (is_analyzer && !signed_value && trueLiteralSpelling({value.begin, value.size()})) {
        const bool single = j + 1 >= tokens.size()
          || tokens[j + 1].type == DB::TokenType::Comma
          || tokens[j + 1].type == DB::TokenType::ClosingRoundBracket
          || tokens[j + 1].type == DB::TokenType::Semicolon
          || tokens[j + 1].type == DB::TokenType::BareWord;  // a following clause keyword
        if (single) found.analyzer_bad[name_idx] = false;
      }
      const std::string_view text(value.begin, value.size());
      const bool radix = value.type == DB::TokenType::Number && text.size() > 1 && text[0] == '0'
        && (text[1] == 'x' || text[1] == 'X' || text[1] == 'b' || text[1] == 'B');
      const bool non_finite = wordIs(value, "inf") || wordIs(value, "nan");
      if ((negative && (radix || non_finite))
          || (strict && (radix || (signed_value && non_finite))))
        found.exotic.insert(lowerName(name));
      ++j;
      if (j >= tokens.size() || tokens[j].type != DB::TokenType::Comma) return;
      ++j;
    }
  };
  if (wordIs(tokens[0], "set")) scanList(1, true, &found.set_shorthand);
  // The escape backstop (rewriter-go SettingsEscapeBackstop): ClickHouse
  // decodes such a name (`\N` to nothing), so it is refused wherever it
  // stands once a SETTINGS / SETTING keyword was seen.
  bool setting_keyword_seen = wordIs(tokens[0], "settings") || wordIs(tokens[0], "setting");
  for (size_t i = 1; i < tokens.size(); ++i) {
    if (setting_keyword_seen && tokens[i].type == DB::TokenType::QuotedIdentifier && i + 1 < tokens.size()
        && tokens[i + 1].type == DB::TokenType::Equals && !settingNameTokenVerbatim(tokens[i]))
      found.escaped_name = true;
    if (wordIs(tokens[i], "settings") || wordIs(tokens[i], "setting")) setting_keyword_seen = true;
    // ALTER … RESET SETTING's key list is names only (rewriter-go
    // modifySettingKeys): each must be its own plain text too.
    if (wordIs(tokens[i - 1], "reset") && wordIs(tokens[i], "setting")) {
      for (size_t j = i + 1; j < tokens.size(); j += 2) {
        if (!settingNameTokenVerbatim(tokens[j])) found.escaped_name = true;
        if (j + 1 >= tokens.size() || tokens[j + 1].type != DB::TokenType::Comma) break;
      }
    }
  }
  for (size_t i = 1; i < tokens.size(); ++i) {
    if (wordIs(tokens[i], "select") || wordIs(tokens[i], "with") || wordIs(tokens[i], "values")
        || wordIs(tokens[i], "format")) seen_body = true;
    if (wordIs(tokens[i], "settings"))
      scanList(i + 1, tail_strict
               || (is_insert && !seen_body && tokens[i - 1].type == DB::TokenType::ClosingRoundBracket));
  }
  return found;
}

// A cheap gate before the token scan: whether the text spells `setting` (the
// SETTINGS keyword, or ALTER … MODIFY / RESET SETTING) in any case.
bool mentionsSettings(const std::string &sql) {
  constexpr std::string_view word = "setting";
  return std::search(sql.begin(), sql.end(), word.begin(), word.end(), [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == b;
  }) != sql.end();
}

} // namespace

bool rejectQuerySettings(const DB::ASTPtr &ast, rewriter::RewriteSQLResponse *response,
                         const std::string *sql) {
  std::vector<const DB::ASTSetQuery *> clauses;
  collectQuerySettings(*ast, clauses);
  // The tokens are read even when the AST holds no settings clause: a
  // SETTINGS list in the data tail of `INSERT … VALUES (…) SETTINGS …` is
  // raw text to ClickHouse's parser but a clause to rewriter-go's.
  const bool text_mentions_settings = sql && mentionsSettings(*sql);
  if (clauses.empty() && !text_mentions_settings) return false;
  const SettingsText text = sql ? scanSettingsText(*sql) : SettingsText{};
  auto refuse = [&](std::string message) {
    response->set_code(rewriter::RewriteCode::UnsupportedStatement);
    response->set_message(std::move(message));
    return true;
  };
  // rewriter-go names a SQL-bearing or dialect setting anywhere in the
  // statement before it judges any value, and reports the first in text order
  // (a `name = DEFAULT` is kept apart from `changes` in the AST, so the tokens
  // give the order).
  for (size_t i = 0; i < text.names.size(); ++i)
    if (sqlBearingSetting(text.names[i]) || (i < text.analyzer_bad.size() && text.analyzer_bad[i]))
      return refuse(tableSettingRefusedMessage(text.names[i]));
  if (text.escaped_name || text.set_shorthand) return refuse(std::string(kUnsupportedStatementMessage));
  if (clauses.empty()) return false;
  for (const auto *clause : clauses) {
    for (const auto &change : clause->changes)
      if (sqlBearingSetting(change.name)) return refuse(tableSettingRefusedMessage(change.name));
    for (const auto &name : clause->default_settings)
      if (sqlBearingSetting(name)) return refuse(tableSettingRefusedMessage(name));
  }
  for (const auto *clause : clauses) {
    // A compound (dotted) name — a custom setting to ClickHouse — is refused
    // wherever it stands (spec 2026-09-26 R5, a deliberate over-refusal that
    // keeps rewriter-go's answer; HouseGate's own keys have no dot).
    for (const auto &change : clause->changes)
      if (change.name.find('.') != std::string::npos || !plainSettingValue(change.value)
          || text.exotic.count(lowerName(change.name)))
        return refuse(std::string(kUnsupportedStatementMessage));
    for (const auto &name : clause->default_settings)
      if (name.find('.') != std::string::npos) return refuse(std::string(kUnsupportedStatementMessage));
    // `SET param_x = …` is a query parameter; its value is stored as text.
    for (const auto &parameter : clause->query_parameters)
      if (text.exotic.count(lowerName("param_" + parameter.first)))
        return refuse(std::string(kUnsupportedStatementMessage));
  }
  return false;
}

namespace {

// A token an IN operand region may hold (rewriter-go opaqueInLiteralToken): a
// number, a string, NULL / TRUE / FALSE, a comma, a sign or a bracket. A hex
// or binary integer is not one: rewriter-go refuses `IN (0x1F)` / `IN (0b1)`
// (measured), so it is refused here too.
bool inRegionLiteralToken(const DB::Token &token) {
  switch (token.type) {
  case DB::TokenType::StringLiteral:
  case DB::TokenType::Comma:
  case DB::TokenType::Minus:
  case DB::TokenType::Plus:
  case DB::TokenType::OpeningRoundBracket:
  case DB::TokenType::ClosingRoundBracket:
  case DB::TokenType::OpeningSquareBracket:
  case DB::TokenType::ClosingSquareBracket:
    return true;
  case DB::TokenType::Number:
    return !(token.size() > 1 && token.begin[0] == '0'
             && (token.begin[1] == 'x' || token.begin[1] == 'X' || token.begin[1] == 'b' || token.begin[1] == 'B'));
  case DB::TokenType::BareWord:
    return wordIs(token, "null") || wordIs(token, "true") || wordIs(token, "false");
  default:
    return false;
  }
}

bool isOpeningBracket(const DB::Token &token) {
  return token.type == DB::TokenType::OpeningRoundBracket || token.type == DB::TokenType::OpeningSquareBracket;
}

bool isClosingBracket(const DB::Token &token) {
  return token.type == DB::TokenType::ClosingRoundBracket || token.type == DB::TokenType::ClosingSquareBracket;
}

// rewriter-go OpaqueInRefusedAt at tokens[i]. An IN-family occurrence is the
// bare IN keyword (after NOT / GLOBAL too, and the callable `in(`) or an
// IN-family name, bare or quoted, in any case. Its operand region is the
// bracket group that follows it (after any run of signs, and after a `tuple` /
// `array` constructor name), or else the single next token; it is refused
// unless every token of the region is a literal token. A missing or
// unterminated region is refused. Nothing before the occurrence matters, so
// `a IN (1, 2)` and `in(42, (1, 2))` pass while `in(a, (1, 2))` and
// `a IN (1, b)` are refused. `IN PARTITION` is the partition clause.
bool inRegionRefusedAt(const std::vector<DB::Token> &tokens, size_t i) {
  const auto &token = tokens[i];
  const bool keyword = wordIs(token, "in");
  if (!keyword) {
    if (token.type != DB::TokenType::BareWord && token.type != DB::TokenType::QuotedIdentifier) return false;
    const auto name = identifierTokenName(token);
    if (!name) return true;  // a quoted name that does not decode: fail closed
    if (!isInFamilyFunction(*name)) return false;
  }
  size_t j = i + 1;
  if (j >= tokens.size()) return true;
  if (keyword && wordIs(tokens[j], "partition")) return false;
  // A sign is never a region by itself (ClickHouse drops a unary plus, so
  // `IN +t` reads table t): the run extends to the group or token after it.
  while (j < tokens.size() && (tokens[j].type == DB::TokenType::Minus || tokens[j].type == DB::TokenType::Plus)) ++j;
  if (j >= tokens.size()) return true;
  if ((wordIs(tokens[j], "tuple") || wordIs(tokens[j], "array")) && j + 1 < tokens.size()
      && tokens[j + 1].type == DB::TokenType::OpeningRoundBracket)
    ++j;
  if (tokens[j].type == DB::TokenType::Comma || isClosingBracket(tokens[j])) return true;
  if (!isOpeningBracket(tokens[j])) return !inRegionLiteralToken(tokens[j]);
  int depth = 0;
  for (size_t k = j; k < tokens.size(); ++k) {
    if (isOpeningBracket(tokens[k])) ++depth;
    else if (isClosingBracket(tokens[k]) && --depth == 0) return false;
    if (!inRegionLiteralToken(tokens[k])) return true;
  }
  return true;  // unterminated
}

// Whether any IN-family occurrence in `sql` has a region that is not
// literal-only. A lexer error is refused (fail closed, as rewriter-go refuses
// a text it cannot tokenize).
bool textHasRefusedInRegion(const std::string &sql) {
  std::vector<DB::Token> tokens;
  DB::Lexer lexer(sql.data(), sql.data() + sql.size());
  for (DB::Token token = lexer.nextToken(); !token.isEnd(); token = lexer.nextToken()) {
    if (token.isError()) return true;
    if (token.type == DB::TokenType::Whitespace || token.type == DB::TokenType::Comment) continue;
    tokens.push_back(token);
  }
  for (size_t i = 0; i < tokens.size(); ++i)
    if (inRegionRefusedAt(tokens, i)) return true;
  return false;
}

// Whether the text holds a comment token, or a lexer error. A comment in an
// opaque VALUES/query text is fail-closed: the SQL lexer's comment boundary can
// diverge from ClickHouse's Values reader, so a comment there may hide a read
// the lexical scan would otherwise miss (a comment token is dropped from
// `significantTokens`).
bool textHasComment(std::string_view text) {
  DB::Lexer lexer(text.data(), text.data() + text.size());
  for (DB::Token token = lexer.nextToken(); !token.isEnd(); token = lexer.nextToken()) {
    if (token.isError()) return true;
    if (token.type == DB::TokenType::Comment) return true;
  }
  return false;
}

// Whether a string literal in the text holds a `\x` escape that is not
// followed by two hex digits. ClickHouse's Values reader always consumes two
// bytes after `\x`, while its SQL lexer consumes one, so the two disagree on
// where such a string ends; the statement is refused rather than read with
// either view. A lexer error counts too.
bool textHasMalformedHexEscape(std::string_view text) {
  DB::Lexer lexer(text.data(), text.data() + text.size());
  for (DB::Token token = lexer.nextToken(); !token.isEnd(); token = lexer.nextToken()) {
    if (token.isError()) return true;
    if (token.type != DB::TokenType::StringLiteral) continue;
    const std::string_view literal(token.begin, token.size());
    for (size_t i = 0; i + 1 < literal.size(); ++i) {
      if (literal[i] != '\\') continue;
      if (literal[i + 1] == 'x') {
        const bool hex = i + 3 < literal.size() && std::isxdigit(static_cast<unsigned char>(literal[i + 2]))
          && std::isxdigit(static_cast<unsigned char>(literal[i + 3]));
        if (!hex) return true;
      }
      ++i;  // skip the escaped byte, as both readers do for a one-byte escape
    }
  }
  return false;
}

// An Identifier query parameter spelled in the tokens: `{`, a name, `:`,
// `Identifier`, `}` (rewriter-go tokensHoldIdentifierParameter, the shape
// ClickHouse's ParserIdentifier accepts).
bool tokensHoldIdentifierParameter(const std::vector<DB::Token> &tokens) {
  for (size_t i = 0; i + 4 < tokens.size(); ++i) {
    if (tokens[i].type == DB::TokenType::OpeningCurlyBrace
        && (tokens[i + 1].type == DB::TokenType::BareWord || tokens[i + 1].type == DB::TokenType::QuotedIdentifier)
        && tokens[i + 2].type == DB::TokenType::Colon && tokens[i + 3].type == DB::TokenType::BareWord
        && std::string_view(tokens[i + 3].begin, tokens[i + 3].size()) == "Identifier"
        && tokens[i + 4].type == DB::TokenType::ClosingCurlyBrace)
      return true;
  }
  return false;
}

bool nameToken(const DB::Token &token) {
  return token.type == DB::TokenType::BareWord || token.type == DB::TokenType::QuotedIdentifier;
}

// The SETTINGS keyword that directly follows an INSERT's column list:
// `[WITH …] INSERT INTO [TABLE] (name[.name] | FUNCTION f(…)) (columns)
// SETTINGS`. nullptr when the header has another shape or cannot be read.
const char *insertHeaderSettings(std::string_view sql) {
  DB::Lexer lexer(sql.data(), sql.data() + sql.size());
  auto next = [&]() -> std::optional<DB::Token> {
    for (DB::Token token = lexer.nextToken(); !token.isEnd() && !token.isError(); token = lexer.nextToken())
      if (token.type != DB::TokenType::Whitespace && token.type != DB::TokenType::Comment) return token;
    return std::nullopt;
  };
  // Consumes a bracket group whose opening bracket was just read.
  auto skipGroup = [&] {
    for (int depth = 1; depth > 0;) {
      const auto token = next();
      if (!token) return false;
      if (isOpeningBracket(*token)) ++depth;
      else if (isClosingBracket(*token)) --depth;
    }
    return true;
  };
  auto token = next();
  if (token && wordIs(*token, "with")) {
    int depth = 0;
    while ((token = next()) && !(depth == 0 && wordIs(*token, "insert"))) {
      if (isOpeningBracket(*token)) ++depth;
      else if (isClosingBracket(*token)) --depth;
    }
  }
  if (!token || !wordIs(*token, "insert") || !(token = next()) || !wordIs(*token, "into") || !(token = next()))
    return nullptr;
  if (wordIs(*token, "table") && !(token = next())) return nullptr;
  if (wordIs(*token, "function")) {
    if (!(token = next()) || !nameToken(*token) || !(token = next())
        || token->type != DB::TokenType::OpeningRoundBracket || !skipGroup() || !(token = next()))
      return nullptr;
  } else {
    if (!nameToken(*token) || !(token = next())) return nullptr;
    if (token->type == DB::TokenType::Dot && (!(token = next()) || !nameToken(*token) || !(token = next())))
      return nullptr;
  }
  if (token->type != DB::TokenType::OpeningRoundBracket || !skipGroup() || !(token = next())) return nullptr;
  return wordIs(*token, "settings") ? token->begin : nullptr;
}

// rewriter-go OpaqueInsertQueryText: the text rewriter-go keeps opaque for an
// INSERT whose column list is followed by a SETTINGS clause, from that keyword
// to the end of the statement. When the keyword cannot be located the AST
// decides (a SETTINGS clause that trails the query is the ordinary form) and
// the whole statement is the text, so nothing is skipped. std::nullopt for
// every other INSERT.
std::optional<std::string_view> opaqueInsertText(const DB::ASTInsertQuery &insert, std::string_view sql) {
  if (!insert.columns || !insert.settings_ast) return std::nullopt;
  if (const char *header = insertHeaderSettings(sql)) return sql.substr(static_cast<size_t>(header - sql.data()));
  if (insert.select && querySelectHasSettings(*insert.select)) return std::nullopt;
  return sql;
}

// rewriter-go OpaqueInsertQueryIsUngoverned over text rewriter-go does not
// structure: a FROM / JOIN / WITH keyword, more SELECT keywords than
// `max_selects` (the query itself may be one), an IN-family occurrence whose
// operand region is not literal-only, a lookup-family call (a quoted name
// decoded as ClickHouse decodes it) or an Identifier query parameter. A lexer
// error, or a quoted name that does not decode before `(`, fails closed.
bool opaqueTextIsUngoverned(std::string_view text, size_t max_selects) {
  if (textHasComment(text)) return true;  // a comment can hide a read from the comment-free scan
  if (textHasMalformedHexEscape(text)) return true;
  std::vector<DB::Token> tokens;
  if (!significantTokens(text, tokens) || tokensHoldIdentifierParameter(tokens)) return true;
  size_t selects = 0;
  for (size_t i = 0; i < tokens.size(); ++i) {
    const auto &token = tokens[i];
    if (inRegionRefusedAt(tokens, i)) return true;
    const bool call = i + 1 < tokens.size() && tokens[i + 1].type == DB::TokenType::OpeningRoundBracket;
    if (call && (token.type == DB::TokenType::BareWord || token.type == DB::TokenType::QuotedIdentifier)) {
      const auto name = identifierTokenName(token);
      if (!name || isStringLookupName(*name)) return true;
    }
    if (wordIs(token, "from") || wordIs(token, "join") || wordIs(token, "with")) return true;
    if (wordIs(token, "select") && ++selects > max_selects) return true;
  }
  return false;
}

// rewriter-go OpaqueTextQualifiedNames: every `name.name` run in the text, as
// (database, table) decoded as ClickHouse decodes a name; the table half of a
// longer run is never a qualifier. Empty on a lexer error (the lexical R2
// rule refuses that text).
std::vector<std::pair<std::string, std::string>> opaqueTextQualifiedNames(std::string_view text) {
  std::vector<DB::Token> tokens;
  std::vector<std::pair<std::string, std::string>> names;
  if (!significantTokens(text, tokens)) return names;
  for (size_t i = 0; i + 2 < tokens.size(); ++i) {
    if (!nameToken(tokens[i]) || tokens[i + 1].type != DB::TokenType::Dot || !nameToken(tokens[i + 2])) continue;
    if (i > 0 && tokens[i - 1].type == DB::TokenType::Dot) continue;
    const auto database = identifierTokenName(tokens[i]);
    const auto table = identifierTokenName(tokens[i + 2]);
    if (database && table) names.emplace_back(*database, *table);
  }
  return names;
}

// A subquery, a SELECT, or a FROM / JOIN table anywhere in an ALTER action,
// and a projection body carrying a WITH clause (rewriter-go refuses a WITH in
// a projection; the projection grammar has no FROM, so its own SELECT is not
// a read of another table).
bool expressionHasUngovernedRead(const DB::IAST &node) {
  if (node.as<DB::ASTSubquery>() || isSelectFamily(node) || node.as<DB::ASTTableIdentifier>()
      || node.as<DB::ASTTableExpression>())
    return true;
  if (const auto *projection = node.as<DB::ASTProjectionSelectQuery>(); projection && projection->with())
    return true;
  bool found = false;
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!found && child) found = expressionHasUngovernedRead(*child);
  });
  return found;
}

// An ALTER action is opaque text to the reference engine. MODIFY QUERY and
// the cross-table partition actions are always refused; any other action is
// refused when it carries a read.
bool alterCommandUngoverned(const DB::ASTAlterCommand &command) {
  switch (command.type) {
  case DB::ASTAlterCommand::MODIFY_QUERY:
  case DB::ASTAlterCommand::FETCH_PARTITION:
    return true;
  case DB::ASTAlterCommand::ATTACH_PARTITION:
  case DB::ASTAlterCommand::REPLACE_PARTITION:
    if (!command.from_table.empty()) return true;
    break;
  case DB::ASTAlterCommand::MOVE_PARTITION:
    if (command.move_destination_type == DB::DataDestinationType::TABLE) return true;
    break;
  default:
    break;
  }
  return expressionHasUngovernedRead(command);
}

} // namespace

std::optional<std::string_view> insertOpaqueQueryText(const DB::ASTInsertQuery &insert, std::string_view sql) {
  return opaqueInsertText(insert, sql);
}

bool rejectUngovernedReads(const DB::ASTPtr &ast, const std::string &sql, rewriter::RewriteSQLResponse *response,
                           const TableRewriteSelection *storage_integrity) {
  bool ungoverned = false;
  if (const auto *alter = ast->as<DB::ASTAlterQuery>(); alter && structuredAlter(*alter)) {
    // rewriter-go structures ADD COLUMN / ADD CONSTRAINT / DROP COLUMN and walks
    // them like a CREATE: a table-free subquery or a tuple IN operand there is
    // not a read.
    ungoverned = structuredStatementHasRead(*ast);
  } else if (alter) {
    if (!alter->command_list) return false;
    const auto &commands = alter->command_list->children;
    ungoverned = std::any_of(commands.begin(), commands.end(), [](const DB::ASTPtr &raw) {
      const auto *command = raw->as<DB::ASTAlterCommand>();
      return command && alterCommandUngoverned(*command);
    });
    // The IN rule is lexical, as in rewriter-go: the AST folds `0x1F` / `inf`
    // into plain literals and keeps no trace of an operand's spelling.
    ungoverned = ungoverned || textHasRefusedInRegion(sql);
  } else if (const auto *create = ast->as<DB::ASTCreateQuery>()) {
    // A REFRESH view reruns its query on a schedule, outside any session.
    ungoverned = create->refresh_strategy != nullptr || structuredStatementHasRead(*ast);
  } else if (const auto *insert = ast->as<DB::ASTInsertQuery>()) {
    if (insert->infile) {
      // FROM INFILE is always refused in dynamic mode (spec 2026-09-26 R12):
      // its rows come from a file, not from the statement.
      ungoverned = true;
    } else if (const auto opaque = opaqueInsertText(*insert, sql)) {
      const auto text = *opaque;
      // T3 defers an SI physical or reserved database to the SI handlers,
      // which never read this text, so it is named here while the SI surface
      // is active (rewriter-go rejectOpaqueReservedQualifiers).
      if (storage_integrity && surfaceActive(*storage_integrity)) {
        for (const auto &[database, table] : opaqueTextQualifiedNames(text)) {
          if (!isStorageIntegrityPhysicalDatabase(database, *storage_integrity->dynamic_args)) continue;
          recordAccessedTable(response, database, table, *storage_integrity);
          response->set_code(rewriter::RewriteCode::UnsupportedStatement);
          response->set_message(storageIntegrityPhysicalRejectMessage(database + "." + table));
          return true;
        }
      }
      ungoverned = opaqueTextIsUngoverned(text, /*max_selects=*/1);
    } else {
      // VALUES rows are read as rewriter-go's parser structures them; a text
      // that does not parse as expression rows fails closed. A comment or a
      // malformed `\x` escape anywhere in the payload is refused outright: the
      // SQL view of such a payload does not prove what ClickHouse's Values
      // reader executes (fail-closed; rewriter-go admits a read-free comment).
      const auto values = insertValues(*ast);
      const char *begin = nullptr;
      const char *end = nullptr;
      const bool has_text = insertValuesText(*insert, &begin, &end);
      const std::string_view payload = has_text ? std::string_view(begin, static_cast<size_t>(end - begin))
                                                : std::string_view();
      ungoverned = (has_text && (textHasComment(payload) || textHasMalformedHexEscape(payload)))
        || values.unparsed.has_value()
        || std::any_of(values.rows.begin(), values.rows.end(), structuredExpressionHasRead);
    }
  } else {
    ungoverned = structuredStatementHasRead(*ast);
  }
  if (!ungoverned) return false;
  response->set_code(rewriter::RewriteCode::UnsupportedStatement);
  response->set_message(std::string(kUnsupportedStatementMessage));
  return true;
}

namespace {

// An IN operand made of literals only: a literal, or a tuple / array / sign /
// arithmetic operator over literals (`IN (1, 2)`, `IN [1, 2]`, `IN -1`).
bool literalOnly(const DB::IAST &node) {
  if (node.as<DB::ASTLiteral>()) return true;
  if (const auto *list = node.as<DB::ASTExpressionList>()) {
    return std::all_of(list->children.begin(), list->children.end(),
                       [](const DB::ASTPtr &child) { return child && literalOnly(*child); });
  }
  if (const auto *fn = node.as<DB::ASTFunction>(); fn && fn->isOperator())
    return !fn->arguments || literalOnly(*fn->arguments);
  return false;
}

// R7 over a SHOW trailing clause as ClickHouse parsed it: a subquery, an
// IN-family operand that is not literal-only, a query parameter, or a name
// part holding a dot (a quoted dotted name such as `db2.x`). These are the
// shapes that can read a table; the text rule below adds the function calls.
bool showExpressionUngoverned(const DB::IAST &node) {
  if (node.as<DB::ASTSubquery>() || isSelectFamily(node) || node.as<DB::ASTQueryParameter>()) return true;
  if (const auto *in = inFamilyCall(node); in && !literalOnly(*in->arguments->children[1])) return true;
  if (const auto *id = dynamic_cast<const DB::ASTIdentifier *>(&node)) {
    if (id->isParam()) return true;
    for (const auto &part : id->name_parts)
      if (part.find('.') != std::string::npos) return true;
  }
  bool found = false;
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!found && child) found = showExpressionUngoverned(*child);
  });
  return found;
}

void showQualifiedNames(const DB::IAST &node, std::vector<std::pair<std::string, std::string>> &out) {
  if (const auto *id = dynamic_cast<const DB::ASTIdentifier *>(&node); id && id->name_parts.size() >= 2)
    out.emplace_back(id->name_parts[0], id->name_parts[1]);
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (child) showQualifiedNames(*child, out);
  });
}

// The text of a SHOW statement's trailing clauses (rewriter-go showBodyStart):
// from its first top-level WHERE / LIKE / ILIKE / LIMIT / OFFSET / SETTINGS /
// FORMAT / INTO keyword after the SHOW kind to the end; empty when it has
// none. std::nullopt on a lexer error.
std::optional<std::string_view> showBodyText(std::string_view sql) {
  std::vector<DB::Token> tokens;
  if (!significantTokens(sql, tokens)) return std::nullopt;
  int depth = 0;
  for (size_t i = 2; i < tokens.size(); ++i) {
    if (isOpeningBracket(tokens[i])) ++depth;
    else if (isClosingBracket(tokens[i])) --depth;
    if (depth != 0) continue;
    for (const char *word : {"where", "like", "ilike", "limit", "offset", "settings", "format", "into"})
      if (wordIs(tokens[i], word)) return sql.substr(static_cast<size_t>(tokens[i].begin - sql.data()));
  }
  return std::string_view();
}

// Keywords a `(` may follow in an expression without making a function call.
// CAST and IF are ClickHouse functions that rewriter-go's tokenizer reads as
// keywords (it admits `CAST(` / `IF(` and refuses `EXTRACT(` / `TRIM(`);
// neither reads a table, and their arguments are scanned like any other.
bool keywordBeforeParenthesis(const DB::Token &token) {
  for (const char *word : {"in", "not", "and", "or", "like", "ilike", "between", "is", "case", "when", "then",
         "else", "where", "limit", "offset", "exists", "any", "all", "global", "interval", "cast", "if"})
    if (wordIs(token, word)) return true;
  return false;
}

// rewriter-go ShowBodyIsUngoverned over the trailing-clause text: a SELECT or
// WITH keyword, a function call (a name, bare or quoted, followed by `(`), an
// IN-family occurrence whose operand region is not literal-only, a `{` (a
// query parameter or a map literal) or a quoted name holding a dot. A lexer
// error, or a quoted name that does not decode, fails closed.
bool showBodyTextUngoverned(std::string_view text) {
  std::vector<DB::Token> tokens;
  if (!significantTokens(text, tokens)) return true;
  for (size_t i = 0; i < tokens.size(); ++i) {
    const auto &token = tokens[i];
    if (wordIs(token, "select") || wordIs(token, "with")) return true;
    if (token.type == DB::TokenType::OpeningCurlyBrace) return true;
    if (token.type == DB::TokenType::QuotedIdentifier) {
      const auto name = identifierTokenName(token);
      if (!name || name->find('.') != std::string::npos
          || std::string_view(token.begin, token.size()).find('.') != std::string_view::npos)
        return true;
    }
    const bool call = i + 1 < tokens.size() && tokens[i + 1].type == DB::TokenType::OpeningRoundBracket;
    if (call && nameToken(token) && !keywordBeforeParenthesis(token)) return true;
    if (inRegionRefusedAt(tokens, i)) return true;
  }
  return false;
}

} // namespace

bool showBodyIsUngoverned(const DB::IAST &show, const std::string &sql) {
  for (const auto *body : showBodies(show))
    if (showExpressionUngoverned(*body)) return true;
  // Calls and the IN rule are read from the text, as rewriter-go reads them:
  // the AST folds `inf` / `0x1F` / `1 AS z` operands into plain literals and
  // does not record whether a function was written as a call (CASE builds
  // multiIf).
  const auto text = showBodyText(sql);
  return !text || showBodyTextUngoverned(*text);
}

std::pair<std::string, std::string> showBodyStorageIntegrityName(
  const DB::IAST &show, const rewriter::RewriteTableDynamicArgs &args) {
  std::vector<std::pair<std::string, std::string>> names;
  for (const auto *body : showBodies(show)) showQualifiedNames(*body, names);
  for (const auto &name : names)
    if (isStorageIntegrityPhysicalDatabase(name.first, args)) return name;
  return {};
}

bool rejectShowBody(const DB::IAST &show, const std::string &sql,
                    const rewriter::RewriteTableDynamicArgs *dynamic,
                    rewriter::RewriteSQLResponse *response) {
  if (!dynamic) return false;
  if (storageIntegritySurfaceActive(*dynamic)) {
    const auto [database, table] = showBodyStorageIntegrityName(show, *dynamic);
    if (!database.empty()) {
      recordPhysicalStorageIntegrityAccess(response, database, table, database);
      response->set_code(rewriter::RewriteCode::UnsupportedStatement);
      response->set_message(storageIntegrityPhysicalRejectMessage(database + "." + table));
      return true;
    }
  }
  if (!showBodyIsUngoverned(show, sql)) return false;
  response->set_code(rewriter::RewriteCode::UnsupportedStatement);
  response->set_message(std::string(kUnsupportedStatementMessage));
  return true;
}

namespace {

// A read a lambda body cannot carry (rewriter-go does not model it): a query
// (subquery / EXISTS), an operator-form IN / NOT IN / GLOBAL IN over any
// operand, or an in-family **callable** (`in(x, t)`, `globalIn(y, p)`) whose
// operand is a table identifier. Measured on 26.2 / 25.8: Go refuses all of
// these in a lambda but admits a callable IN over a literal (`in(x, (1, 2))`),
// so a callable is a read only when its operand is a non-param identifier.
bool holdsLambdaRefusableRead(const DB::IAST &node) {
  if (node.as<DB::ASTSubquery>() || node.as<DB::ASTSelectWithUnionQuery>() || node.as<DB::ASTSelectQuery>()
      || node.as<DB::ASTSelectIntersectExceptQuery>())
    return true;
  if (const auto *fn = node.as<DB::ASTFunction>(); fn && isInFamilyFunction(fn->name)) {
    if (fn->isOperator()) return true;  // operator-form IN, any operand
    if (fn->arguments && fn->arguments->children.size() == 2) {
      const auto *operand = fn->arguments->children[1]->as<DB::ASTIdentifier>();
      if (operand && !operand->isParam()) return true;  // callable `in(x, <table>)`
    }
  }
  bool found = false;
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!found && child) found = holdsLambdaRefusableRead(*child);
  });
  return found;
}

// A lambda (an APPLY transformer's or any higher-order function's) whose body
// holds a read it cannot carry (above). rewriter-go refuses the statement with
// T7 (the SI catch-all while the surface is active) wherever the lambda sits.
bool lambdaHoldsRefusableRead(const DB::IAST &node) {
  if (const auto *fn = node.as<DB::ASTFunction>();
      fn && fn->name == "lambda" && fn->arguments && fn->arguments->children.size() == 2
      && fn->arguments->children[1] && holdsLambdaRefusableRead(*fn->arguments->children[1]))
    return true;
  bool found = false;
  forEachChild(node, [&](const DB::ASTPtr &child) {
    if (!found && child) found = lambdaHoldsRefusableRead(*child);
  });
  return found;
}

} // namespace

bool preflightTableReferences(const DB::ASTPtr &ast,
  const rewriter::RewriteSQLRequest *request,
  rewriter::RewriteSQLResponse *response,
  std::optional<SystemTableRefusal> *deferred_system_table) {
  const auto sel = findActiveTableRewrite(request->options());
  if (sel.mode != TableRewriteMode::Dynamic || !ast) return false;
  const auto &args = *sel.dynamic_args;
  const bool active = storageIntegritySurfaceActive(args);
  const bool unmodelled = unmodelledStatementClass(ast, sel);

  // A lambda whose body holds a read it cannot carry (a query, an operator-form
  // IN, or a callable IN over a table identifier) is refused before any other
  // check, as rewriter-go's walkers refuse it (Task I3).
  if (lambdaHoldsRefusableRead(*ast)) {
    response->set_code(rewriter::RewriteCode::UnsupportedStatement);
    response->set_message(std::string(active ? kStorageIntegrityUnmodelledMessage : kUnsupportedStatementMessage));
    return true;
  }

  // An INSERT's VALUES rows and the text rewriter-go keeps opaque after a
  // column list and a SETTINGS clause carry T2 / T3 / T6 findings too.
  const auto values = insertValues(*ast);
  const auto *insert = ast->as<DB::ASTInsertQuery>();
  const auto opaque_text = insert ? opaqueInsertText(*insert, request->sql()) : std::nullopt;
  auto opaqueTextHoldsParameter = [&] {
    std::vector<DB::Token> tokens;
    return opaque_text && significantTokens(*opaque_text, tokens) && tokensHoldIdentifierParameter(tokens);
  };

  // T2. An unmodelled class is opaque to the reference engine: a parameter
  // anywhere in it names the outcome (spec §5 "Command-text statements").
  if (unmodelled ? anyIdentifierParameter(*ast)
                 : tablePositionParameter(*ast)
                   || std::any_of(values.rows.begin(), values.rows.end(),
                                  [](const DB::ASTPtr &row) { return tablePositionParameter(*row); })
                   || opaqueTextHoldsParameter()) {
    response->set_code(rewriter::RewriteCode::InvalidRewriteRequest);
    response->set_message(std::string(kIdentifierParameterMessage));
    return true;
  }

  // A parametrised alias: the engine cannot re-emit it (anyParametrisedAlias),
  // so the statement is refused rather than forwarded without it.
  if (anyParametrisedAlias(*ast)) {
    response->set_code(rewriter::RewriteCode::UnsupportedStatement);
    response->set_message(std::string(kUnsupportedStatementMessage));
    return true;
  }

  // Shapes no handler models inside an admitted class, refused with T7 in
  // both SI states before their names are examined: a TimeSeries table's
  // target clauses, as rewriter-go refuses them (Task I2 review M4), and the
  // ClickHouse 26.8 additions listed at unmodelledClickHouse268Shape.
  if (unmodelledClickHouse268Shape(*ast)) {
    response->set_code(rewriter::RewriteCode::UnsupportedStatement);
    response->set_message(std::string(kUnsupportedStatementMessage));
    return true;
  }

  // T7 before T3: an unmodelled class is refused before its names are
  // examined. With the SI surface active the SI write preflight names an SI
  // target first, and the reject annotator may name the SI object.
  if (unmodelled) {
    if (active && storageIntegrityWritePreflight(ast, sel, response)) return true;
    // Report the target of a DETACH / ATTACH / OPTIMIZE / UNDROP, the classes
    // rewriter-go's RawTableRefs reads, when it is ordinary. An SI target is
    // left to the reject annotator: an SI-flagged entry would stop it.
    const auto kind = ast->getQueryKind();
    const bool records_target = ast->as<DB::ASTDropQuery>() || ast->as<DB::ASTCreateQuery>()
      || kind == DB::IAST::QueryKind::Optimize || kind == DB::IAST::QueryKind::Undrop;
    if (const auto *q = dynamic_cast<const DB::ASTQueryWithTableAndOutput *>(ast.get()); q && records_target) {
      const std::string effective = q->getDatabase().empty()
        ? storageIntegrityExecutionDatabase(args) : q->getDatabase();
      if (!active || (!lookupStorageIntegrity(q->getDatabase(), q->getTable(), args)
                      && !isStorageIntegrityPhysicalDatabase(effective, args)))
        recordAccessedTable(response, q->getDatabase(), q->getTable(), sel);
    }
    response->set_code(rewriter::RewriteCode::UnsupportedStatement);
    response->set_message(std::string(active ? kStorageIntegrityUnmodelledMessage
                                             : kUnsupportedStatementMessage));
    return true;
  }

  // T3. With the SI surface active an SI physical or reserved database
  // defers to the SI handlers (their messages are pinned), except in a
  // string-lookup argument, which no SI handler classifies.
  std::vector<DatabaseRef> databases;
  collectDatabases(*ast, false, databases);
  // rewriter-go reads the VALUES rows, and the qualifier of every `db.name`
  // run in opaque text. A lookup argument is SI-handler-blind; an SI name in a
  // row or in opaque text is left to the SI write path.
  for (const auto &row : values.rows) collectDatabases(*row, false, databases);
  if (opaque_text) {
    for (auto &[database, table] : opaqueTextQualifiedNames(*opaque_text)) databases.push_back({database, false});
  }
  if (!args.upstream_logical_database_in_context().empty())
    databases.push_back({args.upstream_logical_database_in_context(), false});
  for (const auto &ref : databases) {
    if (!protectedDatabase(ref.database, args)) continue;
    if (active && !ref.blind && isStorageIntegrityPhysicalDatabase(ref.database, args)) continue;
    // A blind SI physical name is reported as the SI object it is, as the SI
    // handlers report one (rewriter-go parity).
    if (active && isStorageIntegrityPhysicalDatabase(ref.database, args))
      recordPhysicalStorageIntegrityAccess(response, ref.database, "", ref.database);
    else
      recordAccessedDatabase(response, ref.database, &args);
    response->set_code(rewriter::RewriteCode::InvalidRewriteRequest);
    response->set_message(protectedDatabaseRejectMessage(ref.database));
    return true;
  }

  // System tables (spec §5 "The system database", rewriter-go #58): a
  // caller-input reference to a system table outside the allowlist, in any
  // position, right after T3 and before T5 / T6 / R5 / R2 and the handlers,
  // in both SI states. While the SI surface is active and the statement also
  // names a storage-integrity object, the SI pipeline answers first and the
  // caller applies the refusal to its would-be Success. A text that cannot be
  // tokenized fails closed.
  if (const auto scan = scanSystemTables(ast, request->sql(), args, &values.rows); scan.failed) {
    response->set_code(rewriter::RewriteCode::UnsupportedStatement);
    response->set_message(std::string(active ? kStorageIntegrityUnmodelledMessage : kUnsupportedStatementMessage));
    return true;
  } else if (scan.refused) {
    if (!active || !scan.storage_integrity_object) {
      rejectSystemTable(response, scan.refusal, request->sql(), sel);
      return true;
    }
    if (deferred_system_table) *deferred_system_table = scan.refusal;
  }

  // T5 on a DESCRIBE target that is a table function, in both SI states: the
  // DESCRIBE handler forwards the statement verbatim and no SI handler
  // classifies a function target (rewriter-go's command-text findings run
  // the allowlist on it in both states).
  if (const auto *fn = describeTargetFunction(*ast); fn && rejectTableFunction(fn->name, response))
    return true;

  // T5 while the surface is inactive; the SI handlers run it after their own
  // namespace policy while it is active.
  if (!active && rejectDisallowedCarriers(ast, response)) return true;
  // T6 in both SI states: no SI handler examines a lookup call.
  if (rejectStringLookups(ast, values.rows, response)) return true;
  // R5 while the surface is inactive; the SI handlers run it after their own
  // checks while it is active.
  if (!active && rejectQuerySettings(ast, response, &request->sql())) return true;
  // R2 while the surface is inactive; the SI write path runs it after the SI
  // write preflight while it is active.
  if (!active && rejectUngovernedReads(ast, request->sql(), response)) return true;
  return false;
}

} // namespace rewriter_handlers
