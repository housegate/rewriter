#pragma once

#include <Parsers/IAST.h>


namespace re2
{
    class RE2;
}

namespace DB
{

/// A list of transformers
class ASTTransformerList : public IAST
{
public:
    String getID(char) const override { return "TransformerList"; }
    ASTPtr clone() const override
    {
        auto clone = std::make_shared<ASTTransformerList>(*this);
        clone->cloneChildren();
        return clone;
    }

protected:
    void formatImpl(const FormatSettings & settings, FormatState & state, FormatStateStacked frame) const override;
};

class IASTTransformer : public IAST
{
public:
    virtual void transform(ASTPtr & ast) const = 0;
    static void transform(const ASTPtr & transformer, ASTPtr & ast);
};

// class ASTApplyTransformer : public IASTTransformer
// {
// public:
//     String getID(char) const override { return "ApplyTransformer"; }
//     ASTPtr clone() const override
//     {
//         auto res = std::make_shared<ASTApplyTransformer>(*this);
//         // if (parameters)
//         //     res->parameters = parameters->clone();
//         // if (lambda)
//         //     res->lambda = lambda->clone();
//         return res;
//     }
//     void transform(ASTPtr & nodes) const override;
//     // void appendColumnName(WriteBuffer & ostr) const override;
//     // void updateTreeHashImpl(SipHash & hash_state, bool ignore_aliases) const override;

//     // // Case 1  APPLY (quantile(0.9))
//     // String func_name;
//     // ASTPtr parameters;

//     // // Case 2 APPLY (x -> quantile(0.9)(x))
//     // ASTPtr lambda;
//     // String lambda_arg;

//     // String column_name_prefix;

// protected:
//     void formatImpl(const FormatSettings & settings, FormatState &, FormatStateStacked) const override;
// };

class RemoteTable 
{
public:
    String addr;
    String database_name;
    String table_name;
    String user_name;
    String password;
};

class TableWithDatabase
{
public:
    String database_name;
    String table_name;
};


class ASTReplaceTransformer : public IASTTransformer
{
public:
    bool is_strict = false;
    String getID(char) const override { return "TableNameReplaceTransformer"; }
    ASTPtr clone() const override
    {
        auto clone = std::make_shared<ASTReplaceTransformer>(*this);
        clone->cloneChildren();
        return clone;
    }
    void transform(ASTPtr & ast) const override;
    void setTableNameMap(const std::map<String, String> & table_name_map);
    void setRemoteTableMap(const std::map<String, RemoteTable> & remote_table_map);
    void setTableWithDatabaseMap(const std::map<String, TableWithDatabase> & table_with_database_map);

    std::map<String, String> table_name_map;
    std::map<String, RemoteTable> remote_table_map;
    std::map<String, TableWithDatabase> table_with_database_map;

protected:
    void formatImpl(const FormatSettings & settings, FormatState &, FormatStateStacked) const override;
};


class ASTRewriteLimitTransformer : public IASTTransformer
{
public:
    bool is_strict = false;
    String getID(char) const override { return "RewriteLimitTransformer"; }
    ASTPtr clone() const override
    {
        auto clone = std::make_shared<ASTRewriteLimitTransformer>(*this);
        clone->cloneChildren();
        return clone;
    }
    void transform(ASTPtr & ast) const override;
    void setLimit(size_t limit, bool force_limit = false);
    void setThreshold(size_t threshold);

    size_t limit = 0;
    bool force_limit = false;
    size_t threshold = 0;

protected:
    void formatImpl(const FormatSettings & settings, FormatState &, FormatStateStacked) const override;
};

class ASTRewriteOffsetTransformer : public IASTTransformer
{
public:
    bool is_strict = false;
    String getID(char) const override { return "RewriteOffsetTransformer"; }
    ASTPtr clone() const override
    {
        auto clone = std::make_shared<ASTRewriteOffsetTransformer>(*this);
        clone->cloneChildren();
        return clone;
    }
    void transform(ASTPtr & ast) const override;
    void setOffset(size_t offset);

    size_t offset = 0;

protected:
    void formatImpl(const FormatSettings & settings, FormatState &, FormatStateStacked) const override;
};

}
