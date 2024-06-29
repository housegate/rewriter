// #include <iostream>

// #include <Parsers/ASTCreateQuery.h>
// #include <Parsers/ParserCreateQuery.h>
// #include <Parsers/formatAST.h>
// #include <Parsers/parseQuery.h>
// #include <IO/WriteBufferFromOStream.h>

#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/formatAST.h>
// #include <Parsers/obfuscateQueries.h>
#include <Parsers/parseQuery.h>
#include <IO/WriteBufferFromString.h>
#include "ASTTransformers.h"

int main(int, char **)
{
    // using namespace DB;

    // std::string input = "CREATE TABLE hits (URL String, UserAgentMinor2 FixedString(2), EventTime DateTime) ENGINE = Log";
    // ParserCreateQuery parser;
    // ASTPtr ast = parseQuery(parser, input.data(), input.data() + input.size(), "", 0, 0, 0);

    // WriteBufferFromOStream out(std::cerr, 4096);
    // formatAST(*ast, out);
    // std::cerr << std::endl;

    String query = "select * from ttt";

  const char * pos = query.data();
  const char * end = pos + query.size();

  DB::ParserQuery parser(end, false);

  DB::ASTPtr ast = parseQuery(parser, pos, end, 0, 0, 0);
  DB::ASTReplaceTransformer transformer = DB::ASTReplaceTransformer();
  std::map<std::string, std::string> table_name_map;
  table_name_map["ttt"] = "ttt2";
  transformer.setTableNameMap(table_name_map);
  transformer.transform(ast);
  DB::WriteBufferFromOwnString out;
  ast->dumpTree(out);
  printf("%s\n", out.str().c_str());
  return 0;

    // std::printf("Hello, World!\n");

}