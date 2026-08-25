// src/handlers/storage_integrity.cc — housegate Spec G policy helpers.
#include "handlers/storage_integrity.h"

#include <Parsers/ASTColumnsTransformers.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTDictionary.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTFunctionWithKeyValueArguments.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSelectIntersectExceptQuery.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTSubquery.h>
#include <Parsers/ASTSystemQuery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ASTWithElement.h>
#include <Parsers/ParserSelectWithUnionQuery.h>
#include <Parsers/parseQuery.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "ASTTransformers.h"
#include "handlers/common.h"
#include "handlers/cte_scope.h"
#include "handlers/name_rewrite.h"

namespace rewriter_handlers {

namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool simpleIdentifier(const std::string &value) {
  if (value.empty()) return false;
  for (size_t i = 0; i < value.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    const bool alpha = std::isalpha(c) != 0 || c == '_';
    const bool digit = std::isdigit(c) != 0;
    if (!alpha && !(i > 0 && digit)) return false;
  }
  return true;
}

bool splitExactQualified(const std::string &value, std::string *database, std::string *table) {
  const auto dot = value.find('.');
  if (dot == std::string::npos || value.find('.', dot + 1) != std::string::npos) return false;
  const std::string db = value.substr(0, dot);
  const std::string tbl = value.substr(dot + 1);
  if (!simpleIdentifier(db) || !simpleIdentifier(tbl)) return false;
  if (database) *database = db;
  if (table) *table = tbl;
  return true;
}

std::pair<std::string, std::string> splitPhysical(const std::string &value) {
  const auto dot = value.find('.');
  if (dot == std::string::npos) return {"", value};
  return {value.substr(0, dot), value.substr(dot + 1)};
}

bool needsQuoting(const std::string &value) {
  return !simpleIdentifier(value);
}

std::string quoteIdent(const std::string &value) {
  if (!needsQuoting(value)) return value;
  std::string out = "`";
  for (char c : value) {
    if (c == '`')
      out += "``";
    else
      out += c;
  }
  out += '`';
  return out;
}

std::string quotePhysical(const std::string &value) {
  const auto [database, table] = splitPhysical(value);
  return database.empty() ? quoteIdent(table) : quoteIdent(database) + "." + quoteIdent(table);
}

std::optional<std::string> staticNamespaceArg(const DB::ASTPtr &arg) {
  if (!arg) return std::nullopt;
  if (const auto *literal = arg->as<DB::ASTLiteral>()) {
    if (literal->value.getType() == DB::Field::Types::String)
      return literal->value.safeGet<String>();
    return std::nullopt;
  }
  if (const auto *identifier = arg->as<DB::ASTIdentifier>()) return identifier->name();
  return std::nullopt;
}

bool isCurrentDatabaseArg(const DB::ASTPtr &arg) {
  const auto *fn = arg ? arg->as<DB::ASTFunction>() : nullptr;
  return fn && lower(fn->name) == "currentdatabase";
}

const DB::ASTs &functionArgs(const DB::ASTFunction &fn) {
  static const DB::ASTs empty;
  if (!fn.arguments) return empty;
  return fn.arguments->children;
}

NamespaceRef decodeSingle(
  NamespaceRefSource source, const std::string &name, const DB::ASTPtr &arg) {
  NamespaceRef ref{source, name};
  ref.uses_current_database = true;
  if (isCurrentDatabaseArg(arg)) return ref;
  const auto value = staticNamespaceArg(arg);
  if (!value) return ref;
  std::string database, table;
  if (splitExactQualified(*value, &database, &table)) {
    ref.database = database;
    ref.table = table;
    ref.resolved = true;
    ref.uses_current_database = false;
    return ref;
  }
  ref.table = *value;
  return ref;
}

NamespaceRef decodePair(
  NamespaceRefSource source, const std::string &name, const DB::ASTs &args, size_t first) {
  NamespaceRef ref{source, name};
  if (first >= args.size()) return ref;
  if (isCurrentDatabaseArg(args[first])) {
    ref.uses_current_database = true;
    if (first + 1 < args.size()) {
      if (const auto table = staticNamespaceArg(args[first + 1])) ref.table = *table;
    }
    return ref;
  }
  const auto first_arg = staticNamespaceArg(args[first]);
  if (first_arg) {
    std::string database, table;
    if (splitExactQualified(*first_arg, &database, &table)) {
      ref.database = database;
      ref.table = table;
      ref.resolved = true;
      return ref;
    }
    ref.database = *first_arg;
  }
  if (first + 1 >= args.size()) return ref;
  const auto second_arg = staticNamespaceArg(args[first + 1]);
  if (second_arg) ref.table = *second_arg;
  ref.resolved =
    first_arg.has_value() && second_arg.has_value() && !first_arg->empty() && !second_arg->empty();
  return ref;
}

std::optional<NamespaceRef> decodeNamespaceFunction(
  const DB::ASTFunction &fn, NamespaceRefSource source) {
  const std::string name = fn.name;
  const std::string lname = lower(name);
  const auto &args = functionArgs(fn);
  if (lname == "remote" || lname == "remotesecure" || lname == "cluster" ||
      lname == "clusterallreplicas")
    return decodePair(source, name, args, 1);
  if (lname == "merge") {
    if (args.size() == 1) return decodeSingle(source, name, args[0]);
    return decodePair(source, name, args, 0);
  }
  if (lname == "loop" || lname == "dictionary" || lname == "timeseriesdata" ||
      lname == "timeseriestags" || lname == "timeseriesmetrics") {
    if (args.size() == 1) return decodeSingle(source, name, args[0]);
    return decodePair(source, name, args, 0);
  }
  if (lname == "timeseriesselector") {
    if (args.size() == 4 && !args.empty()) return decodeSingle(source, name, args[0]);
    return decodePair(source, name, args, 0);
  }
  if (lname == "prometheusquery") {
    if (args.size() == 3 && !args.empty()) return decodeSingle(source, name, args[0]);
    return decodePair(source, name, args, 0);
  }
  if (lname == "prometheusqueryrange") {
    if (args.size() == 5 && !args.empty()) return decodeSingle(source, name, args[0]);
    return decodePair(source, name, args, 0);
  }
  if (lname != "mergetree" && lname.starts_with("mergetree"))
    return decodePair(source, name, args, 0);
  return std::nullopt;
}

std::optional<NamespaceRef> decodeInFunction(const DB::ASTFunction &fn) {
  std::string lname = lower(fn.name);
  // ClickHouse registers the IgnoreSet implementations as callable aliases of
  // the same IN family. Normalize the implementation suffix so every alias is
  // classified by this single namespace-target policy.
  constexpr std::string_view ignore_set_suffix = "ignoreset";
  if (lname.ends_with(ignore_set_suffix))
    lname.resize(lname.size() - ignore_set_suffix.size());
  std::string display;
  if (lname == "in")
    display = "IN";
  else if (lname == "nullin")
    display = "NULL IN";
  else if (lname == "globalin")
    display = "GLOBAL IN";
  else if (lname == "globalnullin")
    display = "GLOBAL NULL IN";
  else if (lname == "notin")
    display = "NOT IN";
  else if (lname == "notnullin")
    display = "NOT NULL IN";
  else if (lname == "globalnotin")
    display = "GLOBAL NOT IN";
  else if (lname == "globalnotnullin")
    display = "GLOBAL NOT NULL IN";
  else
    return std::nullopt;
  const auto &args = functionArgs(fn);
  if (args.size() != 2 || !args[1]->as<DB::ASTIdentifier>()) return std::nullopt;
  NamespaceRef ref = decodeSingle(NamespaceRefSource::InTable, display, args[1]);
  if (ref.table.empty() && !ref.resolved) return std::nullopt;
  return ref;
}

std::optional<DB::ASTPtr> ownedChild(const DB::IAST &parent, const DB::IAST *child) {
  if (!child) return std::nullopt;
  for (const auto &candidate : parent.children)
    if (candidate.get() == child) return candidate;
  return std::nullopt;
}

struct SQLToken {
  enum class Kind { Word, Identifier, String, Symbol } kind;
  std::string text;
  std::string upper;
  size_t begin = 0;
  size_t end = 0;
  int depth = 0;
};

std::string upper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
}

int hexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

char decodedQuotedEscape(char value) {
  switch (value) {
  case 'a': return '\a';
  case 'b': return '\b';
  case 'e': return '\x1b';
  case 'f': return '\f';
  case 'n': return '\n';
  case 'r': return '\r';
  case 't': return '\t';
  case 'v': return '\v';
  case '0': return '\0';
  default: return value;
  }
}

std::vector<SQLToken> tokenizeSQL(const std::string &sql) {
  std::vector<SQLToken> out;
  int depth = 0;
  for (size_t i = 0; i < sql.size();) {
    const unsigned char c = static_cast<unsigned char>(sql[i]);
    if (std::isspace(c)) {
      ++i;
      continue;
    }
    if (sql[i] == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
      i += 2;
      while (i < sql.size() && sql[i] != '\n') ++i;
      continue;
    }
    if (sql[i] == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
      i += 2;
      while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/')) ++i;
      i = std::min(sql.size(), i + 2);
      continue;
    }
    const size_t begin = i;
    if (sql[i] == '\'' || sql[i] == '`' || sql[i] == '"') {
      const char quote = sql[i++];
      std::string value;
      while (i < sql.size()) {
        if (sql[i] == quote) {
          if (i + 1 < sql.size() && sql[i + 1] == quote) {
            value += quote;
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        if (sql[i] == '\\' && i + 1 < sql.size()) {
          // Quoted identifiers are grammar objects, so their token value must
          // match ClickHouse's semantic AST name. In particular `\xHH` is one
          // byte, not the three literal characters xHH. String tokens remain
          // parser-decoded only once and are never identifier targets here.
          if (quote != '\'' && i + 3 < sql.size() && sql[i + 1] == 'x') {
            const int hi = hexDigit(sql[i + 2]);
            const int lo = hexDigit(sql[i + 3]);
            if (hi >= 0 && lo >= 0) {
              value += static_cast<char>((hi << 4) | lo);
              i += 4;
              continue;
            }
          }
          value += decodedQuotedEscape(sql[i + 1]);
          i += 2;
          continue;
        }
        value += sql[i++];
      }
      const auto kind = quote == '\'' ? SQLToken::Kind::String : SQLToken::Kind::Identifier;
      out.push_back({kind,
        value,
        kind == SQLToken::Kind::Identifier ? upper(value) : std::string(),
        begin,
        i,
        depth});
      continue;
    }
    if (std::isalpha(c) || sql[i] == '_') {
      ++i;
      while (i < sql.size()) {
        const unsigned char d = static_cast<unsigned char>(sql[i]);
        if (!std::isalnum(d) && sql[i] != '_' && sql[i] != '$') break;
        ++i;
      }
      const std::string value = sql.substr(begin, i - begin);
      out.push_back({SQLToken::Kind::Word, value, upper(value), begin, i, depth});
      continue;
    }
    if (std::isdigit(c)) {
      ++i;
      while (i < sql.size() &&
             (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '.' || sql[i] == '_'))
        ++i;
      out.push_back({SQLToken::Kind::Symbol, sql.substr(begin, i - begin), "", begin, i, depth});
      continue;
    }
    if (sql[i] == ')') depth = std::max(0, depth - 1);
    out.push_back({SQLToken::Kind::Symbol, std::string(1, sql[i]), "", begin, begin + 1, depth});
    if (sql[i] == '(') ++depth;
    ++i;
  }
  return out;
}

bool identifierToken(const SQLToken &token) {
  return token.kind == SQLToken::Kind::Word || token.kind == SQLToken::Kind::Identifier;
}

struct LexicalTarget {
  std::string database;
  std::string table;
  size_t next = 0;
};

std::optional<LexicalTarget> targetAt(const std::vector<SQLToken> &tokens, size_t index) {
  if (index >= tokens.size() || !identifierToken(tokens[index])) return std::nullopt;
  if (index + 1 < tokens.size() && tokens[index + 1].text == "(") return std::nullopt;
  LexicalTarget out;
  out.table = tokens[index].text;
  out.next = index + 1;
  if (index + 2 < tokens.size() && tokens[index + 1].text == "." &&
      (identifierToken(tokens[index + 2]) || tokens[index + 2].text == "*")) {
    out.database = out.table;
    out.table = tokens[index + 2].text;
    out.next = index + 3;
  }
  return out;
}

std::string lexicalKey(const LexicalTarget &target) {
  return target.database.empty() ? target.table : target.database + "." + target.table;
}

std::vector<LexicalTarget> collectLexicalSelectTargets(const std::vector<SQLToken> &tokens) {
  std::vector<LexicalTarget> targets;
  std::map<int, bool> in_from;
  const std::set<std::string> ends = {"WHERE",
    "PREWHERE",
    "GROUP",
    "HAVING",
    "ORDER",
    "LIMIT",
    "SETTINGS",
    "UNION",
    "INTERSECT",
    "EXCEPT"};
  auto append = [&](const std::optional<LexicalTarget> &target) {
    if (!target) return;
    for (const auto &existing : targets)
      if (existing.database == target->database && existing.table == target->table) return;
    targets.push_back(*target);
  };
  for (size_t i = 0; i < tokens.size(); ++i) {
    const auto &token = tokens[i];
    if (ends.count(token.upper)) in_from[token.depth] = false;
    if (token.upper == "FROM" || token.upper == "JOIN") {
      in_from[token.depth] = true;
      append(targetAt(tokens, i + 1));
    } else if (token.text == "," && in_from[token.depth]) {
      append(targetAt(tokens, i + 1));
    }
  }
  return targets;
}

bool targetTouchesStorageIntegrity(
  const LexicalTarget &target, const rewriter::RewriteTableDynamicArgs &args) {
  if (lookupStorageIntegrity(target.database, target.table, args)) return true;
  const std::string physical =
    target.database.empty() ? storageIntegrityExecutionDatabase(args) : target.database;
  return isStorageIntegrityPhysicalDatabase(physical, args);
}

TableRewriteSelection dynamicSelection(const rewriter::RewriteTableDynamicArgs &args) {
  TableRewriteSelection selection;
  selection.mode = TableRewriteMode::Dynamic;
  selection.dynamic_args = &args;
  return selection;
}

void recordLexicalTargets(rewriter::RewriteSQLResponse *response,
  const std::vector<LexicalTarget> &targets,
  const rewriter::RewriteTableDynamicArgs &args) {
  const auto selection = dynamicSelection(args);
  for (const auto &target : targets) {
    bool exists = false;
    for (const auto &entry : response->original_accessed_tables()) {
      if (entry.original_database() == target.database && entry.original_table() == target.table) {
        exists = true;
        break;
      }
    }
    if (!exists) recordAccessedTable(response, target.database, target.table, selection);
  }
}

bool rejectLexicalStorageIntegrityTarget(rewriter::RewriteSQLResponse *response,
  const LexicalTarget &target,
  const rewriter::RewriteTableDynamicArgs &args,
  rewriter::RewriteCode code) {
  const auto selection = dynamicSelection(args);
  if (target.table == "*" || target.table.empty()) {
    const std::string database =
      target.database.empty() ? args.upstream_logical_database_in_context() : target.database;
    if (!isStorageIntegrityPhysicalDatabase(database, args)) return false;
    recordPhysicalStorageIntegrityAccess(response, database, "", database);
    response->set_code(code);
    response->set_message(storageIntegrityPhysicalDatabaseRejectMessage(database));
    return true;
  }
  if (auto hit = lookupStorageIntegrity(target.database, target.table, args)) {
    recordAccessedTable(response, target.database, target.table, selection);
    std::string logical;
    if (!authorizeStorageIntegrityLogical(target.database, args, &logical)) {
      response->set_code(rewriter::RewriteCode::InvalidRewriteRequest);
      response->set_message(storageIntegrityUnauthorizedMessage(logical));
    } else {
      response->set_code(code);
      response->set_message(storageIntegrityWriteRejectMessage(hit->logical_key));
    }
    return true;
  }
  const std::string physical =
    target.database.empty() ? storageIntegrityExecutionDatabase(args) : target.database;
  if (!isStorageIntegrityPhysicalDatabase(physical, args)) return false;
  recordAccessedTable(response, target.database, target.table, selection);
  response->set_code(code);
  response->set_message(storageIntegrityPhysicalRejectMessage(physical + "." + target.table));
  return true;
}

std::string trimCopy(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.erase(value.begin());
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
  return value;
}

std::string formatMappedTarget(const std::string &mapped) {
  const auto dot = mapped.find('.');
  if (dot == std::string::npos) return quoteIdent(mapped);
  return quoteIdent(mapped.substr(0, dot)) + "." + quoteIdent(mapped.substr(dot + 1));
}

std::optional<NamespaceRef> decodeDictionarySource(const DB::ASTCreateQuery &create) {
  if (!create.is_dictionary || !create.dictionary || !create.dictionary->source)
    return std::nullopt;
  const auto &source = *create.dictionary->source;
  if (lower(source.name) != "clickhouse") return std::nullopt;
  NamespaceRef ref{NamespaceRefSource::DictionarySource, "CLICKHOUSE"};
  const auto *elements = source.elements ? source.elements->as<DB::ASTExpressionList>() : nullptr;
  if (!elements) return ref;
  DB::ASTPtr database_arg;
  DB::ASTPtr table_arg;
  bool has_opaque_setting = false;
  for (const auto &raw : elements->children) {
    const auto *pair = raw->as<DB::ASTPair>();
    if (!pair) continue;
    const auto value = ownedChild(*pair, pair->second);
    const std::string key = lower(pair->first);
    if (key == "db" || key == "database")
      database_arg = value.value_or(nullptr);
    else if (key == "table")
      table_arg = value.value_or(nullptr);
    else if (key == "query" || key == "where" || key == "invalidate_query" || key == "name")
      // SQL-bearing filters/probes and named collections can override or
      // extend DB/TABLE. Constant DB/TABLE values therefore do not prove the
      // final execution namespace and SI policy must fail closed.
      has_opaque_setting = true;
  }
  if (has_opaque_setting) {
    if (database_arg) {
      const DB::ASTs args{database_arg, table_arg};
      ref = decodePair(NamespaceRefSource::DictionarySource, "CLICKHOUSE", args, 0);
      ref.resolved = false;
      ref.uses_current_database = false;
      return ref;
    }
    if (table_arg) {
      if (const auto table = staticNamespaceArg(table_arg)) ref.table = *table;
    }
    return ref;
  }
  if (!database_arg && !table_arg) return ref;
  if (!database_arg) {
    ref = decodeSingle(NamespaceRefSource::DictionarySource, "CLICKHOUSE", table_arg);
    ref.uses_current_database = true;
    return ref;
  }
  const DB::ASTs args{database_arg, table_arg};
  return decodePair(NamespaceRefSource::DictionarySource, "CLICKHOUSE", args, 0);
}

// Split a table identifier's "db.table" (or bare "table") at the first dot,
// the same split collectAccessedTablePairsFromAST uses.
std::pair<std::string, std::string> splitTableIdentifier(const std::string &full) {
  if (const auto dot = full.find('.'); dot != std::string::npos)
    return {full.substr(0, dot), full.substr(dot + 1)};
  return {std::string(), full};
}

void collectReadRefs(const DB::ASTPtr &ast, std::vector<StorageIntegrityReadRef> &out,
  const std::unordered_set<std::string> &cte_scope) {
  if (!ast) return;

  if (const auto *expression = ast->as<DB::ASTTableExpression>()) {
    const auto *identifier = expression->database_and_table_name
      ? expression->database_and_table_name->as<DB::ASTTableIdentifier>()
      : nullptr;
    if (identifier) {
      const auto [database, table] = splitTableIdentifier(identifier->name());
      const bool is_cte_ref = database.empty() && cte_scope.count(table) > 0;
      if (!table.empty() && !is_cte_ref) {
        StorageIntegrityReadRef event;
        event.kind = StorageIntegrityReadRef::Kind::Table;
        event.database = database;
        event.table = table;
        out.push_back(std::move(event));
      }
    }
  }
  if (const auto *create = ast->as<DB::ASTCreateQuery>()) {
    if (create->storage && create->storage->engine) {
      const std::string lname = lower(create->storage->engine->name);
      const auto &args = functionArgs(*create->storage->engine);
      std::optional<NamespaceRef> ref;
      if (lname == "remote" || lname == "distributed")
        ref = decodePair(
          NamespaceRefSource::TableEngine, create->storage->engine->name, args, 1);
      else if (lname == "merge" || lname == "buffer")
        ref = decodePair(
          NamespaceRefSource::TableEngine, create->storage->engine->name, args, 0);
      if (ref) {
        StorageIntegrityReadRef event;
        event.kind = StorageIntegrityReadRef::Kind::Namespace;
        event.namespace_ref = std::move(*ref);
        out.push_back(std::move(event));
      }
    }
    if (auto ref = decodeDictionarySource(*create)) {
      StorageIntegrityReadRef event;
      event.kind = StorageIntegrityReadRef::Kind::Namespace;
      event.namespace_ref = std::move(*ref);
      out.push_back(std::move(event));
    }
  }
  if (const auto *fn = ast->as<DB::ASTFunction>()) {
    // Namespace-bearing functions also occur outside ASTTableExpression
    // wrappers (notably INSERT INTO FUNCTION and CREATE ... AS function).
    // Decode the function node itself so every read/write surface shares the
    // same policy. Ordinary scalar functions are ignored by the decoder.
    if (auto ref = decodeNamespaceFunction(*fn, NamespaceRefSource::TableFunction)) {
      StorageIntegrityReadRef event;
      event.kind = StorageIntegrityReadRef::Kind::Namespace;
      event.namespace_ref = std::move(*ref);
      out.push_back(std::move(event));
    }
    if (auto ref = decodeInFunction(*fn)) {
      // A bare `IN <alias>` whose name is an in-scope CTE alias is a CTE
      // reference, not a table (Spec I D7b). Qualified `db.alias` stays a real
      // table, exactly as collectAccessedTablePairsFromAST treats it.
      if (!(ref->database.empty() && cte_scope.count(ref->table) > 0)) {
        StorageIntegrityReadRef event;
        event.kind = StorageIntegrityReadRef::Kind::Namespace;
        event.namespace_ref = std::move(*ref);
        out.push_back(std::move(event));
      }
    }
  }
  visitChildrenWithCTEVisibility(
    *ast, cte_scope,
    [&](const DB::ASTPtr &child, const CTEAliasScope &child_scope) {
      collectReadRefs(child, out, child_scope);
    });
}

void collectReadRefs(const DB::ASTPtr &ast, std::vector<StorageIntegrityReadRef> &out) {
  static const std::unordered_set<std::string> kEmpty;
  collectReadRefs(ast, out, kEmpty);
}

void collectNamespaceRefs(const DB::ASTPtr &ast, std::vector<NamespaceRef> &out) {
  std::vector<StorageIntegrityReadRef> events;
  collectReadRefs(ast, events);
  for (auto &event : events) {
    if (event.kind == StorageIntegrityReadRef::Kind::Namespace)
      out.push_back(std::move(event.namespace_ref));
  }
}

std::string namespaceSurface(const NamespaceRef &ref) {
  switch (ref.source) {
  case NamespaceRefSource::InTable: return ref.name + " table target";
  case NamespaceRefSource::TableEngine: return ref.name + " table engine";
  case NamespaceRefSource::DictionarySource: return ref.name + " dictionary source";
  case NamespaceRefSource::TableFunction:
    return ref.name.empty() ? "table function" : ref.name + " table function";
  }
  return "namespace";
}

std::string logicalRejectMessage(const NamespaceRef &ref, const std::string &key) {
  if (ref.source == NamespaceRefSource::TableEngine) return storageIntegrityWriteRejectMessage(key);
  return "storage-integrity table " + key + " is not directly addressable through " +
         namespaceSurface(ref);
}

std::string logicalDatabaseRejectMessage(const NamespaceRef &ref, const std::string &database) {
  return "storage-integrity logical database " + database +
         " is not directly addressable through " + namespaceSurface(ref);
}

std::string databaseRejectMessage(const NamespaceRef &ref, const std::string &database) {
  if (ref.source == NamespaceRefSource::TableFunction)
    return "storage-integrity table function namespace " + database +
           " is not directly addressable";
  return "storage-integrity " + namespaceSurface(ref) + " namespace " + database +
         " is not directly addressable";
}

std::string unresolvedRejectMessage(const NamespaceRef &ref) {
  if (ref.source == NamespaceRefSource::TableFunction)
    return "storage-integrity table function namespace is not statically resolvable";
  return "storage-integrity " + namespaceSurface(ref) + " namespace is not statically resolvable";
}

void setNamespaceReject(
  rewriter::RewriteSQLResponse *response, rewriter::RewriteCode code, const std::string &message) {
  response->set_code(code);
  response->set_message(message);
}

using StorageIntegrityBindings = std::map<std::string, bool>;

// Collect one select block's own table bindings (alias, else table name) and
// report whether the block reads a storage-integrity table. Nested blocks own
// their own scope and are not descended into.
bool storageIntegrityBlockScope(const DB::ASTSelectQuery &select,
  const rewriter::RewriteTableDynamicArgs &args,
  StorageIntegrityBindings &bindings) {
  bool block_protected = false;
  const auto tables = const_cast<DB::ASTSelectQuery &>(select).tables();
  const auto *list = tables ? tables->as<DB::ASTTablesInSelectQuery>() : nullptr;
  if (!list) return false;
  for (const auto &child : list->children) {
    const auto *element = child->as<DB::ASTTablesInSelectQueryElement>();
    if (!element || !element->table_expression) continue;
    const auto *expr = element->table_expression->as<DB::ASTTableExpression>();
    if (!expr || !expr->database_and_table_name) continue;
    const auto *id = expr->database_and_table_name->as<DB::ASTTableIdentifier>();
    if (!id) continue;
    const auto [database, table] = splitTableIdentifier(id->name());
    const bool si = lookupStorageIntegrity(database, table, args).has_value();
    block_protected = block_protected || si;
    std::string key = expr->database_and_table_name->tryGetAlias();
    if (key.empty()) key = table;
    if (!key.empty()) bindings[key] = si;
  }
  return block_protected;
}

bool referencesIdentifierInScope(const DB::ASTPtr &ast,
  const std::string &name,
  const rewriter::RewriteTableDynamicArgs &args,
  StorageIntegrityBindings bindings,
  bool block_protected) {
  if (!ast) return false;
  if (const auto *select = ast->as<DB::ASTSelectQuery>()) {
    StorageIntegrityBindings next = bindings;
    const bool next_protected = storageIntegrityBlockScope(*select, args, next);
    for (const auto &child : select->children)
      if (referencesIdentifierInScope(child, name, args, next, next_protected)) return true;
    return false;
  }
  if (const auto *identifier = ast->as<DB::ASTIdentifier>();
      identifier && !ast->as<DB::ASTTableIdentifier>() && identifier->shortName() == name) {
    // A compound reference (`alias.col`, `db.table.col`) is decided by its
    // qualifier when that qualifier is bound in scope; otherwise it falls back
    // to the owning block.
    const auto &parts = identifier->name_parts;
    if (parts.size() >= 2) {
      const auto it = bindings.find(parts[parts.size() - 2]);
      if (it != bindings.end()) return it->second;
    }
    return block_protected;
  }
  if (const auto *replacement = ast->as<DB::ASTColumnsReplaceTransformer::Replacement>();
      replacement && replacement->name == name)
    return block_protected;
  if (!ast->tryGetAlias().empty() && ast->tryGetAlias() == name) return block_protected;
  for (const auto &child : ast->children)
    if (referencesIdentifierInScope(child, name, args, bindings, block_protected)) return true;
  return false;
}

} // namespace

bool validateStorageIntegrity(const rewriter::RewriteTableDynamicArgs &args, std::string *error) {
  if (!args.has_storage_integrity()) return true;
  const auto &si = args.storage_integrity();
  const std::string rid = si.reserved_row_id_column();
  if (!rid.empty() && !simpleIdentifier(rid)) {
    if (error) *error = "storage-integrity reserved_row_id_column must be a simple identifier";
    return false;
  }
  switch (si.read_mode()) {
  case rewriter::StorageIntegrityArgs::READ_MODE_UNSPECIFIED:
  case rewriter::StorageIntegrityArgs::READ_MODE_SAFE:
  case rewriter::StorageIntegrityArgs::READ_MODE_UNSAFE_LATEST: break;
  default:
    if (error)
      *error = "storage-integrity read_mode " + std::to_string(static_cast<int>(si.read_mode())) +
               " is not supported";
    return false;
  }
  std::vector<std::string> keys;
  keys.reserve(static_cast<size_t>(si.tables().size()));
  for (const auto &[key, _] : si.tables()) keys.push_back(key);
  std::sort(keys.begin(), keys.end());
  for (const auto &key : keys) {
    std::string ignored_db, ignored_table;
    if (!splitExactQualified(key, &ignored_db, &ignored_table)) {
      if (error)
        *error = "storage-integrity logical table key " + key +
                 " must have exact <database>.<table> shape";
      return false;
    }
    const auto &table = si.tables().at(key);
    if (table.safe_table().empty() || table.unsafe_table().empty()) {
      if (error)
        *error =
          "storage-integrity table " + key + " requires non-empty safe_table and unsafe_table";
      return false;
    }
    if (!splitExactQualified(table.safe_table(), &ignored_db, &ignored_table)) {
      if (error)
        *error = "storage-integrity table " + key + " safe_table " + table.safe_table() +
                 " must have exact <database>.<table> shape";
      return false;
    }
    if (!splitExactQualified(table.unsafe_table(), &ignored_db, &ignored_table)) {
      if (error)
        *error = "storage-integrity table " + key + " unsafe_table " + table.unsafe_table() +
                 " must have exact <database>.<table> shape";
      return false;
    }
  }
  return true;
}

std::optional<StorageIntegrityHit> lookupStorageIntegrity(const std::string &origin_db,
  const std::string &origin_table,
  const rewriter::RewriteTableDynamicArgs &args) {
  if (!args.has_storage_integrity() || args.storage_integrity().tables().empty() ||
      origin_table.empty())
    return std::nullopt;
  const std::string logical =
    origin_db.empty() ? args.upstream_logical_database_in_context() : origin_db;
  if (logical.empty()) return std::nullopt;
  const std::string key = logical + "." + origin_table;
  const auto it = args.storage_integrity().tables().find(key);
  if (it == args.storage_integrity().tables().end()) return std::nullopt;
  return StorageIntegrityHit{&it->second, key, logical};
}

bool isStorageIntegrityPhysicalDatabase(
  const std::string &database, const rewriter::RewriteTableDynamicArgs &args) {
  if (database.empty() || !args.has_storage_integrity()) return false;
  for (const auto &[_, table] : args.storage_integrity().tables()) {
    for (const auto *value : {&table.safe_table(), &table.unsafe_table()}) {
      std::string physical_db, ignored;
      if (splitExactQualified(*value, &physical_db, &ignored) && physical_db == database)
        return true;
    }
  }
  return false;
}

bool isStorageIntegrityLogicalDatabase(
  const std::string &database, const rewriter::RewriteTableDynamicArgs &args) {
  if (database.empty() || !args.has_storage_integrity()) return false;
  for (const auto &[key, _] : args.storage_integrity().tables()) {
    std::string logical, ignored;
    if (splitExactQualified(key, &logical, &ignored) && logical == database) return true;
  }
  return false;
}

std::vector<std::string> storageIntegrityPhysicalDatabases(
  const rewriter::RewriteTableDynamicArgs &args) {
  std::set<std::string> databases;
  if (args.has_storage_integrity()) {
    for (const auto &[_, table] : args.storage_integrity().tables()) {
      for (const auto *value : {&table.safe_table(), &table.unsafe_table()}) {
        std::string database, ignored;
        if (splitExactQualified(*value, &database, &ignored)) databases.insert(database);
      }
    }
  }
  return {databases.begin(), databases.end()};
}

std::string storageIntegrityExecutionDatabase(const rewriter::RewriteTableDynamicArgs &args) {
  if (args.has_upstream_physical_database_in_context() &&
      !args.upstream_physical_database_in_context().empty())
    return args.upstream_physical_database_in_context();
  const std::string logical = args.upstream_logical_database_in_context();
  if (isStorageIntegrityPhysicalDatabase(logical, args)) return logical;
  if (auto physical = resolvePhysicalDatabase(logical, args)) return *physical;
  return "";
}

bool authorizeStorageIntegrityLogical(const std::string &origin_db,
  const rewriter::RewriteTableDynamicArgs &args,
  std::string *logical_database) {
  const std::string logical =
    origin_db.empty() ? args.upstream_logical_database_in_context() : origin_db;
  if (logical_database) *logical_database = logical;
  return !logical.empty() && args.database_map().count(logical) > 0;
}

std::string reservedRowIdColumn(const rewriter::RewriteTableDynamicArgs &args) {
  if (args.has_storage_integrity() && !args.storage_integrity().reserved_row_id_column().empty())
    return args.storage_integrity().reserved_row_id_column();
  return std::string(kDefaultReservedRowIdColumn);
}

std::string storageIntegrityWriteRejectMessage(const std::string &logical_key) {
  return "storage-integrity table " + logical_key +
         " accepts writes only through the signed statement lane";
}

std::string storageIntegrityUnauthorizedMessage(const std::string &logical_database) {
  return "storage-integrity logical database " + logical_database +
         " is not authorized by database_map";
}

std::string storageIntegrityPhysicalRejectMessage(const std::string &physical_name) {
  return "storage-integrity physical table " + physical_name + " is not directly addressable";
}

std::string storageIntegrityPhysicalDatabaseRejectMessage(const std::string &physical_database) {
  return "storage-integrity physical database " + physical_database +
         " is not directly addressable";
}

std::string storageIntegritySurfaceSQL(
  const rewriter::StorageIntegrityArgs::Table &table, const rewriter::StorageIntegrityArgs &args) {
  const std::string rid = args.reserved_row_id_column().empty()
                            ? std::string(kDefaultReservedRowIdColumn)
                            : args.reserved_row_id_column();
  // Quote the reserved identifier unconditionally; ClickHouse's formatter
  // drops unnecessary quotes but retains them for keywords such as `from`.
  const std::string project = "SELECT * EXCEPT (`" + rid + "`) FROM ";
  std::string out = project + quotePhysical(table.safe_table());
  if (args.read_mode() != rewriter::StorageIntegrityArgs::READ_MODE_UNSAFE_LATEST) return out;
  out += " UNION ALL " + project + quotePhysical(table.unsafe_table());
  if (table.excluded_unsafe_parts_size() > 0) {
    out += " WHERE _part NOT IN (";
    for (int i = 0; i < table.excluded_unsafe_parts_size(); ++i) {
      if (i) out += ", ";
      out += "'" + escapeSqlLiteral(table.excluded_unsafe_parts(i)) + "'";
    }
    out += ")";
  }
  return out;
}

DB::ASTPtr buildStorageIntegrityDerivedTable(
  const rewriter::StorageIntegrityArgs::Table &table, const rewriter::StorageIntegrityArgs &args) {
  const std::string sql = storageIntegritySurfaceSQL(table, args);
  DB::ParserSelectWithUnionQuery parser;
  DB::ASTPtr select =
    DB::parseQuery(parser, sql.data(), sql.data() + sql.size(), /*description=*/"", 0, 0, 0);
  return DB::makeRewriterAST<DB::ASTSubquery>(select);
}

std::string restoreStorageIntegrityProjectionSyntax(
  std::string sql, const std::string &reserved_column) {
  // The pinned ClickHouse formatter erases the parentheses around a single
  // star-EXCEPT identifier and also forgets quoting for keyword-shaped names
  // such as `from`. The generated SI surface is trusted, so restore the
  // canonical cross-engine spelling after formatting.
  static const std::set<std::string> keywords = {"as",
    "by",
    "except",
    "final",
    "from",
    "global",
    "group",
    "having",
    "in",
    "intersect",
    "join",
    "limit",
    "offset",
    "on",
    "order",
    "sample",
    "select",
    "union",
    "using",
    "where",
    "with"};
  const std::string rendered =
    keywords.count(lower(reserved_column)) ? "`" + reserved_column + "`" : reserved_column;
  const std::string needle = "EXCEPT " + reserved_column;
  const std::string replacement = "EXCEPT (" + rendered + ")";
  std::string out;
  out.reserve(sql.size());
  size_t i = 0;
  while (i < sql.size()) {
    const char c = sql[i];
    // Copy a quoted run (string literal or quoted identifier) verbatim: the
    // textual fixup must never reach inside caller data (Spec I D7d).
    if (c == '\'' || c == '`' || c == '"') {
      const char quote = c;
      out += sql[i++];
      while (i < sql.size()) {
        if (sql[i] == '\\' && i + 1 < sql.size()) {
          out += sql[i];
          out += sql[i + 1];
          i += 2;
          continue;
        }
        if (sql[i] == quote) {
          out += sql[i++];
          if (i < sql.size() && sql[i] == quote) {  // '' / `` / "" escape
            out += sql[i++];
            continue;
          }
          break;
        }
        out += sql[i++];
      }
      continue;
    }
    if (sql.compare(i, needle.size(), needle) == 0) {
      const size_t after = i + needle.size();
      if (after == sql.size()
          || !(std::isalnum(static_cast<unsigned char>(sql[after])) || sql[after] == '_')) {
        out += replacement;
        i = after;
        continue;
      }
    }
    out += sql[i++];
  }
  return out;
}

bool prepareStorageIntegritySQL(const std::string &sql,
  const rewriter::RewriteTableDynamicArgs &args,
  rewriter::RewriteSQLResponse *response,
  StorageIntegritySQLCompatibility *compatibility) {
  if (!compatibility) return false;
  compatibility->original_sql = sql;
  compatibility->parse_sql = sql;
  compatibility->restores.clear();

  const auto tokens = tokenizeSQL(sql);
  if (tokens.empty()) return false;

  const auto select_targets = collectLexicalSelectTargets(tokens);
  auto reject_select_surface = [&](const LexicalTarget &bound, const std::string &message) {
    recordLexicalTargets(response, select_targets, args);
    if (auto hit = lookupStorageIntegrity(bound.database, bound.table, args)) {
      std::string logical;
      if (!authorizeStorageIntegrityLogical(bound.database, args, &logical)) {
        response->set_code(rewriter::RewriteCode::InvalidRewriteRequest);
        response->set_message(storageIntegrityUnauthorizedMessage(logical));
      } else {
        response->set_code(rewriter::RewriteCode::RewriteError);
        response->set_message(message);
      }
      return true;
    }
    const std::string physical =
      bound.database.empty() ? storageIntegrityExecutionDatabase(args) : bound.database;
    if (!isStorageIntegrityPhysicalDatabase(physical, args)) return false;
    response->set_code(rewriter::RewriteCode::RewriteError);
    response->set_message(storageIntegrityPhysicalRejectMessage(physical + "." + bound.table));
    return true;
  };

  // The pinned ClickHouse parser does not understand ATTACH GRANT nor an
  // ATTACH DATABASE ... FROM form. Classify their security target before
  // parsing so an unsupported grammar cannot erase the SI marker.
  if (tokens.size() >= 2 && tokens[0].upper == "ATTACH") {
    if (tokens[1].upper == "GRANT" || tokens[1].upper == "REVOKE") {
      for (size_t i = 2; i < tokens.size(); ++i) {
        if (tokens[i].upper != "ON" || tokens[i].depth != 0) continue;
        if (i + 1 < tokens.size() && tokens[i + 1].text == "*") {
          LexicalTarget target;
          target.database = args.upstream_logical_database_in_context();
          target.table = "*";
          if (rejectLexicalStorageIntegrityTarget(
                response, target, args, rewriter::RewriteCode::UnsupportedStatement))
            return true;
        } else if (auto target = targetAt(tokens, i + 1)) {
          if (rejectLexicalStorageIntegrityTarget(
                response, *target, args, rewriter::RewriteCode::UnsupportedStatement))
            return true;
        }
        break;
      }
    } else if (tokens[1].upper == "DATABASE" && tokens.size() >= 3 && identifierToken(tokens[2])) {
      const std::string database = tokens[2].text;
      if (isStorageIntegrityPhysicalDatabase(database, args)) {
        recordPhysicalStorageIntegrityAccess(response, database, "", database);
        response->set_code(rewriter::RewriteCode::UnsupportedStatement);
        response->set_message(storageIntegrityPhysicalDatabaseRejectMessage(database));
        return true;
      }
      if (isStorageIntegrityLogicalDatabase(database, args)) {
        std::string physical;
        if (auto mapped = resolvePhysicalDatabase(database, args)) physical = *mapped;
        recordStorageIntegrityAccess(response, database, "", database, physical);
        std::string logical;
        if (!authorizeStorageIntegrityLogical(database, args, &logical)) {
          response->set_code(rewriter::RewriteCode::InvalidRewriteRequest);
          response->set_message(storageIntegrityUnauthorizedMessage(logical));
        } else {
          response->set_code(rewriter::RewriteCode::UnsupportedStatement);
          response->set_message(
            "storage-integrity logical database " + database + " is not directly addressable");
        }
        return true;
      }
    }
  }

  // Star RENAME is newer than the pinned parser. Detect both source and
  // destination identifiers from real tokens (never strings/comments) and
  // reject only when the SELECT actually touches an SI target.
  const std::string rid = reservedRowIdColumn(args);
  for (size_t i = 0; i + 1 < tokens.size(); ++i) {
    if (tokens[i].upper != "RENAME" || tokens[i + 1].text != "(") continue;
    bool star_transformer = false;
    for (size_t j = i; j > 0; --j) {
      const auto &candidate = tokens[j - 1];
      if (candidate.depth != tokens[i].depth) continue;
      if (candidate.text == "*") star_transformer = true;
      if (candidate.upper == "SELECT" || candidate.upper == "FROM") break;
    }
    if (!star_transformer) continue;
    const int depth = tokens[i + 1].depth;
    bool reserved = false;
    for (size_t j = i + 2; j < tokens.size(); ++j) {
      if (tokens[j].text == ")" && tokens[j].depth == depth) break;
      if (identifierToken(tokens[j]) && tokens[j].text == rid) {
        reserved = true;
        break;
      }
    }
    if (!reserved) continue;
    for (const auto &target : select_targets) {
      if (!targetTouchesStorageIntegrity(target, args)) continue;
      return reject_select_surface(target, "reserved column " + rid + " is not addressable");
    }
  }

  struct Edit {
    size_t begin;
    size_t end;
  };
  std::vector<Edit> edits;

  // Bind WITH OFFSET to the immediately preceding table expression. This is
  // deliberately token-based: whitespace variants are equivalent and a
  // string literal containing the words is not a modifier.
  const std::set<std::string> wrapper_boundaries = {"WHERE",
    "PREWHERE",
    "GROUP",
    "HAVING",
    "ORDER",
    "LIMIT",
    "SETTINGS",
    "UNION",
    "INTERSECT",
    "EXCEPT",
    "ON",
    "USING"};
  for (size_t i = 0; i + 1 < tokens.size(); ++i) {
    if (tokens[i].upper != "WITH" || tokens[i + 1].upper != "OFFSET" ||
        tokens[i].depth != tokens[i + 1].depth)
      continue;
    std::optional<LexicalTarget> bound;
    for (size_t j = i; j > 0; --j) {
      const auto &candidate = tokens[j - 1];
      if (candidate.depth != tokens[i].depth) continue;
      if (wrapper_boundaries.count(candidate.upper)) break;
      if (candidate.upper == "FROM" || candidate.upper == "JOIN" || candidate.text == ",") {
        bound = targetAt(tokens, j);
        break;
      }
    }
    if (!bound) continue;
    if (targetTouchesStorageIntegrity(*bound, args)) {
      return reject_select_surface(*bound, std::string(kStorageIntegrityWrapperMessage));
    }

    size_t end_index = i + 1;
    if (end_index + 2 < tokens.size() && tokens[end_index + 1].upper == "AS" &&
        identifierToken(tokens[end_index + 2])) {
      end_index += 2;
    } else if (end_index + 1 < tokens.size() && identifierToken(tokens[end_index + 1])) {
      ++end_index;
    }
    StorageIntegritySQLCompatibility::Restore restore;
    restore.kind = StorageIntegritySQLCompatibility::Restore::Kind::WithOffset;
    restore.database = bound->database;
    restore.table = bound->table;
    restore.suffix =
      " " + trimCopy(sql.substr(tokens[i].begin, tokens[end_index].end - tokens[i].begin));
    compatibility->restores.push_back(std::move(restore));
    edits.push_back({tokens[i].begin, tokens[end_index].end});
  }

  // PREWHERE is a query-level clause evaluated against the FROM clause's main
  // table, so bind it to the first target after the nearest preceding FROM at
  // the same depth (Spec I D7c). Rejecting here keeps the physical names and
  // the reserved column out of the raw ClickHouse error the client would
  // otherwise receive. Token-based, so a string literal cannot trigger it.
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].kind != SQLToken::Kind::Word || tokens[i].upper != "PREWHERE") continue;
    std::optional<LexicalTarget> bound;
    for (size_t j = i; j > 0; --j) {
      const auto &candidate = tokens[j - 1];
      if (candidate.depth != tokens[i].depth) continue;
      if (candidate.kind == SQLToken::Kind::Word && candidate.upper == "FROM") {
        bound = targetAt(tokens, j);
        break;
      }
    }
    if (!bound) continue;
    if (targetTouchesStorageIntegrity(*bound, args)) {
      return reject_select_surface(*bound, std::string(kStorageIntegrityWrapperMessage));
    }
  }

  // Likewise, strip an ordinary table's alias column list long enough for
  // the pinned parser to build the AST. SI-bound lists are rejected because
  // substituting the table with a derived query would change their meaning.
  std::map<int, bool> in_from;
  const std::set<std::string> from_ends = {"WHERE",
    "PREWHERE",
    "GROUP",
    "HAVING",
    "ORDER",
    "LIMIT",
    "SETTINGS",
    "UNION",
    "INTERSECT",
    "EXCEPT"};
  for (size_t i = 0; i < tokens.size(); ++i) {
    const auto &token = tokens[i];
    if (from_ends.count(token.upper)) in_from[token.depth] = false;
    const bool boundary =
      token.upper == "FROM" || token.upper == "JOIN" || (token.text == "," && in_from[token.depth]);
    if (token.upper == "FROM" || token.upper == "JOIN") in_from[token.depth] = true;
    if (!boundary) continue;
    auto target = targetAt(tokens, i + 1);
    if (!target) continue;
    size_t cursor = target->next;
    std::string alias;
    if (cursor + 1 < tokens.size() && tokens[cursor].upper == "AS" &&
        identifierToken(tokens[cursor + 1])) {
      alias = tokens[cursor + 1].text;
      cursor += 2;
    } else if (cursor < tokens.size() && identifierToken(tokens[cursor]) &&
               cursor + 1 < tokens.size() && tokens[cursor + 1].text == "(") {
      alias = tokens[cursor].text;
      ++cursor;
    }
    if (alias.empty() || cursor >= tokens.size() || tokens[cursor].text != "(") continue;
    size_t close = cursor + 1;
    for (; close < tokens.size(); ++close)
      if (tokens[close].text == ")" && tokens[close].depth == tokens[cursor].depth) break;
    if (close >= tokens.size()) continue;
    if (targetTouchesStorageIntegrity(*target, args)) {
      return reject_select_surface(*target, std::string(kStorageIntegrityWrapperMessage));
    }
    StorageIntegritySQLCompatibility::Restore restore;
    restore.kind = StorageIntegritySQLCompatibility::Restore::Kind::AliasColumns;
    restore.database = target->database;
    restore.table = target->table;
    restore.alias = alias;
    restore.suffix = sql.substr(tokens[cursor].begin, tokens[close].end - tokens[cursor].begin);
    compatibility->restores.push_back(std::move(restore));
    edits.push_back({tokens[cursor].begin, tokens[close].end});
  }

  std::sort(edits.begin(), edits.end(), [](const Edit &left, const Edit &right) {
    return left.begin > right.begin;
  });
  for (const auto &edit : edits) {
    if (edit.begin <= edit.end && edit.end <= compatibility->parse_sql.size())
      compatibility->parse_sql.erase(edit.begin, edit.end - edit.begin);
  }
  return false;
}

