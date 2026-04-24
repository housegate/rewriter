// src/handlers/show_create.h
//
// SHOW CREATE TABLE dispatcher. Parallel to exists.h: accepts SHOW CREATE TABLE
// (applies TableNameRewrite to the target); rejects SHOW CREATE VIEW /
// DICTIONARY / DATABASE.
#pragma once
#include <Parsers/IAST.h>
#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

enum class ShowCreateDispatchResult {
  Handled,
  NotShowCreate,
};

ShowCreateDispatchResult handleShowCreateQuery(DB::ASTPtr ast,
                                               const rewriter::RewriteSQLRequest *request,
                                               rewriter::RewriteSQLResponse *response);

} // namespace rewriter_handlers
