#include "dve/render/voxel_lighting_plan.hpp"

#include <algorithm>
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

void add_dispatch(VoxelLightingFramePlan& plan, VoxelLightingPass pass, std::uint64_t elements) {
    plan.dispatches.push_back({pass, elements, groups_for(elements)});
}
}

VoxelLightingFramePlan make_voxel_lighting_frame_plan(
    std::uint32_t width, std::uint32_t height, const RenderEnvironment& environment) {
    VoxelLightingFramePlan plan;
    plan.width = width;
    plan.height = height;
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

    if (environment.globalIlluminationMode == GlobalIlluminationMode::VoxelOneBounce) {
        plan.globalIlluminationRayCapacity = plan.pixelCount * kMaximumVoxelLightingSamples;
        const std::uint32_t samples = std::clamp(environment.globalIlluminationSamples, 1U,
                                                 kMaximumVoxelLightingSamples);
        plan.activeGlobalIlluminationRayCount = plan.pixelCount * samples;
        add_dispatch(plan, VoxelLightingPass::GenerateGlobalIlluminationRays,
                     plan.globalIlluminationRayCapacity);
        add_dispatch(plan, VoxelLightingPass::TraceGlobalIlluminationRays,
                     plan.globalIlluminationRayCapacity);
        add_dispatch(plan, VoxelLightingPass::ResolveGlobalIllumination, plan.pixelCount);
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
    if (dispatches.empty() || dispatches.back().pass != VoxelLightingPass::ShadePrimary)
        return fail("voxel lighting plan does not terminate in primary shading");
    for (const VoxelLightingDispatch& dispatch : dispatches) {
        if (dispatch.elements == 0U || dispatch.groupsX != groups_for(dispatch.elements))
            return fail("voxel lighting dispatch geometry is invalid");
    }
    return true;
}

} // namespace dve::render
