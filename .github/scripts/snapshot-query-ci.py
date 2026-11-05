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
    assert int(suite.attrib["tests"]) == 2268
    assert all(int(suite.attrib.get(k, "0")) == 0 for k in ("failures", "disabled", "errors"))
    cases = suite.findall(".//testcase")
    assert len(cases) == 2268 and not suite.findall(".//skipped")
    assert sum(c.attrib["classname"] == "SnapshotQuery" for c in cases) == 636
    text = (w/"cpp-run/ctest.log").read_text()
    assert "100% tests passed, 0 tests failed out of 3" in text
    assert not re.search(r"(?:Skipped|Not Run|DISABLED)", text)
    startup = (w/"cpp-run/ctest/Testing/Temporary/LastTest.log").read_text()
    assert "SKIP:" not in startup
    print(json.dumps({"cpp_tests":2268,"snapshot_cases":636,"ctest":3,"failures":0,"skips":0},indent=2))


# The snapshot-query wire surface of rewriter-proto: the two RPCs the lane
# calls (AnalyzeSnapshotQuery, PrepareSnapshotQuery; RC/RG snapshot handlers,
# harness snapshot tests, tests/SNAPSHOT_QUERY.md) and the messages/enums they
# carry. SURFACE_TYPES are the seeds; the check closes over every field type
# they reference, so a referenced type (e.g. MaterializationInputs) is part of
# the surface without being listed. Unknown names fail closed.
SURFACE_RPCS = ("AnalyzeSnapshotQuery", "PrepareSnapshotQuery")
SURFACE_TYPES = ("AnalyzeSnapshotQueryRequest", "AnalyzeSnapshotQueryResponse",
                 "PrepareSnapshotQueryRequest", "PrepareSnapshotQueryResponse",
                 "SnapshotQueryCatalogTable", "SnapshotQueryColumn",
                 "SnapshotQueryColumnGeneration", "SnapshotQueryCode", "SnapshotScratchBinding")
SCALARS = {"double", "float", "int32", "int64", "uint32", "uint64", "sint32", "sint64", "fixed32",
           "fixed64", "sfixed32", "sfixed64", "bool", "string", "bytes"}


def proto_code(line, where):
    """The line without its // comment and with string literals blanked."""
    out, i, quote = [], 0, None
    while i < len(line):
        c = line[i]
        if quote:
            if c == "\\":
                i += 1
            elif c == quote:
                quote = None
            i += 1
            continue
        if line.startswith("//", i):
            break
        if line.startswith("/*", i):
            raise SystemExit("error: unsupported block comment in " + where)
        if c in "\"'":
            quote = c
        else:
            out.append(c)
        i += 1
    if quote:
        raise SystemExit("error: unterminated string literal in " + where)
    return "".join(out)


def proto_blocks(text):
    """Top-level message/enum/service blocks, byte-exact, keyed by name."""
    blocks, lines, i = {}, text.split("\n"), 0
    while i < len(lines):
        m = re.match(r"(message|enum|service)\s+([A-Za-z_]\w*)\s*\{", lines[i])
        if not m:
            if re.match(r"\s*(message|enum|service)\b", lines[i]) and not lines[i].startswith(" "):
                raise SystemExit("error: unrecognised top-level declaration: " + lines[i])
            i += 1
            continue
        start, depth = i, 0
        while True:
            code = proto_code(lines[i], m.group(2))
            depth += code.count("{") - code.count("}")
            i += 1
            if depth == 0:
                break
            if i == len(lines):
                raise SystemExit("error: unterminated block " + m.group(2))
        name = m.group(2)
        if name in blocks:
            raise SystemExit("error: duplicate top-level declaration " + name)
        blocks[name] = (m.group(1), "\n".join(lines[start:i]))
    return blocks


def surface(text):
    blocks = proto_blocks(text)
    header = [l for l in text.split("\n") if re.match(r"(syntax|package)\b", l)]
    if len(header) != 2:
        raise SystemExit("error: rewriter.proto must declare exactly one syntax and one package line")
    out, todo = {}, list(SURFACE_TYPES)
    while todo:
        name = todo.pop()
        if name in out:
            continue
        if name not in blocks or blocks[name][0] == "service":
            raise SystemExit("error: snapshot-query surface type not found: " + name)
        kind, body = blocks[name]
        out[name] = body
        if kind != "message":
            continue
        nested = set(re.findall(r"^\s+(?:message|enum)\s+([A-Za-z_]\w*)", body, re.M))
        for line in body.split("\n")[1:]:
            code = proto_code(line, name).strip()
            f = re.fullmatch(r"(?:repeated\s+|optional\s+)?(map<\s*\w+\s*,\s*[\w.]+\s*>|[\w.]+)\s+\w+\s*=\s*\d+\s*(?:\[[^]]*\])?\s*;", code)
            if not f:
                if code and not re.fullmatch(r"\}|(?:message|enum|oneof)\s+\w+\s*\{|reserved\b.*;", code):
                    raise SystemExit("error: unrecognised field in %s: %s" % (name, code))
                continue
            t = f.group(1)
            m = re.fullmatch(r"map<\s*\w+\s*,\s*([\w.]+)\s*>", t)
            t = m.group(1) if m else t
            if t in SCALARS or t in nested:
                continue
            todo.append(t)
    service = [b for k, b in blocks.values() if k == "service"]
    if len(service) != 1 or not service[0].startswith("service RewriterService {"):
        raise SystemExit("error: rewriter.proto must declare exactly one service RewriterService")
    for rpc in SURFACE_RPCS:
        found = [l.strip() for l in service[0].split("\n") if re.match(r"\s*rpc\s+%s\s*\(" % rpc, l)]
        if len(found) != 1:
            raise SystemExit("error: snapshot-query rpc not found exactly once: " + rpc)
        out["rpc " + rpc] = found[0]
    out["header"] = "\n".join(header)
    return out


def protocol(repo, pinned, head):
    """The snapshot-query surface is byte-identical at the pinned and build proto."""
    show = lambda sha: subprocess.check_output(["git", "-C", repo, "show", sha + ":proto/rewriter.proto"]).decode()
    a, b = surface(show(pinned)), surface(show(head))
    if a.keys() != b.keys():
        raise SystemExit("error: snapshot-query proto surface names differ: %s vs %s" % (sorted(a), sorted(b)))
    changed = sorted(k for k in a if a[k] != b[k])
    if changed:
        raise SystemExit("error: snapshot-query proto surface changed between %s and %s: %s" % (pinned, head, ", ".join(changed)))
    for k in sorted(a):
        print("%s %s" % (hashlib.sha256(a[k].encode()).hexdigest(), k))
    print("snapshot-query proto surface identical: %d blocks, %s -> %s" % (len(a), pinned, head))


if __name__ == "__main__":
    {"inputs": inputs, "generate": generate, "counts": counts, "protocol": protocol}[sys.argv[1]](*sys.argv[2:])
