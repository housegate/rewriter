// src/handlers/metadata_access.cc — see metadata_access.h.
#include "handlers/metadata_access.h"

#include <Parsers/ASTAlterQuery.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTQueryWithTableAndOutput.h>
#include <Parsers/ASTRenameQuery.h>
#include <Parsers/ASTShowColumnsQuery.h>
#include <Parsers/ASTShowIndexesQuery.h>
#include <Parsers/ASTShowTablesQuery.h>
#include <Parsers/ASTUseQuery.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <unordered_set>
#include <utility>

#include "handlers/table_reference.h"

namespace rewriter_handlers {

namespace {

std::string lowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

// Case-insensitive on purpose: ClickHouse database names are case-sensitive,
// so `SYSTEM` is not the metadata database, but refusing it errs toward
// closed and matches what upstream sql-rewriter answered this caller.
bool isProtectedDatabase(std::string database) {
  database.erase(std::remove(database.begin(), database.end(), '`'), database.end());
  const std::string lower = lowerAscii(database);
  return lower == "system" || lower == "information_schema";
}

// "db.table" with exactly one dot and both parts non-empty.
std::optional<std::pair<std::string, std::string>> splitQualified(std::string name) {
  name.erase(std::remove(name.begin(), name.end(), '`'), name.end());
  const auto dot = name.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()
      || name.find('.', dot + 1) != std::string::npos)
    return std::nullopt;
  return std::make_pair(name.substr(0, dot), name.substr(dot + 1));
}

size_t argumentCount(const DB::ASTFunction &fn) {
  return fn.arguments ? fn.arguments->children.size() : 0;
}

// A string literal or a plain identifier argument; nullopt for anything the
// rewriter cannot read statically (an expression, a parameter, a collection
// override).
std::optional<std::string> staticName(const DB::ASTFunction &fn, size_t index) {
  if (argumentCount(fn) <= index) return std::nullopt;
  const auto &argument = fn.arguments->children[index];
  if (const auto *literal = argument->as<DB::ASTLiteral>()) {
    if (literal->value.getType() == DB::Field::Types::String) return literal->value.safeGet<std::string>();
    return std::nullopt;
  }
  if (const auto *identifier = argument->as<DB::ASTIdentifier>()) {
    if (identifier->isParam()) return std::nullopt;
    return identifier->name();
  }
  return std::nullopt;
}

// Whether a database argument may name a protected database: unreadable, or
// protected.
bool databaseArgumentMayBeProtected(const DB::ASTFunction &fn, size_t index) {
  const auto database = staticName(fn, index);
  return !database || isProtectedDatabase(*database);
}

// Whether a "db.table" (or bare table) argument may name a protected table:
// unreadable, or qualified with a protected database. A bare name resolves in
// the session database.
bool qualifiedArgumentMayBeProtected(const DB::ASTFunction &fn, size_t index) {
  const auto name = staticName(fn, index);
  if (!name) return true;
  const auto qualified = splitQualified(*name);
  return qualified && isProtectedDatabase(qualified->first);
}

// Table functions, and the table engines of the same name (an ENGINE clause
// parses into an ASTFunction too), that read another table by name.
bool tableReaderMayReachMetadata(const DB::ASTFunction &fn, const std::string &name) {
  const size_t count = argumentCount(fn);
  if (name == "remote" || name == "remotesecure" || name == "cluster" || name == "clusterallreplicas") {
    // One argument resolves to system.one; a named-collection override or an
    // expression cannot be proven to stay outside the protected databases.
    if (count <= 1) return true;
    const auto second = staticName(fn, 1);
    if (!second) return true;
    if (const auto qualified = splitQualified(*second)) return isProtectedDatabase(qualified->first);
    // A two-argument call needs a qualified db.table; a bare name cannot show
    // which database it addresses.
    if (count == 2) return true;
    return isProtectedDatabase(*second);
  }
  if (name == "merge") {
    // merge(regexp) reads the session database; a database regex or
    // expression can match a protected one.
    if (count <= 1) return true;
    return databaseArgumentMayBeProtected(fn, 0);
  }
  if (name == "mergetreeindex" || name == "mergetreeprojection" || name == "mergetreetextindex"
      || name == "mergetreeanalyzeindexes") {
    if (count < 2) return true;
    return databaseArgumentMayBeProtected(fn, 0);
  }
  if (name == "loop") {
    if (count == 1) {
      // An unqualified table inherits the session database: not provably safe.
      const auto table = staticName(fn, 0);
      if (!table) return true;
      const auto qualified = splitQualified(*table);
      return !qualified || isProtectedDatabase(qualified->first);
    }
    if (count == 2) return databaseArgumentMayBeProtected(fn, 0);
    return true;
  }
  // ENGINE = Distributed(cluster, db, table, …) / Buffer(db, table, …).
  if (name == "distributed") return count < 3 || databaseArgumentMayBeProtected(fn, 1);
  if (name == "buffer") return count < 2 || databaseArgumentMayBeProtected(fn, 0);
  return false;
}

// String lookups that read a table or dictionary by name: the dict* family
// (dictGet*, dictHas, dictIsIn, …, and the dictionary() table function) and
// joinGet*, whose first argument is "[db.]name".
bool stringLookupMayReachMetadata(const DB::ASTFunction &fn, const std::string &name) {
  if (name.starts_with("dict") || name == "joinget" || name == "joingetornull")
    return argumentCount(fn) < 1 || qualifiedArgumentMayBeProtected(fn, 0);
  if (name == "hascolumnintable") {
    // hasColumnInTable([hostname[, user[, password]],] database, table, column):
    // the remote-host forms address another server and are refused outright.
    if (argumentCount(fn) != 3) return true;
    return databaseArgumentMayBeProtected(fn, 0);
  }
  return false;
}

bool isMetadataFunction(const DB::ASTFunction &fn) {
  const std::string name = lowerAscii(fn.name);
  // version() reveals the deployment to a static-mode caller, which (unlike a
  // native-protocol HouseGate client) is handed no server hello.
  if (name == "version") return true;
  if (tableReaderMayReachMetadata(fn, name) || stringLookupMayReachMetadata(fn, name)) return true;
  // An IN operand naming a table: `x IN system.users`.
  if (isInFamilyFunction(fn.name) && argumentCount(fn) == 2) {
    if (const auto *operand = fn.arguments->children[1]->as<DB::ASTIdentifier>()) {
      if (operand->compound() && operand->name_parts.size() == 2 && isProtectedDatabase(operand->name_parts[0]))
        return true;
    }
  }
  return false;
}

bool nodeReadsMetadata(const DB::IAST &node) {
  if (const auto *table = node.as<DB::ASTTableIdentifier>()) {
    if (isProtectedDatabase(table->getTableId().database_name)) return true;
  }
  if (const auto *fn = node.as<DB::ASTFunction>()) {
    if (isMetadataFunction(*fn)) return true;
  }
  // SHOW TABLES / DATABASES / DICTIONARIES / CLUSTERS / SETTINGS / MERGES /
  // CACHES all read system tables.
  if (node.as<DB::ASTShowTablesQuery>()) return true;
  // DESCRIBE / EXISTS / SHOW CREATE / DROP / TRUNCATE / ALTER / … targets,
  // database-level forms (SHOW CREATE DATABASE system) included.
  if (const auto *query = dynamic_cast<const DB::ASTQueryWithTableAndOutput *>(&node)) {
    if (isProtectedDatabase(query->getDatabase())) return true;
  }
  if (const auto *insert = node.as<DB::ASTInsertQuery>()) {
    if (isProtectedDatabase(insert->getDatabase())) return true;
  }
  if (const auto *show = node.as<DB::ASTShowColumnsQuery>()) {
    if (isProtectedDatabase(show->database)) return true;
  }
  if (const auto *show = node.as<DB::ASTShowIndexesQuery>()) {
    if (isProtectedDatabase(show->database)) return true;
  }
  if (const auto *create = node.as<DB::ASTCreateQuery>()) {
    if (isProtectedDatabase(create->as_database)) return true;
  }
  if (const auto *command = node.as<DB::ASTAlterCommand>()) {
    if (isProtectedDatabase(command->from_database) || isProtectedDatabase(command->to_database)) return true;
  }
  if (const auto *rename = node.as<DB::ASTRenameQuery>()) {
    for (const auto &element : rename->getElements()) {
      if (isProtectedDatabase(element.from.getDatabase()) || isProtectedDatabase(element.to.getDatabase()))
        return true;
    }
  }
  if (const auto *use = node.as<DB::ASTUseQuery>()) {
    if (isProtectedDatabase(use->getDatabase())) return true;
  }
  return false;
}

bool anyNodeReadsMetadata(const DB::IAST &node) {
  if (nodeReadsMetadata(node)) return true;
  return std::any_of(node.children.begin(), node.children.end(),
                     [](const DB::ASTPtr &child) { return child && anyNodeReadsMetadata(*child); });
}

} // namespace

bool readsMetadata(const DB::ASTPtr &ast, const std::string &sql) {
  if (!ast) return false;
  if (anyNodeReadsMetadata(*ast)) return true;
  return firstDeploymentIdentityCall(sql).has_value() || firstBareDeploymentIdentityName(ast).has_value();
}

} // namespace rewriter_handlers
