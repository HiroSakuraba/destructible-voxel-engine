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
# HLSL register bindings are `register(<class><index>)` or, for SM 5.1+ register spaces (which map
# to Vulkan descriptor sets), `register(<class><index>, space<N>)`. The manifest stores the text
# inside the parentheses (scripts/compile_shaders.py checks `register(<value>)` literally), so
# the canonical form is "b0" or "b0, space0".
REGISTER_BODY = r"[^)]*"
CBUFFER = re.compile(rf"\bcbuffer\s+(\w+)\s*:\s*register\(({REGISTER_BODY})\)")
REGISTER_VALUE = re.compile(r"^\s*([tusb])(\d+)\s*(?:,\s*space(\d+)\s*)?$")
RESOURCE = re.compile(
    r"\b(RWStructuredBuffer|StructuredBuffer|RWByteAddressBuffer|ByteAddressBuffer|"
    r"RWTexture\w*|Texture\w*|SamplerComparisonState|SamplerState)"
    rf"(?:\s*<[^;{{}}]+?>)?\s+(\w+)\s*:\s*register\(({REGISTER_BODY})\)"
)


@dataclass(frozen=True, order=True)
class RegisterSlot:
    register_class: str
    index: int
    space: int | None

    @property
    def effective_space(self) -> int:
        # An omitted space is space0 in both D3D12 and DXC's SPIR-V mapping.
        return 0 if self.space is None else self.space

    def canonical(self) -> str:
        base = f"{self.register_class}{self.index}"
        return base if self.space is None else f"{base}, space{self.space}"


def parse_register(value: object) -> RegisterSlot:
    """Parse "b0" / "t3, space1" into a slot; raise ValueError for anything else."""
    if not isinstance(value, str):
        raise ValueError(f"register must be a string, got {value!r}")
    match = REGISTER_VALUE.match(value)
    if not match:
        raise ValueError(f"malformed register {value!r} (expected e.g. 't0' or 't0, space1')")
    register_class, index, space = match.groups()
    return RegisterSlot(register_class, int(index), None if space is None else int(space))


def register_sort_key(register: str) -> tuple[str, int, int]:
    slot = parse_register(register)
    return slot.register_class, slot.index, slot.effective_space


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
    declarations = [(name, "constant_buffer", "read", register) for name, register in CBUFFER.findall(expanded)]
    for type_name, name, register in RESOURCE.findall(expanded):
        kind, access = resource_kind(type_name)
        declarations.append((name, kind, access, register))
    for name, kind, access, register in declarations:
        try:
            slot = parse_register(register)
        except ValueError as exc:
            raise ValueError(f"{path.name}: binding {name}: {exc}") from None
        problem = register_class_problem(kind, access, slot)
        if problem:
            raise ValueError(f"{path.name}: binding {name}: {problem}")
        bindings.append(Binding(name, kind, access, slot.canonical()))
    names: set[str] = set()
    slots: dict[tuple[str, int, int], str] = {}
    for binding in bindings:
        if binding.name in names:
            raise ValueError(f"{path.name}: duplicate binding name {binding.name}")
        slot = parse_register(binding.register)
        key = (slot.register_class, slot.index, slot.effective_space)
        if key in slots:
            raise ValueError(
                f"{path.name}: register collision at {slot.register_class}{slot.index}, "
                f"space{slot.effective_space} ({slots[key]} and {binding.name})")
        names.add(binding.name)
        slots[key] = binding.name
    bindings.sort(key=lambda item: (*register_sort_key(item.register), item.name))
    return threads, bindings


EXPECTED_REGISTER_CLASS = {
    ("constant_buffer", "read"): "b",
    ("storage_buffer", "read"): "t",
    ("storage_buffer", "read_write"): "u",
    ("byte_address_buffer", "read"): "t",
    ("byte_address_buffer", "read_write"): "u",
    ("sampled_texture", "read"): "t",
    ("storage_texture", "read_write"): "u",
    ("sampler", "read"): "s",
}


def register_class_problem(kind: str, access: str, slot: RegisterSlot) -> str | None:
    expected = EXPECTED_REGISTER_CLASS.get((kind, access))
    if expected is None:
        return f"unknown binding kind/access {kind}/{access}"
    if slot.register_class != expected:
        return f"{kind} ({access}) must use a '{expected}' register, not {slot.canonical()!r}"
    return None


