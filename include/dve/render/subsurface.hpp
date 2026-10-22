#pragma once

// Subsurface translucency for the CPU reference: sunlight that enters a Subsurface material on
// its sunlit side, scatters through it and comes out on the shaded side (leaves, ears, wax,
// thin stone).
//
// At a point P facing away from the sun, march from P toward the sun through the same material
// to where the sunlight enters (the exit of the march, E, with outward normal N_E). Then
//   transmitted radiance = sun x cos(N_E, sun) x V(E) x T(subsurfaceColor, d / s, cos(N_E, sun))
// where
//   - V(E) is the sun's visibility at E (the far side can itself be shadowed);
//   - d is the thickness: the distance from P to the plane of the sunlit surface at E. For a slab
//     this is the slab's thickness at any sun angle; the sun's slant is already in cos(N_E, sun);
//   - s is the material's subsurfaceScatterDistanceMeters;
//   - T is the share of that light which scatters through a slab d / s thick and comes out the
//     other side, per channel for that channel's subsurface colour as albedo, from the
//     random-walk ground truth (subsurface_transmission.hpp; the subsurface colour is inside T).
// Burley's thickness form 1/4 e^(-d/s) + 3/4 e^(-d/3s) (Unity HDRP, Unreal) is kept below for
// comparison; the random walk shows it is far off (docs/subsurface-random-walk-2026-10-22.md).
// Points facing the sun get nothing here: their light is the ordinary diffuse term.

#include <cstdint>
#include <optional>

#include "dve/asset_cooker.hpp"
#include "dve/render/voxel_reference_renderer.hpp"
#include "dve/transform.hpp"

namespace dve {
class PolygonBvh;
}

namespace dve::render {

// Burley's thickness transmittance 1/4 e^(-d/s) + 3/4 e^(-d/(3s)): his diffusion profile
// R(r) = (e^(-r/s) + e^(-r/3s)) / (8 pi s r) summed over the plane at depth d (Burley 2015). d and
// s in the same units; 0 when s <= 0. Not used for shading (see above).
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

// The first surface of a mesh along a ray (both sides count), with its interpolated vertex normal
// as authored (outward) and its material index. Used to place diffusion samples on the surface.
struct SubsurfaceSurfacePoint {
    Float3 position{};
    Float3 normal{};
    std::uint32_t materialIndex{};
};
[[nodiscard]] std::optional<SubsurfaceSurfacePoint> probe_polygon_surface(const PolygonBvh& bvh, const CookedPolygonAsset& asset,
                                                                          const RigidTransform& transform, Float3 origin,
                                                                          Float3 direction, float maximumDistance);

// Front-lit diffusion (Burley's profile R(r) = (e^(-r/d) + e^(-r/3d)) / (8 pi d r), normalised
// to 1 over the plane): the i-th of n sample radii, stratified, drawn exactly from r R(r) 2 pi,
// which is the mixture 1/4 Exp(mean d) + 3/4 Exp(mean 3d). With these, the profile-weighted
// average of anything over the surface is the plain mean of the samples.
[[nodiscard]] float burley_sample_radius(float u, float scatterDistance) noexcept;
// The share of the profile beyond a straight line at distance x (a shadow edge): the exact
// diffused sun term at distance x outside a lit half-plane is that share, and inside it is one
// minus it. Numerical (midpoint rule along each of the profile's two exponentials).
[[nodiscard]] float burley_half_plane_share(float x, float scatterDistance) noexcept;

// The transmitted radiance for an exit found by one of the marches, with the far-side sun
// visibility already known. metersPerUnit converts world units to metres. Same units as the
// renderers' direct term (sun x intensity x cosine x albedo).
[[nodiscard]] Float3 subsurface_transmitted_radiance(const VoxelMaterialDefinition& material, const SubsurfaceExit& exit,
                                                     Float3 towardSun, float metersPerUnit, float sunVisibility,
                                                     const RenderEnvironment& environment) noexcept;

} // namespace dve::render
