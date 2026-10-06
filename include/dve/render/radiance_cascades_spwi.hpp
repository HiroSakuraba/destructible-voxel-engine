#pragma once

// Phase 2 of docs/RADIANCE_CASCADES.md: a deterministic CPU reference for radiance cascades with
// screen-space probes and world-space intervals (SPWI, Sannikov §4.5) on top of
// ReferenceVoxelRenderer. Probes sit on the primary-hit G-buffer; every interval is traced through
// the authoritative VoxelObject occupancy with the exact DDA of raycast_voxels / TraceVoxelRay;
// radiance at interval hits is one_bounce_diffuse_radiance with an explicit sun-visibility ray.
// The result is the same "indirect" term the one-bounce GI path feeds to shading, so it can be
// certified against a many-sample brute-force estimate of that same integral.
//
// This is validation/reference code, not the production GPU path (Phase 3).

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "dve/query.hpp"
#include "dve/render/ray_lighting_reference.hpp"
#include "dve/render/voxel_reference_renderer.hpp"
#include "dve/render_environment.hpp"

namespace dve::render::spwi {

// ---- Scene tracing -------------------------------------------------------------------------

struct VoxelTraceHit {
    std::uint32_t instance{};
    RayHit objectHit{};
    Float3 worldPosition{};
    Float3 worldNormal{};
};

// Traces a span of VoxelReferenceInstance with results bit-identical to
// raycast_voxels_transformed (and to the renderer's nearest-instance loop). Two exact
// accelerations: occupancy is snapshotted into a dense per-instance array covering the allocated
// bricks, and a ray stops as soon as it has left those bricks' bounds on an axis it is moving
// away from. Nothing survives an edit: build a new tracer after mutating a VoxelObject.
class VoxelSceneTracer {
public:
    explicit VoxelSceneTracer(std::span<const VoxelReferenceInstance> instances,
                              std::uint64_t maximumDenseVoxelsPerInstance = 64ULL << 20U);

    // Nearest hit over visible instances; strict "closer than the current nearest" like the
    // renderer's trace_scene.
    [[nodiscard]] std::optional<VoxelTraceHit> trace(Float3 origin, Float3 direction,
                                                     float maximumDistance) const;
    [[nodiscard]] std::optional<VoxelTraceHit> trace_instance(std::size_t instance, Float3 origin,
                                                              Float3 direction,
                                                              float maximumDistance) const;
    [[nodiscard]] bool occluded(Float3 origin, Float3 direction, float maximumDistance) const {
        return trace(origin, direction, maximumDistance).has_value();
    }

    [[nodiscard]] std::span<const VoxelReferenceInstance> instances() const noexcept {
        return instances_;
    }
    // Indexed by MaterialId; missing entries resolve to the renderer's magenta diagnostic.
    [[nodiscard]] const VoxelMaterialDefinition& material(std::uint32_t instance,
                                                          MaterialId id) const noexcept;
    [[nodiscard]] std::uint64_t dense_bytes() const noexcept;

private:
    struct Grid {
        bool active{};
        bool dense{};
        Int3 minimum{};
        Int3 maximum{}; // inclusive
        Int3 extent{};
        std::vector<MaterialId> cells;
        const VoxelObject* object{};
    };
    [[nodiscard]] std::optional<RayHit> raycast_local(const Grid& grid, Float3 origin,
                                                      Float3 direction,
                                                      float maximumDistance) const;