def validate_manifest_bindings(source: str, bindings: object) -> tuple[list[dict], list[str]]:
    """Check each manifest binding's register (class, index and optional space) before comparing."""
    if not isinstance(bindings, list):
        return [], [f"{source}: manifest bindings must be a list"]
    errors: list[str] = []
    valid: list[dict] = []
    for item in bindings:
        if not isinstance(item, dict) or not {"name", "kind", "access", "register"} <= set(item):
            errors.append(f"{source}: malformed manifest binding {item!r}")
            continue
        try:
            slot = parse_register(item["register"])
        except ValueError as exc:
            errors.append(f"{source}: manifest binding {item.get('name')}: {exc}")
            continue
        if item["register"] != slot.canonical():
            errors.append(f"{source}: manifest binding {item['name']}: register {item['register']!r} "
                          f"is not canonical (write {slot.canonical()!r})")
            continue
        problem = register_class_problem(str(item["kind"]), str(item["access"]), slot)
        if problem:
            errors.append(f"{source}: manifest binding {item['name']}: {problem}")
            continue
        valid.append(item)
    return sorted(valid, key=lambda item: (*register_sort_key(item["register"]), item["name"])), errors


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

def self_test() -> int:
    """Exercise register parsing (index + optional space) without touching the real manifest."""
    import tempfile

    problems: list[str] = []

    def expect(condition: bool, message: str) -> None:
        if not condition:
            problems.append(message)

    expect(parse_register("b0") == RegisterSlot("b", 0, None), "plain register did not parse")
    expect(parse_register("t12, space3") == RegisterSlot("t", 12, 3), "register with space did not parse")
    expect(parse_register("s7 ,  space2").canonical() == "s7, space2", "register did not canonicalise")
    for bad in ("0, space0", "b", "x1", "b0, space", "b0 space0", "b0, space0, space1", "b-1", 5):
        try:
            parse_register(bad)
        except ValueError:
            continue
        problems.append(f"malformed register {bad!r} was accepted")

    source = """
cbuffer Frame : register(b0, space0) { float4 gValue; };
Texture2D<float4> gAtlas : register(t0, space1);
Texture2D<float4> gPlain : register(t0);
SamplerState gLinear : register(s6, space1);
RWTexture2D<float4> gOut : register(u1);
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {}
"""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "ok.hlsl").write_text(source, encoding="utf-8")
        _threads, bindings = inspect_shader(root / "ok.hlsl", root, "main", "compute")
        got = [(b.name, b.register) for b in bindings]
        want = [("Frame", "b0, space0"), ("gLinear", "s6, space1"), ("gPlain", "t0"),
                ("gAtlas", "t0, space1"), ("gOut", "u1")]
        expect(got == want, f"source bindings {got} != {want}")
        manifest = [binding.to_json() for binding in reversed(bindings)]
        expected, errors = validate_manifest_bindings("ok.hlsl", manifest)
        expect(not errors and expected == [b.to_json() for b in bindings],
               f"manifest with register spaces was rejected: {errors}")
        for register, why in (("0, space0", "missing class"), ("s6,space1", "non-canonical"),
                              ("s6, space1x", "garbage space"), ("t6, space1", "wrong class")):
            broken = [dict(item) for item in manifest]
            target = next(item for item in broken if item["name"] == "gLinear")
            target["register"] = register
            _expected, errors = validate_manifest_bindings("ok.hlsl", broken)
            expect(bool(errors), f"manifest register {register!r} ({why}) was accepted")
        drifted = [dict(item) for item in manifest]
        next(item for item in drifted if item["name"] == "gAtlas")["register"] = "t0, space2"
        expected, errors = validate_manifest_bindings("ok.hlsl", drifted)
        expect(not errors and expected != [b.to_json() for b in bindings], "space drift was not detected")

        # Same index in different spaces is fine; an explicit space0 collides with an implicit one.
        (root / "collide.hlsl").write_text(
            "Texture2D<float4> gA : register(t0);\nTexture2D<float4> gB : register(t0, space0);\n"
            "[numthreads(1, 1, 1)] void main() {}\n", encoding="utf-8")
        try:
            inspect_shader(root / "collide.hlsl", root, "main", "compute")
            problems.append("t0 and t0, space0 collision was not detected")
        except ValueError as exc:
            expect("collision" in str(exc), f"unexpected collision error: {exc}")
        (root / "badclass.hlsl").write_text(
            "cbuffer C : register(t0, space0) { float x; };\n[numthreads(1, 1, 1)] void main() {}\n",
            encoding="utf-8")
        try:
            inspect_shader(root / "badclass.hlsl", root, "main", "compute")
            problems.append("cbuffer bound to a t register was accepted")
        except ValueError:
            pass

    for problem in problems:
        print(f"FAIL: {problem}", file=sys.stderr)
    if problems:
        return 1
    print("dve_shader_contract_validator_self_test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--write-manifest", action="store_true")
    parser.add_argument("--self-test", action="store_true",
                        help="run the validator's own register-parsing tests and exit")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
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
            expected, binding_errors = validate_manifest_bindings(source, record.get("bindings", []))
            if binding_errors:
                failures.extend(binding_errors)
            elif expected != actual_bindings:
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
