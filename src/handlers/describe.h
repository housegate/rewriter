#pragma once

#include <Parsers/IAST.h>

#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

bool handleDescribeQuery(DB::ASTPtr ast,
  const rewriter::RewriteSQLRequest *request,
  rewriter::RewriteSQLResponse *response);

} // namespace rewriter_handlers
