#include "dve/render/voxel_lighting_plan.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dve::render {
namespace {
std::uint32_t groups_for(std::uint64_t elements) noexcept {
    if (elements == 0U) return 0U;
    const std::uint64_t groups = (elements + kVoxelLightingThreadsPerGroup - 1U) /
                                 kVoxelLightingThreadsPerGroup;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        groups, std::numeric_limits<std::uint32_t>::max()));
}

void add_dispatch(VoxelLightingFramePlan& plan, VoxelLightingPass pass, std::uint64_t elements,
                  std::uint32_t level = kNoRadianceCascadeLevel) {
    plan.dispatches.push_back({pass, elements, groups_for(elements), level});
}

constexpr std::uint32_t kMaximumCascades = 10U;
constexpr std::uint64_t kProbeBytes = 32U;
constexpr std::uint64_t kRadianceTexelBytes = 8U; // RGBA16F

bool valid_cascades(const GpuRadianceCascadeSettings& settings) noexcept {
    return settings.baseProbeSpacingPixels >= 1U && settings.baseProbeSpacingPixels <= 64U &&
           settings.baseDirectionResolution >= 2U && settings.baseDirectionResolution <= 64U &&
           settings.baseDirectionResolution % 2U == 0U &&
           std::isfinite(settings.baseIntervalLength) && settings.baseIntervalLength > 0.0F &&
           std::isfinite(settings.intervalGrowth) && settings.intervalGrowth >= 1.0F &&
           settings.intervalGrowth <= 16.0F && settings.maximumCascades <= kMaximumCascades;
}

void add_cascades(VoxelLightingFramePlan& plan, const RenderEnvironment& environment,
                  const GpuRadianceCascadeSettings& settings, float metersPerVoxel) {
    if (!valid_cascades(settings)) return;
    const float maximumDistance = environment.globalIlluminationMaxDistanceMeters /
                                  resolve_meters_per_voxel(metersPerVoxel);
    if (!(maximumDistance > 0.0F) || !std::isfinite(maximumDistance)) return;
    const auto cap = settings.maximumCascades == 0U ? kMaximumCascades : settings.maximumCascades;
    const std::uint32_t longestSide = std::max(plan.width, plan.height);
    float start = 0.0F, length = settings.baseIntervalLength;
    for (std::uint32_t i = 0; i < cap; ++i) {
        GpuRadianceCascadeLevel level;
        level.probeSpacingPixels = settings.baseProbeSpacingPixels << i;
        level.probesX = 1U + (plan.width - 1U) / level.probeSpacingPixels;
        level.probesY = 1U + (plan.height - 1U) / level.probeSpacingPixels;
        level.directionResolution = settings.baseDirectionResolution << i;
        level.intervalStart = start;
        const float nextEnd = start + length;
        const bool last = nextEnd >= maximumDistance || i + 1U == cap ||
                          std::uint64_t(level.probeSpacingPixels) * 4U > longestSide;
        level.intervalEnd = last ? maximumDistance : nextEnd;
        level.probes = std::uint64_t(level.probesX) * level.probesY;
        const auto hemisphereDirections = std::uint64_t(level.directionResolution) *
                                          level.directionResolution / 2U;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (level.probes > maximum / hemisphereDirections / (kRadianceTexelBytes * 2U) ||
            level.probes > maximum / kProbeBytes ||
            plan.radianceCascadeProbeBytes > maximum - level.probes * kProbeBytes) {
            plan.radianceCascadeLevels.clear();
            plan.radianceCascadeRadianceBytes = plan.radianceCascadeProbeBytes = 0U;
            return; // validate() reports a missing/oversized layout.
        }
        level.directionTexels = level.probes * hemisphereDirections;
        level.radianceBytes = level.directionTexels * kRadianceTexelBytes;
        level.probeBytes = level.probes * kProbeBytes;
        plan.radianceCascadeRadianceBytes = std::max(plan.radianceCascadeRadianceBytes,
                                                     level.radianceBytes * 2U);
        plan.radianceCascadeProbeBytes += level.probeBytes;
        plan.radianceCascadeLevels.push_back(level);
        if (last) break;
        start = nextEnd;
        length *= settings.intervalGrowth;
    }
    for (std::uint32_t i = 0; i < plan.radianceCascadeLevels.size(); ++i)
        add_dispatch(plan, VoxelLightingPass::BuildRadianceCascadeProbes,
                     plan.radianceCascadeLevels[i].probes, i);
    // Two radiance buffers are reused as the top-down merge progresses. The sun pass has a
    // worst-case dispatch for each direction; inactive/miss intervals are skipped by the shader.
    for (std::uint32_t i = static_cast<std::uint32_t>(plan.radianceCascadeLevels.size()); i-- > 0;) {
        const auto elements = plan.radianceCascadeLevels[i].directionTexels;
        add_dispatch(plan, VoxelLightingPass::TraceRadianceCascadeIntervals, elements, i);
        add_dispatch(plan, VoxelLightingPass::TraceRadianceCascadeSun, elements, i);
        add_dispatch(plan, VoxelLightingPass::MergeRadianceCascade, elements, i);
    }
    add_dispatch(plan, VoxelLightingPass::GatherRadianceCascades, plan.pixelCount);
}
}

