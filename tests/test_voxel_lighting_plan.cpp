#include <iostream>
#include <stdexcept>

#include "dve/render/radiance_cascades_spwi.hpp"
#include "dve/render/voxel_lighting_plan.hpp"

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}

int main() {
    try {
        using namespace dve;
        using namespace dve::render;
        RenderEnvironment environment;
        const auto defaults = make_voxel_lighting_frame_plan(1920U, 1080U, environment);
        std::string error;
        require(defaults.validate(&error), error.c_str());
        require(defaults.activeShadowRayCount == defaults.pixelCount * 4U,
                "default soft shadows do not use four active samples");
        require(defaults.activeGlobalIlluminationRayCount == defaults.pixelCount * 4U,
                "default GI does not use four active samples");
        require(defaults.dispatches.size() == 7U, "default lighting pass sequence is incomplete");

        environment.shadowMode = ShadowMode::Hard;
        environment.globalIlluminationMode = GlobalIlluminationMode::Off;
        const auto hard = make_voxel_lighting_frame_plan(17U, 9U, environment);
        require(hard.validate(&error), error.c_str());
        require(hard.activeShadowRayCount == hard.pixelCount, "hard shadow ray budget is wrong");
        require(hard.globalIlluminationRayCapacity == 0U, "disabled GI allocated a ray buffer");
        require(hard.dispatches.size() == 4U, "hard-shadow pass sequence is wrong");

        environment.shadowMode = ShadowMode::Contact;
        environment.globalIlluminationMode = GlobalIlluminationMode::AmbientHemisphere;
        const auto contact = make_voxel_lighting_frame_plan(31U, 17U, environment);
        require(contact.validate(&error), error.c_str());
        require(contact.activeShadowRayCount == contact.pixelCount,
                "contact shadows do not use one bounded near-field ray per pixel");
        require(contact.globalIlluminationRayCapacity == 0U,
                "ambient hemisphere incorrectly allocated GI rays");

        environment.shadowMode = ShadowMode::Off;
        const auto unshadowed = make_voxel_lighting_frame_plan(31U, 17U, environment);
        require(unshadowed.validate(&error), error.c_str());
        require(unshadowed.shadowRayCapacity == 0U && unshadowed.activeShadowRayCount == 0U,
                "disabled shadows still allocated rays");
        require(unshadowed.dispatches.size() == 1U &&
                    unshadowed.dispatches.front().pass == VoxelLightingPass::ShadePrimary,
                "disabled shadows and non-traced GI should schedule only primary shading");

        environment.shadowMode = ShadowMode::Hybrid;
        environment.shadowSamples = 16U;
        environment.globalIlluminationMode = GlobalIlluminationMode::AmbientHemisphere;
        const auto hybrid = make_voxel_lighting_frame_plan(64U, 64U, environment);
        require(hybrid.activeShadowRayCount == hybrid.pixelCount * 16U,
                "hybrid shadows exceeded or underfilled fixed capacity");
        require(hybrid.globalIlluminationRayCapacity == 0U,
                "ambient hemisphere incorrectly allocated GI rays");

        const auto empty = make_voxel_lighting_frame_plan(0U, 10U, environment);
        require(empty.validate(&error) && empty.dispatches.empty(), "empty plan is invalid");

        environment.globalIlluminationMode = GlobalIlluminationMode::RadianceCascades;
        environment.globalIlluminationMaxDistanceMeters = 12.0F;
        const auto fallback = make_voxel_lighting_frame_plan(1920U, 1080U, environment);
        require(fallback.validate(&error) && fallback.radianceCascadeLevels.empty() &&
                    fallback.globalIlluminationMode == GlobalIlluminationMode::VoxelOneBounce,
                "unimplemented GPU path stopped falling back to one bounce");
        GpuRadianceCascadeSettings gpuSettings;
        gpuSettings.enabled = true;
        const auto rc = make_voxel_lighting_frame_plan(1920U, 1080U, environment, gpuSettings);
        require(rc.validate(&error), error.c_str());
        require(rc.radianceCascadeLevels.size() == 5U, "default GPU cascade count is wrong");
        require(rc.radianceCascadeRadianceBytes == 16711680U,
                "GPU ping-pong allocation missed partial probe rows");
        require(rc.globalIlluminationRayCapacity == 0U &&
                    rc.activeGlobalIlluminationRayCount == 0U,
                "cascades allocated one-bounce rays");
        require(rc.dispatches.size() == 3U + 4U * 5U + 2U,
                "GPU cascade passes do not replace the one-bounce trio");
        for (std::uint32_t i = 0; i < rc.radianceCascadeLevels.size(); ++i) {
            require(rc.dispatches[3U + i].pass == VoxelLightingPass::BuildRadianceCascadeProbes &&
                        rc.dispatches[3U + i].cascadeLevel == i,
                    "probe build order is wrong");
            const auto base = 3U + 5U + 3U * i;
            const auto level = 4U - i;
            require(rc.dispatches[base].pass == VoxelLightingPass::TraceRadianceCascadeIntervals &&
                        rc.dispatches[base + 1U].pass == VoxelLightingPass::TraceRadianceCascadeSun &&
                        rc.dispatches[base + 2U].pass == VoxelLightingPass::MergeRadianceCascade &&
                        rc.dispatches[base].cascadeLevel == level,
                    "cascade passes do not merge top down");
        }
        require(rc.dispatches[rc.dispatches.size() - 2U].pass ==
                    VoxelLightingPass::GatherRadianceCascades,
                "cascade gather is not before shading");

        // Match the CPU's level geometry and interval bounds. The GPU plan stores the
        // hemisphere only, in RGBA16F, so its direction count is half the CPU's.
        RadianceCascadeSettings cpuSettings;
        cpuSettings.baseProbeSpacingPixels = 4U;
        cpuSettings.baseDirectionResolution = 4U;
        for (const auto [width, height] : {std::pair{320U, 180U}, std::pair{17U, 9U},
                                           std::pair{1920U, 1080U}}) {
            const auto gpu = make_voxel_lighting_frame_plan(width, height, environment, gpuSettings);
            const auto cpu = spwi::describe_cascades(cpuSettings, environment, width, height);
            require(gpu.validate(&error) && gpu.radianceCascadeLevels.size() == cpu.size(),
                    "GPU/CPU cascade level count diverged");
            for (std::size_t i = 0; i < cpu.size(); ++i) {
                const auto& level = gpu.radianceCascadeLevels[i];
                require(level.probeSpacingPixels == cpu[i].probeSpacingPixels &&
                            level.probesX == cpu[i].probesX && level.probesY == cpu[i].probesY &&
                            level.directionResolution == cpu[i].directionResolution &&
                            level.intervalStart == cpu[i].intervalStart &&
                            level.intervalEnd == cpu[i].intervalEnd &&
                            level.directionTexels * 2U == cpu[i].texels,
                        "GPU/CPU cascade layout diverged");
            }
        }
        GpuRadianceCascadeSettings invalid;
        invalid.enabled = true;
        invalid.baseProbeSpacingPixels = 0U;
        require(!make_voxel_lighting_frame_plan(64U, 64U, environment, invalid).validate(),
                "invalid GPU cascade settings silently produced a complete plan");
        auto broken = make_voxel_lighting_frame_plan(64U, 64U, environment, gpuSettings);
        broken.dispatches[broken.dispatches.size() - 2U].pass =
            VoxelLightingPass::ResolveGlobalIllumination;
        require(!broken.validate(), "missing cascade gather was accepted");
        std::cout << "dve_voxel_lighting_plan_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_voxel_lighting_plan_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
