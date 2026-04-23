# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

C++23 gRPC service that rewrites ClickHouse SQL before it reaches a backend ClickHouse cluster. Links directly against the ClickHouse source tree (vendored as the `clickHouse/` git submodule) so it can parse queries into ClickHouse's own AST, mutate them via transformers, and format them back out.

Entry point: `src/rewriter-server.cc` — gRPC server + `main()` (guarded by `REWRITER_RELEASE_MAIN`). Invocation: `clickhouse_sentio_rewriter [grpc_port] [http_port]`. Defaults: gRPC `50051`, HTTP `grpc_port + 1`.

The request-handling logic lives in protocol-agnostic `RewriterServiceImpl::doRewrite` and `doRewriteErrorMessage`. The gRPC `Rewrite` / `RewriteErrorMessage` methods are thin wrappers that delegate and return `Status::OK` — never add logic to the wrappers, always to the `do*` methods, so the HTTP façade stays in sync.

**HTTP+JSON façade**: [src/rewriter-http-server.h](src/rewriter-http-server.h) + [src/rewriter-http-server.cc](src/rewriter-http-server.cc) wrap the same `RewriterServiceImpl` instance behind a Poco::Net HTTPServer (Poco is already a transitive dep via `clickhouse_common_io`, no new linkage). Routes: `POST /Rewrite`, `POST /RewriteErrorMessage` (also accept gRPC-style `/rewriter.RewriterService/<Method>`), `GET /healthz`. Body is JSON matching the proto messages — converted via `google::protobuf::util::JsonStringToMessage` / `MessageToJsonString`. The HTTP server runs in its own Poco thread pool started from `RunServer`; the gRPC `Wait()` is what blocks main.

`src/main.cc` and `scripttest.sh` are deprecated — not wired into any build target. Don't spend time on them.

## Build / test / run

**ClickHouse does not compile on local dev machines** — all builds and tests happen on a dedicated remote build box. Local workflow is: edit here → rsync/scp changes → SSH in → run `./script.sh`.

```shell
ssh sentio@BUILD_HOST -p 30100
# remote repo: /home/sentio/chen/sql-rewriter
cd /home/sentio/chen/sql-rewriter
./script.sh rebuild     # or build / test
```

All day-to-day commands go through `./script.sh` (Chinese comments inside). It handles symlinks and submodule patches that `ninja` otherwise trips on.

```shell
./script.sh prepare          # install toolchain (brew on macOS, apt+LLVM on Linux)
./script.sh build            # full clean build (~15–20 min first time)
./script.sh rebuild          # incremental; use this for day-to-day work
./script.sh test             # build + ctest
./script.sh clean            # rm -rf build/
./script.sh docker build <v> # stages build/clickhouse_sentio_rewriter → docker/bare/root/ then buildx
./script.sh docker push  <v>
```

Prerequisite submodule init on a fresh checkout (from `README.md`): `cd clickHouse && git submodule update --init --depth=1`.

Single-test runs (gtest binary):
```shell
./build/rewriter_tests --gtest_filter='CTEOnly.RunByName_Simple'
```

Under the hood `build` runs `cmake -B build -GNinja -DENABLE_RUST=OFF -DENABLE_TESTS=OFF -DBUILD_TESTING=ON -DENABLE_THINLTO=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold` then `ninja -C build clickhouse_sentio_rewriter rewriter_tests`. The main target name is `clickhouse_sentio_rewriter`; tests are `rewriter_tests`.

### Submodule brittleness (important)

`script.sh`'s `build` runs `git submodule foreach --recursive 'git reset --hard && git clean -fdx'` at the end, which also wipes the `clickHouse/src/clickHouse` self-symlink needed for `#include "clickHouse/..."` to resolve, and reverts in-tree patches. Both `build` and `rebuild` call `ensure_symlinks` + `ensure_patches` on entry to re-establish them:

- `clickHouse/src/clickHouse -> .` and `./contrib -> clickHouse/contrib`
- `flatbuffers/include/flatbuffers/stl_emulation.h`: drop `const` from `size_type count_`
- `numactl-cmake/CMakeLists.txt`: append `distance.c / affinity.c / sysfs.c / rtnetlink.c` so the static lib exports `resolve_affinity` etc.

If you touch the build by hand (not via `script.sh`), apply the same symlinks/patches or ninja will fail with "file not found" on `Dictionaries/DictionaryFactory.h` or `undefined symbol: resolve_affinity`.

## Architecture

### Request flow

`RewriterService.Rewrite` in [src/rewriter-server.cc](src/rewriter-server.cc):

