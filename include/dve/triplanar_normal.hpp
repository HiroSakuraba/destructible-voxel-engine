#pragma once

// Whiteout (UDN-style) triplanar normal blend shared by the polygon renderer's triplanar normal
// maps and voxel normal maps (VoxelMaterialDefinition::normalMap). Mirrored by SampleLiveMaterialNormal in
// shaders/common/live_material_sampling.hlsli; tools/validate_shader_contracts.py keeps the swizzles equal.
//
// tx, ty, tz are the decoded tangent-space samples of the X (u = z, v = y), Y (u = x, v = z) and
// Z (u = x, v = y) projections with their u/v components already scaled by the normal strength.
// Each is added to the geometric normal `n` swizzled into that projection's (u, v, axis) frame,
// swizzled back to world space, weighted and renormalized. The sign of n's axis component flips
// negative faces. A neutral sample (0, 0, 1) reproduces `n` exactly on every projection, so a flat
// map leaves the normal unchanged however the projections are blended.

#include <cmath>

#include "dve/transform.hpp"
#include "dve/types.hpp"

namespace dve {

[[nodiscard]] inline Float3 triplanar_whiteout_normal(Float3 n, Float3 tx, Float3 ty, Float3 tz,
                                                      Float3 weights) noexcept {
    const Float3 wx{std::abs(tx.z)*n.x,tx.y+n.y,tx.x+n.z};
    const Float3 wy{ty.x+n.x,std::abs(ty.z)*n.y,ty.y+n.z};
    const Float3 wz{tz.x+n.x,tz.y+n.y,std::abs(tz.z)*n.z};
    return normalize(add(add(multiply(wx,weights.x),multiply(wy,weights.y)),multiply(wz,weights.z)));
}

} // namespace dve
