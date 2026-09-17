#!/usr/bin/env python3
"""Validate DVE shader inventory, includes, entry points, threads, bindings, and CPU/HLSL mirrors.

This intentionally does not replace DXC/glslang compilation. It closes the failure mode where a
shader or shared include changes while the checked-in manifest and CPU-facing ABI silently drift.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

COMMENT_BLOCK = re.compile(r"/\*.*?\*/", re.S)
COMMENT_LINE = re.compile(r"//.*")
INCLUDE = re.compile(r'^\s*#include\s+"([^"]+)"', re.M)
NUMTHREADS = re.compile(r"\[\s*numthreads\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)\s*\]")
CBUFFER = re.compile(r"\bcbuffer\s+(\w+)\s*:\s*register\(\s*(b\d+)\s*\)")
RESOURCE = re.compile(
    r"\b(RWStructuredBuffer|StructuredBuffer|RWByteAddressBuffer|ByteAddressBuffer|"
    r"RWTexture\w*|Texture\w*|SamplerComparisonState|SamplerState)"
    r"(?:\s*<[^;{}]+?>)?\s+(\w+)\s*:\s*register\(\s*([tusb]\d+)\s*\)"
)


@dataclass(frozen=True)
class Binding:
    name: str
    kind: str
    access: str
    register: str

    def to_json(self) -> dict[str, str]:
        return {"name": self.name, "kind": self.kind, "access": self.access, "register": self.register}


def clean(text: str) -> str:
    return "\n".join(COMMENT_LINE.sub("", line) for line in COMMENT_BLOCK.sub("", text).splitlines())


def include_closure(path: Path, shader_root: Path, visited: set[Path] | None = None) -> str:
    visited = set() if visited is None else visited
    resolved = path.resolve()
    if resolved in visited:
        return ""
    if shader_root.resolve() not in resolved.parents and resolved != shader_root.resolve():
        raise ValueError(f"include escapes shader root: {path}")
    if not resolved.is_file():
        raise ValueError(f"missing shader include: {path}")
    visited.add(resolved)
    text = path.read_text(encoding="utf-8")
    chunks: list[str] = []
    cursor = 0
    for match in INCLUDE.finditer(text):
        chunks.append(text[cursor:match.start()])
        chunks.append(include_closure(path.parent / match.group(1), shader_root, visited))
        cursor = match.end()
    chunks.append(text[cursor:])
    return "\n".join(chunks)


def resource_kind(type_name: str) -> tuple[str, str]:
    if type_name == "StructuredBuffer":
        return "storage_buffer", "read"
    if type_name == "RWStructuredBuffer":
        return "storage_buffer", "read_write"
    if type_name == "ByteAddressBuffer":
        return "byte_address_buffer", "read"
    if type_name == "RWByteAddressBuffer":
        return "byte_address_buffer", "read_write"
    if type_name.startswith("RWTexture"):
        return "storage_texture", "read_write"
    if type_name.startswith("Texture"):
        return "sampled_texture", "read"
    return "sampler", "read"


def inspect_shader(path: Path, shader_root: Path, entry: str, stage: str) -> tuple[list[int] | None, list[Binding]]:
    expanded = clean(include_closure(path, shader_root))
    if not re.search(rf"\b\w+(?:<[^>]+>)?\s+{re.escape(entry)}\s*\(", expanded):
        raise ValueError(f"{path.name}: entry point {entry!r} was not found")
    thread_matches = NUMTHREADS.findall(expanded)
    if stage == "compute":
        if len(thread_matches) != 1:
            raise ValueError(f"{path.name}: expected exactly one numthreads declaration, found {len(thread_matches)}")
        threads: list[int] | None = [int(value) for value in thread_matches[0]]
    else:
        if thread_matches:
            raise ValueError(f"{path.name}: graphics shader unexpectedly declares numthreads")
        threads = None
    bindings: list[Binding] = []
    for name, register in CBUFFER.findall(expanded):
        bindings.append(Binding(name, "constant_buffer", "read", register))
    for type_name, name, register in RESOURCE.findall(expanded):
        kind, access = resource_kind(type_name)
        bindings.append(Binding(name, kind, access, register))
    names: set[str] = set()
    registers: set[str] = set()
    for binding in bindings:
        if binding.name in names:
            raise ValueError(f"{path.name}: duplicate binding name {binding.name}")
        if binding.register in registers:
            raise ValueError(f"{path.name}: register collision at {binding.register}")
        names.add(binding.name)
        registers.add(binding.register)
    bindings.sort(key=lambda item: (item.register[0], int(item.register[1:]), item.name))
    return threads, bindings


def validate_material_abi(root: Path) -> list[str]:
    errors: list[str] = []
    cpp = (root / "include/dve/gpu_material_buffer.hpp").read_text(encoding="utf-8")
    hlsl = (root / "shaders/common/material_record.hlsli").read_text(encoding="utf-8")
    cpp_body = re.search(r"struct\s+GpuMaterialRecord\s*\{(.*?)^\};\s*$", cpp, re.S | re.M)
    hlsl_body = re.search(r"struct\s+MaterialRecord\s*\{(.*?)^\};\s*$", hlsl, re.S | re.M)
    if not cpp_body or not hlsl_body:
        return ["could not locate GPU material record declarations"]
    cpp_fields = [("uint" if t == "std::uint32_t" else t, n)
                  for t, n in re.findall(r"\b(float|std::uint32_t)\s+(\w+)\s*(?:\{[^;]*\})?\s*;", cpp_body.group(1))]
    hlsl_fields = re.findall(r"\b(float|uint)\s+(\w+)\s*;", hlsl_body.group(1))
    if cpp_fields != hlsl_fields:
        errors.append(f"GpuMaterialRecord/MaterialRecord field drift: C++={cpp_fields}, HLSL={hlsl_fields}")

    cpp_values = dict(re.findall(
        r"MaterialShadingModel::(\w+)\)\s*==\s*(\d+)",
        (root / "src/gpu_material_buffer.cpp").read_text(encoding="utf-8")))
    hlsl_values = dict(re.findall(r"kShadingModel(\w+)\s*=\s*(\d+)u", hlsl))
    if cpp_values != hlsl_values:
        errors.append(f"material shading-model numeric drift: C++={cpp_values}, HLSL={hlsl_values}")

    limits = (root / "include/dve/material_parameter_collection.hpp").read_text(encoding="utf-8")
    scalar = re.search(r"kMaximumMaterialGlobalScalars\s*=\s*(\d+)", limits)
    vector = re.search(r"kMaximumMaterialGlobalVectors\s*=\s*(\d+)", limits)
    mpc = (root / "shaders/common/material_parameter_collection.hlsli").read_text(encoding="utf-8")
    scalar_groups = re.search(r"gMaterialGlobalScalarGroups\[(\d+)\]", mpc)
    vector_slots = re.search(r"gMaterialGlobalVectors\[(\d+)\]", mpc)
    if not all((scalar, vector, scalar_groups, vector_slots)):
        errors.append("could not locate material parameter collection capacity declarations")
    elif int(scalar.group(1)) != int(scalar_groups.group(1)) * 4 or int(vector.group(1)) != int(vector_slots.group(1)):
        errors.append("material parameter collection C++/HLSL capacity drift")
    return errors




def validate_material_mapping_abi(root: Path) -> list[str]:
    errors: list[str] = []
    cpp = (root / "include/dve/gpu_material_mapping.hpp").read_text(encoding="utf-8")
    hlsl = (root / "shaders/common/material_mapping.hlsli").read_text(encoding="utf-8")
    cpp_body = re.search(r"struct\s+GpuPolygonMaterialMappingRecord\s*\{(.*?)^\};\s*$", cpp, re.S | re.M)
    hlsl_body = re.search(r"struct\s+PolygonMaterialMappingRecord\s*\{(.*?)^\};\s*$", hlsl, re.S | re.M)
    if not cpp_body or not hlsl_body:
        return ["could not locate polygon material mapping GPU record declarations"]
    cpp_fields = [("uint" if t == "std::uint32_t" else t, n)
                  for t, n in re.findall(r"\b(float|std::uint32_t)\s+(\w+)\s*(?:\{[^;]*\})?\s*;", cpp_body.group(1))]
    hlsl_fields = re.findall(r"\b(float|uint)\s+(\w+)\s*;", hlsl_body.group(1))
    if cpp_fields != hlsl_fields:
        errors.append(f"GpuPolygonMaterialMappingRecord/HLSL field drift: C++={cpp_fields}, HLSL={hlsl_fields}")
    source = (root / "src/gpu_material_mapping.cpp").read_text(encoding="utf-8")
    cpp_mapping = dict(re.findall(r"MaterialMappingMode::(\w+)\)\s*==\s*(\d+)U", source))
    hlsl_mapping = dict(re.findall(r"kMaterialMapping(\w+)\s*=\s*(\d+)u", hlsl))
    if cpp_mapping != hlsl_mapping:
        errors.append(f"material mapping mode numeric drift: C++={cpp_mapping}, HLSL={hlsl_mapping}")
    cpp_height = dict(re.findall(r"HeightMappingMode::(\w+)\)\s*==\s*(\d+)U", source))
    hlsl_height = dict(re.findall(r"kHeightMapping(\w+)\s*=\s*(\d+)u", hlsl))
    if cpp_height != hlsl_height:
        errors.append(f"height mapping mode numeric drift: C++={cpp_height}, HLSL={hlsl_height}")
    return errors

def validate_render_environment_abi(root: Path) -> list[str]:
    errors: list[str] = []
    cpp = (root / "include/dve/gpu_render_environment.hpp").read_text(encoding="utf-8")
    hlsl = (root / "shaders/common/render_environment.hlsli").read_text(encoding="utf-8")
    cpp_body = re.search(r"struct\s+GpuRenderEnvironment\s*\{(.*?)^\};\s*$", cpp, re.S | re.M)
    hlsl_body = re.search(r"cbuffer\s+RenderEnvironmentConstants\s*:\s*register\([^)]*\)\s*\{(.*?)^\}", hlsl, re.S | re.M)
    if not cpp_body or not hlsl_body:
        return ["could not locate render environment declarations"]

    cpp_fields: list[tuple[str, str]] = []
    for type_name, declarations in re.findall(r"\b(float|std::uint32_t)\s+([^;]+);", cpp_body.group(1)):
        normalized_type = "uint" if type_name == "std::uint32_t" else "float"
        for declaration in declarations.split(","):
            match = re.match(r"\s*(\w+)", declaration)
            if match:
                cpp_fields.append((normalized_type, match.group(1)))

    vector_map = {
        "gSunDirection": ["sunDirectionX", "sunDirectionY", "sunDirectionZ"],
        "gSunColor": ["sunColorR", "sunColorG", "sunColorB"],
        "gSkyColor": ["skyColorR", "skyColorG", "skyColorB"],
        "gGroundColor": ["groundColorR", "groundColorG", "groundColorB"],
        "gGlobalTint": ["globalTintR", "globalTintG", "globalTintB"],
    }
    scalar_map = {
        "gSunIntensity": "sunIntensity",
        "gSubsurfaceMaxDistanceMeters": "subsurfaceMaxDistanceMeters",
        "gExposure": "exposure",
        "gTonemapOperator": "tonemapOperator",
        "gBloomThreshold": "bloomThreshold",
        "gBloomIntensity": "bloomIntensity",
        "gBloomRadius": "bloomRadius",
        "gImageWidth": "imageWidth",
        "gImageHeight": "imageHeight",
        "gGlobalIlluminationMode": "globalIlluminationMode",
        "gGlobalIlluminationIntensity": "globalIlluminationIntensity",
        "gGlobalIlluminationMaxDistanceMeters": "globalIlluminationMaxDistanceMeters",
        "gGlobalIlluminationSamples": "globalIlluminationSamples",
        "gShadowMode": "shadowMode",
        "gShadowStrength": "shadowStrength",
        "gShadowSoftnessRadians": "shadowSoftnessRadians",
        "gShadowSamples": "shadowSamples",
        "gShadowMaxDistanceMeters": "shadowMaxDistanceMeters",
        "gContactShadowDistanceMeters": "contactShadowDistanceMeters",
        "gShadowBiasMeters": "shadowBiasMeters",
        "gMetersPerVoxel": "metersPerVoxel",
    }
    expected: list[tuple[str, str]] = []
    for type_name, name in re.findall(r"\b(float3|float|uint)\s+(\w+)\s*;", clean(hlsl_body.group(1))):
        if type_name == "float3":
            names = vector_map.get(name)
            if not names:
                errors.append(f"unmapped float3 render environment field {name}")
                continue
            expected.extend(("float", item) for item in names)
        else:
            mapped = scalar_map.get(name)
            if not mapped:
                errors.append(f"unmapped scalar render environment field {name}")
                continue
            expected.append((type_name, mapped))
    if cpp_fields != expected:
        errors.append(f"GpuRenderEnvironment/HLSL field drift: C++={cpp_fields}, HLSL={expected}")

    source = (root / "src/gpu_render_environment.cpp").read_text(encoding="utf-8")
    cpp_gi = dict(re.findall(r"GlobalIlluminationMode::(\w+)\)\s*==\s*(\d+)", source))
    hlsl_gi = dict(re.findall(r"kGlobalIllumination(\w+)\s*=\s*(\d+)u", hlsl))
    if cpp_gi != hlsl_gi:
        errors.append(f"GI mode numeric drift: C++={cpp_gi}, HLSL={hlsl_gi}")
    cpp_shadow = dict(re.findall(r"ShadowMode::(\w+)\)\s*==\s*(\d+)", source))
    hlsl_shadow = dict(re.findall(r"kShadow(\w+)\s*=\s*(\d+)u", hlsl))
    if cpp_shadow != hlsl_shadow:
        errors.append(f"shadow mode numeric drift: C++={cpp_shadow}, HLSL={hlsl_shadow}")
    return errors


def validate_brick_palette_resolve_abi(root: Path) -> list[str]:
    errors: list[str] = []
    cpp = (root / "include/dve/render/brick_palette_gpu_resolve.hpp").read_text(encoding="utf-8")
    hlsl = (root / "shaders/common/brick_palette_resolve.hlsli").read_text(encoding="utf-8")
    for struct_name in ("BrickPaletteGpuMaterialRecord", "BrickPaletteGpuResolveRequest"):
        cpp_body = re.search(rf"struct\s+{struct_name}\s*\{{(.*?)^\}};", cpp, re.S | re.M)
        hlsl_body = re.search(rf"struct\s+{struct_name}\s*\{{(.*?)^\}};", hlsl, re.S | re.M)
        if not cpp_body or not hlsl_body:
            errors.append(f"could not locate {struct_name} C++/HLSL declarations")
            continue
        cpp_fields = [("uint" if type_name == "std::uint32_t" else type_name, name)
                      for type_name, name in re.findall(
                          r"\b(float|std::uint32_t)\s+(\w+)\s*(?:\{[^;]*\})?\s*;",
                          cpp_body.group(1))]
        hlsl_fields = re.findall(r"\b(float|uint)\s+(\w+)\s*;", hlsl_body.group(1))
        if cpp_fields != hlsl_fields:
            errors.append(
                f"{struct_name} C++/HLSL field drift: C++={cpp_fields}, HLSL={hlsl_fields}")
    return errors


def validate_camera_cinematic_abi(root: Path) -> list[str]:
    errors: list[str] = []
    cpp = (root / "include/dve/camera_runtime.hpp").read_text(encoding="utf-8")
    hlsl = (root / "shaders/common/camera_cinematic.hlsli").read_text(encoding="utf-8")
    cpp_body = re.search(r"struct\s+CameraGpuPacket\s*\{(.*?)^\};", cpp, re.S | re.M)
    hlsl_body = re.search(r"struct\s+CameraGpuPacket\s*\{(.*?)^\};", hlsl, re.S | re.M)
    if not cpp_body or not hlsl_body:
        return ["could not locate CameraGpuPacket C++/HLSL declarations"]

    cpp_fields: list[tuple[str, str, int]] = []
    for count, name in re.findall(
            r"std::array<float,\s*(\d+)>\s+(\w+)\s*(?:\{[^;]*\})?\s*;",
            cpp_body.group(1)):
        value_count = int(count)
        if value_count not in (4, 16):
            errors.append(f"unsupported CameraGpuPacket float array width {value_count} for {name}")
        cpp_fields.append(("float4", name, value_count // 4))
    cpp_fields.extend(("uint", name, 1) for name in re.findall(
        r"std::uint32_t\s+(\w+)\s*(?:\{[^;]*\})?\s*;", cpp_body.group(1)))

    hlsl_fields: list[tuple[str, str, int]] = []
    for type_name, name, array_count in re.findall(
            r"\b(float4|uint)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*;",
            hlsl_body.group(1)):
        hlsl_fields.append((type_name, name, int(array_count) if array_count else 1))
    if cpp_fields != hlsl_fields:
        errors.append(f"CameraGpuPacket C++/HLSL field drift: C++={cpp_fields}, HLSL={hlsl_fields}")

    cpp_size = sum((16 * count) if type_name == "float4" else 4
                   for type_name, _name, count in cpp_fields)
    hlsl_size = sum((16 * count) if type_name == "float4" else 4
                    for type_name, _name, count in hlsl_fields)
    if cpp_size != hlsl_size:
        errors.append(f"CameraGpuPacket byte-size drift: C++={cpp_size}, HLSL={hlsl_size}")
    return errors

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--write-manifest", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    shader_root = root / "shaders"
    manifest_path = shader_root / "shader_manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    failures: list[str] = []

    records = manifest.get("shaders", [])
    by_source = {record.get("source"): record for record in records}
    disk_sources = sorted(path.name for path in shader_root.glob("*.hlsl"))
    if sorted(by_source) != disk_sources:
        failures.append(f"shader inventory mismatch: manifest={sorted(by_source)}, disk={disk_sources}")

    for source in disk_sources:
        record = by_source.get(source)
        if not record:
            continue
        stage = str(record.get("stage", ""))
        profile = str(record.get("profile", ""))
        expected_prefix = {"compute": "cs_", "vertex": "vs_", "pixel": "ps_"}.get(stage)
        if expected_prefix is None or not profile.startswith(expected_prefix):
            failures.append(f"{source}: stage/profile mismatch stage={stage!r} profile={profile!r}")
            continue
        entry = str(record.get("entry", ""))
        try:
            threads, bindings = inspect_shader(shader_root / source, shader_root, entry, stage)
        except ValueError as exc:
            failures.append(str(exc))
            continue
        actual_bindings = [binding.to_json() for binding in bindings]
        if args.write_manifest:
            if threads is None:
                record.pop("threads", None)
            else:
                record["threads"] = threads
            record["bindings"] = actual_bindings
        else:
            if threads is not None and record.get("threads") != threads:
                failures.append(f"{source}: numthreads manifest={record.get('threads')} source={threads}")
            if threads is None and "threads" in record:
                failures.append(f"{source}: graphics shader must not declare manifest threads")
            expected = sorted(record.get("bindings", []), key=lambda item: (item["register"][0], int(item["register"][1:]), item["name"]))
            if expected != actual_bindings:
                failures.append(f"{source}: binding manifest drift\n  manifest={expected}\n  source={actual_bindings}")

    failures.extend(validate_material_abi(root))
    failures.extend(validate_material_mapping_abi(root))
    failures.extend(validate_render_environment_abi(root))
    failures.extend(validate_brick_palette_resolve_abi(root))
    failures.extend(validate_camera_cinematic_abi(root))
    if args.write_manifest and not failures:
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"dve_shader_contract_validation: PASS ({len(disk_sources)} shaders)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