void restoreStorageIntegritySQL(
  rewriter::RewriteSQLResponse *response, const StorageIntegritySQLCompatibility &compatibility) {
  if (!response || response->code() != rewriter::RewriteCode::Success ||
      compatibility.restores.empty())
    return;
  std::string sql = response->sql_after_rewrite();
  bool failed = false;
  for (auto it = compatibility.restores.rbegin(); it != compatibility.restores.rend(); ++it) {
    const auto &restore = *it;
    if (restore.kind == StorageIntegritySQLCompatibility::Restore::Kind::AliasColumns) {
      const std::string plain = " AS " + restore.alias;
      const std::string quoted = " AS " + quoteIdent(restore.alias);
      size_t pos = sql.rfind(plain);
      size_t length = plain.size();
      if (pos == std::string::npos) {
        pos = sql.rfind(quoted);
        length = quoted.size();
      }
      if (pos == std::string::npos) {
        failed = true;
        break;
      }
      sql.insert(pos + length, restore.suffix);
      continue;
    }

    const std::string key =
      restore.database.empty() ? restore.table : restore.database + "." + restore.table;
    std::vector<std::string> candidates;
    if (const auto mapped = response->table_rewrites().find(key);
      mapped != response->table_rewrites().end()) {
      candidates.push_back(formatMappedTarget(mapped->second));
      candidates.push_back(mapped->second);
      candidates.push_back(quotePhysical(mapped->second));
    }
    candidates.push_back(formatMappedTarget(key));
    candidates.push_back(key);
    candidates.push_back(quotePhysical(key));
    size_t anchor = std::string::npos;
    size_t anchor_length = 0;
    for (const auto &candidate : candidates) {
      const size_t pos = sql.rfind(candidate);
      if (pos != std::string::npos && (anchor == std::string::npos || pos > anchor)) {
        anchor = pos;
        anchor_length = candidate.size();
      }
    }
    if (anchor == std::string::npos) {
      failed = true;
      break;
    }
    size_t insert = sql.size();
    const std::vector<std::string> boundaries = {" ON ",
      " JOIN ",
      ", ",
      " WHERE ",
      " PREWHERE ",
      " GROUP ",
      " ORDER ",
      " HAVING ",
      " LIMIT ",
      " SETTINGS "};
    for (const auto &boundary : boundaries) {
      const size_t pos = sql.find(boundary, anchor + anchor_length);
      if (pos != std::string::npos) insert = std::min(insert, pos);
    }
    sql.insert(insert, restore.suffix);
  }
  if (failed) {
    response->set_sql_after_rewrite(compatibility.original_sql);
    response->set_statement_type(rewriter::STATEMENT_TYPE_UNSPECIFIED);
    response->set_code(rewriter::RewriteCode::RewriteError);
    response->set_message(std::string(kStorageIntegrityWrapperMessage));
    return;
  }
  response->set_sql_after_rewrite(std::move(sql));
}