    std::span<const VoxelReferenceInstance> instances_;
    std::vector<Grid> grids_;
};

// ---- Primary-hit G-buffer ------------------------------------------------------------------

struct VoxelGBufferTexel {
    bool valid{};
    std::uint32_t instance{};
    MaterialId material{};
    Float3 position{};
    Float3 normal{};
    float viewDistance{};
    float normalizedDepth{1.0F};
};

struct VoxelGBuffer {
    std::uint32_t width{};
    std::uint32_t height{};
    Float3 cameraPosition{};
    // World size of one pixel at unit view distance (2·tan(fov/2)/height).
    float pixelWorldScale{};
    std::vector<VoxelGBufferTexel> texels;
};

// Same camera rays, near/far rejection, instance order and depth test as
// ReferenceVoxelRenderer::render. initialDepth (optional, normalized, width·height) plays the
// role of a preserved target depth.
[[nodiscard]] VoxelGBuffer build_voxel_gbuffer(const VoxelSceneTracer& tracer,
                                               const PolygonCamera& camera, std::uint32_t width,
                                               std::uint32_t height,
                                               std::span<const float> initialDepth = {},
                                               std::uint32_t threadCount = 0);

// ---- Radiance model (shared with the one-bounce GI path) -----------------------------------

[[nodiscard]] Float3 environment_radiance(const RenderEnvironment& environment,
                                          Float3 direction) noexcept;

// Outgoing diffuse radiance at a GI/interval hit: one_bounce_diffuse_radiance with the hit's
// hemisphere ambient and a hard sun-visibility ray (GPU: generate_gi_sun_rays + resolve_gi).
// sunRays (optional) counts the visibility rays actually cast (none when n·l <= 0).
//
// Units: tracer positions and hits are world units = voxels. Every `*Meters` distance of the
// environment (GI max distance, shadow max distance, shadow bias) is divided by metersPerVoxel
// (voxel_lighting_distances), like the GPU's MetersToVoxelUnits. The same parameter on the
// solvers below means the same thing.
[[nodiscard]] Float3 bounce_hit_radiance(const VoxelSceneTracer& tracer, const VoxelTraceHit& hit,
                                         const RenderEnvironment& environment,
                                         std::uint64_t* sunRays = nullptr,
                                         float metersPerVoxel = kDefaultMetersPerVoxel);

// ---- Octahedral direction maps (world space, +Y at the centre) -----------------------------

[[nodiscard]] Float3 octahedral_decode(float u, float v) noexcept; // u,v in [0,1]
[[nodiscard]] Float3 octahedral_texel_direction(std::uint32_t tx, std::uint32_t ty,
                                                std::uint32_t resolution) noexcept;
// Exact-to-quadrature solid angle of every texel (sums to 4π).
[[nodiscard]] std::vector<float> octahedral_texel_solid_angles(std::uint32_t resolution);

// ---- Cascades --------------------------------------------------------------------------------

struct CascadeLevelInfo {
    std::uint32_t level{};
    std::uint32_t probeSpacingPixels{};
    std::uint32_t probesX{};
    std::uint32_t probesY{};
    std::uint32_t directionResolution{};
    float intervalStart{};
    float intervalEnd{};
    std::uint64_t texels{};
    std::uint64_t bytes{}; // merged radiance + validity for this level
};

// Interval bounds are reported in world units (voxels) for a probe whose interval unit is
// `worldUnitsPerIntervalUnit` (1 for World scaling; the cascade-0 cell size in world units at
// the nearest probe for ProbeSpacing scaling, which also fixes the cascade count). The top
// interval always ends at globalIlluminationMaxDistanceMeters / metersPerVoxel.
[[nodiscard]] std::vector<CascadeLevelInfo> describe_cascades(
    const RadianceCascadeSettings& settings, const RenderEnvironment& environment,
    std::uint32_t width, std::uint32_t height, float worldUnitsPerIntervalUnit = 1.0F,
    float metersPerVoxel = kDefaultMetersPerVoxel);

struct SolveStats {
    std::uint32_t threads{};
    std::uint32_t cascades{};
    std::uint64_t probes{};
    std::uint64_t intervalRays{};
    std::uint64_t fallbackRays{};
    std::uint64_t sunRays{};
    std::uint64_t gatherFallbackPixels{};
    std::uint64_t peakBytes{};      // radiance storage resident at once (two levels)
    std::uint64_t allLevelsBytes{}; // if every level were kept (GPU-style)
    double milliseconds{};
    std::vector<double> levelMilliseconds; // index = cascade level
    double gatherMilliseconds{};
};

struct IndirectResult {
    std::uint32_t width{};
    std::uint32_t height{};
    // Per-pixel indirect diffuse (already scaled by globalIlluminationIntensity), exactly the
    // term ReferenceVoxelRenderer feeds to shading. Zero for pixels without a primary hit.
    std::vector<Float3> indirect;
    SolveStats stats{};
};

[[nodiscard]] IndirectResult solve_radiance_cascades(const VoxelSceneTracer& tracer,
                                                     const VoxelGBuffer& gbuffer,
                                                     const RenderEnvironment& environment,
                                                     const RadianceCascadeSettings& settings,
                                                     float metersPerVoxel = kDefaultMetersPerVoxel);

// Cosine-weighted Monte Carlo over the full GI max distance with `samples` rays per pixel
// (rotated Hammersley, deterministic), no 16-sample cap. The reference for error metrics.
[[nodiscard]] IndirectResult solve_brute_force_indirect(const VoxelSceneTracer& tracer,
                                                        const VoxelGBuffer& gbuffer,
                                                        const RenderEnvironment& environment,
                                                        std::uint32_t samples,
                                                        std::uint32_t threadCount = 0,
                                                        float metersPerVoxel = kDefaultMetersPerVoxel);

struct IndirectError {
    std::uint64_t pixels{};
    double meanReference{};
    double rmse{};
    double meanAbsolute{};
    double maxAbsolute{};
    double relativeRmse{};
    double relativeMax{};
    double meanBias{}; // mean(test - reference), luminance
};

[[nodiscard]] float luminance(Float3 value) noexcept;
// Luminance errors over pixels with a primary hit (optionally restricted by mask).
[[nodiscard]] IndirectError compare_indirect(const IndirectResult& test,
                                             const IndirectResult& reference,
                                             const VoxelGBuffer& gbuffer,
                                             std::span<const std::uint8_t> mask = {});

// ---- Synthetic voxel scenes (tests and dve_rc_spwi_bench) --------------------------------------
// World units are voxels. The environments are authored in metres for metersPerVoxel = 0.1 (the
// engine default), so a 1-voxel wall is 0.1 m thick and the 4.8 m GI max distance is 48 voxels.

enum class SyntheticScene : std::uint32_t {
    // Open-air floor with coloured walls, an overhang (bounce-lit shade), a block and a pillar.
    Courtyard = 0,
    // Two floor halves split by a 1-voxel wall seen from above. Only an emissive strip on the
    // left lights anything (sun and sky off), so the brute-force answer on the right is exactly 0.
    ThinWall = 1,
    // An enclosed room beside a sunlit yard, viewed from inside; `removable` is a section of the
    // shared 1-voxel wall for the destruction demo.
    Bunker = 2,
};

struct SyntheticSceneSetup {
    std::unique_ptr<VoxelObject> object;
    std::vector<VoxelMaterialDefinition> materials;
    PolygonCamera camera;
    RenderEnvironment environment;
    // World scale the environment's `*Meters` distances were authored for; pass it to the
    // renderer (ReferenceVoxelRenderer::metersPerVoxel) and the solvers.
    float metersPerVoxel{kDefaultMetersPerVoxel};
    Int3 removableMinimum{};
    Int3 removableMaximum{}; // inclusive
    // Voxel x of the thin wall (ThinWall) or the shared wall (Bunker).
    std::int32_t wallX{};

    [[nodiscard]] VoxelReferenceInstance instance() const noexcept {
        return {object ? object->id() : 0U, object.get(), {}, materials, true};
    }
};

[[nodiscard]] SyntheticSceneSetup make_synthetic_scene(SyntheticScene scene);
// Sets every voxel in the inclusive box to air; returns the number of voxels that changed.
std::uint32_t remove_box(VoxelObject& object, Int3 minimum, Int3 maximum);

} // namespace dve::render::spwi
