# SQL Rewriter

A C++ service that rewrites ClickHouse SQL before it hits a backend ClickHouse
cluster. Runs as a gRPC server, an HTTP/JSON server on the same process,
and ships with a standalone CLI for one-off rewrites.

## What's in the box

| Binary | Role |
|--------|------|
| `clickhouse_housegate_rewriter`     | Long-running server. Listens on gRPC (default `:50051`) and HTTP/JSON (`:50052`). |
| `clickhouse_housegate_rewriter_cli` | One-shot CLI — takes a SQL string + rewrite options, prints rewritten SQL. |

## Clone

```shell
git clone --recurse-submodules --shallow-submodules https://github.com/sentioxyz/sql-rewriter.git
cd sql-rewriter && git submodule update --init --depth=1
```

## Build

The ClickHouse source tree is huge and only compiles on a decently powerful
Linux box. The `scripts.sh` wrapper handles dependency install and the
submodule quirks.

```shell
./scripts.sh prepare          # install toolchain (LLVM 19/20, cmake, ninja, mold, ...)
./scripts.sh build            # full clean build (first time, ~15-20 min)
./scripts.sh rebuild          # incremental (day-to-day, seconds to a few min)
./scripts.sh test             # build + ctest
./scripts.sh clean            # wipe build/
```

After a successful build:

- `build/clickhouse_housegate_rewriter`
- `build/clickhouse_housegate_rewriter_cli`
- `build/tests/rewriter_tests` (if `BUILD_TESTING=ON`, the default)

## Run the server

```shell
./build/clickhouse_housegate_rewriter              # gRPC :50051, HTTP :50052
./build/clickhouse_housegate_rewriter 60000        # gRPC :60000, HTTP :60001
./build/clickhouse_housegate_rewriter 60000 7000   # gRPC :60000, HTTP :7000
```

### Talk to it over HTTP

```shell
# Health check.
curl -s http://localhost:50052/healthz

# Rewrite a SELECT by renaming a table and forcing a LIMIT.
curl -s -XPOST http://localhost:50052/rewrite \
  -H 'Content-Type: application/json' \
  -d '{
    "sql": "SELECT * FROM events",
    "options": [
      {"op": "TableNameRewrite", "table_name_args": {"table_map": {"events": "events_v2"}}},
      {"op": "LimitRewrite",     "limit_args":      {"force_limit": 100}}
    ]
  }'
```

### Talk to it over gRPC

Any gRPC client works; for quick shell usage install `grpcurl`:

```shell
grpcurl -plaintext -proto protos/rewriter.proto \
  -d '{"sql": "SELECT 1"}' \
  localhost:50051 rewriter.RewriterService/Rewrite
```

## Run the CLI

```shell
# Simple: inline SQL.
./build/clickhouse_housegate_rewriter_cli --sql 'SELECT * FROM events'

# Rename a table.
./build/clickhouse_housegate_rewriter_cli \
  --sql 'SELECT * FROM events' --rename-table events=events_v2

# Force a LIMIT.
./build/clickhouse_housegate_rewriter_cli --sql 'SELECT 1' --force-limit 10

# Full proto options from a JSON file (matches the RewriteSQLRequest message).
./build/clickhouse_housegate_rewriter_cli --sql-file query.sql --options options.json

# Read SQL from stdin.
cat query.sql | ./build/clickhouse_housegate_rewriter_cli --force-limit 100

# Rewrite an error message (uses the rewritten -> original inverse transform).
./build/clickhouse_housegate_rewriter_cli \
  --sql 'SELECT * FROM events_v2' \
  --rename-table events=events_v2 \
  --error-message 'Table events_v2 does not exist'

# Emit the full RewriteSQLResponse as JSON.
./build/clickhouse_housegate_rewriter_cli --sql 'SELECT 1' --json
```

Run `clickhouse_housegate_rewriter_cli --help` for the full flag list.

## Docker

Pre-built images are published to Google Artifact Registry:

```shell
docker pull us-west1-docker.pkg.dev/sentio-352722/sentio/sql-rewriter:<version>
docker run --rm -p 50051:50051 -p 50052:50052 \
  us-west1-docker.pkg.dev/sentio-352722/sentio/sql-rewriter:<version>
```

See `.github/workflows/release.yml` for how releases are produced. The docker
image is built and pushed by the CI workflow; manual local builds use
`./scripts.sh docker build <version>` + `./scripts.sh docker push <version>`.

## Build-box coordination

There's a single dedicated build host. Both the CI workdir
(`~/ci/sql-rewriter`) and the developer workdir share the same ClickHouse
submodule via a symlink to save disk. Two concurrent `./scripts.sh build` runs
will race on submodule patches. If a release workflow is in flight, hold off
on local builds on the same host until it's green — the CI job's
`concurrency: group: build-box` guards CI-vs-CI races, not CI-vs-developer.