1. **Preprocess** — `QueryPreprocessor::preprocess` (declared in [src/rewriter-server.h](src/rewriter-server.h)) regex-scans for `IN (...)` / `IN [...]` clauses with ≥50 elements and splits them into OR-joined batches of 50. This is a stack-overflow guard for ClickHouse's recursive-descent parser; it runs **before** `parseQuery` and only rewrites literal text.
2. **Parse** — `DB::parseQuery(DB::ParserQuery, ...)` produces an `ASTPtr`. Parse errors return `RewriteCode::SyntaxError` with `Status::OK` (never a gRPC error).
3. **Dispatch by statement kind**:
   - `ASTUseQuery` → `DatabaseNameRewrite` option; three-branch resolution (`database_map` → `known_physical_databases` → `default_database` fallback). See the proto comment on `RewriteDatabaseNameArgs` for the exact semantics — the rewriter must stay in lock-step with the proxy's old regex behavior.
   - `ASTShowTablesQuery` → `ShowTablesRewrite` option; emits a filtered `SELECT … FROM system.tables WHERE database = '<physical>' AND startsWith(name, '<logical>')` with the outer `multiIf` stripping the `<logical>` prefix. Resolution: `physical := args.physical_database`; `logical := FROM/IN-clause || args.logical_database`. If `logical == ""` → passthrough (sets `show_stmt_value.show_tables_passthrough=true`, returns the original SQL). Note the FROM-clause is interpreted as a **logical** name (table prefix), not a physical DB.
   - `ASTCreateQuery / ASTDropQuery / ASTAlterQuery / ASTInsertQuery / ASTUpdateQuery / ASTDeleteQuery` → **single-table write path**. Not rewritten — validated and echoed back. On acceptance: `response.write_stmt_value.target_table = "<db>.<table>"` (or bare `<table>` if DB missing), `statement_type = "CREATE_TABLE"|"DROP_TABLE"|…`. Unsupported variants (CREATE VIEW/DICTIONARY/DATABASE, DROP DATABASE/VIEW/multi-table, TRUNCATE, RENAME, OPTIMIZE, GRANT/REVOKE, KILL, BACKUP/RESTORE, INSERT INTO FUNCTION, …) return `RewriteError` with an explanatory message.
   - Everything else (`SELECT`, other DML) → falls through to `rewriteSQLImpl`.
4. **SELECT path** — collect `original_accessed_table_names` via `DB::collectTableNamesFromAST` (plus any CTE-injected tables), then `rewriteSQLImpl` iterates `request.options()` and applies transformers in option order: Limit → TableName → Offset → Settings → CTE.
5. **Format** — `ast->formatWithPossiblyHidingSensitiveData(0, one_line=true, hilite=true, show_secrets=false, rule=WhenNecessary, style=Backticks)` produces the output SQL.

### AST transformers ([src/ASTTransformers.cc](src/ASTTransformers.cc), [src/ASTTransformers.h](src/ASTTransformers.h))

Each transformer subclasses `IASTTransformer` (itself an `IAST` so it can live in the tree during debugging). They mutate the AST in place:

- `ASTReplaceTransformer` — table-name rewrites. Three maps: `table_name_map` (plain rename), `remote_table_map` (swap a bare identifier for a `remote('addr', db, table, user, password)` call), `table_with_database_map` (attach an explicit database).
- `ASTRewriteCTETransformer` — injects named CTEs into `WITH` clauses of every `SELECT` subquery that actually references them. See `parseCTEMapToAST` (parses each CTE SQL once, tolerant of individual failures) and `collectCTEKeysForSelect`.
- `ASTRewriteLimitTransformer` — two modes: `setLimit(force_limit=true)` unconditionally replaces; otherwise applies `replace_to` when original limit is 0 or `> threshold`.
- `ASTRewriteOffsetTransformer` / `ASTSettingsTransformer` — straightforward.

`collectTableNamesFromAST` also lives here and is what fills `response.original_accessed_table_names`.

### Protocol

[protos/rewriter.proto](protos/rewriter.proto) defines the contract. Key invariants worth preserving:

- `RewriteOp` + `RewriteOption.value` are a tagged-union by `op` — adding a new op means: new enum value, new `*Args` message, new `oneof` case, new `case` in the dispatch switch, and probably new response fields. Bump both C++ and Go clients.
- Response `statement_type` is a free-form string the caller switches on — keep existing values (`"SELECT"`, `"USE"`, `"SHOW_TABLES"`, `"CREATE_TABLE"`, `"DROP_TABLE"`, `"ALTER_TABLE"`, `"INSERT"`, `"UPDATE"`, `"DELETE"`) stable.
- Statement-specific response payloads live inside the `extra_value` oneof: `use_stmt_value.specified_database` (USE default-db fallback), `show_stmt_value.show_tables_passthrough` (SHOW TABLES passthrough), `write_stmt_value.target_table` (single-table writes). Each exists to preserve an exact behavior of the legacy regex-based proxy they replaced — read the proto comments before changing the branches that set them.
- Syntactic / semantic rewrite failures return `Status::OK` with a non-Success `RewriteCode`; only unexpected server errors would return a gRPC error.

### Tests ([tests/rewriter_test.cc](tests/rewriter_test.cc))

GoogleTest v1.14 fetched via `FetchContent`. Tests link the full server implementation (`rewriter-server.cc` + `ASTTransformers.cc`) and call `RewriterServiceImpl::Rewrite` directly (`main()` is excluded by `REWRITER_RELEASE_MAIN=0` for the test target). Use `RewriterServiceImpl::RewriteForTest` if you need to go through the public-looking entrypoint.

## Conventions

- Comments and script output are often in Chinese; match the surrounding language when editing those files, otherwise English.
- Use ClickHouse's own AST types and `DB::` helpers — do not reach for a second SQL parser. Look under `clickHouse/src/Parsers/` for `ASTSelectQuery`, `ASTShowTablesQuery`, `ASTUseQuery`, `ASTCreateQuery`, etc.
- Formatting: output must go through `formatWithPossiblyHidingSensitiveData` with `IdentifierQuotingStyle::Backticks` and `IdentifierQuotingRule::WhenNecessary` to match what the downstream ClickHouse expects.
- When adding a new statement kind, also update: the dispatch in `Rewrite`, the write-kind rejection switch (so it doesn't fall through to SELECT), and a matching test.
