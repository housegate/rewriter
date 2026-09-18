# Snapshot-query RED corpus

This is a test-first checkpoint for the closed snapshot-query profile. The
production entrypoints acknowledge no capability and fail closed. All shared
semantic cases are intentionally RED until the measured loaders and handlers
land; no skip or expected-failure mechanism converts them into a passing suite.
The old generated-default UNIMPLEMENTED test remains a separate transport test.

`testdata/snapshot_query_cases.json` is byte-identical in both engines. Each
case carries exact protobuf JSON requests and complete expected responses.
Comparison includes code, stable error category in `message`, version, exact Q,
SQL, positional target columns and sorted unique read IDs. Identifier quotes,
SQL and actual response identities are never normalized. All expected failures
leave executable SQL/target/columns/closure empty. Acknowledged ordinary
classification additionally has NOT_SNAPSHOT_QUERY and no executable outputs.

The corpus assumes an authenticated catalog supplied by its caller. Omitted
ordinary/system/view/temp relations represent sources outside that catalog;
RP has no table-kind, session-settings, or native-protocol parameter field.
Consequently native transport/session setting vectors belong to later HG/B/D
integration. SQL placeholders/settings and all represented request fields are
tested here. Scalar-subquery cases describe analysis shapes, never evaluate
zero/one/multiple data rows; runtime cardinality remains a B/D execution gate.
The same legacy schema hash is deliberately retained for generation variants:
a name/type hash cannot conceal changed generation semantics. Ineligible
untouched tables remain in the input catalog; full-state preservation is a
later executor obligation because these RPCs return only target/read closure.

## Initial profile

`testdata/snapshot_query_profile_record.json` is an unmeasured record template
for HG's offline generator. Its four measured digest strings are deliberately
empty; it is not an installed profile. Supply final files and measured
platforms via the generator recipe after building complete executables.
SQL execution platform linux/amd64 is the proposed recipe platform, not a
claim that this checkpoint measured or qualified any ClickHouse artifact.

The five original settings and eight nonzero limits are frozen A4 resource-test values.
The scalar-output ruling also fixes cast_keep_nullable=0. Settings are sorted: max_threads=1; read_overflow_mode, result_overflow_mode,
sort_overflow_mode, timeout_overflow_mode=throw. The template preserves
column-profile-v1/output-order-v1. Scalar entries map to typed column/literal,
integer equals/notEquals/less/lessOrEquals/greater/greaterOrEquals, boolean
and/or/not, and IN. rand/rand32/rand64 are allowed only as zero-argument
materialization occurrences with supplied integer pools. They consume each
occurrence once; rand/rand32 reject values above UInt32 and rand64 preserves
all UInt64 bits. Time/date, float RNG, UUID, random(), and other volatile
aliases are outside the initial profile even with complete pools.

The direct-literal conversion exception is `prepare-integer-literal-exact-v1`:
Analyze proves original decimal token/sign fidelity and exact target range;
it preserves that mathematical value in signed logical SQL. Prepare repeats
that proof and lowers direct final integer literals to
`accurateCast('<canonical exact decimal>', '<canonical target integer type>')`.
The helper never enters signed Analyze SQL and is not a user function allowlist
entry. All eight integer target widths, extrema and one-past values are pinned,
including -9223372036854775809 and 18446744073709551616 precision traps.
Parentheses/output aliases preserve direct literal status. CTE/UNION runtime
outputs require exact types; arbitrary aliases never acquire literal provenance.
Direct final scalar subqueries use the separate generated operation
`prepare-integer-scalar-null-throw-v1`: Nullable(T) may be lowered only to the
same integer T (Int/UInt8/16/32/64), using Prepare-only
`accurateCast((SELECT ...), 'T')` with exact internal cast_keep_nullable=0.
Zero/NULL throws, one row retains exact type/value, and multiple rows retain
the cardinality error when evaluated. Analyze never adds the helper and never
evaluates cardinality. No new Nullable output/column type is admitted. Predicate
scalar/IN expressions keep SQL three-valued logic and are not wrapped.
Missing/different/ignored cast_keep_nullable or missing/unimplemented operation
must refuse profile support in the later loader/qualification stages. The
nonexistent scalar_subquery_always_return_nullable setting is never installed. General casts, unchecked arithmetic, float computation and ORDER BY
remain refused. Same-type columns/floats remain copies.

## External identity resolution

Only `${QUERY_PROFILE_ID}` at request.query_profile_id,
expected_response.query_profile_id, prepare_request.analysis.query_profile_id
and expected_prepare_response.query_profile_id is a substitution slot.
`resolve_snapshot_corpus.py --input SOURCE --output COPY --query-profile-id Q`
creates a separate fully resolved copy. It rejects malformed Q, duplicate JSON
keys, overwriting the source and slots anywhere else. It preserves fixed wrong-Q
negative vectors. Source corpus copies and resolved copies must each be compared
byte-for-byte across repositories. Never run this resolver on engine responses.

Both harnesses accept `SNAPSHOT_QUERY_CORPUS` as a runtime path, so final measured
test executables need no rebuild after Q exists. The default source corpus
resolves typed identity slots to synthetic 0x11...11 solely to exercise the
current fail-closed stubs. This synthetic identity is not installed/supported
and must never be reused as measured GREEN evidence. Later qualification must
compile, measure, generate profiles/resolve copies, then run the exact unchanged
binaries. Native/service test executables and the actual RC service have their
own measured identities and matching bundles.

Go `make ffi` then `make test` loads the actual FFI and runs Service and
NativeRewriter for every case, plus a real gRPC oracle when
REWRITER_ORACLE_ADDR is set. No FFI is a hard error in the new semantic suite.
C++ dynamically registers every case as `SnapshotQuery.<name>` in the normal
rewriter_tests/ctest target. Build exclusively on the shared locked remote
lane using scripts.sh rebuild, explicit ninja -C build rewriter_tests, then
ctest --test-dir build --output-on-failure; preserve its baseline and cache.

Exact response equality is stronger than the current capability failure.
This checkpoint does not qualify native Linux measurement, loaders, actual
AST acceptance, generated SQL execution/types, resource enforcement, or
cross-engine GREEN parity. Those remain A4.2 through A4.6 and later role gates.
