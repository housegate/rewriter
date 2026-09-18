#!/usr/bin/env python3
"""Bounded fresh-process runtime selection probe; not all-zone SQL support."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

cfg = json.loads(Path(sys.argv[1]).read_text())
work = Path(sys.argv[2])
container = sys.argv[3]
normal = work / "timezone/zoneinfo"
conflict = work / "timezone-conflict"
missing = work / "timezone-missing"
conflict.mkdir()
missing.mkdir()
for relative in ("UTC", "America/New_York", "HG153/ProbeUTC"):
    path = conflict / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(normal / "UTC", path)
    path.chmod(0o444)
cases = []
ny = "SELECT version(), timezone(), toUnixTimestamp(toDateTime('2024-01-01 12:00:00', 'America/New_York')), toUnixTimestamp(toDateTime('2024-07-01 12:00:00', 'America/New_York'))"
alias = "SELECT toUnixTimestamp(toDateTime('2024-07-01 12:00:00', 'HG153/ProbeUTC'))"
for name, directory, sql, code, expected in (
    ("normal", normal, ny, 0, "25.8.32.4\tUTC\t1704128400\t1719849600\n"),
    ("conflict", conflict, ny, 0, "25.8.32.4\tUTC\t1704128400\t1719849600\n"),
    ("fallback_present", conflict, alias, 0, "1719835200\n"),
    ("fallback_missing", missing, alias, 36, ""),
):
    command = ["docker", "run", "--rm", "--name", container, "--label", "housegate.task=" + container, "--network", "none", "--read-only", "--user", f"{os.getuid()}:{os.getgid()}", "--cpus", "2", "--memory", "1g", "--pids-limit", "512", "--cap-drop", "ALL", "--security-opt", "no-new-privileges=true", "--tmpfs", "/tmp:rw,nosuid,nodev,size=128m", "-v", str(directory) + ":/tzdata:ro", "-v", cfg["files"]["clickhouse"] + ":/clickhouse:ro", "-e", "TZDIR=/tzdata", "-e", "TZ=UTC"]
    for root in ("/usr/share/zoneinfo", "/apex", "/data", "/system", "/config", "/pkg"):
        command += ["--tmpfs", root + ":ro,nosuid,nodev,size=1m"]
    command += [cfg["images"]["native"], "/clickhouse", "local", "--path=/tmp/ch", "--max_threads=1", "--query", sql]
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    record = dict(name=name, argv=command, exit=result.returncode, stdout=result.stdout, stderr=result.stderr)
    cases.append(record)
    (work / "timezone-probe.json").write_text(json.dumps(dict(scope="Fresh-process bounded source-selection only; no all-zone or analyzer admission claim", cases=cases), indent=2) + "\n")
    assert result.returncode == code and result.stdout == expected, record
    if code:
        assert result.stderr.startswith("Code: 36."), record
    absent = subprocess.run(["docker", "inspect", container], capture_output=True)
    assert absent.returncode != 0, "owned timezone container survived"
print("TZDATA_SELECTION_PASS cases=4; embedded New_York wins conflict; unembedded alias uses controlled TZDIR")
