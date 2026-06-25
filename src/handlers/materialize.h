// src/handlers/materialize.h
//
// Phase-1 non-determinism materialization: rewrite non-deterministic ClickHouse
// functions into deterministic literals from caller-supplied inputs, before the
// agent/SDK signs a statement. Mirrors housegate/rewriter-go#17. Reads no clock
// and no RNG — all values come from request.inputs().
#pragma once

#include <string>

#include "rewriter.grpc.pb.h"

namespace rewriter_handlers {

inline constexpr const char *kDefaultMaterializationProfileId = "sentio-p1-nondet-v1";

// Full self-contained handler: validate policy, parse request.sql(), materialize
// supported non-deterministic functions, and fill `response`. Never throws past
// the boundary; failures are reported via response.code() with sql_after_
// materialization == request.sql().
void materialize(const rewriter::MaterializeSQLRequest *request,
                 rewriter::MaterializeSQLResponse *response);

} // namespace rewriter_handlers
