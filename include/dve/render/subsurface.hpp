#pragma once

// Subsurface translucency for the CPU reference: sunlight that enters a Subsurface material on
// its sunlit side, scatters through it and comes out on the shaded side (leaves, ears, wax,
// thin stone).
//
// At a point P facing away from the sun, march from P toward the sun through the same material
// to where the sunlight enters (the exit of the march, E, with outward normal N_E). Then
//   transmitted radiance = subsurfaceColor x sun x cos(N_E, sun) x V(E) x T(d / s)
// where
//   - V(E) is the sun's visibility at E (the far side can itself be shadowed);
//   - d is the thickness: the distance from P to the plane of the sunlit surface at E. For a slab
//     this is the slab's thickness at any sun angle; the sun's slant is already in cos(N_E, sun);
//   - s is the material's subsurfaceScatterDistanceMeters;
//   - T is Burley's normalized-diffusion transmittance, the diffusion profile
//     R(r) = A s (e^(-r/s) + e^(-r/3s)) / (8 pi s^2 r) integrated over a plane at depth d:
//       T(d) = 1/4 e^(-d/s) + 3/4 e^(-d/(3s)),   T(0) = 1
//     (Burley 2015, "Extending the Disney BRDF to a BSDF with Integrated Subsurface Scattering";
//     the same thickness form as Unity HDRP's and Unreal's subsurface-profile transmission).
// Points facing the sun get nothing here: their light is the ordinary diffuse term.

#include <cstdint>

#include "dve/asset_cooker.hpp"
#include "dve/render/voxel_reference_renderer.hpp"
#include "dve/transform.hpp"

namespace dve {
class PolygonBvh;
}

namespace dve::render {

// T(d) above, with d and s in the same units. 0 when s <= 0.
[[nodiscard]] float burley_transmittance(float thickness, float scatterDistance) noexcept;

// Does this material scatter light through itself?
[[nodiscard]] inline bool material_has_subsurface(const VoxelMaterialDefinition& material) noexcept {
    return material.shadingModel == MaterialShadingModel::Subsurface && material.subsurfaceScatterDistanceMeters > 0.0F;
}

// Where a march from inside a subsurface material toward the sun leaves it.
struct SubsurfaceExit {
    bool found{};            // left the material within the maximum distance
    bool open{};             // what lies beyond lets light in (air), not another solid
    Float3 position{};       // world
    Float3 normal{};         // world, outward at the exit (facing the sun side)
    float chord{};           // world units travelled from the start point
    float thickness{};       // world units: distance from the start point to the exit's plane
};

// Voxels: from `point` (inside a cell of `material` in `instance`) along `direction` through cells
// of that same material. Beyond is open when it is air.
[[nodiscard]] SubsurfaceExit march_voxel_subsurface(const VoxelReferenceInstance& instance, MaterialId material,
                                                    Float3 point, Float3 direction, float maximumDistance);

// Polygons: from `point` just inside a closed mesh along `direction` to the first surface of the
// same mesh (shading normals interpolated from the vertex normals, so a smooth mesh is smooth).
[[nodiscard]] SubsurfaceExit march_polygon_subsurface(const PolygonBvh& bvh, const CookedPolygonAsset& asset,
                                                      const RigidTransform& transform, Float3 point, Float3 direction,
                                                      float maximumDistance);

// The transmitted radiance for an exit found by one of the marches, with the far-side sun
// visibility already known. metersPerUnit converts world units to metres. Same units as the
// renderers' direct term (sun x intensity x cosine x albedo).
[[nodiscard]] Float3 subsurface_transmitted_radiance(const VoxelMaterialDefinition& material, const SubsurfaceExit& exit,
                                                     Float3 towardSun, float metersPerUnit, float sunVisibility,
                                                     const RenderEnvironment& environment) noexcept;

} // namespace dve::render
