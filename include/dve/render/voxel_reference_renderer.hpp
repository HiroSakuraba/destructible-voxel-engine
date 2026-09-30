#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "dve/asset_cooker.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/transform.hpp"
#include "dve/voxel_object.hpp"

namespace dve::render {

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
};

struct VoxelReferenceRenderStats {
    std::uint64_t submittedInstances{};
    std::uint64_t tracedRays{};
    std::uint64_t hitRays{};
    std::uint64_t materialFallbacks{};
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
    // Used only when RenderEnvironment::globalIlluminationMode is RadianceCascades.
    RadianceCascadeSettings radianceCascades{};
    // Scene world scale. The reference path traces in voxel-index units (instance transforms are
    // rigid, so 1 world unit = 1 voxel); every RenderEnvironment `*Meters` distance (GI max
    // distance, shadow max/contact distance, shadow bias) is divided by this, exactly like the
    // GPU's MetersToVoxelUnits with GpuRenderEnvironment::metersPerVoxel. Non-positive or
    // non-finite values fall back to kDefaultMetersPerVoxel (0.1 m).
    float metersPerVoxel{kDefaultMetersPerVoxel};

    [[nodiscard]] VoxelReferenceRenderStats render(
        std::span<const VoxelReferenceInstance> instances,
        const PolygonCamera& camera,
        const RenderEnvironment& environment,
        PolygonRenderTarget& target,
        bool preserveExistingDepth = false) const;
};

// Direct hybrid reference composition. Voxel occupancy is traced into the shared target first;
// polygons then depth-test and alpha-blend against that target. This validates polygon-in-front,
// voxel-in-front, and translucent-over-voxel behavior without a synthetic voxel layer.
struct HybridReferenceRenderStats {
    VoxelReferenceRenderStats voxels{};
    PolygonRenderStats polygons{};
};

[[nodiscard]] HybridReferenceRenderStats render_hybrid_reference(
    std::span<const VoxelReferenceInstance> voxels,
    std::span<const PolygonRenderInstance> polygons,
    const PolygonCamera& camera,
    const RenderEnvironment& environment,
    PolygonRenderTarget& target,
    const PolygonRenderOptions& polygonOptions = {},
    float metersPerVoxel = kDefaultMetersPerVoxel);

} // namespace dve::render
