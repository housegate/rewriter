#pragma once

#include <Parsers/IAST.h>

#include "rewriter.grpc.pb.h"
#include "Parsers/ASTSelectQuery.h"

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

  void setTableNameMap(const std::map<String, String> &table_name_map);

  void setRemoteTableMap(const std::map<String, RemoteTable> &remote_table_map);

  void setTableWithDatabaseMap(
    const std::map<String, TableWithDatabase> &table_with_database_map);

  std::map<String, String> table_name_map;
  std::map<String, RemoteTable> remote_table_map;
  std::map<String, TableWithDatabase> table_with_database_map;

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

private:
  bool injectOnce(ASTPtr &node) const;
  auto buildWithList() const -> ASTPtr;
  void mergeWithIntoSelect(ASTSelectQuery *sel, ASTPtr &new_list) const;


  std::map<String, DB::CommonTableExpr> cte_map;

protected:
  void format(WriteBuffer &ostr, const FormatSettings &settings) const;
};

class ASTRewriteLimitTransformer : public IASTTransformer {
public:
  bool is_strict = false;

  String getID(char) const override {
    return "RewriteLimitTransformer";
  }

  ASTPtr clone() const override {
    auto clone = std::make_shared<ASTRewriteLimitTransformer>(*this);
    clone->cloneChildren();
    return clone;
  }

  void transform(ASTPtr &ast) const override;

  void setLimit(size_t limit, bool force_limit = false);

  void setThreshold(size_t threshold);

  size_t limit = 0;
  bool force_limit = false;
  size_t threshold = 0;

protected:
  void format(WriteBuffer &ostr, const FormatSettings &settings) const;
};

class ASTRewriteOffsetTransformer : public IASTTransformer {
public:
  bool is_strict = false;

  String getID(char) const override {
    return "RewriteOffsetTransformer";
  }

  ASTPtr clone() const override {
    auto clone = std::make_shared<ASTRewriteOffsetTransformer>(*this);
    clone->cloneChildren();
    return clone;
  }

  void transform(ASTPtr &ast) const override;

  void setOffset(size_t offset);

  size_t offset = 0;

protected:
  void format(WriteBuffer &ostr, const FormatSettings &settings) const;
};

class ASTSettingsTransformer : public IASTTransformer {
public:
  bool is_strict = false;

  String getID(char) const override {
    return "SettingsTransformer";
  }

  ASTPtr clone() const override {
    auto clone = std::make_shared<ASTSettingsTransformer>(*this);
    clone->cloneChildren();
    return clone;
  }

  void transform(ASTPtr &ast) const override;

  void setSettings(const std::vector<Setting> &settings);

  std::vector<Setting> settings_;

protected:
  void format(WriteBuffer &ostr, const FormatSettings &settings) const;
};
} // namespace DB