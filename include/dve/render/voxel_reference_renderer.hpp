#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include <string>

#include "dve/asset_cooker.hpp"
#include "dve/render/volumetric_fog.hpp"
#include "dve/render/ambient_occlusion.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/surface_marks.hpp"
#include "dve/transform.hpp"
#include "dve/voxel_object.hpp"

namespace dve { class PolygonBvh; }

namespace dve::render {
class SceneLighting;

class CausticMap;
namespace spwi { class VoxelSceneTracer; }

// CPU reference path for validating the shared voxel/polygon camera-depth contract. It traces
// authoritative VoxelObject occupancy directly and writes the same HDR/depth/ID target used by
// ReferencePolygonRenderer. This is validation and editor-preview code, not the production GPU
// brickmap tracer.
struct VoxelReferenceInstance {
    std::uint64_t objectId{};
    const VoxelObject* object{};
    RigidTransform transform{};
    // Indexed by MaterialId. Missing entries use a visible magenta diagnostic material.
    std::span<const VoxelMaterialDefinition> materials{};
    bool visible{true};
    Float3 spacing{1,1,1}; // local cell dimensions in scene world units
};

struct VoxelReferenceRenderStats {
    std::uint64_t submittedInstances{};
    std::uint64_t tracedRays{};
    std::uint64_t hitRays{};
    std::uint64_t materialFallbacks{};
    std::uint64_t layeredSurfaces{};   // pixels where a material layer covered the surface
    std::uint64_t markedSurfaces{};    // pixels a damage mark shaded
    std::uint64_t decalSurfaces{};     // pixels a decal covered
    std::uint64_t shadowRays{};
    std::uint64_t shadowBlockedRays{};
    std::uint64_t globalIlluminationRays{};
    std::uint64_t globalIlluminationHits{};
    // Next-event sun-visibility rays cast from GI bounce hits (one per sunlit bounce hit),
    // mirroring generate_gi_sun_rays.hlsl. Counted separately from primary shadowRays.
    std::uint64_t globalIlluminationSunRays{};
    // GlobalIlluminationMode::RadianceCascades only: interval rays traced by the CPU SPWI solve
    // (including bilinear-fix and fallback rays).
    std::uint64_t radianceCascadeIntervalRays{};
    // Subsurface translucency: marches from shaded-side Subsurface hits toward the sun, and how
    // many of them reached an open sunlit side within the subsurface max distance.
    std::uint64_t subsurfaceMarches{};
    std::uint64_t subsurfaceExits{};
    // Subsurface diffusion: probe rays placing profile samples on the surface.
    std::uint64_t subsurfaceDiffusionProbes{};
    // Ambient-occlusion rays (AmbientHemisphere ambient only).
    std::uint64_t ambientOcclusionRays{};
};

// CPU screen-probe / world-space-interval (SPWI) radiance cascades. See
// dve/render/radiance_cascades_spwi.hpp and docs/RADIANCE_CASCADES.md (Phase 2).
enum class RadianceCascadeMerge : std::uint32_t {
    // One ray per interval; the upper cascade is blended with depth/normal-aware weights.
    Vanilla = 0,
    // One ray per (interval, upper probe): each ray ends where that upper probe's interval
    // starts, so occluders between the probes are seen (Osborne & Sannikov 2024 §2.5).
    BilinearFix = 1,
};

enum class RadianceCascadeIntervalScaling : std::uint32_t {
    // baseIntervalLength is in world units for every probe.
    World = 0,
    // baseIntervalLength is in units of the probe's cascade-0 cell size in world space
    // (baseProbeSpacingPixels pixels at the probe's view distance), so every interval keeps the
    // same ratio to the screen-probe spacing at any depth (penumbra condition).
    ProbeSpacing = 1,
};

struct RadianceCascadeSettings {
    // Cascade i places screen probes every baseProbeSpacingPixels·2^i pixels on the primary-hit
    // G-buffer and stores a (baseDirectionResolution·2^i)^2 world-space octahedral direction map
    // (full sphere; only texels above the probe's surface are traced). Defaults come from the
    // Phase 2 sweep in docs/RADIANCE_CASCADES.md.
    std::uint32_t baseProbeSpacingPixels{2};
    std::uint32_t baseDirectionResolution{8};
    // Interval i covers [L0·(g^i-1)/(g-1), L0·(g^(i+1)-1)/(g-1)) world units along each
    // direction; the top interval always ends at globalIlluminationMaxDistanceMeters (converted
    // to world units). World units are the reference path's voxel-index units (1 voxel = 1
    // unit), so L0 = 1 is one voxel, whatever ReferenceVoxelRenderer::metersPerVoxel is.
    float baseIntervalLength{1.0F};
    float intervalGrowth{4.0F};
    RadianceCascadeIntervalScaling intervalScaling{RadianceCascadeIntervalScaling::World};
    // Extends each lower interval by this many upper-probe world spacings (leak fix).
    float intervalOverlap{1.0F};
    // 0 = as many cascades as needed to reach the GI max distance (capped at 10).
    std::uint32_t maximumCascades{0};
    // Default by decision (docs/RADIANCE_CASCADES.md §6): the bilinear fix does not leak through
    // 1-voxel walls at low resolution, where vanilla + overlap does.
    RadianceCascadeMerge merge{RadianceCascadeMerge::BilinearFix};
    // Bilateral weights: plane distance tolerance in upper-probe world spacings, and the power
    // applied to max(0, dot(n_lower, n_upper)).
    float depthToleranceProbeSpacings{1.0F};
    float normalPower{8.0F};
    // 0 = std::thread::hardware_concurrency() (capped at 16). Output is identical for any count.
    std::uint32_t threadCount{0};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

class ReferenceVoxelRenderer {
public:
    const SceneLighting* sceneLighting{}; // shared frame integrator; outlives render
    // Used only when RenderEnvironment::globalIlluminationMode is RadianceCascades.
    RadianceCascadeSettings radianceCascades{};
    // Scene world scale. The reference path traces in voxel-index units (instance transforms are
    // rigid, so 1 world unit = 1 voxel); every RenderEnvironment `*Meters` distance (GI max
    // distance, shadow max/contact distance, shadow bias) is divided by this, exactly like the
    // GPU's MetersToVoxelUnits with GpuRenderEnvironment::metersPerVoxel. Non-positive or
    // non-finite values fall back to kDefaultMetersPerVoxel (0.1 m).
    float metersPerVoxel{kDefaultMetersPerVoxel};
    // Worker threads for the primary/shadow/GI pass (rows are split across threads).
    // 0 = std::thread::hardware_concurrency() (capped at 16). Output is identical for any count.
    std::uint32_t threadCount{0};
    // Caustics (optional, see caustic_map.hpp): sunlight focused by glass and water, added to the
    // sun term of every voxel it reaches. Must outlive render().
    const CausticMap* caustics{};
    // Polygons that block the sun on voxels (otherwise only voxels cast shadows here). Glass and
    // water block it too: the light they let through arrives as caustics.
    std::span<const PolygonRenderInstance> sunOccluders{};
    // Subsurface diffusion (front-lit light spreading under the surface with Burley's profile,
    // see subsurface.hpp): samples per Subsurface pixel. 0 = off (the sun term at the point
    // only). Off as well when the environment's subsurface max distance is 0.
    std::uint32_t subsurfaceDiffusionSamples{16};
    // Polygons that ambient-occlusion rays see, besides the voxels and sunOccluders (the hybrid
    // reference and the CPU player pass their polygons here). Must outlive render().
    std::span<const PolygonRenderInstance> occlusionPolygons{};
    // Damage marks (dve/surface_marks.hpp), matched to instances by objectId, in each object's
    // own metres; and decals (dve/material_decal.hpp), box-projected in scene world units (voxel
    // units here). Both shade only: after the material's maps and layers. Must outlive render().
    std::span<const SurfaceMark> surfaceMarks{};
    std::span<const MaterialDecal> decals{};

