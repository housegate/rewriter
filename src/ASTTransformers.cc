#include <map>
#include "ASTTransformers.h"
#include <IO/WriteHelpers.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Common/SipHash.h>
#include <Common/quoteString.h>
#include <Common/re2.h>
#include <IO/Operators.h>
#include <stack>

namespace DB
{

namespace ErrorCodes
{
  extern const int ILLEGAL_TYPE_OF_ARGUMENT;
  extern const int NO_SUCH_COLUMN_IN_TABLE;
  extern const int CANNOT_COMPILE_REGEXP;
}

void ASTTransformerList::formatImpl(const FormatSettings & settings, FormatState & state, FormatStateStacked frame) const
{
  for (const auto & child : children)
  {
      settings.ostr << ' ';
      child->formatImpl(settings, state, frame);
  }
}

void IASTTransformer::transform(const ASTPtr & transformer, ASTPtr & ast)
{
  if (const auto * apply = transformer->as<ASTReplaceTransformer>())
  {
      apply->transform(ast);
  }
}

void ASTReplaceTransformer::setTableNameMap(const std::map<std::string, std::string> & table_name_map_)
{
  table_name_map = table_name_map_;
}

void ASTReplaceTransformer::setRemoteTableMap(const std::map<std::string, DB::RemoteTable> & remote_table_map_)
{
  remote_table_map = remote_table_map_;
}

void ASTReplaceTransformer::formatImpl(const FormatSettings & settings, FormatState & state, FormatStateStacked frame) const
{
  settings.ostr << (settings.hilite ? hilite_keyword : "") << "APPLY" << (settings.hilite ? hilite_none : "") << " ";

  if (!table_name_map.empty()) {
    settings.ostr << "table_name_map: ";
    for (const auto & kv : table_name_map)
    {
        settings.ostr << kv.first << " -> " << kv.second << ", ";
    }
  }
}

void ASTReplaceTransformer::transform(ASTPtr & ast) const
{
    // replace table identifier with table name from map
    if (auto * table_expression = ast->as<ASTTableExpression>())
    {
        if (table_expression->database_and_table_name) {
            auto * identifier = table_expression->database_and_table_name->as<ASTTableIdentifier>();
            if (table_name_map.find(identifier->name()) != table_name_map.end())
            {
                identifier->setShortName(table_name_map.at(identifier->name()));
            }
            else if (remote_table_map.find(identifier->name()) != remote_table_map.end())
            {
                // use remote() function to replace table identifier
                auto remote_table = remote_table_map.at(identifier->name());
                auto remote_function = std::make_shared<ASTFunction>();
                auto arguments = std::make_shared<ASTExpressionList>();
                auto addr = std::make_shared<ASTLiteral>(remote_table.addr);
                auto database_name = std::make_shared<ASTLiteral>(remote_table.database_name);
                auto table_name = std::make_shared<ASTLiteral>(remote_table.table_name);
                auto user_name = std::make_shared<ASTLiteral>(remote_table.user_name);
                auto password = std::make_shared<ASTLiteral>(remote_table.password);
                table_expression->children.clear();
                table_expression->database_and_table_name = nullptr;
                table_expression->table_function = DB::makeASTFunction("remote", addr, database_name, table_name, user_name, password);
                table_expression->children.push_back(table_expression->table_function);
            }
        }
    }
    else
    {
        for (auto & child : ast->children)
        {
            transform(child);
        }
    }
}

void ASTRewriteLimitTransformer::formatImpl(const FormatSettings & settings, FormatState & state, FormatStateStacked frame) const
{
  settings.ostr << (settings.hilite ? hilite_keyword : "") << "APPLY" << (settings.hilite ? hilite_none : "") << " ";
  settings.ostr << "limit: " << limit;
}

void ASTRewriteLimitTransformer::setLimit(size_t limit_)
{
  limit = limit_;
}

void ASTRewriteLimitTransformer::transform(ASTPtr & ast) const
{
  // replace table identifier with table name from map
  if (auto * identifier = ast->as<ASTSelectQuery>())
  {
    if (limit > 0)
    {
        identifier->setExpression(ASTSelectQuery::Expression::LIMIT_LENGTH, std::make_shared<ASTLiteral>(limit));
    }
  }
  else
  {
    for (auto & child : ast->children)
    {
        transform(child);
    }
  }
}

void ASTRewriteOffsetTransformer::formatImpl(const FormatSettings & settings, FormatState & state, FormatStateStacked frame) const
{
  settings.ostr << (settings.hilite ? hilite_keyword : "") << "APPLY" << (settings.hilite ? hilite_none : "") << " ";
  settings.ostr << "offset: " << offset;
}

void ASTRewriteOffsetTransformer::setOffset(size_t offset_)
{
  offset = offset_;
}

void ASTRewriteOffsetTransformer::transform(ASTPtr & ast) const
{
  // replace table identifier with table name from map
  if (auto * identifier = ast->as<ASTSelectQuery>())
  {
    if (offset > 0)
    {
        identifier->setExpression(ASTSelectQuery::Expression::LIMIT_OFFSET, std::make_shared<ASTLiteral>(offset));
    }
  }
  else
  {
    for (auto & child : ast->children)
    {
        transform(child);
    }
  }
}
}