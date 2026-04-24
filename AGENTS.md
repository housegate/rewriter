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

Use `./scripts.sh build` (not `rebuild`) only when `build/` is missing or the build state looks corrupt — full build is ~15-20min. `rebuild` re-runs `cmake` configure on every invocation (~1-3s no-op when nothing changed), so `CMakeLists.txt` edits and `git describe`-derived version stamps are picked up automatically; you don't need to remember to switch commands.

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
3. **Dispatch by statement kind** (handlers live under [src/handlers/](src/handlers/); each returns a disposition that either consumes the request or falls through to the next). The single `TableNameRewrite` option carries two sub-messages: `select_stmt_args` (three-map policy for SELECT) and `general_args` (dynamic construction for everything else). Handlers read the one they need.
   - `ASTUseQuery` → [handlers/use.cc](src/handlers/use.cc). Consumes `general_args.database_map` + `general_args.known_physical_databases`. `database_map` hit → `USE <physical>` + `table_rewrites` entry. Target is already a known physical DB → passthrough. Neither → `InvalidRewriteRequest` ("user does not have this database"). No option → passthrough.
   - `ASTShowTablesQuery` → [handlers/show_tables.cc](src/handlers/show_tables.cc). Consumes `general_args`. Resolves `logical := FROM-clause || upstream_logical_database_in_context`, `physical := database_map[logical]` (or `logical` itself if known physical). Emits a filtered `SELECT … FROM system.tables WHERE database = '<physical>' AND startsWith(name, '<prefix>')` with `multiIf` stripping the prefix; `prefix = logical + delim + (extra_args joined by delim + delim, iff non-empty)`. Rejects with `InvalidRewriteRequest` when logical can't be resolved. No option → passthrough.
   - `ASTExistsTableQuery` → [handlers/exists.cc](src/handlers/exists.cc). Consumes `general_args`; applies `applyDynamicRewrite` to the target. `statement_type = STATEMENT_TYPE_EXISTS_TABLE`. `EXISTS DATABASE / VIEW / DICTIONARY` return `UnsupportedStatement`.
   - `ASTShowCreateTableQuery` → [handlers/show_create.cc](src/handlers/show_create.cc). Parallel to EXISTS; `statement_type = STATEMENT_TYPE_SHOW_CREATE_TABLE`. `SHOW CREATE DATABASE / VIEW / DICTIONARY` return `UnsupportedStatement`.
   - `ASTCreateQuery / ASTDropQuery / ASTAlterQuery / ASTInsertQuery / ASTUpdateQuery / ASTDeleteQuery / ASTRenameQuery` → [handlers/writes.cc](src/handlers/writes.cc). Each target is rewritten via `applyDynamicRewrite(origin_db, origin_table, general_args)`. `statement_type = STATEMENT_TYPE_CREATE_TABLE | _DROP_TABLE | _ALTER_TABLE | _INSERT | _UPDATE | _DELETE | _RENAME_TABLE`. For CREATE TABLE AS, the source table is rewritten the same way. Unsupported variants (CREATE VIEW/DICTIONARY/DATABASE, DROP DATABASE/VIEW/multi-table, RENAME DATABASE/DICTIONARY, TRUNCATE, OPTIMIZE, GRANT/REVOKE, KILL, BACKUP/RESTORE, INSERT INTO FUNCTION, `ALTER TABLE ... ATTACH/REPLACE/FETCH PARTITION FROM other`, `ALTER TABLE ... MOVE PARTITION TO TABLE other`, …) return `UnsupportedStatement`. Unqualified target + empty `upstream_logical_database_in_context`, or logical that resolves to no physical DB → `InvalidRewriteRequest`. No option → passthrough (echo the AST unchanged).
   - Everything else (`SELECT`, other DML) → falls through to [handlers/select.cc](src/handlers/select.cc).
4. **SELECT path** — collect `original_accessed_table_names` via `DB::collectTableNamesFromAST` (plus any CTE-injected tables), then iterate `request.options()` and apply transformers in option order: Limit → TableName (via `select_stmt_args`) → Offset → Settings → CTE.
5. **Populate `table_rewrites`** — every rewrite that fired (in SELECT tree or single-target handlers) gets recorded as `"origin_db.origin_table" → "rewritten_db.rewritten_table"` in the top-level `response.table_rewrites` map. No-op rewrites (origin == rewritten) are omitted. For USE, both sides of the entry are DB-only.
6. **Format** — `ast->formatWithPossiblyHidingSensitiveData(0, one_line=true, hilite=true, show_secrets=false, rule=WhenNecessary, style=Backticks)` produces the output SQL.