bool astReferencesIdentifier(const DB::ASTPtr &ast, const std::string &name) {
  if (!ast) return false;
  if (const auto *replacement = ast->as<DB::ASTColumnsReplaceTransformer::Replacement>();
    replacement && replacement->name == name)
    return true;
  if (const auto *identifier = ast->as<DB::ASTIdentifier>();
    identifier && !ast->as<DB::ASTTableIdentifier>() && identifier->shortName() == name)
    return true;
  if (!ast->tryGetAlias().empty() && ast->tryGetAlias() == name) return true;
  for (const auto &child : ast->children)
    if (astReferencesIdentifier(child, name)) return true;
  return false;
}

bool astReferencesIdentifierInScope(const DB::ASTPtr &ast,
  const std::string &name,
  const rewriter::RewriteTableDynamicArgs &args) {
  return referencesIdentifierInScope(ast, name, args, StorageIntegrityBindings{}, false);
}

std::string describeMetadataSQL(const std::string &safe_table, const std::string &reserved_column) {
  const auto [database, table] = splitPhysical(safe_table);
  // system.columns exposes default_kind; alias it to native DESCRIBE's
  // default_type field. Protocol-owned SI DDL has no column CODEC or TTL, so
  // those two fields are empty (and ttl_expression is absent from
  // system.columns on ClickHouse 25.8), preserving an executable 7-field shape.
  return "SELECT name, type, default_kind AS default_type, default_expression, comment, "
         "'' AS codec_expression, '' AS ttl_expression FROM system.columns WHERE database = '" +
         escapeSqlLiteral(database) + "' AND table = '" + escapeSqlLiteral(table) +
         "' AND name != '" + escapeSqlLiteral(reserved_column) + "' ORDER BY position";
}

