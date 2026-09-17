#!/usr/bin/env python3
"""Compare SPIRV-Cross JSON reflection with one DVE shader-manifest record.

This verifies compiled descriptor names, kinds, set numbers, and binding numbers. It deliberately
operates on compiler output; source parsing alone cannot satisfy this check.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

REGISTER = re.compile(r"([buts])(\d+)$")


def expected_kind(binding: dict[str, Any]) -> str:
    kind = binding["kind"]
    if kind == "constant_buffer":
        return "uniform_buffer"
    if kind in {"structured_buffer", "byte_address_buffer"}:
        return "storage_buffer"
    if kind == "storage_texture":
        return "storage_image"
    if kind == "sampler":
        return "sampler"
    if kind == "sampled_texture":
        return "sampled_image"
    raise ValueError(f"unsupported manifest binding kind: {kind}")


def expected_record(binding: dict[str, Any]) -> dict[str, Any]:
    match = REGISTER.fullmatch(binding["register"])
    if not match:
        raise ValueError(f"invalid HLSL register: {binding['register']}")
    return {
        "name": binding["name"],
        "kind": expected_kind(binding),
        "set": 0,
        "binding": int(match.group(2)),
    }


def reflected_records(reflection: dict[str, Any]) -> list[dict[str, Any]]:
    categories = {
        "ubos": "uniform_buffer",
        "ssbos": "storage_buffer",
        "storage_images": "storage_image",
        "separate_images": "sampled_image",
        "sampled_images": "sampled_image",
        "separate_samplers": "sampler",
        "textures": "combined_image_sampler",
    }
    output: list[dict[str, Any]] = []
    for key, kind in categories.items():
        for resource in reflection.get(key, []) or []:
            output.append({
                "name": resource.get("name", ""),
                "kind": kind,
                "set": int(resource.get("set", 0)),
                "binding": int(resource.get("binding", -1)),
            })
    return output


def compare(manifest_record: dict[str, Any], reflection: dict[str, Any]) -> dict[str, Any]:
    expected = [expected_record(binding) for binding in manifest_record.get("bindings", [])]
    actual = reflected_records(reflection)
    failures: list[str] = []
    matched: list[dict[str, Any]] = []
    unmatched_actual = actual.copy()
    for item in expected:
        candidates = [entry for entry in unmatched_actual if entry["name"] == item["name"]]
        if not candidates:
            failures.append(f"missing compiled resource {item['name']}")
            continue
        entry = candidates[0]
        unmatched_actual.remove(entry)
        if entry["kind"] != item["kind"]:
            failures.append(
                f"{item['name']}: kind expected {item['kind']} compiled {entry['kind']}")
        if entry["set"] != item["set"] or entry["binding"] != item["binding"]:
            failures.append(
                f"{item['name']}: location expected set={item['set']} binding={item['binding']} "
                f"compiled set={entry['set']} binding={entry['binding']}")
        matched.append({"expected": item, "compiled": entry})
    for extra in unmatched_actual:
        failures.append(
            f"unexpected compiled resource {extra['name']} ({extra['kind']} "
            f"set={extra['set']} binding={extra['binding']})")
    return {
        "status": "passed" if not failures else "failed",
        "shader": manifest_record.get("name"),
        "matched": matched,
        "failures": failures,
        "expected_count": len(expected),
        "compiled_count": len(actual),
    }


def self_test() -> int:
    record = {
        "name": "fixture",
        "bindings": [
            {"name": "Frame", "kind": "constant_buffer", "register": "b8"},
            {"name": "Albedo", "kind": "sampled_texture", "register": "t18"},
            {"name": "LinearSampler", "kind": "sampler", "register": "s1"},
        ],
    }
    reflection = {
        "ubos": [{"name": "Frame", "set": 0, "binding": 8}],
        "separate_images": [{"name": "Albedo", "set": 0, "binding": 18}],
        "separate_samplers": [{"name": "LinearSampler", "set": 0, "binding": 1}],
    }
    passed = compare(record, reflection)
    if passed["status"] != "passed":
        print(json.dumps(passed, indent=2), file=sys.stderr)
        return 1
    reflection["ubos"][0]["binding"] = 7
    failed = compare(record, reflection)
    if failed["status"] != "failed":
        print("reflection self-test failed to reject binding drift", file=sys.stderr)
        return 1
    print("dve_compiled_shader_reflection_self_test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--shader")
    parser.add_argument("--reflection", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.manifest or not args.shader or not args.reflection:
        parser.error("--manifest, --shader, and --reflection are required")
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    records = [record for record in manifest.get("shaders", []) if record.get("name") == args.shader]
    if len(records) != 1:
        raise SystemExit(f"manifest shader not found or ambiguous: {args.shader}")
    reflection = json.loads(args.reflection.read_text(encoding="utf-8"))
    result = compare(records[0], reflection)
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
