#!/usr/bin/env bash
# Runs inside the existing private build-box lock/timebox, after authenticating
# the RC event source and building its final service and explicit test target.
set -euo pipefail
export GIT_NO_REPLACE_OBJECTS=1
rc=${1:?RC checkout}
expected_rc=${2:?exact RC commit}
work=${3:?owned output directory}
verifier=${4:?unchanged workflow raw integrity verifier}
rg=${5:-$work/rewriter-go}
hg=${6:-$work/housegate}
[[ $rc == "$HOME/ci/rewriter-ci" && $expected_rc =~ ^[0-9a-f]{40}$ ]]
[[ $work == /tmp/rewriter-snapshot-* && -d $work && ! -L $work ]]
[[ $(git -C "$rc" rev-parse HEAD) == "$expected_rc" ]]
[[ -z $(git -C "$rc" symbolic-ref -q HEAD || true) ]]
helper=$rc/.github/scripts/snapshot-query-ci.py
python3 "$helper" inputs "$rc/tests/testdata/snapshot_query_ci_pins.json" "$work/inputs.json" | tee "$work/prerequisites.log"
get(){ python3 -c 'import json,sys; x=json.load(open(sys.argv[1]));
for k in sys.argv[2:]: x=x[k]
print(x)' "$work/inputs.json" "$@"; }
rg_sha=$(get manifest sources rg)
hg_sha=$(get manifest sources hg)
rp_sha=$(get manifest sources rp)
polyglot_sha=$(get manifest sources polyglot)
native_image=$(get images native)
cpp_image=$(get images cpp)
ffi=$(get files ffi)
tool=$(get files profile_tool)
ch=$(get files clickhouse)
tz=$(get files tzdata)
# rewriter-proto: the RC build's gitlink may advance past the manifest's `rp`
# pin (the pin names the protocol the prerequisites were measured against).
# It is accepted only when (a) `rp` is an ancestor of the gitlink and (b) the
# snapshot-query wire surface (the AnalyzeSnapshotQuery/PrepareSnapshotQuery
# rpcs, the messages/enums they carry and their field-type closure, plus the
# syntax/package lines; `snapshot-query-ci.py protocol`) is byte-identical at
# both commits. History comes from a task-owned public fetch, because CI's
# submodule checkout is shallow. Every other pin is unchanged.
rp_head=$(git -C "$rc" ls-tree "$expected_rc" -- third_party/rewriter-proto | awk '$1 == "160000" && $2 == "commit" { print $3 }')
if [[ ! $rp_head =~ ^[0-9a-f]{40}$ ]]; then
 echo 'error: RC commit has no valid rewriter-proto gitlink' >&2; exit 1
fi
if [[ $(git -C "$rc/third_party/rewriter-proto" rev-parse HEAD) != "$rp_head" ]]; then
 echo "error: rewriter-proto checkout is not at the RC gitlink $rp_head" >&2; exit 1
fi
rp_history=$work/rewriter-proto-history
git init -q "$rp_history"
git -C "$rp_history" remote add origin https://github.com/housegate/rewriter-proto.git
git -C "$rp_history" fetch --no-tags -q origin "$rp_head"
[[ $(git -C "$rp_history" rev-parse 'FETCH_HEAD^{commit}') == "$rp_head" ]]
[[ -z $(git -C "$rp_history" for-each-ref --format='%(refname)' refs/replace/) ]]
if ! git -C "$rp_history" merge-base --is-ancestor "$rp_sha" "$rp_head"; then
 echo "error: pinned rewriter-proto $rp_sha is not an ancestor of the RC gitlink $rp_head" >&2; exit 1
fi
if ! python3 "$helper" protocol "$rp_history" "$rp_sha" "$rp_head" > "$work/protocol-surface.log" 2>&1; then
 echo "error: snapshot-query proto surface differs between pinned rewriter-proto $rp_sha and the RC gitlink $rp_head" >&2
 cat "$work/protocol-surface.log" >&2; exit 1
fi
cat "$work/protocol-surface.log"

# Local rehearsal may provide exact bundle-derived checkouts via argv5/6. They
# receive the same raw object/index/worktree verification as public CI fetches.
fetch_public(){
 local path=$1 url=$2 sha=$3
 if [[ ! -e $path ]]; then
  git init "$path"
  git -C "$path" remote add origin "$url"
  git -C "$path" fetch --no-tags --depth=1 origin "$sha"
  [[ $(git -C "$path" rev-parse 'FETCH_HEAD^{commit}') == "$sha" ]]
  git -C "$path" checkout --detach "$sha"
  git -C "$path" submodule update --init --recursive
 fi
 [[ $(git -C "$path" remote get-url origin) == "$url" ]]
 [[ $(git -C "$path" rev-parse HEAD) == "$sha" ]]
 [[ -z $(git -C "$path" symbolic-ref -q HEAD || true) ]]
 [[ -z $(git -C "$path" for-each-ref --format='%(refname)' refs/replace/) ]]
 [[ ! -e $(git -C "$path" rev-parse --git-path info/grafts) ]]
}
fetch_public "$rg" https://github.com/housegate/rewriter-go.git "$rg_sha"
fetch_public "$hg" https://github.com/housegate/housegate.git "$hg_sha"
[[ $(git -C "$rg/third_party/polyglot-src" rev-parse HEAD) == "$polyglot_sha" ]]
python3 "$verifier" "$rg" "$rg_sha" '' '' "$hg" "$hg_sha" '' '' | tee "$work/public-source-before.log"
[[ $(sha256sum "$rg/third_party/polyglot-src/Cargo.lock" | awk '{print $1}') == $(get manifest files ffi cargo_lock_sha256) ]]
rg_rp_version=$(sed -n 's/^[[:space:]]*github.com\/housegate\/rewriter-proto \([^ ]*\)$/\1/p' "$rg/go.mod")
[[ $rg_rp_version == *-${rp_sha:0:12} ]]
cmp "$rg/internal/harness/testdata/snapshot_query_cases.json" "$rc/tests/testdata/snapshot_query_cases.json"
cmp "$rg/internal/harness/testdata/snapshot_query_profile_record.json" "$rc/tests/testdata/snapshot_query_profile_record.json"

prefix=hg-snapshot-$(basename "$work")
service=$prefix-service
native=$prefix-native
cpp=$prefix-cpp
builder=$prefix-build
network=$prefix-network
network_created=0
for name in "$service" "$native" "$cpp" "$builder"; do
 if docker inspect "$name" >/dev/null 2>&1; then echo "container already exists: $name" >&2; exit 1; fi
done
if docker network inspect "$network" >/dev/null 2>&1; then echo 'network already exists' >&2; exit 1; fi
finish(){
 result=$?; cleanup=1
 set +e
 trap - EXIT INT TERM HUP
 if docker inspect "$service" >/dev/null 2>&1; then
  docker logs "$service" > "$work/service.log" 2>&1 || result=91
  [[ $(docker inspect "$service" --format '{{.State.Running}}') == true ]] || result=92
 fi
 for name in "$service" "$native" "$cpp" "$builder"; do
  if docker inspect "$name" >/dev/null 2>&1; then docker rm -f "$name" >/dev/null || { result=93; cleanup=0; }; fi
 done
 if [[ $network_created == 1 ]]; then docker network rm "$network" > "$work/network-cleanup.log" || { result=94; cleanup=0; }; fi
 docker ps -a > "$work/containers-after.log" || { result=95; cleanup=0; }
 for name in "$service" "$native" "$cpp" "$builder"; do if docker inspect "$name" >/dev/null 2>&1; then result=96; cleanup=0; fi; done
 if [[ $network_created == 1 ]] && docker network inspect "$network" >/dev/null 2>&1; then result=97; cleanup=0; fi
 if [[ -f $work/measured-inputs.sha256 ]]; then sha256sum -c "$work/measured-inputs.sha256" > "$work/after-run.sha256" || result=98; fi
 # Preserve actual raw test/response evidence in the private job log before
 # the workflow removes this task's scratch directory. No executable is emitted.
 for log in "$work/native-build.log" "$work/ordinary.log" "$work/vet.log" "$work/cpp-run/ctest.log" "$work/cpp-run/ctest/Testing/Temporary/LastTest.log" "$work/cpp-run/gtest.xml" "$work/cpp-run/si.log" "$work/cpp-run/explicit-counts.log" "$work/native-run/root.log" "$work/native-run/engine.log" "$work/native-run/harness.log" "$work/native-run/execution.jsonl" "$work/native-run/transport.jsonl" "$work/timezone-probe.json" "$work/public-source-before.log" "$work/public-source-after-build.log" "$work/source-after-run.log" "$work/measured-inputs.sha256" "$work/after-run.sha256" "$work/protocol-module.json" "$work/service-isolation.json" "$work/service-provenance.json" "$work/test-provenance.json" "$work/deadline-provenance.json" "$work/service.log"; do
  if [[ -f $log ]]; then printf '\nPRIVATE_EVIDENCE %s\n' "$log"; cat "$log" || result=99; fi
 done
 for member in clickhousegate_rewriter rewriter_tests rewriter.test engine.test harness.test libpolyglot_sql_ffi.so; do
  log=$work/elf/$member.txt
  if [[ -f $log ]]; then printf '\nPRIVATE_ELF_EVIDENCE %s\n' "$member"; cat "$log" || result=99; fi
 done
 printf 'PAIRED_GATE_EXIT=%s\nOWNED_CLEANUP_COMPLETE=%s\n' "$result" "$cleanup" | tee "$work/paired.done"
 exit "$result"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
mkdir -p "$work/native" "$work/cache" "$work/go" "$work/native-run" "$work/cpp-run/ctest/tests" "$work/cpp-tmp"

# All build-producing Go commands precede generation. Source/FFI are read-only;
# every module/build cache belongs to this run. Ordinary tests are labelled as
# such and are not members of the measured analyzer set.
docker run --rm --name "$builder" --label housegate.task="$prefix" --network bridge --read-only --user "$(id -u):$(id -g)" --cpus 4 --memory 8g --pids-limit 512 --cap-drop ALL --security-opt no-new-privileges=true --tmpfs /tmp:rw,exec,nosuid,nodev,size=512m -v "$rg:/src:ro" -v "$work:/work:rw" -v "$ffi:/ffi.so:ro" -w /src -e GOCACHE=/work/cache -e GOPATH=/work/go -e GOTOOLCHAIN=local -e CGO_ENABLED=0 -e POLYGLOT_SQL_FFI_PATH=/ffi.so -e SNAPSHOT_QUERY_ORDINARY=1 "$native_image" bash -euc '
 go version
 test "$(go env GOVERSION)" = go1.26.4
 go list -m -json github.com/housegate/rewriter-proto > /work/protocol-module.json
 go test -mod=readonly ./... > /work/ordinary.log 2>&1
 go vet -mod=readonly ./... > /work/vet.log 2>&1
 bash scripts/test-snapshot-measured.sh compile /work/native
 for f in /work/native/*.test; do go version -m "$f"; done
' > "$work/native-build.log" 2>&1
python3 - "$work/protocol-module.json" "$rg_rp_version" <<'PY'
import json,sys
p=json.load(open(sys.argv[1]));assert p['Version']==sys.argv[2] and 'Replace' not in p
PY
python3 "$verifier" "$rg" "$rg_sha" '' '' "$hg" "$hg_sha" '' '' > "$work/public-source-after-build.log"
mkdir -p "$work/elf"
for binary in "$rc/build/clickhousegate_rewriter" "$rc/build/tests/rewriter_tests" "$work/native/rewriter.test" "$work/native/engine.test" "$work/native/harness.test" "$ffi"; do
 readelf -hW -lW -dW --dyn-syms "$binary" > "$work/elf/$(basename "$binary").txt"
done
python3 "$helper" generate "$work/inputs.json" "$work" "$rg" "$rc" | tee "$work/generate.log"
sha256sum "$rc/build/clickhousegate_rewriter" "$rc/build/tests/rewriter_tests" "$work/native/rewriter.test" "$work/native/engine.test" "$work/native/harness.test" "$ffi" "$tool" "$ch" "$tz" "$work/"*-profiles.json "$work/"*-corpus.json > "$work/measured-inputs.sha256"
git -C "$rc/clickHouse" status --porcelain=v1 --untracked-files=all > "$work/clickhouse-state-before.txt"
cp "$rc/build/CTestTestfile.cmake" "$work/cpp-run/ctest/"
cp "$rc/build/tests/CTestTestfile.cmake" "$work/cpp-run/ctest/tests/"

# Full ordinary/SI/snapshot CTest and startup negatives run in network-none.
docker run --rm --name "$cpp" --label housegate.task="$prefix" --network none --read-only --user "$(id -u):$(id -g)" --cpus 4 --memory 8g --pids-limit 512 --cap-drop ALL --security-opt no-new-privileges=true -v "$rc:$rc:ro" -v "$work:/qualification:ro" -v "$work/cpp-run:/result:rw" -v "$work/cpp-tmp:/tmp:rw" -e SNAPSHOT_QUERY_PROFILE_FILE=/qualification/test-profiles.json -e SNAPSHOT_SERVICE_PROFILE_FILE=/qualification/service-profiles.json -e SNAPSHOT_QUERY_CORPUS=/qualification/test-corpus.json -e GTEST_OUTPUT=xml:/result/gtest.xml -e RC_ROOT="$rc" "$cpp_image" bash -euc '
 ctest --test-dir /result/ctest --output-on-failure > /result/ctest.log 2>&1
 binary=$RC_ROOT/build/tests/rewriter_tests
 GTEST_OUTPUT=xml:/result/si.xml "$binary" --gtest_filter="SpecG/StorageIntegrityGolden.*:StorageIntegrityCorpus.*:SINormalize.*" > /result/si.log 2>&1
 for spec in "SpecG/StorageIntegrityGolden.* 1162" "StorageIntegrityCorpus.* 2" "SINormalize.* 11"; do
  read -r family expected <<< "$spec"
  count=$(GTEST_OUTPUT= "$binary" --gtest_filter="$family" --gtest_list_tests | awk "/^  / { count++ } END { print count + 0 }")
  echo "$family cases: $count"
  test "$count" -eq "$expected"
 done
' > "$work/cpp-run/explicit-counts.log" 2>&1
python3 "$helper" counts "$work" | tee "$work/cpp-counts.json"
python3 "$rc/.github/scripts/snapshot-query-timezone.py" "$work/inputs.json" "$work" "$native" | tee "$work/timezone-probe.log"

docker network create --internal --label housegate.task="$prefix" "$network" > "$work/network-id.txt"
network_created=1
docker run -d --name "$service" --label housegate.task="$prefix" --network "$network" --network-alias rewriter --read-only --user "$(id -u):$(id -g)" --cpus 2 --memory 4g --pids-limit 256 --cap-drop ALL --security-opt no-new-privileges=true --tmpfs /tmp:rw,nosuid,nodev,size=64m -v "$rc/build:/build:ro" -v "$work:/qualification:ro" "$cpp_image" /build/clickhousegate_rewriter 50051 50052 --snapshot-query-profile-file /qualification/rpc-profiles.json > "$work/service-id.txt"
docker inspect "$service" > "$work/service-isolation.json"
# A measured-profile startup SHA-256s the whole executable before it listens:
# about 11s for the ~4 GB 26.8 RelWithDebInfo binary on the build box (no
# SHA-NI), so allow 60s, like tests/snapshot_profile_startup.py's budget.
docker exec "$service" python3 -c 'import time,urllib.request
for attempt in range(600):
 try:
  r=urllib.request.urlopen("http://127.0.0.1:50052/healthz",timeout=1)
  assert r.status==200
  break
 except Exception:
  if attempt==599: raise
  time.sleep(.1)
'
[[ $(docker exec "$service" readlink /proc/1/exe) == /build/clickhousegate_rewriter ]]

# Native DSO/compiler fixtures require this compiler-capable pinned image and
# executable private tmpfs. /src is also the original runtime.Caller path.
docker run --rm --name "$native" --label housegate.task="$prefix" --network "$network" --read-only --user "$(id -u):$(id -g)" --cpus 4 --memory 8g --pids-limit 512 --cap-drop ALL --security-opt no-new-privileges=true --tmpfs /tmp:rw,exec,nosuid,nodev,size=2g --tmpfs /usr/share/zoneinfo:ro,size=1m --tmpfs /apex:ro,size=1m --tmpfs /data:ro,size=1m --tmpfs /system:ro,size=1m --tmpfs /config:ro,size=1m --tmpfs /pkg:ro,size=1m -v "$rg:/src:ro" -v "$work:/qualification:ro" -v "$work/native-run:/result:rw" -v "$ffi:/ffi.so:ro" -v "$ch:/clickhouse:ro" -v "$tz:/tzdata.tar:ro" -v "$work/timezone/zoneinfo:/tzdata:ro" -w /src -e REWRITER_ORACLE_ADDR=rewriter:50051 -e SNAPSHOT_CLICKHOUSE=/clickhouse -e SNAPSHOT_TZDATA_ARCHIVE=/tzdata.tar -e SNAPSHOT_DEADLINE_PROFILE=/qualification/deadline-profiles.json -e SNAPSHOT_RUN_LOG_DIR=/result -e TZDIR=/tzdata -e TZ=UTC "$native_image" bash scripts/test-snapshot-measured.sh run /qualification/native /ffi.so /qualification/service-profiles.json /qualification/service-corpus.json > "$work/native-run/all.log" 2>&1
python3 "$verifier" "$rg" "$rg_sha" '' '' "$hg" "$hg_sha" '' '' "$rc" "$expected_rc" build 'clickHouse,third_party/rewriter-proto' "$rc/third_party/rewriter-proto" "$rp_head" '' '' > "$work/source-after-run.log"
git -C "$rc/clickHouse" status --porcelain=v1 --untracked-files=all > "$work/clickhouse-state-after.txt"
cmp "$work/clickhouse-state-before.txt" "$work/clickhouse-state-after.txt"
cat "$work/cpp-counts.json" "$work/native-run/counts.json" "$work/identities.json"
echo 'PRIVATE_PAIRED_QUALIFICATION_PASS=1'