VoxelLightingFramePlan make_voxel_lighting_frame_plan(
    std::uint32_t width, std::uint32_t height, const RenderEnvironment& environment,
    const GpuRadianceCascadeSettings& cascades, float metersPerVoxel) {
    VoxelLightingFramePlan plan;
    plan.width = width;
    plan.height = height;
    plan.globalIlluminationMode = environment.globalIlluminationMode == GlobalIlluminationMode::RadianceCascades &&
                                          !cascades.enabled
                                      ? GlobalIlluminationMode::VoxelOneBounce
                                      : environment.globalIlluminationMode;
    plan.pixelCount = static_cast<std::uint64_t>(width) * height;
    if (plan.pixelCount == 0U) return plan;

    if (environment.shadowMode != ShadowMode::Off) {
        plan.shadowRayCapacity = plan.pixelCount * kMaximumVoxelLightingSamples;
        std::uint32_t activeSamples = 1U;
        if (environment.shadowMode == ShadowMode::Soft)
            activeSamples = std::clamp(environment.shadowSamples, 1U, kMaximumVoxelLightingSamples);
        else if (environment.shadowMode == ShadowMode::Hybrid)
            activeSamples = 1U + std::clamp(environment.shadowSamples, 1U,
                                            kMaximumVoxelLightingSamples - 1U);
        plan.activeShadowRayCount = plan.pixelCount * activeSamples;
        add_dispatch(plan, VoxelLightingPass::GenerateShadowRays, plan.shadowRayCapacity);
        add_dispatch(plan, VoxelLightingPass::TraceShadowRays, plan.shadowRayCapacity);
        add_dispatch(plan, VoxelLightingPass::ResolveShadows, plan.pixelCount);
    }

    if (plan.globalIlluminationMode == GlobalIlluminationMode::VoxelOneBounce) {
        plan.globalIlluminationRayCapacity = plan.pixelCount * kMaximumVoxelLightingSamples;
        const std::uint32_t samples = std::clamp(environment.globalIlluminationSamples, 1U,
                                                 kMaximumVoxelLightingSamples);
        plan.activeGlobalIlluminationRayCount = plan.pixelCount * samples;
        add_dispatch(plan, VoxelLightingPass::GenerateGlobalIlluminationRays,
                     plan.globalIlluminationRayCapacity);
        add_dispatch(plan, VoxelLightingPass::TraceGlobalIlluminationRays,
                     plan.globalIlluminationRayCapacity);
        add_dispatch(plan, VoxelLightingPass::ResolveGlobalIllumination, plan.pixelCount);
    } else if (plan.globalIlluminationMode == GlobalIlluminationMode::RadianceCascades) {
        add_cascades(plan, environment, cascades, metersPerVoxel);
    }

    add_dispatch(plan, VoxelLightingPass::ShadePrimary, plan.pixelCount);
    return plan;
}

