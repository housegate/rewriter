#!/usr/bin/env bash
set -euo pipefail

binary=${1:?usage: rewriter_startup_smoke.sh /path/to/clickhousegate_rewriter}
test_dir=$(mktemp -d)
server_pid=''

cleanup() {
  if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -r -- "$test_dir"
}
trap cleanup EXIT

read -r grpc_port http_port < <(
  python3 - <<'PY'
import socket

sockets = []
ports = []
for _ in range(2):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    sockets.append(sock)
    ports.append(sock.getsockname()[1])

print(*ports)
PY
)

server_log="$test_dir/server.log"
"$binary" "$grpc_port" "$http_port" >"$server_log" 2>&1 &
server_pid=$!

if ! python3 - "$http_port" "$server_pid" <<'PY'
import os
import sys
import time
import urllib.request

port = int(sys.argv[1])
pid = int(sys.argv[2])
url = f"http://127.0.0.1:{port}/healthz"

for _ in range(100):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        sys.exit(1)

    try:
        with urllib.request.urlopen(url, timeout=0.2) as response:
            if response.status == 200 and response.read() == b"ok":
                sys.exit(0)
    except OSError:
        pass

    time.sleep(0.1)

sys.exit(1)
PY
then
  echo "rewriter did not become healthy" >&2
  sed -n '1,160p' "$server_log" >&2
  exit 1
fi

kill -0 "$server_pid"