void recordStorageIntegrityAccess(rewriter::RewriteSQLResponse *response,
  const std::string &original_database,
  const std::string &original_table,
  const std::string &logical_database,
  const std::string &physical_database) {
  for (const auto &entry : response->original_accessed_tables()) {
    if (entry.original_database() == original_database && entry.original_table() == original_table)
      return;
  }
  auto *entry = response->add_original_accessed_tables();
  entry->set_original_database(original_database);
  entry->set_original_table(original_table);
  entry->set_logical_database(logical_database);
  entry->set_physical_database(physical_database);
  entry->set_is_storage_integrity(true);
  entry->set_is_remote(false);
}

void recordLogicalStorageIntegrityAccess(rewriter::RewriteSQLResponse *response,
  const std::string &original_database,
  const std::string &original_table,
  const rewriter::RewriteTableDynamicArgs &args) {
  const std::string logical =
    original_database.empty() ? args.upstream_logical_database_in_context() : original_database;
  std::string physical;
  if (auto resolved = resolvePhysicalDatabase(logical, args)) physical = *resolved;
  recordStorageIntegrityAccess(response, original_database, original_table, logical, physical);
}

void recordPhysicalStorageIntegrityAccess(rewriter::RewriteSQLResponse *response,
  const std::string &original_database,
  const std::string &original_table,
  const std::string &physical_database) {
  recordStorageIntegrityAccess(response, original_database, original_table, "", physical_database);
}

