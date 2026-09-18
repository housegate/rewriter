#!/usr/bin/env python3
"""Resolve only explicit snapshot Q identity slots in an external corpus copy.

Q is supplied by the offline measured-profile generator; this tool neither
measures nor qualifies an engine. Never rewrite an actual analyzer response.
"""
import argparse
import copy
import json
import pathlib
import re

SLOT = "${QUERY_PROFILE_ID}"

def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key: " + key)
        result[key] = value
    return result

def resolve(corpus, query_profile_id):
    if not re.fullmatch(r"0x[0-9a-f]{64}", query_profile_id):
        raise ValueError("Q must be a lowercase 32-byte digest")
    out = copy.deepcopy(corpus)
    for case in out["cases"]:
        identities = [case["request"], case["expected_response"]]
        if "prepare_request" in case:
            analysis = case["prepare_request"].get("analysis")
            if analysis is not None:
                identities.append(analysis)
            identities.append(case["expected_prepare_response"])
        for message in identities:
            if message.get("query_profile_id") == SLOT:
                message["query_profile_id"] = query_profile_id
    # Slots in SQL/catalog/messages or unknown fields are errors, not substitutions.
    if SLOT in json.dumps(out):
        raise ValueError("profile slot outside an allowed identity field")
    return out

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--query-profile-id", required=True)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error("write a separate resolved copy, never overwrite the source corpus")
    corpus = json.loads(args.input.read_text(), object_pairs_hook=unique_object)
    output = resolve(corpus, args.query_profile_id)
    with args.output.open("x") as stream:
        stream.write(json.dumps(output, ensure_ascii=False, indent=2) + "\n")

if __name__ == "__main__":
    main()
