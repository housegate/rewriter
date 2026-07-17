# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

C++23 gRPC service that rewrites ClickHouse SQL before it reaches a backend ClickHouse cluster. Links directly against the ClickHouse source tree (vendored as the `clickHouse/` git submodule) so it can parse queries into ClickHouse's own AST, mutate them via transformers, and format them back out.

Entry point: `src/rewriter-server.cc` — gRPC server + `main()` (guarded by `REWRITER_RELEASE_MAIN`). Invocation: `clickhousegate_rewriter [grpc_port] [http_port]`. Defaults: gRPC `50051`, HTTP `grpc_port + 1`.

The request-handling logic lives in protocol-agnostic `RewriterServiceImpl::doRewrite` and `doRewriteErrorMessage`. The gRPC `Rewrite` / `RewriteErrorMessage` methods are thin wrappers that delegate and return `Status::OK` — never add logic to the wrappers, always to the `do*` methods, so the HTTP façade stays in sync.

**HTTP+JSON façade**: [src/rewriter-http-server.h](src/rewriter-http-server.h) + [src/rewriter-http-server.cc](src/rewriter-http-server.cc) wrap the same `RewriterServiceImpl` instance behind a Poco::Net HTTPServer (Poco is already a transitive dep via `clickhouse_common_io`, no new linkage). Routes: `POST /rewrite`, `POST /rewriteerrormessage`, `POST /optimize`, `POST /materialize` (also accept gRPC-style `/rewriter.RewriterService/<Method>`), `GET /healthz`. Body is JSON matching the proto messages — converted via `google::protobuf::util::JsonStringToMessage` / `MessageToJsonString`. The HTTP server runs in its own Poco thread pool started from `RunServer`; the gRPC `Wait()` is what blocks main.

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

