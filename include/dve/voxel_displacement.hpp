#pragma once

// Triplanar displacement on voxel materials (VoxelMaterialDefinition::heightMap with
// displacementDepthMeters > 0).
//
// The height map carves into each exposed face of a voxel: a point inside the voxel is solid
// only if, below every exposed face, it lies at least (1 - h) x depth under that face, h being the
// height sampled with the face's projection (X -> (z, y), Y -> (x, z), Z -> (x, y), as for the
// texture set). h = 1 is the original face, h = 0 the full depth. The depth is capped at half a
// voxel, so the carving of one voxel never reaches another and neighbours stay consistent.
//
// Two ways to draw it:
//   Traced   - the voxel tracer finds the carved surface itself (trace_displaced_voxel), so
//              outlines, shadows, AO, GI, light shafts and the depth pass all see the relief; a ray
//              that passes through a carved-away part continues to whatever lies behind.
//   Parallax - shading only (voxel_parallax): a parallax-occlusion march on the hit face shifts
//              where the textures are read and tilts the normal; the outline stays cubic. Also
//              the fallback for traced displacement beyond RenderEnvironment::
//              displacementTraceDistanceMeters from the camera.
//
// Positions here are in the voxel's local cell units (voxel indices, 1 per voxel) unless named
// Meters; metresPerUnit converts (cell units x spacing x metres per voxel).

#include <cstdint>
#include <optional>

#include "dve/asset_cooker.hpp"
#include "dve/material_surface_detail.hpp"
#include "dve/types.hpp"

namespace dve {

// Height of the face with outward axis `axis` (0 x, 1 y, 2 z) at an object-space point in metres.
[[nodiscard]] float voxel_face_height(const VoxelMaterialDefinition& material, Float3 positionMeters, int axis, float level) noexcept;

// The carved surface's normal (object space, unit) at a point on the face with outward normal
// `faceNormal` (axis-aligned unit vector): faceNormal - depth x gradient(h), from central
// differences one texel apart at `level`.
[[nodiscard]] Float3 voxel_displaced_normal(const VoxelMaterialDefinition& material, Float3 positionMeters, Float3 faceNormal,
                                            float depthMeters, float level) noexcept;

// The displacement depth actually used, in metres: the material's, capped at half a voxel.
[[nodiscard]] float voxel_displacement_depth(const VoxelMaterialDefinition& material, float metersPerVoxel) noexcept;
// The height source: the height map, or else the material's procedural surface pattern
// (surface_pattern_height, object space), which carves as a solid texture.
[[nodiscard]] inline bool material_has_height_source(const VoxelMaterialDefinition& m) noexcept {
    return m.heightMap != nullptr || surface_pattern_is_height_source(m.surfaceDetail);
}
[[nodiscard]] inline bool material_has_displacement(const VoxelMaterialDefinition& m) noexcept {
    return material_has_height_source(m) && m.displacementDepthMeters > 0.0F;
}

struct VoxelDisplacedHit {
    float t{};                // ray parameter of the hit (same units as the ray's)
    Float3 normal{};          // object-space unit normal of the carved surface
    int axis{-1};             // the face that shaped it (-1: an unexposed side, plain)
};

// The first point along origin + t x direction, t in [tEnter, tExit], inside the carved solid of
// the voxel `cell` (cell units: the voxel spans [cell, cell + 1)^3). exposedFaces bits: 1 -x,
// 2 +x, 4 -y, 8 +y, 16 -z, 32 +z. metersPerCell converts cell units to metres along each axis.
// nullopt when the ray crosses the voxel through carved-away space only.
[[nodiscard]] std::optional<VoxelDisplacedHit> trace_displaced_voxel(const VoxelMaterialDefinition& material, Int3 cell,
                                                                     std::uint8_t exposedFaces, Float3 origin, Float3 direction,
                                                                     float tEnter, float tExit, Float3 metersPerCell, float level,
                                                                     std::uint32_t steps = 24U);

struct VoxelParallaxResult {
    Float3 positionMeters{};  // where to read the textures (object space, metres)
    Float3 normal{};          // object-space unit normal
    float depthMeters{};      // how far below the face the view ray met the height field
    std::uint32_t samples{};
};
// Parallax occlusion on an axis-aligned face. viewDirection points from the surface toward the
// camera (object space, unit).
[[nodiscard]] VoxelParallaxResult voxel_parallax(const VoxelMaterialDefinition& material, Float3 positionMeters, Float3 faceNormal,
                                                 Float3 viewDirection, float depthMeters, float level, std::uint32_t steps = 32U) noexcept;

} // namespace dve