### Dynamic construction (non-SELECT) — the policy in one place

For every non-SELECT target, [handlers/name_rewrite.{h,cc}](src/handlers/name_rewrite.cc)'s `applyDynamicRewrite` resolves `(origin_db, origin_table)` + `general_args` into `(physical_db, new_table)` as follows:

```
logical   := origin_db (SQL qualifier) OR general_args.upstream_logical_database_in_context
physical  := general_args.database_map[logical]
             OR logical  (if logical ∈ general_args.known_physical_databases)
             OR reject with InvalidRewriteRequest
delim     := general_args.delim ("_" by default)
new_table := logical + delim + (extra_arguments joined by delim + delim, iff non-empty)
                     + origin_table
```

The "extras block is optional + its trailing delim is conditional" rule is what keeps an empty `extra_arguments` from producing a double delim (`tenant1__events`). `buildDynamicTablePrefix` (same file) is just `applyDynamicRewrite`'s construction with an empty `origin_table` — used by SHOW TABLES to filter / strip the prefix.

### AST transformers ([src/ASTTransformers.cc](src/ASTTransformers.cc), [src/ASTTransformers.h](src/ASTTransformers.h))

Each transformer subclasses `IASTTransformer` (itself an `IAST` so it can live in the tree during debugging). They mutate the AST in place:

- `ASTReplaceTransformer` — SELECT-tree table rewrites. Walks `ASTTableExpression` nodes and dispatches to `planTableRewrite()` for every `ASTTableIdentifier` it finds. Three map kinds in precedence order: `table_name_map` (plain rename) → `remote_table_map` (swap the identifier for a `remote('addr', db, table, user, password)` function call) → `table_with_database_map` (attach an explicit database). Every match gets recorded in `transformer.rewrites` so the SELECT handler can surface it on the response.
- `ASTRewriteCTETransformer` — injects named CTEs into `WITH` clauses of every `SELECT` subquery that actually references them. See `parseCTEMapToAST` (parses each CTE SQL once, tolerant of individual failures) and `collectCTEKeysForSelect`.

`planTableRewrite()` is the SELECT-path primitive. Given `(origin_name, three maps)` it returns a `TableRewritePlan { kind, db, table, remote }`. Only `ASTReplaceTransformer` consumes it; non-SELECT handlers use `applyDynamicRewrite` (see "Dynamic construction" above) instead — the two paths don't share logic by design.

`LimitRewrite` / `OffsetRewrite` / `SettingsRewrite` used to have their own transformer classes but are now applied inline in [handlers/select.cc](src/handlers/select.cc) — all three batch into a `PendingSelectTransforms` struct and flush in a single AST walk.

`collectTableNamesFromAST` also lives here and is what fills `response.original_accessed_table_names`.

### Protocol

[protos/rewriter.proto](protos/rewriter.proto) defines the contract. Key invariants worth preserving:

- `RewriteOp` + `RewriteOption.value` are a tagged-union by `op` — adding a new op means: new enum value, new `*Args` message, new `oneof` case, new `case` in the dispatch switch, and probably new response fields. Bump both C++ and Go clients.
- `TableNameRewrite` is the only table-touching op. Its `RewriteTableNameArgs` has two sub-messages:
  - `select_stmt_args` (`RewriteTableForSelectStmtArgs`) — used by the SELECT path: the three maps (`table_map`, `remote_table_map`, `table_with_database_map`) in lookup-precedence order.
  - `general_args` (`RewriteTableForDynamicArgs`) — used by every non-SELECT handler: `database_map` (logical → physical), `known_physical_databases` (logical that's already physical → passthrough), `upstream_logical_database_in_context` (default logical for unqualified SQL), `delim` (default `"_"`), `extra_arguments` (repeated, joined by `delim`).
- Response `statement_type` is a `StatementType` enum (`STATEMENT_TYPE_SELECT`, `_USE`, `_SHOW_TABLES`, `_SHOW_CREATE_TABLE`, `_EXISTS_TABLE`, `_CREATE_TABLE`, `_DROP_TABLE`, `_ALTER_TABLE`, `_RENAME_TABLE`, `_INSERT`, `_UPDATE`, `_DELETE`). `STATEMENT_TYPE_UNSPECIFIED` (= 0) is the default when classification didn't run (parse errors, etc.). Adding new values is fine; renumbering or reusing numbers is not.
- `RewriteCode` splits rejection taxonomy into four cases — callers should branch on this, not on `message` strings:
  - `Success` (0): accepted.
  - `SyntaxError` (1): ClickHouse's `ParserQuery` rejected the SQL text; `message` carries the parser's `displayText()`.
  - `RewriteError` (2): generic bucket kept for unexpected rewrite failures. Prefer the more specific codes below when classifying new reject paths.
  - `UnsupportedStatement` (3): the SQL is valid but the rewriter doesn't handle this statement kind / variant (CREATE VIEW, DROP DATABASE, TRUNCATE, OPTIMIZE, RENAME DATABASE, EXISTS VIEW/DATABASE/DICTIONARY, SHOW CREATE VIEW/DATABASE/DICTIONARY, INSERT INTO FUNCTION, multi-table DROP, ALTER cross-table partition refs, GRANT/KILL/BACKUP/...). Callers MAY pass the SQL through to ClickHouse unchanged.
  - `InvalidRewriteRequest` (4): the caller's `RewriteOption` can't be applied — their rewrite config is wrong for the SQL they sent. Callers MUST NOT pass through; surface the error. Non-SELECT cases: the logical DB resolves to no physical (not in `database_map`, not in `known_physical_databases`), or the target is unqualified and `upstream_logical_database_in_context` is empty.
- Top-level `response.table_rewrites: map<string, string>` is the canonical "what got rewritten" output. Keys/values are `"db.table"` (or bare `"db"` for USE, or bare `"table"` when no db was present). Populated by every statement kind that honors `TableNameRewrite` — including SELECT. Only entries whose origin differs from the rewritten form are recorded. For SELECT `remote_table_map` hits, the value is the remote-side `db.table`; the full `remote('addr', …)` call lives in `sql_after_rewrite`.
- `RewriteSQLResponse` is flat — no `extra_value` oneof, no `{use,show,write}_stmt_value` messages. Everything a caller needs for routing lives in `statement_type`, `code`, `message`, `sql_after_rewrite`, `original_accessed_table_names`, `table_rewrites`, and `failed_cte_aliases`.
- Syntactic / semantic rewrite failures return `Status::OK` with a non-Success `RewriteCode`; only unexpected server errors would return a gRPC error.

### Tests ([tests/rewriter_test.cc](tests/rewriter_test.cc))

GoogleTest v1.14 fetched via `FetchContent`. Tests link the full server implementation (`rewriter-server.cc` + `ASTTransformers.cc`) and call `RewriterServiceImpl::Rewrite` directly (`main()` is excluded by `REWRITER_RELEASE_MAIN=0` for the test target). Use `RewriterServiceImpl::RewriteForTest` if you need to go through the public-looking entrypoint.

## Conventions

- Comments and script output are often in Chinese; match the surrounding language when editing those files, otherwise English.
- Use ClickHouse's own AST types and `DB::` helpers — do not reach for a second SQL parser. Look under `clickHouse/src/Parsers/` for `ASTSelectQuery`, `ASTShowTablesQuery`, `ASTUseQuery`, `ASTCreateQuery`, etc.
- Formatting: output must go through `formatWithPossiblyHidingSensitiveData` with `IdentifierQuotingStyle::Backticks` and `IdentifierQuotingRule::WhenNecessary` to match what the downstream ClickHouse expects.
- When adding a new statement kind, also update: the dispatch in `Rewrite`, the write-kind rejection switch (so it doesn't fall through to SELECT), CMakeLists (if it's a new handler file), and a matching test.
- When a new statement kind honors `TableNameRewrite`, pick the right path: SELECT-like tree walks extend `ASTReplaceTransformer` and read `select_stmt_args`; single-target statements call `applyDynamicRewrite` from [handlers/name_rewrite.h](src/handlers/name_rewrite.h) and read `general_args`. Either way call `recordTableRewrite` so `response.table_rewrites` stays consistent.


<claude-mem-context>
# Memory Context

# [rewriter-grpc] recent context, 2026-04-24 6:25pm GMT+8

No previous sessions found.
</claude-mem-context>