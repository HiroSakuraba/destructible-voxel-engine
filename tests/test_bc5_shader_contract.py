#!/usr/bin/env python3
"""BC5 normal-map shader contract: verify both UV and triplanar code paths.

This is a source-level guard, not an HLSL compiler or GPU image-quality test.
The C++ test covers bind-time format decisions and the numerical reconstruction.
"""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
gpu = (root / "include/dve/gpu_material_mapping.hpp").read_text()
common = (root / "shaders/common/material_mapping.hlsli").read_text()
live = (root / "shaders/common/live_material_sampling.hlsli").read_text()
dashr = (root / "shaders/dashr_shell_pbr_ps.hlsl").read_text()

def require(condition, message):
    if not condition:
        raise AssertionError(message)

require("kGpuMaterialMappingNormalBc5 = 1U << 4U" in gpu, "BC5 C++ flag must be bit 4")
require("kMaterialFlagNormalBc5 = 1u << 4u" in common, "BC5 HLSL flag must be bit 4")
require(re.search(r"float3\s+DecodeMaterialNormalSample\s*\(", common), "HLSL decode helper missing")
require("sqrt(max(0.0f, 1.0f - dot(xy, xy)))" in common, "BC5 Z reconstruction missing")
require("rsqrt(max(dot(n, n), 1.0e-12f))" in common, "BC5 unit normalization missing")
require("(mappingFlags & kMaterialFlagNormalBc5) == 0u" in common,
        "non-BC5 maps must return the legacy normal unchanged")
require("DecodeMaterialNormalSample(" in dashr and "mapping.mappingFlags" in dashr,
        "DASHR derivative normal path missing BC5 guard")
require(live.count("DecodeMaterialNormalSample(") >= 4,
        "both UV and all three triplanar projections must decode BC5")
require("m.mappingFlags" in live, "live normal path missing mapping format flags")
require("const float3 wx=" in live and "const float3 wy=" in live and "const float3 wz=" in live,
        "triplanar whiteout path must be preserved")
print("T4 BC5 shader source contracts: PASS")
