# SQL Rewriter

A C++ service that rewrites ClickHouse SQL before it hits a backend ClickHouse
cluster. Runs as a gRPC server, an HTTP/JSON server on the same process,
and ships with a standalone CLI for one-off rewrites.

Covers `SELECT` (with CTE injection, LIMIT/OFFSET/SETTINGS tweaks, table-name
rewrites — including `remote()` substitution), `USE`, `SHOW TABLES`,
`SHOW CREATE TABLE`, `EXISTS TABLE`, `CREATE/DROP/ALTER TABLE`, `INSERT`,
`UPDATE`, `DELETE`, and `RENAME TABLE`. The rewrites that fired are reported
on two top-level response maps: `table_rewrites` (`"db.table"` keys, populated
by SELECT / writes / EXISTS / SHOW CREATE / RENAME) and `database_rewrites`
(bare DB keys, populated by USE / SHOW TABLES). They are kept separate so
callers can branch on "did a DB change?" vs "did a table change?" without
parsing key formats.

`TableNameRewrite` carries two modes:
- **SELECT** uses `static_args` — three maps (`table_map`,
  `remote_table_map`, `table_with_database_map`) the caller populates directly.
- **Non-SELECT** uses `dynamic_args` — dynamic construction from
  `database_map` (logical → physical), `known_physical_databases`,
  `upstream_logical_database_in_context` (last `USE`), `delim` (default `_`),
  and `extra_arguments`. Final target =
  `` `<physical_db>`.`<logical_db><delim>[extras joined by delim<delim>]<origin_table>` ``.
  Empty `extra_arguments` collapses the middle block so you don't get a double
  delim.

### Storage-integrity v1

An effective `dynamic_args.storage_integrity` with a non-empty table map must request `STORAGE_INTEGRITY_CONTRACT_V1`; the response echoes the same version only after the backend validated and activated the policy. Logical SI databases must also be authorized by `database_map`. A minimal safe-read payload looks like:

```json
{
  "database_map": {"db1": "tenant_physical"},
  "storage_integrity": {
    "tables": {
      "db1.t": {
        "safe_table": "hg_safe.db1__t",
        "unsafe_table": "hg_unsafe.db1__t"
      }
    },
    "read_mode": "READ_MODE_SAFE",
    "reserved_row_id_column": "_hg_row_id",
    "contract_version": "STORAGE_INTEGRITY_CONTRACT_V1"
  }
}
```

SELECT-family reads use the safe table, or safe plus unpromoted unsafe parts in `READ_MODE_UNSAFE_LATEST`, while hiding and reserving the row-id column. `EXISTS TABLE` uses the safe table and `DESCRIBE TABLE` reads filtered `system.columns` metadata. INSERT remains an authorized ordinary physical rewrite marked `is_storage_integrity`; an SI table is append-only, so every other SI write/DDL/DCL is rejected with `storage-integrity table <t> is append-only: ...` (contract V2 adds the plain `DROP TABLE` and re-`CREATE` route for schema changes), and a view body, table engine, ALTER / UPDATE / DELETE operand or write into an SI table that reads one gets `storage-integrity table <t> can be read only by SELECT, ...`. Configured safe/unsafe physical databases are protocol-owned namespaces and cannot be addressed directly or indirectly through table functions, table engines, dictionary sources, or `IN <table>` forms. In dynamic mode a table-reference policy (housegate spec 2026-09-26) runs before every handler: query parameters in table positions, unmodelled statement classes, databases named by `database_map` values or `protected_databases`, system tables outside a fixed allowlist, disallowed table functions, engines and settings (SQL-bearing, name-binding and analyzer-off settings included), string lookups, unqualified names that do not resolve, reads in positions no rewrite reaches, and literal spellings the formatter cannot re-emit faithfully are refused with fixed cross-engine messages, while IN operands and INSERT … SELECT / CTAS / view / `view()` bodies are rewritten and reported like FROM. The cross-engine golden contract is [tests/testdata/storage_integrity_cases.json](tests/testdata/storage_integrity_cases.json); its rewriter-go copy must remain byte-identical.

Responses carry two enums for programmatic dispatch: `statement_type`
(`StatementType` — `STATEMENT_TYPE_SELECT`, `_INSERT`, `_ALTER_TABLE`, …) for
routing / auth control, and `code` (`RewriteCode`) with a four-way rejection
taxonomy: `Success`, `SyntaxError`, `UnsupportedStatement` (statement kind out
of scope — caller may pass through), and `InvalidRewriteRequest` (caller's
options don't match the SQL, e.g. a non-SELECT target named a logical DB
that's neither in `database_map` nor in `known_physical_databases` — caller
must NOT pass through). The canonical contract comes from the pinned
[`rewriter-proto`](third_party/rewriter-proto/proto/rewriter.proto) submodule.

## What's in the box

| Binary | Role |
|--------|------|
| `clickhousegate_rewriter`     | Long-running server. Listens on gRPC (default `:50051`) and HTTP/JSON (`:50052`). |
| `clickhousegate_rewriter_cli` | One-shot CLI — takes a SQL string + rewrite options, prints rewritten SQL. |

## Clone

```shell
git clone --recurse-submodules --shallow-submodules https://github.com/housegate/rewriter.git
cd rewriter && git submodule update --init --depth=1
```

## Build

