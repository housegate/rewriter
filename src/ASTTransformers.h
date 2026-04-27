
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <Parsers/IAST.h>

#include "Parsers/ASTSelectQuery.h"
#include "rewriter.grpc.pb.h"

namespace re2 {
class RE2;
}

namespace DB {
class ASTTransformerList : public IAST {
public:
  String getID(char) const override {
    return "TransformerList";
  }

  ASTPtr clone() const override {
    auto clone = std::make_shared<ASTTransformerList>(*this);
    clone->cloneChildren();
    return clone;
  }

protected:
  void format(WriteBuffer &ostr, const FormatSettings &settings) const;
};

class IASTTransformer : public IAST {
public:
  virtual void transform(ASTPtr &ast) const = 0;

  static void transform(const ASTPtr &transformer, ASTPtr &ast);
};

class RemoteTable {
public:
  String addr;
  String database_name;
  String table_name;
  String user_name;
  String password;
};

class TableWithDatabase {
public:
  String database_name;
  String table_name;
};

// One of the three TableNameRewrite branches, or None if no map matched.
// Produced by planTableRewrite() and consumed by each statement handler to
// apply the rewrite to its own AST slot. Keeping this as a plain struct (vs.
// a fatter transformer that walks every AST kind) lets each handler own its
// AST-specific mutation logic while sharing the same lookup precedence.
enum class TableRewriteKind { None, Rename, WithDatabase, Remote };

struct TableRewritePlan {
  TableRewriteKind kind = TableRewriteKind::None;
  // Rename: `table` holds the new short name; `db` is empty (caller preserves
  //         whatever db the original AST node had).
  // WithDatabase: `db` + `table` fully specify the target (either may be empty
  //         depending on how the caller populated table_with_database_map).
  // Remote: `db`/`table` are unused; `remote` carries the full remote() args.
  String db;
  String table;
  RemoteTable remote;
};

// Lookup precedence matches ASTReplaceTransformer::transform():
//   table_name_map → remote_table_map → table_with_database_map.
// The lookup key is exactly `origin_name` (caller decides whether that's a
// short name or a dot-qualified "db.table" form — the maps must be keyed to
// match).
TableRewritePlan planTableRewrite(
  const String &origin_name,
  const std::unordered_map<String, String> &table_name_map,
  const std::unordered_map<String, RemoteTable> &remote_table_map,
  const std::unordered_map<String, TableWithDatabase> &table_with_database_map);

class CommonTableExpr {
public:
  String alias;
  String sql;
};

class Setting {
public:
  String name;
  rewriter::SettingType value_type;
  String string_value;
  size_t int_value;
  bool bool_value;
  uint64_t uint64_value;
};

class ASTReplaceTransformer : public IASTTransformer {
public:
  bool is_strict = false;

  String getID(char) const override {
    return "TableNameReplaceTransformer";
  }

  ASTPtr clone() const override {
    auto clone = std::make_shared<ASTReplaceTransformer>(*this);
    clone->cloneChildren();
    return clone;
  }

  void transform(ASTPtr &ast) const override;

  void setTableNameMap(const std::unordered_map<String, String> &table_name_map);

  void setRemoteTableMap(const std::unordered_map<String, RemoteTable> &remote_table_map);

  void setTableWithDatabaseMap(
    const std::unordered_map<String, TableWithDatabase> &table_with_database_map);

  std::unordered_map<String, String> table_name_map;
  std::unordered_map<String, RemoteTable> remote_table_map;
  std::unordered_map<String, TableWithDatabase> table_with_database_map;

  // Populated by transform(): maps every origin identifier name that got
  // rewritten to its post-rewrite form. For Remote hits, the value is the
  // remote-side "db.table" (or bare "table" when db is empty). Unmatched
  // identifiers produce no entry. mutable so transform() can stay const.
  mutable std::map<std::string, std::string> rewrites;

  void clearRewrites() { rewrites.clear(); }

protected:
  void format(WriteBuffer &ostr, const FormatSettings &settings) const;
};

class ASTRewriteCTETransformer : public IASTTransformer {
public:
  bool is_strict = false;

  String getID(char) const override {
    return "RewriteCTETransformer";
  }

  ASTPtr clone() const override {
    auto clone = std::make_shared<ASTRewriteCTETransformer>(*this);
    clone->cloneChildren();
    return clone;
  }

  void transform(ASTPtr &ast) const override;
  void setCTEMap(const std::map<String, DB::CommonTableExpr> &cte_map_);
  void setCTEASTMap(const std::unordered_map<String, ASTPtr> &cte_ast_map);

private:
  ASTPtr buildWithList(
    const DB::ASTSelectQuery *select_ast, const std::unordered_set<String> &used_key) const;
  std::unordered_set<String> collectCTEKeysForSelect(
    const DB::ASTSelectQuery *select_ast, std::unordered_map<String, bool> &used_cte_key_map) const;
  void injectOnce(const ASTPtr &ast, std::unordered_map<String, bool> &used_cte_key_map) const;
  void mergeWithIntoSelect(ASTSelectQuery *sel, ASTPtr &new_list) const;

  std::unordered_map<String, ASTPtr> cte_ast_map;
  bool cte_ast_map_set = false;
  std::map<String, DB::CommonTableExpr> cte_map;

protected:
  void format(WriteBuffer &ostr, const FormatSettings &settings) const;
};

// Parse each CTE's SQL into an AST once; invalid entries are skipped but others continue.
// When `failed_aliases` is non-null, aliases whose SQL failed to parse are appended to it
// (in map iteration order) so the caller can surface them to the client.
std::unordered_map<String, ASTPtr> parseCTEMapToAST(
    const std::map<String, CommonTableExpr> &cte_map,
    std::vector<String> *failed_aliases = nullptr);
} // namespace DB
