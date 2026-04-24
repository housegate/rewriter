# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Project

C++23 gRPC service that rewrites ClickHouse SQL before it reaches a backend ClickHouse cluster. Links directly against the ClickHouse source tree (vendored as the `clickHouse/` git submodule) so it can parse queries into ClickHouse's own AST, mutate them via transformers, and format them back out.

Entry point: `src/rewriter-server.cc` — gRPC server + `main()` (guarded by `REWRITER_RELEASE_MAIN`). Invocation: `clickhousegate_rewriter [grpc_port] [http_port]`. Defaults: gRPC `50051`, HTTP `grpc_port + 1`.

The request-handling logic lives in protocol-agnostic `RewriterServiceImpl::doRewrite` and `doRewriteErrorMessage`. The gRPC `Rewrite` / `RewriteErrorMessage` methods are thin wrappers that delegate and return `Status::OK` — never add logic to the wrappers, always to the `do*` methods, so the HTTP façade stays in sync.

**HTTP+JSON façade**: [src/rewriter-http-server.h](src/rewriter-http-server.h) + [src/rewriter-http-server.cc](src/rewriter-http-server.cc) wrap the same `RewriterServiceImpl` instance behind a Poco::Net HTTPServer (Poco is already a transitive dep via `clickhouse_common_io`, no new linkage). Routes: `POST /rewrite`, `POST /rewriteerrormessage` (also accept gRPC-style `/rewriter.RewriterService/<Method>`), `GET /healthz`. Body is JSON matching the proto messages — converted via `google::protobuf::util::JsonStringToMessage` / `MessageToJsonString`. The HTTP server runs in its own Poco thread pool started from `RunServer`; the gRPC `Wait()` is what blocks main.

`src/main.cc` and `scripttest.sh` are deprecated — not wired into any build target. Don't spend time on them.

## Build / test / run

**ClickHouse does not compile on local dev machines** — all builds and tests happen on a dedicated remote build box. The local checkout is for editing; every build crosses the network.

### Remote build box

- **SSH:** `ssh -p 30100 sentio@BUILD_HOST`
- **Dev workdir (has build cache):** `/home/sentio/chen/rewriter-grpc/`
- **CI workdir (used by `.github/workflows/release.yml`):** `~/ci/rewriter/`
- Other directories like `~/chen/sql-rewriter/` or `~/limengyu/sql-rewriter/` are stale — ignore them.

### Typical dev loop

```shell
# 1. Push local edits to the dev workdir. Exclude heavy dirs so rsync is fast.
rsync -az --delete \
  --exclude='.git' --exclude='build/' --exclude='clickHouse/' \
  --exclude='contrib' --exclude='docs/' \
  -e "ssh -p 30100" \
  ./ sentio@BUILD_HOST:/home/sentio/chen/rewriter-grpc/

# 2. Incremental build (30s-3min).
ssh -p 30100 sentio@BUILD_HOST \
  "cd /home/sentio/chen/rewriter-grpc && ./scripts.sh rebuild"

# 3. Run tests.
ssh -p 30100 sentio@BUILD_HOST \
  "cd /home/sentio/chen/rewriter-grpc && ctest --test-dir build --output-on-failure"
```

Use `./scripts.sh build` (not `rebuild`) only when `build/` is missing or `CMakeLists.txt` changed — full build is ~15-20min.

### `./scripts.sh` commands

The wrapper handles symlinks and submodule patches that `ninja` otherwise trips on. Chinese comments inside.

```shell
./scripts.sh prepare          # install toolchain (apt+LLVM on Linux, brew on macOS)
./scripts.sh build            # full clean build (wipes build/, ~15-20 min)
./scripts.sh rebuild          # incremental; use this day-to-day. Falls through to build if build/ is missing.
./scripts.sh test             # build + ctest
./scripts.sh clean            # rm -rf build/
./scripts.sh docker build <v> # stages build/clickhousegate_rewriter → docker/bare/root/ then buildx
./scripts.sh docker push  <v>
```

Prerequisite submodule init on a fresh clone: `cd clickHouse && git submodule update --init --depth=1`.

Single-test runs (gtest binary):
```shell
./build/rewriter_tests --gtest_filter='CTEOnly.RunByName_Simple'
```

Under the hood `build` runs `cmake -B build -GNinja -DENABLE_RUST=OFF -DENABLE_TESTS=OFF -DBUILD_TESTING=ON -DENABLE_THINLTO=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold` then `ninja -C build clickhousegate_rewriter rewriter_tests`. The main target name is `clickhousegate_rewriter`; tests are `rewriter_tests`.