std::vector<NamespaceRef> collectStorageIntegrityNamespaceRefs(const DB::ASTPtr &ast) {
  std::vector<NamespaceRef> refs;
  collectNamespaceRefs(ast, refs);
  return refs;
}

std::vector<StorageIntegrityReadRef> collectStorageIntegrityReadRefs(const DB::ASTPtr &ast) {
  std::vector<StorageIntegrityReadRef> refs;
  collectReadRefs(ast, refs);
  return refs;
}

bool rejectStorageIntegrityNamespaces(rewriter::RewriteSQLResponse *response,
  const std::vector<NamespaceRef> &refs,
  const rewriter::RewriteTableDynamicArgs &args,
  rewriter::RewriteCode code) {
  if (!args.has_storage_integrity() || args.storage_integrity().tables().empty()) return false;
  for (auto ref : refs) {
    if (ref.uses_current_database) {
      if (!ref.table.empty()) {
        if (auto hit = lookupStorageIntegrity("", ref.table, args)) {
          recordLogicalStorageIntegrityAccess(response, "", ref.table, args);
          std::string logical;
          if (!authorizeStorageIntegrityLogical("", args, &logical)) {
            setNamespaceReject(response,
              rewriter::RewriteCode::InvalidRewriteRequest,
              storageIntegrityUnauthorizedMessage(logical));
          } else {
            setNamespaceReject(response, code, logicalRejectMessage(ref, hit->logical_key));
          }
          return true;
        }
      }
      ref.database = storageIntegrityExecutionDatabase(args);
      ref.resolved = !ref.database.empty() && !ref.table.empty();
    }
    if (ref.resolved) {
      if (isStorageIntegrityPhysicalDatabase(ref.database, args)) {
        recordPhysicalStorageIntegrityAccess(response, ref.database, ref.table, ref.database);
        setNamespaceReject(
          response, code, storageIntegrityPhysicalRejectMessage(ref.database + "." + ref.table));
        return true;
      }
      if (auto hit = lookupStorageIntegrity(ref.database, ref.table, args)) {
        recordLogicalStorageIntegrityAccess(response, ref.database, ref.table, args);
        std::string logical;
        if (!authorizeStorageIntegrityLogical(ref.database, args, &logical)) {
          setNamespaceReject(response,
            rewriter::RewriteCode::InvalidRewriteRequest,
            storageIntegrityUnauthorizedMessage(logical));
        } else {
          setNamespaceReject(response, code, logicalRejectMessage(ref, hit->logical_key));
        }
        return true;
      }
      if (isStorageIntegrityLogicalDatabase(ref.database, args)) {
        const auto logical = ref.database;
        std::string physical;
        if (auto resolved = resolvePhysicalDatabase(logical, args)) physical = *resolved;
        recordStorageIntegrityAccess(response, ref.database, "", logical, physical);
        std::string authorized_logical;
        if (!authorizeStorageIntegrityLogical(ref.database, args, &authorized_logical)) {
          setNamespaceReject(response,
            rewriter::RewriteCode::InvalidRewriteRequest,
            storageIntegrityUnauthorizedMessage(authorized_logical));
        } else {
          setNamespaceReject(response, code, logicalDatabaseRejectMessage(ref, ref.database));
        }
        return true;
      }
      continue;
    }
    if (!ref.database.empty()) {
      if (isStorageIntegrityPhysicalDatabase(ref.database, args)) {
        recordPhysicalStorageIntegrityAccess(response, ref.database, "", ref.database);
        setNamespaceReject(response, code, databaseRejectMessage(ref, ref.database));
        return true;
      }
      if (isStorageIntegrityLogicalDatabase(ref.database, args)) {
        std::string physical;
        if (auto resolved = resolvePhysicalDatabase(ref.database, args)) physical = *resolved;
        recordStorageIntegrityAccess(response, ref.database, "", ref.database, physical);
        std::string logical;
        if (!authorizeStorageIntegrityLogical(ref.database, args, &logical)) {
          setNamespaceReject(response,
            rewriter::RewriteCode::InvalidRewriteRequest,
            storageIntegrityUnauthorizedMessage(logical));
        } else {
          setNamespaceReject(response, code, logicalDatabaseRejectMessage(ref, ref.database));
        }
        return true;
      }
    }
    for (const auto &physical : storageIntegrityPhysicalDatabases(args))
      recordPhysicalStorageIntegrityAccess(response, physical, "", physical);
    setNamespaceReject(response, code, unresolvedRejectMessage(ref));
    return true;
  }
  return false;
}