bool VoxelLightingFramePlan::validate(std::string* error) const noexcept {
    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (pixelCount != static_cast<std::uint64_t>(width) * height)
        return fail("voxel lighting pixel count does not match extent");
    if (pixelCount == 0U) return dispatches.empty() ? true : fail("empty extent has dispatches");
    if (shadowRayCapacity != 0U && shadowRayCapacity != pixelCount * kMaximumVoxelLightingSamples)
        return fail("shadow ray capacity does not use the fixed per-pixel stride");
    if (activeShadowRayCount > shadowRayCapacity)
        return fail("active shadow ray count exceeds capacity");
    if (globalIlluminationRayCapacity != 0U &&
        globalIlluminationRayCapacity != pixelCount * kMaximumVoxelLightingSamples)
        return fail("GI ray capacity does not use the fixed per-pixel stride");
    if (activeGlobalIlluminationRayCount > globalIlluminationRayCapacity)
        return fail("active GI ray count exceeds capacity");
    if (globalIlluminationMode == GlobalIlluminationMode::RadianceCascades &&
        radianceCascadeLevels.empty())
        return fail("radiance cascades have no valid levels");
    if (!radianceCascadeLevels.empty()) {
        if (globalIlluminationMode != GlobalIlluminationMode::RadianceCascades)
            return fail("radiance cascade levels belong to another GI mode");
        if (globalIlluminationRayCapacity || activeGlobalIlluminationRayCount)
            return fail("radiance cascades allocated one-bounce GI rays");
        std::uint64_t maximumRadiance{}, probeBytes{};
        for (const auto& level : radianceCascadeLevels) {
            if (!level.probes || !level.directionTexels ||
                level.probes != std::uint64_t(level.probesX) * level.probesY ||
                level.directionTexels != level.probes * level.directionResolution *
                                         level.directionResolution / 2U ||
                level.radianceBytes != level.directionTexels * kRadianceTexelBytes ||
                level.probeBytes != level.probes * kProbeBytes ||
                !(level.intervalEnd > level.intervalStart))
                return fail("radiance cascade level geometry is invalid");
            maximumRadiance = std::max(maximumRadiance, level.radianceBytes);
            probeBytes += level.probeBytes;
        }
        if (radianceCascadeRadianceBytes != maximumRadiance * 2U ||
            radianceCascadeProbeBytes != probeBytes)
            return fail("radiance cascade buffer capacity is invalid");
        const auto count = static_cast<std::uint32_t>(radianceCascadeLevels.size());
        if (dispatches.size() < count * 4U + 2U)
            return fail("radiance cascade passes are missing");
        const auto first = dispatches.size() - (count * 4U + 2U);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& dispatch = dispatches[first + i];
            if (dispatch.pass != VoxelLightingPass::BuildRadianceCascadeProbes ||
                dispatch.cascadeLevel != i || dispatch.elements != radianceCascadeLevels[i].probes)
                return fail("radiance cascade probes are out of order");
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto level = count - i - 1U;
            for (std::uint32_t pass = 0; pass < 3U; ++pass) {
                const auto& dispatch = dispatches[first + count + i * 3U + pass];
                const auto expected = static_cast<VoxelLightingPass>(
                    static_cast<unsigned>(VoxelLightingPass::TraceRadianceCascadeIntervals) + pass);
                if (dispatch.pass != expected || dispatch.cascadeLevel != level ||
                    dispatch.elements != radianceCascadeLevels[level].directionTexels)
                    return fail("radiance cascade merge order is invalid");
            }
        }
        if (dispatches[dispatches.size() - 2U].pass != VoxelLightingPass::GatherRadianceCascades ||
            dispatches[dispatches.size() - 2U].elements != pixelCount)
            return fail("radiance cascade gather is missing");
    } else if (radianceCascadeRadianceBytes || radianceCascadeProbeBytes)
        return fail("radiance cascade buffers have no levels");
    if (dispatches.empty() || dispatches.back().pass != VoxelLightingPass::ShadePrimary)
        return fail("voxel lighting plan does not terminate in primary shading");
    for (const VoxelLightingDispatch& dispatch : dispatches) {
        if (dispatch.elements == 0U || dispatch.groupsX != groups_for(dispatch.elements))
            return fail("voxel lighting dispatch geometry is invalid");
        if (dispatch.cascadeLevel != kNoRadianceCascadeLevel &&
            dispatch.cascadeLevel >= radianceCascadeLevels.size())
            return fail("voxel lighting dispatch references an absent cascade");
    }
    return true;
}

} // namespace dve::render
