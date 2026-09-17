#include <iostream>
#include <stdexcept>

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
        std::cout << "dve_voxel_lighting_plan_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_voxel_lighting_plan_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
