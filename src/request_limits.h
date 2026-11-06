// src/request_limits.h
//
// Resource bounds for the request paths, each overridable from the
// environment. The names and defaults match the overload protection of the
// upstream sql-rewriter this repository was forked from, which bounded the
// same paths after a 2026-09-03 CPU-saturation incident.
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace rewriter_limits {

// A non-negative decimal integer from the environment, or `fallback` when the
// variable is unset, empty, signed, malformed or above `max_value`.
// strtoull() accepts a leading '-' and wraps it ("-1" becomes SIZE_MAX), so a
// sign is rejected explicitly rather than turning a limit into "unbounded".
inline size_t envSize(const char *name, size_t fallback, size_t max_value = SIZE_MAX) {
  const char *raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') return fallback;
  const char *cursor = raw;
  while (*cursor == ' ' || *cursor == '\t') ++cursor;
  if (*cursor == '-' || *cursor == '+') return fallback;
  errno = 0;
  char *endptr = nullptr;
  const unsigned long long parsed = std::strtoull(cursor, &endptr, 10);
  if (errno != 0 || endptr == cursor || (endptr != nullptr && *endptr != '\0')) return fallback;
  if (parsed > static_cast<unsigned long long>(max_value)) return fallback;
  return static_cast<size_t>(parsed);
}

// Total bytes of caller SQL one Rewrite or RewriteErrorMessage request may
// carry: the statement plus every CommonTableExprRewrite body (all of them are
// parsed). 0 disables the check.
inline size_t maxRequestSqlBytes() {
  return envSize("REWRITER_MAX_SQL_BYTES", 4u * 1024u * 1024u);
}

// Bytes of ClickHouse error text RewriteErrorMessage accepts. The text can
// echo the whole query back. 0 disables the check.
inline size_t maxErrorMessageBytes() {
  return envSize("REWRITER_MAX_ERROR_BYTES", 1u * 1024u * 1024u);
}

// Bytes RewriteErrorMessage may scan across all of its passes (the
// whole-query swap and one pass per table / database mapping). Once spent, the
// error is withheld rather than returned partly rewritten. 0 disables it.
inline size_t errorRewriteScanBudget() {
  return envSize("REWRITER_ERROR_SCAN_BUDGET", 128u * 1024u * 1024u);
}

// Bound on the gRPC sync-server thread pool. Once it is exhausted, excess RPCs
// fail fast with RESOURCE_EXHAUSTED instead of queueing without limit. The
// floor is 2: a sync server needs a poller to reserve its replacement before it
// becomes a worker, so a quota of 1 would answer every call RESOURCE_EXHAUSTED.
inline int maxServerThreads() {
  constexpr size_t kMinServerThreads = 2;
  const size_t configured = envSize("REWRITER_MAX_SERVER_THREADS", 64,
    static_cast<size_t>(std::numeric_limits<int>::max()));
  return static_cast<int>(configured < kMinServerThreads ? kMinServerThreads : configured);
}

// gRPC receive cap. gRPC's own default (4 MiB) would otherwise be the
// effective bound and make the application caps above unreachable, so the
// default is derived from them plus 1 MiB for options and framing. A zero
// (disabled) application cap saturates the transport cap instead of
// tightening it. Clamped to INT_MAX for SetMaxReceiveMessageSize.
inline int maxReceiveMessageBytes() {
  constexpr size_t kOverheadBytes = 1u * 1024u * 1024u;
  const size_t int_max = static_cast<size_t>(std::numeric_limits<int>::max());
  const size_t sql = maxRequestSqlBytes();
  const size_t err = maxErrorMessageBytes();
  size_t derived = int_max;
  if (sql != 0 && err != 0 && sql <= int_max && err <= int_max - sql
      && kOverheadBytes <= int_max - sql - err) {
    derived = sql + err + kOverheadBytes;
  }
  return static_cast<int>(envSize("REWRITER_MAX_RECV_MESSAGE_BYTES", derived, int_max));
}

// HTTP request-body cap. JSON escaping can roughly double a payload, so the
// default is twice the gRPC receive cap.
inline size_t maxHttpBodyBytes() {
  return envSize("REWRITER_MAX_HTTP_BODY_BYTES", 2u * static_cast<size_t>(maxReceiveMessageBytes()));
}

} // namespace rewriter_limits