    [[nodiscard]] VoxelReferenceRenderStats render(
        std::span<const VoxelReferenceInstance> instances,
        const PolygonCamera& camera,
        const RenderEnvironment& environment,
        PolygonRenderTarget& target,
        bool preserveExistingDepth = false) const;
};

// The reference path's sun visibility at one surface point: the function every reference pixel
// uses (hard, contact, soft-area or hybrid per RenderEnvironment::shadowMode, then
// shadowStrength). Soft shadows send shadowSamples rays to points spread over the sun disc, so the
// result is the visible fraction of the disc and overlapping penumbrae merge as the union of what
// each occluder hides. `seed` rotates the sample pattern (each pixel uses its own); `point` and
// `normal` are in the reference's world units (voxel index units of rigid instances).
[[nodiscard]] float reference_sun_visibility(
    std::span<const VoxelReferenceInstance> instances, Float3 point, Float3 normal,
    const RenderEnvironment& environment, float metersPerVoxel = kDefaultMetersPerVoxel,
    std::uint32_t seed = 0U);

// Nearest hits over voxel instances and polygon meshes together, for the ambient-occlusion rays
// of a hybrid image (voxels and polygons occlude each other). Both spans must outlive it.
class HybridOcclusionScene {
public:
    HybridOcclusionScene(std::span<const VoxelReferenceInstance> voxels, std::span<const PolygonRenderInstance> polygons);
    ~HybridOcclusionScene();
    HybridOcclusionScene(const HybridOcclusionScene&) = delete;
    HybridOcclusionScene& operator=(const HybridOcclusionScene&) = delete;
    [[nodiscard]] std::optional<float> nearest(Float3 origin, Float3 direction, float maximumDistance) const;
    // Traced displacement for these rays (spwi::VoxelSceneTracer::set_displacement).
    void set_displacement(float metersPerVoxel, Float3 focus, float focusRadius) noexcept;
    // For PolygonRenderOptions::occlusionRay; valid while this scene lives.
    [[nodiscard]] AmbientOcclusionRay ray() const;

private:
    std::unique_ptr<spwi::VoxelSceneTracer> tracer_;
    std::vector<std::pair<std::shared_ptr<const PolygonBvh>, RigidTransform>> polygons_;
};

// Direct hybrid reference composition. Voxel occupancy is traced into the shared target first;
// polygons then depth-test and alpha-blend against that target. This validates polygon-in-front,
// voxel-in-front, and translucent-over-voxel behavior without a synthetic voxel layer. In
// Physical sky mode the sky is painted behind the voxels before the polygons are drawn
// (paint_sky_background) and aerial perspective is added after; then volumetric fog
// (apply_volumetric_fog, shadow rays through voxels and polygons); the environment's distance
// fog is applied last (apply_distance_fog). Render passes are filled when
// the target has them enabled.
struct HybridReferenceRenderStats {
    VoxelReferenceRenderStats voxels{};
    PolygonRenderStats polygons{};
    VolumetricFogStats volumetricFog{};
};

[[nodiscard]] HybridReferenceRenderStats render_hybrid_reference(
    std::span<const VoxelReferenceInstance> voxels,
    std::span<const PolygonRenderInstance> polygons,
    const PolygonCamera& camera,
    const RenderEnvironment& environment,
    PolygonRenderTarget& target,
    const PolygonRenderOptions& polygonOptions = {},
    float metersPerVoxel = kDefaultMetersPerVoxel,
    std::uint32_t voxelThreadCount = 0);

} // namespace dve::render
