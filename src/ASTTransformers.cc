#include <map>
#include "ASTTransformers.h"
#include <IO/WriteHelpers.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
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
  if (auto * identifier = ast->as<ASTTableIdentifier>())
  {
    if (table_name_map.find(identifier->name()) != table_name_map.end())
    {
        identifier->setShortName(table_name_map.at(identifier->name()));
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