bool rejectUnmodelledStorageIntegrityStatement(const DB::ASTPtr &ast,
  const rewriter::RewriteTableDynamicArgs &args,
  rewriter::RewriteSQLResponse *response) {
  if (!ast || !response) return false;
  if (!args.has_storage_integrity() || args.storage_integrity().tables().empty()) return false;
  if (ast->as<DB::ASTSelectWithUnionQuery>() || ast->as<DB::ASTSelectQuery>()
      || ast->as<DB::ASTSelectIntersectExceptQuery>())
    return false;
  response->set_code(rewriter::RewriteCode::UnsupportedStatement);
  response->set_message(std::string(kStorageIntegrityUnmodelledMessage));
  return true;
}

namespace {

bool annotateStructuredStorageIntegrityTarget(
  rewriter::RewriteSQLResponse *response,
  const std::string &database,
  const std::string &table,
  const rewriter::RewriteTableDynamicArgs &args) {
  if (table.empty()) {
    if (isStorageIntegrityPhysicalDatabase(database, args)) {
      response->set_message(storageIntegrityPhysicalDatabaseRejectMessage(database));
      return true;
    }
    if (isStorageIntegrityLogicalDatabase(database, args)) {
      response->set_message(
        "storage-integrity logical database " + database + " is not directly addressable");
      return true;
    }
    return false;
  }

  if (!database.empty()) {
    if (isStorageIntegrityPhysicalDatabase(database, args)) {
      response->set_message(
        storageIntegrityPhysicalRejectMessage(database + "." + table));
      return true;
    }
    if (auto hit = lookupStorageIntegrity(database, table, args)) {
      response->set_message(storageIntegrityWriteRejectMessage(hit->logical_key));
      return true;
    }
    return false;
  }

  if (auto hit = lookupStorageIntegrity("", table, args)) {
    response->set_message(storageIntegrityWriteRejectMessage(hit->logical_key));
    return true;
  }
  const std::string physical = storageIntegrityExecutionDatabase(args);
  if (isStorageIntegrityPhysicalDatabase(physical, args)) {
    response->set_message(storageIntegrityPhysicalRejectMessage(physical + "." + table));
    return true;
  }
  return false;
}

} // namespace

