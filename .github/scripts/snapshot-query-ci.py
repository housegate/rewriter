#!/usr/bin/env python3
"""Private paired qualification helpers; no digest overrides or build actions."""
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tarfile
import xml.etree.ElementTree as ET


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def read(path):
    def unique(pairs):
        out = {}
        for key, value in pairs:
            if key in out:
                raise ValueError("duplicate JSON key")
            out[key] = value
        return out
    return json.loads(Path(path).read_text(), object_pairs_hook=unique)


def inputs(manifest, output):
    p = read(manifest)
    assert p["version"] == 1
    assert all(re.fullmatch("[0-9a-f]{40}", s) for s in p["sources"].values())
    root = Path(p["prerequisite_root"])
    assert root == Path("/home/sentio/ci/snapshot-query-prerequisites/issue-153-v2")
    assert root.resolve() == root and root.is_dir()
    assert p["corpus"] == {"sha256": "3d7c1f1077b1ac6c1bd73d7703b135e95c92bde10d1ce4393c8cb091cca1c933", "analyze": 636, "prepare": 130}
    files = {}
    for key, item in p["files"].items():
        assert re.fullmatch(r"[a-zA-Z0-9_.-]+", item["name"])
        path = root / item["name"]
        st = path.lstat()
        assert stat.S_ISREG(st.st_mode) and st.st_size == item["bytes"], path
        assert digest(path) == item["sha256"], path
        files[key] = str(path)
    images = {}
    for key, item in p["images"].items():
        assert re.fullmatch("sha256:[0-9a-f]{64}", item["id"])
        actual = json.loads(subprocess.check_output(["docker", "image", "inspect", item["id"]]))[0]
        assert actual["Id"] == item["id"] and actual["Os"] + "/" + actual["Architecture"] == item["platform"] == "linux/amd64"
        images[key] = actual["Id"]
    result = dict(manifest=p, files=files, images=images)
    Path(output).write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


def generate(config, work, rg, rc):
    cfg = read(config)
    w, rg, rc = map(Path, (work, rg, rc))
    p = cfg["manifest"]
    a = rg / "internal/harness/testdata/snapshot_query_cases.json"
    b = rc / "tests/testdata/snapshot_query_cases.json"
    assert a.read_bytes() == b.read_bytes() and digest(a) == p["corpus"]["sha256"]
    cases = read(a)["cases"]
    assert len(cases) == 636 and sum("prepare_request" in c for c in cases) == 130
    record = read(rg / "internal/harness/testdata/snapshot_query_profile_record.json")
    files = cfg["files"]
    base = dict(version=1, record=record, native_members=[dict(platform="linux/amd64", executable_path=str(w/"native"/name), ffi_path=files["ffi"]) for name in p["qualification"]["native_members"]], clickhouse_executable_path=files["clickhouse"], tzdata_artifact_path=files["tzdata"])
    for role, binary in (("service", rc/"build/clickhousegate_rewriter"), ("test", rc/"build/tests/rewriter_tests"), ("deadline", rc/"build/clickhousegate_rewriter")):
        recipe = json.loads(json.dumps(base))
        recipe["grpc_executable_path"] = str(binary)
        if role == "deadline":
            recipe["record"]["limits"]["max_execution_ms"] = 1
        recipe_path = w / (role + "-recipe.json")
        recipe_path.write_text(json.dumps(recipe, separators=(",", ":")))
        subprocess.run([files["profile_tool"], "-recipe", str(recipe_path), "-profile-out", str(w/(role+"-profiles.json")), "-provenance-out", str(w/(role+"-provenance.json"))], check=True)
        if role != "deadline":
            q = read(w/(role+"-profiles.json"))["profiles"][0]["query_profile_id"]
            subprocess.run(["python3", str(rg/"internal/harness/resolve_snapshot_corpus.py"), "--input", str(a), "--output", str(w/(role+"-corpus.json")), "--query-profile-id", q], check=True)
    bundle = read(w/"service-profiles.json")
    bundle["profiles"] += read(w/"deadline-profiles.json")["profiles"]
    bundle["profiles"].sort(key=lambda x: x["query_profile_id"])
    (w/"rpc-profiles.json").write_text(json.dumps(bundle, separators=(",", ":")))
    tz = w / "timezone"
    tz.mkdir()
    with tarfile.open(files["tzdata"], "r:") as archive:
        members = archive.getmembers()
        assert len([m for m in members if m.isfile()]) == 600
        assert len({m.name for m in members}) == len(members)
        for m in members:
            assert (m.name == "zoneinfo" or m.name.startswith("zoneinfo/")) and (m.isdir() or m.isfile())
            assert not any(x in ("..", ".", "") for x in Path(m.name).parts)
        archive.extractall(tz, filter="data")
    (w/"identities.json").write_text(json.dumps({role: read(w/(role+"-profiles.json")) for role in ("service", "test", "deadline")}, indent=2)+"\n")


def counts(work):
    w = Path(work)
    suite = ET.parse(w/"cpp-run/gtest.xml").getroot()
    assert int(suite.attrib["tests"]) == 1252
    assert all(int(suite.attrib.get(k, "0")) == 0 for k in ("failures", "disabled", "errors"))
    cases = suite.findall(".//testcase")
    assert len(cases) == 1252 and not suite.findall(".//skipped")
    assert sum(c.attrib["classname"] == "SnapshotQuery" for c in cases) == 636
    text = (w/"cpp-run/ctest.log").read_text()
    assert "100% tests passed, 0 tests failed out of 3" in text
    assert not re.search(r"(?:Skipped|Not Run|DISABLED)", text)
    startup = (w/"cpp-run/ctest/Testing/Temporary/LastTest.log").read_text()
    assert "SKIP:" not in startup
    print(json.dumps({"cpp_tests":1252,"snapshot_cases":636,"ctest":3,"failures":0,"skips":0},indent=2))


if __name__ == "__main__":
    {"inputs": inputs, "generate": generate, "counts": counts}[sys.argv[1]](*sys.argv[2:])