### Cutting a release

Don't bump versions by hand. Use the `cut-release` workflow (`Actions → cut-release → Run workflow`). It tags `main` as `vX.Y.Z` in UTC — first cut of a new UTC day bumps Y (minor), subsequent cuts same day bump Z (patch) — then dispatches `release.yml` which builds + pushes the docker image on the build host.

### Submodule brittleness (important)

`scripts.sh`'s `build` runs `git submodule foreach --recursive 'git reset --hard && git clean -fdx'` at the end, which also wipes the `clickHouse/src/clickHouse` self-symlink needed for `#include "clickHouse/..."` to resolve, and reverts in-tree patches. Both `build` and `rebuild` call `ensure_symlinks` + `ensure_patches` on entry to re-establish them:

- `clickHouse/src/clickHouse -> .` and `./contrib -> clickHouse/contrib`
- `flatbuffers/include/flatbuffers/stl_emulation.h`: drop `const` from `size_type count_`
- `numactl-cmake/CMakeLists.txt`: append `distance.c / affinity.c / sysfs.c / rtnetlink.c` so the static lib exports `resolve_affinity` etc.

If you touch the build by hand (not via `scripts.sh`), apply the same symlinks/patches or ninja will fail with "file not found" on `Dictionaries/DictionaryFactory.h` or `undefined symbol: resolve_affinity`.

## Architecture

### Request flow

`RewriterService.Rewrite` in [src/rewriter-server.cc](src/rewriter-server.cc):

1. **Preprocess** — `QueryPreprocessor::preprocess` (declared in [src/rewriter-server.h](src/rewriter-server.h)) regex-scans for `IN (...)` / `IN [...]` clauses with ≥50 elements and splits them into OR-joined batches of 50. This is a stack-overflow guard for ClickHouse's recursive-descent parser; it runs **before** `parseQuery` and only rewrites literal text.
2. **Parse** — `DB::parseQuery(DB::ParserQuery, ...)` produces an `ASTPtr`. Parse errors return `RewriteCode::SyntaxError` with `Status::OK` (never a gRPC error).
3. **Dispatch by statement kind** (handlers live under [src/handlers/](src/handlers/); each returns a disposition that either consumes the request or falls through to the next):
   - `ASTUseQuery` → [handlers/use.cc](src/handlers/use.cc). `DatabaseNameRewrite` option; three-branch resolution (`database_map` → `known_physical_databases` → `default_database` fallback). See the proto comment on `RewriteDatabaseNameArgs` for the exact semantics — the rewriter must stay in lock-step with the proxy's old regex behavior.
   - `ASTShowTablesQuery` → [handlers/show_tables.cc](src/handlers/show_tables.cc). `ShowTablesRewrite` option; emits a filtered `SELECT … FROM system.tables WHERE database = '<physical>' AND startsWith(name, '<logical>')` with the outer `multiIf` stripping the `<logical>` prefix. Resolution: `physical := args.physical_database`; `logical := FROM/IN-clause || args.logical_database`. If `logical == ""` → passthrough (sets `show_stmt_value.show_tables_passthrough=true`, returns the original SQL). Note the FROM-clause is interpreted as a **logical** name (table prefix), not a physical DB.
   - `ASTExistsTableQuery` → [handlers/exists.cc](src/handlers/exists.cc). Applies `TableNameRewrite` to the target; `statement_type = STATEMENT_TYPE_EXISTS_TABLE`. `EXISTS DATABASE / VIEW / DICTIONARY` return `UnsupportedStatement` (rewrite pipeline is table-oriented).
   - `ASTShowCreateTableQuery` → [handlers/show_create.cc](src/handlers/show_create.cc). Parallel to EXISTS; `statement_type = STATEMENT_TYPE_SHOW_CREATE_TABLE`. `SHOW CREATE DATABASE / VIEW / DICTIONARY` return `UnsupportedStatement`.
   - `ASTCreateQuery / ASTDropQuery / ASTAlterQuery / ASTInsertQuery / ASTUpdateQuery / ASTDeleteQuery / ASTRenameQuery` → [handlers/writes.cc](src/handlers/writes.cc). Each target is rewritten via `TableNameRewrite` (other ops like `LimitRewrite` are ignored for writes). On acceptance: `statement_type = STATEMENT_TYPE_CREATE_TABLE | _DROP_TABLE | _ALTER_TABLE | _INSERT | _UPDATE | _DELETE | _RENAME_TABLE`; `write_stmt_value.target_table` (deprecated, see protocol section) carries the post-rewrite primary target. For CREATE TABLE AS, the source table is rewritten with the same map. Unsupported variants (CREATE VIEW/DICTIONARY/DATABASE, DROP DATABASE/VIEW/multi-table, RENAME DATABASE/DICTIONARY, TRUNCATE, OPTIMIZE, GRANT/REVOKE, KILL, BACKUP/RESTORE, INSERT INTO FUNCTION, `ALTER TABLE ... ATTACH/REPLACE/FETCH PARTITION FROM other`, `ALTER TABLE ... MOVE PARTITION TO TABLE other`, …) return `UnsupportedStatement`. **A `remote_table_map` hit on any write/EXISTS/SHOW CREATE target is a hard reject with `InvalidRewriteRequest`** — `remote()` only makes sense as a SELECT source, not as a write target.
   - Everything else (`SELECT`, other DML) → falls through to [handlers/select.cc](src/handlers/select.cc).
