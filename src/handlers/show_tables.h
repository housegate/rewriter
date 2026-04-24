// src/handlers/show_tables.h
//
// SHOW TABLES rewrite. Converts the statement into a filtered SELECT on
// system.tables that strips the configured logical-DB prefix. See
// RewriteShowTablesArgs in rewriter.proto for the exact semantics,
// including the passthrough branch used when no logical DB is resolved.
#pragma once
#include <Parsers/IAST.h>
#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

bool handleShowTablesQuery(DB::ASTPtr ast,
                           const rewriter::RewriteSQLRequest *request,
                           rewriter::RewriteSQLResponse *response);

} // namespace rewriter_handlers