Prerequisite submodule init on a fresh clone (run from the repository root):
`git submodule update --init --recursive --depth=1`.

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
3. **Dispatch by statement kind** (handlers live under [src/handlers/](src/handlers/); each returns a disposition that either consumes the request or falls through to the next). The single `TableNameRewrite` option carries two sub-messages: `static_args` (the three-map policy: `table_map` / `remote_table_map` / `table_with_database_map`) and `dynamic_args` (dynamic construction). Every table-targeting handler — SELECT plus the non-SELECT single-target handlers (writes / EXISTS / SHOW CREATE / RENAME) — picks mode via the **unified static→dynamic precedence**: if `static_args` is set, use static-mode (even when every map is empty — that's an explicit "static-only, no rewrite" intent); otherwise fall back to `dynamic_args`. Once static-mode is selected, unmatched targets pass through unchanged (statement-level — no per-target fallback to dynamic). Database-level handlers (USE / SHOW TABLES / SHOW DATABASES / CREATE DATABASE) bypass this precedence and read `dynamic_args` directly via `findDynamicArgs`, since `static_args` is table-keyed and has no semantic meaning for a db-level operation.
   - `ASTUseQuery` → [handlers/use.cc](src/handlers/use.cc). Consumes `dynamic_args.database_map` + `dynamic_args.known_physical_databases`. `database_map` hit → `USE <physical>` + `table_rewrites` entry. Target is already a known physical DB → passthrough. Neither → `InvalidRewriteRequest` ("user does not have this database"). No option → passthrough.
   - `ASTShowTablesQuery` → [handlers/show_tables.cc](src/handlers/show_tables.cc). Consumes `dynamic_args`. Resolves `logical := FROM-clause || upstream_logical_database_in_context`, `physical := database_map[logical]` (or `logical` itself if known physical). Emits a filtered `SELECT … FROM system.tables WHERE database = '<physical>' AND startsWith(name, '<prefix>')` with `multiIf` stripping the prefix; `prefix = logical + delim + (extra_args joined by delim + delim, iff non-empty)`. When `logical` is in `dynamic_args.logical_database_to_remote_upstream_index` the `FROM system.tables` source is swapped for `FROM remote('<addr>', system, tables, '<user>', '<password>')` so the enumeration runs against the upstream cluster (filter + prefix-strip unchanged); a logical pointing at a missing `remote_upstreams` key rejects with `InvalidRewriteRequest`. Rejects with `InvalidRewriteRequest` when logical can't be resolved. No option → passthrough.
   - `ASTShowTablesQuery` with `databases == true` (= `SHOW DATABASES`) → [handlers/show_databases.cc](src/handlers/show_databases.cc). Consumes `dynamic_args`. With no `dynamic_args`: passthrough. With `dynamic_args`: emits `SELECT name FROM (<union>) [WHERE name [NOT] (I)LIKE '<pat>'] ORDER BY name`. **Only logical DBs from `database_map` are surfaced** — `known_physical_databases` is consulted as a trust anchor but its members are never emitted as result rows (multi-tenant isolation: physical DB names are an internal detail). Per-`database_map` entry, sorted alphabetically by `<logical>`: (1) skip the entry entirely when `<physical>` is not a member of `known_physical_databases` (config-drift guard — no subquery, no `database_rewrites` record); (2) otherwise emit a bare `SELECT '<logical>' AS name`. We don't existence-check tables anymore: a declared+trusted logical DB is always returned (an empty logical DB is still a valid DB the caller can write into, and remote-mapped logicals would have failed a local existence check anyway). When no entries survive the filters, the body is the zero-row sentinel `SELECT '' AS name WHERE 0`. `extra_arguments` and `delim` are ignored here — they only matter for the table-prefix construction used by SHOW TABLES. Honors `SHOW DATABASES LIKE / NOT LIKE / ILIKE`. Records the kept `database_map` entries in `response.database_rewrites`. `statement_type = STATEMENT_TYPE_SHOW_DATABASES`.
   - `ASTExistsTableQuery` → [handlers/exists.cc](src/handlers/exists.cc). Resolves the active mode via `findActiveTableRewrite` (unified static→dynamic precedence). Static-mode: three-map lookup; unmatched → passthrough; `remote_table_map` hit → `UnsupportedStatement`. Dynamic-mode: `applyDynamicRewrite` on the target. `statement_type = STATEMENT_TYPE_EXISTS_TABLE`. `EXISTS DATABASE / VIEW / DICTIONARY` return `UnsupportedStatement`.
   - `ASTShowCreateTableQuery` → [handlers/show_create.cc](src/handlers/show_create.cc). Parallel to EXISTS; `statement_type = STATEMENT_TYPE_SHOW_CREATE_TABLE`. `SHOW CREATE DATABASE / VIEW / DICTIONARY` return `UnsupportedStatement`.
   - `ASTGrantQuery` → [handlers/grant.cc](src/handlers/grant.cc). Dispatched **before** writes (so it never falls into the write-kind switch). Symmetric to CREATE/DROP DATABASE: the rewriter never executes the GRANT/REVOKE on ClickHouse — a logical DB shares its physical DB with sibling tenants via prefix-sharing, so a CH-level grant scoped to the physical DB would leak across tenants. With `dynamic_args` present, walks `access_rights_elements`, resolves logical → physical via `resolvePhysicalDatabase`, and emits one `PrivilegeDelta` per element into `response.privileges_deltas` (action = GRANT/REVOKE; scope = SCOPE_TABLE for `db.t`, SCOPE_DATABASE for `db.*`; privilege keywords from `AccessFlags::toKeywords()` preserved verbatim; grantees translated from the `ASTRolesOrUsersSet` with `is_current_user` set for the CURRENT_USER literal and names left otherwise un-mapped). Then drops `cluster` from the AST and rewrites the SQL to `SELECT '<original SQL minus ON CLUSTER>' AS gstmt` (or `... AS rstmt` for REVOKE) so the user's intent ends up in CH `query_log`. `statement_type = STATEMENT_TYPE_GRANT | _REVOKE`. Unsupported variants (`ON *.*`, role-membership grants — `GRANT role TO user` — detected via non-null `ASTGrantQuery::roles`, column-level grants `GRANT SELECT(c1) ON …`, `WITH REPLACE OPTION`, `REVOKE FROM ALL`/`ALL EXCEPT`, `GRANT CURRENT GRANTS`, `ATTACH GRANT`) return `UnsupportedStatement`. No `dynamic_args` → also `UnsupportedStatement` (mirrors CREATE/DROP DATABASE; we won't ship raw GRANT to the physical DB without policy context). Logical not in `database_map`/`known_physical_databases`, or bare table without `upstream_logical_database_in_context`, → `InvalidRewriteRequest`. Does NOT populate `original_accessed_tables` — permission management isn't a data access; `privileges_deltas` carries everything.
   - `ASTCreateQuery / ASTDropQuery / ASTAlterQuery / ASTInsertQuery / ASTUpdateQuery / ASTDeleteQuery / ASTRenameQuery` → [handlers/writes.cc](src/handlers/writes.cc). Each target goes through the unified `rewriteOneTarget`: in static-mode it's a three-map lookup (unmatched → passthrough; `remote_table_map` hit → `UnsupportedStatement` since writes can't target a `remote()` function); in dynamic-mode it's `applyDynamicRewrite`. `statement_type = STATEMENT_TYPE_CREATE_TABLE | _DROP_TABLE | _TRUNCATE_TABLE | _ALTER_TABLE | _INSERT | _UPDATE | _DELETE | _RENAME_TABLE`. TRUNCATE TABLE and DROP TABLE share the unified single-target path (same `ASTDropQuery`, just `kind == Truncate`); only the response `statement_type` distinguishes them. For CREATE TABLE AS, the source table is rewritten the same way; for RENAME, every `from` and `to` element. INSERT has an extra wrinkle: `ASTInsertQuery::formatImpl` only emits the prelude (`INSERT INTO ... [columns] [SETTINGS ...] [VALUES|FORMAT <fmt>]`) — the inline payload after `VALUES` / `FORMAT <fmt>` lives off-AST as raw `data`/`end` pointers into the original SQL buffer, since ClickHouse normally streams those bytes to the executor out-of-band. The handler appends `[data, end)` verbatim to the formatted SQL so a `VALUES (1)` request round-trips as `... VALUES (1)` instead of a bare `... FORMAT Values` (which would put the upstream into "wait for wire-format data" mode and hang the client). `CREATE VIEW` / `CREATE MATERIALIZED VIEW` → `handleViewCreate` in [handlers/writes.cc](src/handlers/writes.cc): the view name and (for a materialized view) the `TO` target go through the unified single-target `rewriteOneTarget`; the body `SELECT` goes through `rewriteEmbeddedViewBody` ([handlers/select.cc](src/handlers/select.cc)) — full option pipeline plus the cross-shard GLOBAL pass. `statement_type = STATEMENT_TYPE_CREATE_VIEW | _CREATE_MATERIALIZED_VIEW`. `CREATE LIVE VIEW` / `WINDOW VIEW` / `CREATE DICTIONARY` still return `UnsupportedStatement`. `DROP VIEW` flows through the single-target `DROP TABLE` path with `statement_type = STATEMENT_TYPE_DROP_VIEW`; `TRUNCATE VIEW` / `DROP DICTIONARY` stay rejected. Unsupported variants (CREATE LIVE/WINDOW VIEW, CREATE DICTIONARY, TRUNCATE VIEW, DROP DICTIONARY, multi-table DROP/TRUNCATE, RENAME DATABASE/DICTIONARY, TRUNCATE DATABASE / TRUNCATE ALL TABLES FROM, DETACH, UNDROP, OPTIMIZE, KILL, BACKUP/RESTORE, INSERT INTO FUNCTION, `ALTER TABLE ... ATTACH/REPLACE/FETCH PARTITION FROM other`, `ALTER TABLE ... MOVE PARTITION TO TABLE other`, …) return `UnsupportedStatement`. Dynamic-mode unqualified target + empty `upstream_logical_database_in_context`, or logical that resolves to no physical DB → `InvalidRewriteRequest`. No option → passthrough.
   - `ASTCreateQuery` with no table (`CREATE DATABASE <db>`) → still in [handlers/writes.cc](src/handlers/writes.cc) but follows a special path: the rewriter never emits the DDL against ClickHouse. With `dynamic_args` present it (1) rejects `InvalidRewriteRequest` if `<db>` is already in `database_map` and the SQL is not `IF NOT EXISTS`, (2) rejects if `dynamic_args.upstream_physical_database_in_context` is set but not in `known_physical_databases`, (3) otherwise rewrites to `SELECT '<original SQL>' AS cdstmt` so the user's intent ends up in ClickHouse `query_log`. `statement_type = STATEMENT_TYPE_CREATE_DATABASE`. Without `dynamic_args`, falls back to `UnsupportedStatement` (we won't ship raw CREATE DATABASE to the physical DB).
   - `ASTDropQuery` with no table (`DROP DATABASE <db>`) → still in [handlers/writes.cc](src/handlers/writes.cc), symmetric to CREATE DATABASE: never emits the DDL against ClickHouse (a logical DB shares its physical DB with sibling tenants via prefix-sharing, so a single CH-level `DROP DATABASE` would nuke unrelated tenants). With `dynamic_args` present it (1) rejects `InvalidRewriteRequest` if `<db>` is not in `database_map` and the SQL is not `IF EXISTS` (`known_physical_databases` is deliberately not consulted — physical-DB lifecycle is out of scope), (2) on a `database_map` hit records `database_rewrites[<db>] = <physical>` so an external GC binary knows where to scan for prefixed tables, (3) rewrites to `SELECT '<original SQL>' AS ddstmt` so the intent lands in `query_log` for the GC binary to tail. `statement_type = STATEMENT_TYPE_DROP_DATABASE`. Without `dynamic_args`, falls back to `UnsupportedStatement`. Actual physical-side reclamation is the GC binary's responsibility — the rewriter is fire-and-forget here.
   - Everything else (`SELECT`, other DML) → falls through to [handlers/select.cc](src/handlers/select.cc).