4. **SELECT path** — collect `original_accessed_table_names` via `DB::collectTableNamesFromAST` (plus any CTE-injected tables), then iterate `request.options()` and apply transformers in option order: Limit → TableName → Offset → Settings → CTE.
5. **Populate `table_rewrites`** — every `TableNameRewrite` hit (in SELECT tree or single-target handlers) gets recorded as `"origin_db.origin_table" → "rewritten_db.rewritten_table"` in the top-level `response.table_rewrites` map. No-op rewrites (origin == rewritten) are omitted.
6. **Format** — `ast->formatWithPossiblyHidingSensitiveData(0, one_line=true, hilite=true, show_secrets=false, rule=WhenNecessary, style=Backticks)` produces the output SQL.

### AST transformers ([src/ASTTransformers.cc](src/ASTTransformers.cc), [src/ASTTransformers.h](src/ASTTransformers.h))

Each transformer subclasses `IASTTransformer` (itself an `IAST` so it can live in the tree during debugging). They mutate the AST in place:

- `ASTReplaceTransformer` — SELECT-tree table rewrites. Walks `ASTTableExpression` nodes and dispatches to `planTableRewrite()` for every `ASTTableIdentifier` it finds. Three map kinds in precedence order: `table_name_map` (plain rename) → `remote_table_map` (swap the identifier for a `remote('addr', db, table, user, password)` function call) → `table_with_database_map` (attach an explicit database). Every match gets recorded in `transformer.rewrites` so the SELECT handler can surface it on the response.
- `ASTRewriteCTETransformer` — injects named CTEs into `WITH` clauses of every `SELECT` subquery that actually references them. See `parseCTEMapToAST` (parses each CTE SQL once, tolerant of individual failures) and `collectCTEKeysForSelect`.

`planTableRewrite()` is the shared primitive: given `(origin_name, three maps)` it returns a `TableRewritePlan { kind, db, table, remote }`. SELECT uses it inside the tree walk; the single-target handlers (writes, EXISTS, SHOW CREATE) use [handlers/name_rewrite.h](src/handlers/name_rewrite.h)'s `applySingleTableRewrite` wrapper — which iterates `TableNameRewrite` options, runs `planTableRewrite`, and returns a `{NoChange | Rewritten | RejectRemote}` outcome. Keeping the lookup policy in one place means the three map precedences stay consistent across every statement kind.

`LimitRewrite` / `OffsetRewrite` / `SettingsRewrite` used to have their own transformer classes but are now applied inline in [handlers/select.cc](src/handlers/select.cc) — all three batch into a `PendingSelectTransforms` struct and flush in a single AST walk.

`collectTableNamesFromAST` also lives here and is what fills `response.original_accessed_table_names`.

### Protocol

[protos/rewriter.proto](protos/rewriter.proto) defines the contract. Key invariants worth preserving:

