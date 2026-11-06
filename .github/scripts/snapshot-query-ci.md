# Private measured snapshot qualification

The existing `Build & test (build box)` required check runs this gate inside its
unchanged shared lock and 85-minute timebox. The source verifier's Python body is
identical to the release workflow; CI saves that body in its task directory and
uses it unchanged to authenticate the exact RC event SHA, public HG/RG pins,
protocol and Polyglot sources. Public RG CI still proves ordinary regressions.
Private RC executables are never uploaded to a public repository or artifact.

`tests/testdata/snapshot_query_ci_pins.json` is the versioned source and prerequisite
manifest. The four prerequisite files are reviewed prebuilt inputs, not rebuilt
per job. They must exist as regular files with their exact size and SHA-256 under
`~/ci/snapshot-query-prerequisites/issue-153-v1`; the two exact Linux/AMD64
Docker image IDs must already exist. Missing, different or unsupported inputs fail
before qualification. There is no `/tmp` fallback, source-label digest override,
mutable image tag or automatic cache refresh.

The manifest's `rp` pin names the rewriter-proto commit the prerequisites were measured against. The RC build's `third_party/rewriter-proto` gitlink may advance past it, for example when a branch adds Rewrite-only fields. It is accepted only when both conditions hold:

- `rp` is an ancestor of the gitlink. This is checked in a task-owned full-history public fetch, because CI's submodule checkout is shallow.
- `snapshot-query-ci.py protocol` finds the snapshot-query wire surface byte-identical at both commits. The surface is the `AnalyzeSnapshotQuery` / `PrepareSnapshotQuery` rpc lines, the messages and enums they carry with the closure of their field types (including `MaterializationInputs`), and the `syntax` / `package` lines.

A missing name, an unrecognised declaration or any changed block fails closed with an explicit error. The post-run source verifier checks the checkout against the gitlink. RG's own `rewriter-proto` dependency must still equal `rp`, and every other pin and prerequisite is unchanged.

Every required run freshly builds RC through `./scripts.sh rebuild` and explicit
`ninja -C build rewriter_tests`, then freshly compiles the pinned RG root, engine
and harness test binaries in the pinned Go 1.26.4 image. All ordinary Go tests and
vet run before profile generation. The reviewed HG tool opens these actual files,
the retained actual FFI, final RC artifacts, ClickHouse and timezone archive to
produce separate service/test G profiles and a one-millisecond diagnostic profile.
Only then do direct, unchanged binary invocations run; no subsequent Go/C++ build
is permitted. The three actually executed native binaries constitute N.

Full CTest, actual startup/loader negatives and the existing SI suites/counts run
inside a read-only, network-none runtime. A task-owned internal Docker bridge
connects the service and native oracle; no port is published. The compiler-capable
native runtime provides actual loader fixture compilation, private executable
scratch and the original `/src` source path. Its execution fixture verifies actual
ClickHouse/tzdata bytes, the selected complete timezone directory and masked
fallback roots, and reads all six settings back in each ClickHouse local session.
Each Service/Native/RPC Prepare result supplies its own executed SQL. Lossless
numeric JSON, positional metadata/values and numeric exception codes are recorded;
a failed process never counts partial rows as success. Expected scalar exception
349 is distinct from Unix exit93. Frozen corpus remains 636 Analyze/130 Prepare;
the separate execution matrix is 102 fixtures, 228 executions and 78 analyzer
refusals. Actual transport coverage contains 14 control/cancel/deadline calls.

A four-case fresh-process timezone probe establishes the bounded selection rule:
embedded New_York wins a conflicting fallback file; an unembedded test alias uses
controlled TZDIR and fails when absent. It does not claim all-zone functional
support. B2/B3/B4/D4 still own authenticated restore, canonical output roots,
execution limits and atomic no-commit behavior after failure.

Raw test/response evidence and a bounded identity/count manifest go to the private
job log. The runner removes only its named containers and bridge, rechecks original
artifact bytes and exact sources, and fails if cleanup or integrity fails. The
workflow removes only its unique task directory, including its private Go caches,
before releasing the lock. Its required aggregate check propagates all failures.
Local rehearsals retain their designated source/artifact/raw-log evidence separately
and do not count as a pushed GitHub CI pass.

## Deliberate prerequisite recovery

Recover from the retained reviewed artifacts only after checking the full manifest
and source provenance. Create the versioned directory and files exclusively; refuse
existing different objects, symlinks or unverified hashes. Do not overwrite a cache
entry or rebuild an image automatically. The original build recipes are:

- Polyglot at the pinned `polyglot` source: Rust/Cargo1.97.1, native
  `x86_64-unknown-linux-gnu`, `cargo build --locked -p polyglot-sql-ffi --profile
  ffi_release`, with the Cargo.lock hash in the manifest. cbindgen's generated
  header is an explicit derived output; preserve tracked source bytes and put the
  target/toolchain/cache in a dedicated task directory. The reviewed build used
  4 CPUs/16 GiB after an 8 GiB OOM; do not claim the failed attempt qualified.
- HG at the exact `hg` pin: Bazel9.1.0 and Go1.26.3,
  `bazel build //tools/snapshot-query-profile:snapshot-query-profile
  --platforms=@rules_go//go/toolchain:linux_amd64`. The tool is not an N member.
- ClickHouse25.8.32.4 Linux/AMD64: the exact binary and source commit in the
  manifest. Verify final bytes; version output alone does not establish identity.
- cctz at the exact `cctz` pin: preserve all 600 `testdata/zoneinfo` regular files,
  authenticate the exact Git tree/blobs, and create sorted POSIX ustar rooted at
  `zoneinfo/`, uid/gid0, mtime0, directory0555 and file0444. The complete archive
  includes three `.tab` files in addition to the 597 embedded TZif entries.
- Native runtime: the manifest's immutable `golang@sha256` registry digest and
  expected image ID. C++ runtime: its pinned Ubuntu base with Python3.12.3,
  CTest3.28.3 and glibc2.39-0ubuntu8.9 as recorded. Its build was `apt-get update`
  followed by `apt-get install --no-install-recommends python3 cmake`; repeating
  package resolution later is not guaranteed to reproduce its image ID.

Any rebuilt bytes or replacement image require explicit review, updated provenance
and manifest pins, and affected final artifact/profile requalification. Retaining a
source revision or an old Q does not authorize a replacement binary.