4. **SELECT path** — collect `original_accessed_tables` (one `AccessedTable` per `ASTTableExpression`, carrying `original_database` / `original_table` plus the rewriter's resolved `logical_database` / `physical_database` / `is_remote`) via `collectAccessedTablePairsFromAST` in [handlers/select.cc](src/handlers/select.cc) and the per-pair `resolveAccessedTable` in [handlers/name_rewrite.cc](src/handlers/name_rewrite.cc). Both `collectAccessedTablePairsFromAST` and the dynamic-mode rewrite walker carry a CTE-alias scope set: every `ASTSelectQuery` they enter contributes its `WITH …` element names to a forked-per-subtree set, and any **bare** `ASTTableIdentifier` whose name matches an in-scope alias is skipped entirely — it's a CTE reference, not a real table, so it neither lands in `original_accessed_tables` nor gets rewritten as `<logical>.<alias>`. Qualified `db.alias` references are still treated as real tables (the qualifier disambiguates). Externally-supplied CTE bodies (from a `CommonTableExprRewrite` option) additionally erase the alias from `originals` and re-walk the body so the real tables it accesses inline. Then iterate `request.options()` and apply transformers in option order: Limit → TableName → Offset → Settings → CTE. The TableName step follows the unified static→dynamic precedence: `static_args` set → `ASTReplaceTransformer` (three-map walk; an empty static_args means every identifier is unmatched and passes through). Otherwise falls back to walking the AST and applying `applyDynamicRewrite` from `dynamic_args` to each `ASTTableExpression` (lenient — identifiers that can't be resolved are left alone, since SELECT may legitimately reference system tables; inline CTE aliases are filtered out by the scope set above before resolution is even attempted).
5. **Populate `table_rewrites` / `database_rewrites`** — every rewrite that fired is recorded on the response. Table-level rewrites (SELECT, writes, EXISTS, SHOW CREATE, RENAME) go into `table_rewrites` keyed by `"origin_db.origin_table"` → `"rewritten_db.rewritten_table"`. DB-level rewrites (USE, SHOW TABLES, DROP DATABASE on a `database_map` hit) go into `database_rewrites` keyed by the original logical DB → physical DB. No-op rewrites (origin == rewritten) are omitted from both maps.
6. **Cross-shard correctness pass (SELECT only)** — `forceGlobalForRemoteAsymmetry` in [handlers/select.cc](src/handlers/select.cc) walks the post-rewrite AST bottom-up. For each `ASTSelectQuery`: (a) scan `tables()` left→right and, for any `ASTTableJoin` where the accumulated-left side has a remote source (`remote` / `remoteSecure` / `cluster` / `clusterAllReplicas` table function) and the current element has a real-table local source, or vice-versa, bump `locality` from `Unspecified` to `Global`; (b) if the FROM list contains any remote source, walk the rest of the SELECT body (skipping `tables()` and `with()` and not descending into nested `ASTSubquery` — those have already been processed bottom-up) for `ASTFunction` calls named `in` / `notIn` / `nullIn` / `notNullIn` whose right operand has a local source, and rename them to `globalIn` / `globalNotIn` / etc. Symmetric remote-vs-remote and local-vs-local joins are intentionally left untouched (per request) — GLOBAL costs an extra broadcast and the symmetric cases work without it. User-written `GLOBAL` / `LOCAL` are preserved (we only override `Unspecified`). Bare CTE-alias references (in scope) don't count as local sources, so a JOIN against a CTE alias whose body references a local table is a current limitation — callers who hit this can write `GLOBAL` explicitly. The pass runs *after* option application so static-mode `remote_table_map` and dynamic-mode `logical_database_to_remote_upstream_index` rewrites that *introduce* remote() calls are visible. Non-SELECT statements that contain SELECT subqueries (e.g., `INSERT INTO t SELECT …`) currently bypass this pass. CREATE VIEW / MATERIALIZED VIEW bodies are the exception: they run the pass via `rewriteEmbeddedViewBody`.
7. **Format** — `ast->formatWithPossiblyHidingSensitiveData(0, one_line=true, hilite=true, show_secrets=false, rule=WhenNecessary, style=Backticks)` produces the output SQL.

### Dynamic construction — the policy in one place

The shared dynamic-mode logic lives in [handlers/name_rewrite.{h,cc}](src/handlers/name_rewrite.cc)'s `applyDynamicRewrite` — invoked by every non-SELECT single-target handler in dynamic-mode, and by SELECT's per-`ASTTableExpression` walker when SELECT falls back to dynamic. It resolves `(origin_db, origin_table)` + `dynamic_args` into `(physical_db, new_table)` as follows:

```
logical   := origin_db (SQL qualifier) OR dynamic_args.upstream_logical_database_in_context
physical  := dynamic_args.database_map[logical]
             OR logical  (if logical ∈ dynamic_args.known_physical_databases)
             OR reject with InvalidRewriteRequest
delim     := dynamic_args.delim ("_" by default)
new_table := logical + (delim + extra_arguments joined by delim, iff extras non-empty)
                     + "." + origin_table
```

The trailing separator between the prefix and `origin_table` is **always** a literal `.` — it is NOT the configurable `delim`. The CH formatter wraps the resulting table name in backticks via `IdentifierQuotingRule::WhenNecessary` because it contains a dot, so callers see SQL like `` physical_db_1.`tenant1.events` ``. Examples (`delim="_"`):

| extras | original | result |
|---|---|---|
| `[]` | `tenant1.events` | `` physical_db_1.`tenant1.events` `` |
| `[]` | `events` (USE tenant1) | `` physical_db_1.`tenant1.events` `` |
| `["shard0"]` | `tenant1.events` | `` physical_db_1.`tenant1_shard0.events` `` |

The "extras block is optional" rule keeps an empty `extra_arguments` from producing a stray leading delim before the dot. `buildDynamicTablePrefix` (same file) is just `applyDynamicRewrite`'s construction with an empty `origin_table`, i.e. `logical + (delim + extras.join(delim) iff non-empty) + "."` — used by SHOW TABLES to filter / strip the prefix.

### AST transformers ([src/ASTTransformers.cc](src/ASTTransformers.cc), [src/ASTTransformers.h](src/ASTTransformers.h))

Each transformer subclasses `IASTTransformer` (itself an `IAST` so it can live in the tree during debugging). They mutate the AST in place:

- `ASTReplaceTransformer` — SELECT-tree table rewrites. Walks `ASTTableExpression` nodes and dispatches to `planTableRewrite()` for every `ASTTableIdentifier` it finds. Three map kinds in precedence order: `table_name_map` (plain rename) → `remote_table_map` (swap the identifier for a `remote('addr', db, table, user, password)` function call) → `table_with_database_map` (attach an explicit database). Every match gets recorded in `transformer.rewrites` so the SELECT handler can surface it on the response.
- `ASTRewriteCTETransformer` — injects named CTEs into `WITH` clauses of every `SELECT` subquery that actually references them. See `parseCTEMapToAST` (parses each CTE SQL once, tolerant of individual failures) and `collectCTEKeysForSelect`.

`planTableRewrite()` is the SELECT-path primitive. Given `(origin_name, three maps)` it returns a `TableRewritePlan { kind, db, table, remote }`. Only `ASTReplaceTransformer` consumes it; non-SELECT handlers use `applyDynamicRewrite` (see "Dynamic construction" above) instead — the two paths don't share logic by design.

`LimitRewrite` / `OffsetRewrite` / `SettingsRewrite` used to have their own transformer classes but are now applied inline in [handlers/select.cc](src/handlers/select.cc) — all three batch into a `PendingSelectTransforms` struct and flush in a single AST walk.

`response.original_accessed_tables` is populated by every table-targeting handler:

- **SELECT** uses `collectAccessedTablePairsFromAST` (private to [handlers/select.cc](src/handlers/select.cc)) to walk pre-rewrite ASTs (main SQL + each CTE body), skipping inline CTE aliases via the scope set described in the SELECT-path step above and erasing externally-supplied CTE alias entries before re-walking their bodies. Each remaining pair runs through `resolveAccessedTable` ([handlers/name_rewrite.cc](src/handlers/name_rewrite.cc)) to fill in `logical_database` / `physical_database` / `is_remote`.
- **Non-SELECT table-targeting handlers** (writes / EXISTS / SHOW CREATE) call `recordAccessedTable` (also in [handlers/name_rewrite.cc](src/handlers/name_rewrite.cc)) once per `(db, table)` target processed by `rewriteOneTarget` (in writes.cc) or its inline equivalents in exists.cc / show_create.cc. RENAME records both `from` and `to`; `CREATE TABLE x AS source` records target plus source.
- **CREATE DATABASE / DROP DATABASE** call `recordAccessedDatabase` ([handlers/name_rewrite.cc](src/handlers/name_rewrite.cc)) — emits one db-level entry with `original_table` empty, `logical_database` = target db, and `physical_database` resolved via `database_map` when possible (typical for DROP, typically empty for CREATE). Callers branch on `original_table.empty()` to tell db-level entries apart from table-level ones.
- USE / SHOW TABLES / SHOW DATABASES still do NOT populate the field — they're pure DB rewrites surfaced via `database_rewrites`.
- The recording happens before any reject path, so a partial list may appear on rejected (non-Success) responses.

### Protocol

The pinned
[`rewriter-proto`](third_party/rewriter-proto/proto/rewriter.proto) submodule
defines the contract. Key invariants worth preserving:

- `RewriteOp` + `RewriteOption.value` are a tagged-union by `op` — adding a new op means: new enum value, new `*Args` message, new `oneof` case, new `case` in the dispatch switch, and probably new response fields. Bump both C++ and Go clients.
- `TableNameRewrite` is the only table-touching op. Its `RewriteTableNameArgs` has two sub-messages:
  - `static_args` (`RewriteTableStaticArgs`) — used by the SELECT path: the three maps (`table_map`, `remote_table_map`, `table_with_database_map`) in lookup-precedence order.
  - `dynamic_args` (`RewriteTableDynamicArgs`) — used by every non-SELECT handler: `database_map` (logical → physical), `known_physical_databases` (logical that's already physical → passthrough), `upstream_logical_database_in_context` (default logical for unqualified SQL), `delim` (default `"_"`), `extra_arguments` (repeated, joined by `delim`).
- Response `statement_type` is a `StatementType` enum (`STATEMENT_TYPE_SELECT`, `_USE`, `_SHOW_TABLES`, `_SHOW_CREATE_TABLE`, `_EXISTS_TABLE`, `_CREATE_TABLE`, `_DROP_TABLE`, `_ALTER_TABLE`, `_RENAME_TABLE`, `_INSERT`, `_UPDATE`, `_DELETE`, `_CREATE_DATABASE`, `_SHOW_DATABASES`, `_DROP_DATABASE`, `_TRUNCATE_TABLE`, `_GRANT`, `_REVOKE`, `_CREATE_VIEW`, `_CREATE_MATERIALIZED_VIEW`, `_DROP_VIEW`). `STATEMENT_TYPE_UNSPECIFIED` (= 0) is the default when classification didn't run (parse errors, etc.). Adding new values is fine; renumbering or reusing numbers is not.
- `RewriteCode` splits rejection taxonomy into four cases — callers should branch on this, not on `message` strings:
  - `Success` (0): accepted.
  - `SyntaxError` (1): ClickHouse's `ParserQuery` rejected the SQL text; `message` carries the parser's `displayText()`.
  - `RewriteError` (2): generic bucket kept for unexpected rewrite failures. Prefer the more specific codes below when classifying new reject paths.
  - `UnsupportedStatement` (3): the SQL is valid but the rewriter doesn't handle this statement kind / variant (CREATE LIVE/WINDOW VIEW, CREATE DICTIONARY, TRUNCATE VIEW, DROP DICTIONARY, TRUNCATE DATABASE, OPTIMIZE, RENAME DATABASE, EXISTS VIEW/DATABASE/DICTIONARY, SHOW CREATE VIEW/DATABASE/DICTIONARY, INSERT INTO FUNCTION, multi-table DROP/TRUNCATE, ALTER cross-table partition refs, KILL/BACKUP/...). Also: CREATE/DROP DATABASE without a `dynamic_args` option (we won't ship the raw DDL to the physical DB without policy context). Also: GRANT/REVOKE variants the privilege-delta path doesn't model (`ON *.*`, role-membership grants, column-level grants, `WITH REPLACE OPTION`, `REVOKE FROM ALL`/`ALL EXCEPT`, `GRANT CURRENT GRANTS`, `ATTACH GRANT`, or any GRANT/REVOKE without `dynamic_args`). Callers MAY pass the SQL through to ClickHouse unchanged.
  - `InvalidRewriteRequest` (4): the caller's `RewriteOption` can't be applied — their rewrite config is wrong for the SQL they sent. Callers MUST NOT pass through; surface the error. Non-SELECT cases: the logical DB resolves to no physical (not in `database_map`, not in `known_physical_databases`), or the target is unqualified and `upstream_logical_database_in_context` is empty.
- Top-level `response.table_rewrites: map<string, string>` records TABLE rewrites only — keys/values formatted as `"db.table"` (or bare `"table"` when no db was present). Populated by SELECT, writes, EXISTS, SHOW CREATE, RENAME. For SELECT `remote_table_map` hits, the value is the remote-side `db.table`; the full `remote('addr', …)` call lives in `sql_after_rewrite`.
- Top-level `response.database_rewrites: map<string, string>` records DB-level rewrites only — keys are the original logical DB, values are the resolved physical DB. Populated by USE and SHOW TABLES (the two statement kinds whose rewrite is purely at the database level). Kept separate from `table_rewrites` so callers can branch on "did the DB change?" vs "did a table change?" without parsing key formats.
- Both maps omit no-op entries (origin == rewritten).
- `RewriteSQLResponse` is flat — no `extra_value` oneof, no `{use,show,write}_stmt_value` messages. Everything a caller needs for routing lives in `statement_type`, `code`, `message`, `sql_after_rewrite`, `original_accessed_tables` (structured `AccessedTable` records: `original_database` / `original_table` / `logical_database` / `physical_database` / `is_remote`), `table_rewrites`, `database_rewrites`, `failed_cte_aliases`, `privileges_deltas` (structured `PrivilegeDelta` records emitted only by GRANT/REVOKE — action / scope / original+logical+physical db / original+physical table / privilege keyword list / grantees / `grant_option`), and `existence_clause` (an `ExistenceClause` enum — `IF_NOT_EXISTS` for a CREATE that carried `IF NOT EXISTS`, `IF_EXISTS` for a DROP/TRUNCATE that carried `IF EXISTS`, `UNSPECIFIED` otherwise; set from the parsed AST before dispatch, so accurate even on a rejected response — only `SyntaxError` leaves it `UNSPECIFIED`). Field tag 4 (the legacy flat `original_accessed_table_names`) is `reserved`.
- Syntactic / semantic rewrite failures return `Status::OK` with a non-Success `RewriteCode`; only unexpected server errors would return a gRPC error.

### `Optimize` RPC

A second top-level RPC, separate from `Rewrite`. Lives in `RewriterServiceImpl::doOptimize` / `Optimize` in [src/rewriter-server.cc](src/rewriter-server.cc) and is routed by [src/rewriter-http-server.cc](src/rewriter-http-server.cc) at `POST /optimize` (also accepts `/rewriter.RewriterService/Optimize`).

- **What it does**: currently, JOIN left/right swap based on caller-supplied physical-table sizes, applied only to SELECT queries. Skip rules match ClickHouse's own `optimizeJoin.cpp` policy (ASOF, PASTE, USING, SEMI/ANTI strictness are left alone — see [src/handlers/join_swap.h](src/handlers/join_swap.h)).
- **Why separate from `Rewrite`**:
  1. *Table-size key disambiguation* — by the time `Optimize` is called, the SQL has already been through `Rewrite`, so every table identifier in the SQL is a physical one. `OptimizeRequest.table_sizes` keys are unambiguously `<physical_db>.<physical_table>` (or bare `<physical_table>` when the SQL is unqualified). Callers populate them by reading `system.tables` / `system.parts` after `Rewrite`, using the response's `original_accessed_tables` field to discover which physical tables to look up. (Inside `Rewrite`, callers could not tell whether a size key was supposed to be logical or physical — the request hadn't been resolved yet.)
  2. *Rewrite ≠ optimize* — one fixes semantics (logical → physical, multi-tenant prefix translation, etc.), the other tunes performance. Different lifecycle, different failure modes; keeping them in one RPC was muddling the contract.
  3. *Best-effort optimization with clean fallback* — `Optimize` is fire-and-forget: any failure path (parse error, no JOIN, ASOF/PASTE/etc. skip rule) returns `sql_after_optimize == request.sql` with a non-OK `OptimizeCode`. Callers can always run `sql_after_optimize` regardless of code; the code is for observability, not for branching fallback.
- **Failure semantics**:
  - `OptimizeOK` (0): a JOIN was swapped. `swap_applied = true`.
  - `OptimizeSyntaxError` (1): `ParserQuery` rejected the SQL. `sql_after_optimize` echoes the input.
  - `OptimizeUnoptimizable` (2): SQL parsed but had nothing to swap (single table, non-SELECT, JOIN that hits a skip rule, sizes don't justify a swap, etc.). Not an error — callers proceed with the original SQL.
- **`forceGlobalForRemoteAsymmetry`**: `Optimize` does NOT re-run this pass. The pass already ran inside `Rewrite` and JOIN swap doesn't introduce new remote/local asymmetries (it only reorders existing ones), so the GLOBAL markers placed by `Rewrite` remain correct.
- **Why a separate RPC rather than `query_plan_join_swap_table` on ClickHouse**: CH's own optimizer's `estimateReadRowsCount` recognises only `ReadFromMergeTree` / `ReadFromMemoryStorageStep` — anything else (notably `ReadFromRemote` / `ReadFromCluster`) returns `nullopt`, and the auto-mode decision requires both sides to estimate. For SQL that involves `remote()` / `cluster()` table functions, CH's swap is a no-op. The proxy layer that calls this rewriter has out-of-band knowledge of remote-shard sizes that CH cannot see locally, so this RPC is the only place that swap decision can be made for our workload.

### `MaterializeSQL` RPC

A third top-level RPC, separate from `Rewrite`. The **Phase-1 signing normalizer** — it runs at the trusted agent/SDK *before* a `StatementEnvelopeV2` is signed (vs `Rewrite`, the runtime physical-name rewrite that runs after ingress). It rewrites non-deterministic ClickHouse functions in the SQL text to deterministic literal expressions, so the signed SQL hashes reproducibly and every replay executor computes the same result. Wire-compatible with the Go service ([housegate/rewriter-go#17](https://github.com/housegate/rewriter-go/pull/17)) so either service can be the materializer. Lives in `RewriterServiceImpl::doMaterializeSQL` / `MaterializeSQL` in [src/rewriter-server.cc](src/rewriter-server.cc) delegating to `rewriter_handlers::materialize` in [src/handlers/materialize.cc](src/handlers/materialize.cc); routed by [src/rewriter-http-server.cc](src/rewriter-http-server.cc) at `POST /materialize` (also `/rewriter.RewriterService/MaterializeSQL`).

- **Reads no clock / no RNG.** Every value comes from `MaterializeSQLRequest.inputs` (`MaterializationInputs`): `now_unix_ns` (optional int64), `random_uint64_values`, `random_float64_values`, `uuid_values` (all repeated, consumed positionally in left-to-right traversal order). The rewriter is a pure function of `(sql, inputs, policy)`.
- **Policy** (`MaterializationPolicy`): `profile_id` empty or `"sentio-p1-nondet-v1"` (the default) only; `timezone` empty or `"UTC"` (case-insensitive) only — else `MaterializeInvalidRequest`. The resolved profile is echoed in `materializer_profile_id` (default even on a policy reject, mirroring Go).
- **Mechanism** ([handlers/materialize.cc](src/handlers/materialize.cc)): parse with `ParserQuery`; a `Materializer` walks the AST pre-order and replaces each `ASTFunction` whose lowercased name is in the Go-mirrored `isKnownNondeterministic` set with a constructed literal/`toX(...)` AST node, recording a `MaterializedReplacement` (function_name / 0-based ordinal / literal_sql = `formatAst(node)` / value_type). `INSERT … VALUES` is special: ClickHouse leaves the VALUES payload off-AST as raw bytes (`insert->format == "Values"`), so `materializeValuesData` tokenizes it with `DB::Lexer` to split top-level `(...)` rows, parses each row's columns with `ParserExpressionList`, runs the **same** `Materializer` (cursors/ordinals continue), and re-emits — output is `formatAst(prelude) + tail` (`INSERT INTO t FORMAT Values(...)`, ClickHouse's own no-space round-trip form).
- **Supported functions** → literal: now/current_timestamp()/localtimestamp/localtime/nowInBlock → `toDateTime(s)`; utctimestamp/utc_timestamp → `toDateTime(s,'UTC')`; now64/nowInBlock64 → `fromUnixTimestamp64Milli(ms)`; today/current_date/curdate → `toDate(toDateTime(s))`; yesterday → `toDate(toDateTime(s-86400))`; rand/rand32/randConstant → `<n>` (UInt32, overflow-checked); rand64/random → `<n>` (UInt64); randCanonical → `toFloat64(x)` (finite, [0,1)); generateUUIDv4/**generateUUIDv7** → `toUUID('<lowercased>')`. `s = now_unix_ns/1e9`, `ms = now_unix_ns/1e6` (integer). The C++ extensions beyond Go's set — `generateUUIDv7`, `nowInBlock`, `nowInBlock64`, `randConstant` — reuse existing input channels (no proto change); Go rejects `generateUUIDv7`, so the two materializers diverge on what they *accept* (acceptable: materialization runs once at signing).
- **ClickHouse divergence from Go/polyglot**: bare SQL-standard keywords without parens (`CURRENT_TIMESTAMP`, `CURRENT_DATE`, `localtime`, `localtimestamp`) parse as **identifiers** in ClickHouse and error as `UNKNOWN_IDENTIFIER` at execution — they are *not* volatile, so the materializer correctly leaves them untouched. Only the parens forms (`current_timestamp()`, an alias of `now`) are `ASTFunction`s and get materialized.
- **Fail closed** (`MaterializeCode`): `MaterializeSuccess` (0); `MaterializeSyntaxError` (1, `ParserQuery` rejected); `MaterializeUnsupportedStatement` (2, a known volatile not in the supported set — e.g. `generateSnowflakeID`, `randNormal` — or a supported function carrying arguments like `now('UTC')`); `MaterializeInvalidRequest` (3, bad policy, or a supported function whose input channel is missing/exhausted/invalid — UInt32 overflow, float ∉ [0,1), malformed UUID); `MaterializeError` (4, internal/format failure, or an unparseable VALUES data section). On **every** non-Success path — and when zero replacements were applied — `sql_after_materialization == sql` (the original, echoed verbatim, no re-canonicalization).
- **Out of scope** (matching Go#17): `_hg_row_id` payload injection (operates on payload rows, not SQL text); distribution samplers / string generators (would need new input channels).

### Tests ([tests/rewriter_test.cc](tests/rewriter_test.cc))

GoogleTest v1.14 fetched via `FetchContent`. Tests link the full server implementation (`rewriter-server.cc` + `ASTTransformers.cc`) and call `RewriterServiceImpl::Rewrite` directly (`main()` is excluded by `REWRITER_RELEASE_MAIN=0` for the test target). Use `RewriterServiceImpl::RewriteForTest` if you need to go through the public-looking entrypoint.

## Development workflow

- **Branch-mode development.** Never commit feature work directly to `main`.
  Cut a dedicated branch first — `feat/<topic>` for features, `fix/<topic>`
  for fixes — and land the work via PR.
- **Commit design docs.** Brainstorming / design specs live under
  `docs/superpowers/specs/` and are committed to git alongside the feature
  branch they belong to, so the design is reviewable next to the code.

## Conventions

- Comments and script output are often in Chinese; match the surrounding language when editing those files, otherwise English.
- Use ClickHouse's own AST types and `DB::` helpers — do not reach for a second SQL parser. Look under `clickHouse/src/Parsers/` for `ASTSelectQuery`, `ASTShowTablesQuery`, `ASTUseQuery`, `ASTCreateQuery`, etc.
- Formatting: output must go through `formatWithPossiblyHidingSensitiveData` with `IdentifierQuotingStyle::Backticks` and `IdentifierQuotingRule::WhenNecessary` to match what the downstream ClickHouse expects.
- When adding a new statement kind, also update: the dispatch in `Rewrite`, the write-kind rejection switch (so it doesn't fall through to SELECT), CMakeLists (if it's a new handler file), and a matching test.
- When a new statement kind honors `TableNameRewrite`, pick the right path: SELECT-like tree walks extend `ASTReplaceTransformer` and read `static_args`; single-target statements call `applyDynamicRewrite` from [handlers/name_rewrite.h](src/handlers/name_rewrite.h) and read `dynamic_args`. Either way call `recordTableRewrite` so `response.table_rewrites` stays consistent.
