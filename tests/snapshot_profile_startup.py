#!/usr/bin/env python3
"""Real process probes. Run inside a task-owned network-none container.

No hash injection: the final service measures itself. External profile generation
must finish before this script runs. No rebuild or mutation of the original ELF.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

binary = Path(sys.argv[1]).resolve()
profile = os.environ.get("SNAPSHOT_SERVICE_PROFILE_FILE")
if not profile:
    print("SKIP: snapshot startup requires an externally measured service profile")
    sys.exit(77)

def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1024 * 1024), b""):
            h.update(b)
    return "0x" + h.hexdigest()

def ports():
    with socket.socket() as a, socket.socket() as b:
        a.bind(("127.0.0.1", 0)); b.bind(("127.0.0.1", 0))
        return [str(a.getsockname()[1]), str(b.getsockname()[1])]

def terminate(p):
    if p.poll() is None:
        p.terminate()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()

def refused(name, args, env=None, executable=binary, text="Rewriter startup refused:"):
    p = subprocess.Popen([str(executable), *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    try:
        out, _ = p.communicate(timeout=30)
        assert p.returncode == 1 and text in out.decode(), (name, p.returncode, out.decode())
        assert "Server listening on" not in out.decode(), (name, out.decode())
        print("PASS", name, out.decode().strip())
    finally:
        terminate(p)

def healthy(name, extra, mutate=None, default_ports=False):
    chosen = ["50051", "50052"] if default_ports else ports()
    args = extra if default_ports else [chosen[0], *extra, chosen[1]]
    with tempfile.TemporaryFile() as log:
        p = subprocess.Popen([str(binary), *args], stdout=log, stderr=log)
        try:
            for _ in range(300):
                if p.poll() is not None:
                    log.seek(0); raise AssertionError((name, p.returncode, log.read().decode()))
                try:
                    with urllib.request.urlopen(f"http://127.0.0.1:{chosen[1]}/healthz", timeout=.2) as response:
                        if response.status == 200 and response.read() == b"ok": break
                except OSError: pass
                time.sleep(.1)
            else: raise AssertionError((name, "health timeout"))
            if mutate:
                mutate()
                with urllib.request.urlopen(f"http://127.0.0.1:{chosen[1]}/healthz", timeout=1) as response:
                    assert response.read() == b"ok"
            print("PASS", name)
        finally: terminate(p)

before = digest(binary)
with tempfile.TemporaryDirectory(prefix="snapshot-startup-") as td:
    root = Path(td)
    canonical = Path(profile).read_bytes()
    bundle = json.loads(canonical)
    assert all(e["record"]["grpc_analyzer_build_digest"] == before for e in bundle["profiles"])
    actual = root / "profiles.json"; actual.write_bytes(canonical)
    healthy("ordinary-default-ports", [], default_ports=True)
    healthy("ordinary-positional-ports", [])
    refused("missing-option-value", ["--snapshot-query-profile-file"])
    refused("empty-option-value", ["--snapshot-query-profile-file="])
    refused("unknown-option", ["--snapshot-query-profile-files", str(actual)])
    refused("duplicate-option", ["--snapshot-query-profile-file", str(actual)] * 2)
    refused("missing-file", ["--snapshot-query-profile-file", str(root / "absent")])
    malformed = root / "malformed.json"; malformed.write_text("{}")
    refused("malformed-file", ["--snapshot-query-profile-file", str(malformed)])
    fifo = root / "fifo"; os.mkfifo(fifo)
    refused("nonregular-file", ["--snapshot-query-profile-file", str(fifo)])
    fixture = json.loads((Path(__file__).parent / "testdata/snapshot_query_build_identity_v1.json").read_text())
    foreign = root / "nonlocal.json"; foreign.write_text(fixture["profile_file_json"])
    refused("all-nonlocal-file", ["--snapshot-query-profile-file", str(foreign)], text="no locally measured candidate")
    healthy("measured-file", ["--snapshot-query-profile-file", str(actual)])
    healthy("immutable-after-startup", ["--snapshot-query-profile-file=" + str(actual)], lambda: actual.write_text("{}"))
    actual.write_bytes(canonical)
    mixed = json.loads(fixture["profile_file_json"])
    mixed["profiles"].extend(bundle["profiles"]); mixed["profiles"].sort(key=lambda x: x["query_profile_id"])
    mixedfile = root / "mixed.json"; mixedfile.write_text(json.dumps(mixed, separators=(",", ":"), ensure_ascii=False))
    healthy("mixed-local-nonlocal", ["--snapshot-query-profile-file", str(mixedfile)])
    for variable in ["LD_LIBRARY_PATH", "LD_AUDIT", "LD_PRELOAD", "GLIBC_TUNABLES"]:
        env = dict(os.environ); env[variable] = "/snapshot/unsupported"
        refused("unsupported-" + variable, ["--snapshot-query-profile-file", str(actual)], env=env, text="unsupported loader environment")
    # Appending to a separate actual ELF changes G while preserving executable
    # code. Original service is read-only and its before/after digest is checked.
    altered = root / "changed-service"; shutil.copyfile(binary, altered)
    with altered.open("ab") as f: f.write(b"independent changed ELF witness")
    altered.chmod(0o555)
    assert digest(altered) != before
    refused("changed-actual-ELF", ["--snapshot-query-profile-file", str(actual)], executable=altered, text="no locally measured candidate")
assert digest(binary) == before
print("PASS unchanged-service-digest", before)