The ClickHouse source tree is huge and only compiles on a decently powerful
Linux box. The `scripts.sh` wrapper handles dependency install and the
submodule quirks.

```shell
./scripts.sh prepare          # install toolchain (LLVM 22+, cmake, ninja, mold, ...)
./scripts.sh build            # full clean build (first time, ~15-20 min)
./scripts.sh rebuild          # incremental (day-to-day, seconds to a few min)
./scripts.sh test             # build + ctest
./scripts.sh clean            # wipe build/, reset submodules to their pins
```

After a successful build:

- `build/clickhousegate_rewriter`
- `build/clickhousegate_rewriter_cli`
- `build/tests/rewriter_tests` (always built; a test compile failure fails the command)

The ClickHouse compatibility patches that `scripts.sh` applies stay in the
submodule for as long as `build/` exists; `./scripts.sh clean` reverts them.

## Run the server

```shell
./build/clickhousegate_rewriter              # gRPC :50051, HTTP :50052
./build/clickhousegate_rewriter 60000        # gRPC :60000, HTTP :60001
./build/clickhousegate_rewriter 60000 7000   # gRPC :60000, HTTP :7000
```

### Talk to it over HTTP

```shell
# Health check.
curl -s http://localhost:50052/healthz

# Rewrite a SELECT — three-map policy under `static_args`.
curl -s -XPOST http://localhost:50052/rewrite \
  -H 'Content-Type: application/json' \
  -d '{
    "sql": "SELECT * FROM events",
    "options": [
      {"op": "TableNameRewrite",
       "table_name_args": {"static_args": {"table_map": {"events": "events_v2"}}}},
      {"op": "LimitRewrite", "limit_args": {"force_limit": 100}}
    ]
  }'

# Rewrite a write — dynamic construction under `dynamic_args`.
# Final target = `<physical>`.`<logical>_<original>` (delim defaults to "_").
curl -s -XPOST http://localhost:50052/rewrite \
  -H 'Content-Type: application/json' \
  -d '{
    "sql": "INSERT INTO events VALUES (1)",
    "options": [
      {"op": "TableNameRewrite",
       "table_name_args": {"dynamic_args": {
         "database_map": {"tenant1": "testnet"},
         "upstream_logical_database_in_context": "tenant1"}}}
    ]
  }'
# => { "code": "Success", "statement_type": "STATEMENT_TYPE_INSERT",
#      "sql_after_rewrite": "INSERT INTO testnet.tenant1_events VALUES (1)",
#      "table_rewrites": {"events": "testnet.tenant1_events"}, ... }
```

### Talk to it over gRPC

Any gRPC client works; for quick shell usage install `grpcurl`:

```shell
grpcurl -plaintext -proto third_party/rewriter-proto/proto/rewriter.proto \
  -d '{"sql": "SELECT 1"}' \
  localhost:50051 rewriter.RewriterService/Rewrite
```

## Run the CLI

```shell
# Simple: inline SQL.
./build/clickhousegate_rewriter_cli --sql 'SELECT * FROM events'

# Rename a table.
./build/clickhousegate_rewriter_cli \
  --sql 'SELECT * FROM events' --rename-table events=events_v2

# Force a LIMIT.
./build/clickhousegate_rewriter_cli --sql 'SELECT 1' --force-limit 10

# Full proto options from a JSON file (matches the RewriteSQLRequest message).
./build/clickhousegate_rewriter_cli --sql-file query.sql --options options.json

# Read SQL from stdin.
cat query.sql | ./build/clickhousegate_rewriter_cli --force-limit 100

# Rewrite an error message (uses the rewritten -> original inverse transform).
./build/clickhousegate_rewriter_cli \
  --sql 'SELECT * FROM events_v2' \
  --rename-table events=events_v2 \
  --error-message 'Table events_v2 does not exist'

# Emit the full RewriteSQLResponse as JSON.
./build/clickhousegate_rewriter_cli --sql 'SELECT 1' --json
```

Run `clickhousegate_rewriter_cli --help` for the full flag list.

## Docker

Release images are published to a private registry; `<image-repository>`
below stands for that repository.

```shell
docker pull <image-repository>:<version>
docker run --rm -p 50051:50051 -p 50052:50052 <image-repository>:<version>
```

See `.github/workflows/release.yml` for how releases are produced. The docker
image is built and pushed by the CI workflow, which takes the repository from
the `IMAGE_REPOSITORY` secret; manual builds set `REWRITER_IMAGE_REPO` and run
`./scripts.sh docker build <version>` + `./scripts.sh docker push <version>`.

## Build-box coordination

There's a single dedicated build host. Both the CI workdir
(`~/ci/rewriter`) and the developer workdir share the same ClickHouse
submodule via a symlink to save disk. Two concurrent `./scripts.sh build` runs
will race on submodule patches. If a release workflow is in flight, hold off
on local builds on the same host until it's green — the CI job's
`concurrency: group: build-box` guards CI-vs-CI races, not CI-vs-developer.

## License

Copyright 2024-2026 The housegate rewriter authors. Licensed under the
[GNU Affero General Public License v3.0](LICENSE): if you run a modified
version as a network service, you must offer its users the corresponding
source. ClickHouse, which this service is built from and patches, is licensed
under the Apache License 2.0; see [NOTICE](NOTICE).