- `RewriteOp` + `RewriteOption.value` are a tagged-union by `op` — adding a new op means: new enum value, new `*Args` message, new `oneof` case, new `case` in the dispatch switch, and probably new response fields. Bump both C++ and Go clients.
- Response `statement_type` is a `StatementType` enum (`STATEMENT_TYPE_SELECT`, `_USE`, `_SHOW_TABLES`, `_SHOW_CREATE_TABLE`, `_EXISTS_TABLE`, `_CREATE_TABLE`, `_DROP_TABLE`, `_ALTER_TABLE`, `_RENAME_TABLE`, `_INSERT`, `_UPDATE`, `_DELETE`). `STATEMENT_TYPE_UNSPECIFIED` (= 0) is the default when classification didn't run (parse errors, etc.). Adding new values is fine; renumbering or reusing numbers is not.
- `RewriteCode` splits rejection taxonomy into four cases — callers should branch on this, not on `message` strings:
  - `Success` (0): accepted.
  - `SyntaxError` (1): ClickHouse's `ParserQuery` rejected the SQL text; `message` carries the parser's `displayText()`.
  - `RewriteError` (2): generic bucket kept for unexpected rewrite failures. Prefer the more specific codes below when classifying new reject paths.
  - `UnsupportedStatement` (3): the SQL is valid but the rewriter doesn't handle this statement kind / variant (CREATE VIEW, DROP DATABASE, TRUNCATE, OPTIMIZE, RENAME DATABASE, EXISTS VIEW/DATABASE/DICTIONARY, SHOW CREATE VIEW/DATABASE/DICTIONARY, INSERT INTO FUNCTION, multi-table DROP, ALTER cross-table partition refs, GRANT/KILL/BACKUP/...). Callers MAY pass the SQL through to ClickHouse unchanged.
  - `InvalidRewriteRequest` (4): the caller's `RewriteOption` can't be applied — their rewrite config is wrong for the SQL they sent. Callers MUST NOT pass through; surface the error. Current case: any write / EXISTS / SHOW CREATE target hitting `remote_table_map`.
- Top-level `response.table_rewrites: map<string, string>` is the canonical "what got rewritten" output. Keys/values are `"db.table"` (or bare `"table"` when no db was present). Populated by every statement kind that honors `TableNameRewrite` — including SELECT. Only entries whose origin differs from the rewritten form are recorded. For `remote_table_map` hits (SELECT only), the value is the remote-side `db.table`; the full `remote('addr', …)` call lives in `sql_after_rewrite`.
- Statement-specific response payloads live inside the `extra_value` oneof: `use_stmt_value.specified_database` (USE default-db fallback), `show_stmt_value.show_tables_passthrough` (SHOW TABLES passthrough), `write_stmt_value.target_table` (single-table writes — **deprecated, prefer `table_rewrites`**; kept for backward compat; empty for RENAME). Each exists to preserve an exact behavior of the legacy regex-based proxy they replaced — read the proto comments before changing the branches that set them.
- Syntactic / semantic rewrite failures return `Status::OK` with a non-Success `RewriteCode`; only unexpected server errors would return a gRPC error.

### Tests ([tests/rewriter_test.cc](tests/rewriter_test.cc))

GoogleTest v1.14 fetched via `FetchContent`. Tests link the full server implementation (`rewriter-server.cc` + `ASTTransformers.cc`) and call `RewriterServiceImpl::Rewrite` directly (`main()` is excluded by `REWRITER_RELEASE_MAIN=0` for the test target). Use `RewriterServiceImpl::RewriteForTest` if you need to go through the public-looking entrypoint.

## Conventions

- Comments and script output are often in Chinese; match the surrounding language when editing those files, otherwise English.
- Use ClickHouse's own AST types and `DB::` helpers — do not reach for a second SQL parser. Look under `clickHouse/src/Parsers/` for `ASTSelectQuery`, `ASTShowTablesQuery`, `ASTUseQuery`, `ASTCreateQuery`, etc.
- Formatting: output must go through `formatWithPossiblyHidingSensitiveData` with `IdentifierQuotingStyle::Backticks` and `IdentifierQuotingRule::WhenNecessary` to match what the downstream ClickHouse expects.
- When adding a new statement kind, also update: the dispatch in `Rewrite`, the write-kind rejection switch (so it doesn't fall through to SELECT), CMakeLists (if it's a new handler file), and a matching test.
- When a new statement kind honors `TableNameRewrite`, wire it through `applySingleTableRewrite` (or extend `ASTReplaceTransformer` if it's a SELECT-like tree walk) and call `recordTableRewrite` from [handlers/name_rewrite.h](src/handlers/name_rewrite.h) so `response.table_rewrites` stays consistent across the codebase. A `remote_table_map` hit on anything that isn't a SELECT source should be a hard reject, not a silent passthrough.


<claude-mem-context>
# Memory Context

# [rewriter-grpc] recent context, 2026-04-24 6:25pm GMT+8

No previous sessions found.
</claude-mem-context>