void annotateStorageIntegrityReject(rewriter::RewriteSQLResponse *response,
  const DB::ASTPtr &ast,
  const std::string &sql,
  const rewriter::RewriteTableDynamicArgs &args) {
  if (!response || response->code() == rewriter::RewriteCode::Success) return;
  if (!args.has_storage_integrity() || args.storage_integrity().tables().empty()) return;
  for (const auto &entry : response->original_accessed_tables())
    if (entry.is_storage_integrity()) return;

  // A failed parse has no proven target. Preserve ParserQuery's diagnostic;
  // lexical lookalikes in malformed SQL must never replace SyntaxError.
  if (!ast) return;

  // SYSTEM statements carry their object separately from ON CLUSTER and
  // other operands. Only those structured target fields participate in D2
  // attribution; cluster names and arbitrary words are not database/table
  // events.
  if (const auto *system = ast->as<DB::ASTSystemQuery>()) {
    if (annotateStructuredStorageIntegrityTarget(
          response, system->getDatabase(), system->getTable(), args)) return;
    for (const auto &[database, table] : system->tables) {
      if (annotateStructuredStorageIntegrityTarget(
            response, database, table, args)) return;
    }
    return;
  }

  const auto tokens = tokenizeSQL(sql);
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (!identifierToken(tokens[i])) continue;
    const auto target = targetAt(tokens, i);
    if (!target) continue;
    if (!target->database.empty()) {
      i = target->next - 1;  // consume `db . table`
      if (isStorageIntegrityPhysicalDatabase(target->database, args)) {
        response->set_message(
          storageIntegrityPhysicalRejectMessage(target->database + "." + target->table));
        return;
      }
      if (auto hit = lookupStorageIntegrity(target->database, target->table, args)) {
        response->set_message(storageIntegrityWriteRejectMessage(hit->logical_key));
        return;
      }
      continue;
    }
    if (isStorageIntegrityPhysicalDatabase(target->table, args)) {
      // A bare name in a database position: TRUNCATE DATABASE hg_safe,
      // TRUNCATE ALL TABLES FROM hg_unsafe, ALTER DATABASE hg_safe ...
      response->set_message(storageIntegrityPhysicalDatabaseRejectMessage(target->table));
      return;
    }
    if (auto hit = lookupStorageIntegrity("", target->table, args)) {
      response->set_message(storageIntegrityWriteRejectMessage(hit->logical_key));
      return;
    }
  }
}

} // namespace rewriter_handlers
