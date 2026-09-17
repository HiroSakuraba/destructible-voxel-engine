#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "dve/fluoddity.hpp"
#include "dve/fluoddity_solver.hpp"

#ifndef DVE_TEST_SOURCE_DIR
#define DVE_TEST_SOURCE_DIR "."
#endif

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
}

int main() {
    try {
        const auto source = std::filesystem::path(DVE_TEST_SOURCE_DIR) /
                            "tests/assets/fluoddity_v7_av0.json";
        auto imported = dve::import_fluoddity_preset_json(source);
        require(static_cast<bool>(imported), imported.error);
        dve::FluoddityReferenceSettings settings;
        settings.trailResolution = 8U;
        settings.boundsHalfExtent = {1.0F, 1.0F, 1.0F};
        settings.deltaSeconds = 1.0F / 120.0F;
        auto state = dve::make_fluoddity_reference_state(*imported.asset, 256U, settings);
        std::string error;
        std::vector<dve::FluoddityTrailVoxel> interpolationTrail(8U);
        interpolationTrail[0].velocity = {0.0F, 0.0F, 0.0F};
        interpolationTrail[1].velocity = {1.0F, 0.0F, 0.0F};
        interpolationTrail[2].velocity = {0.0F, 1.0F, 0.0F};
        interpolationTrail[3].velocity = {1.0F, 1.0F, 0.0F};
        interpolationTrail[4].velocity = {0.0F, 0.0F, 1.0F};
        interpolationTrail[5].velocity = {1.0F, 0.0F, 1.0F};
        interpolationTrail[6].velocity = {0.0F, 1.0F, 1.0F};
        interpolationTrail[7].velocity = {1.0F, 1.0F, 1.0F};
        dve::FluoddityReferenceSettings interpolationSettings = settings;
        interpolationSettings.trailResolution = 2U;
        const auto centerSample = dve::sample_fluoddity_trail(
            interpolationTrail, interpolationSettings, {0.0F, 0.0F, 0.0F});
        require(std::abs(centerSample.velocity.x - 0.5F) < 1.0e-5F &&
                std::abs(centerSample.velocity.y - 0.5F) < 1.0e-5F &&
                std::abs(centerSample.velocity.z - 0.5F) < 1.0e-5F,
                "Fluoddity trilinear trail sampling is incorrect");
        require(state.validate(settings, &error), error);
        state.particles.front().velocity = {20.0F, 0.0F, 0.0F};
        const auto stepPlan = dve::plan_fluoddity_reference_step(settings, state);
        require(stepPlan.validate(&error) && stepPlan.substeps > 1U,
                "Fluoddity adaptive step plan did not react to fast motion");
        state.particles.front().velocity = {1.0F, 0.0F, 0.0F};
        const auto before = state.particles.front().position;
        const auto first = dve::step_fluoddity_reference(*imported.asset, settings, state);
        settings.frameNumber = 1U;
        const auto second = dve::step_fluoddity_reference(*imported.asset, settings, state);
        require(first.movedParticles == 256U && second.movedParticles == 256U,
                "not all Fluoddity particles were integrated");
        require(first.depositedParticles > 0U && second.depositedParticles > 0U,
                "Fluoddity particles did not deposit trail data");
        require(state.trailReadIndex == 0U, "trail ping-pong did not return after two steps");
        const auto after = state.particles.front().position;
        require(before.x != after.x || before.y != after.y || before.z != after.z,
                "Fluoddity particle state did not advance");
        float trailMagnitude{};
        for (const auto& voxel : dve::fluoddity_read_trail(state)) {
            trailMagnitude += std::abs(voxel.velocity.x) + std::abs(voxel.velocity.y) +
                              std::abs(voxel.velocity.z) + std::abs(voxel.density);
        }
        require(trailMagnitude > 0.0F, "resolved Fluoddity trail is empty");
        require(first.nonFiniteCorrections == 0U && second.nonFiniteCorrections == 0U,
                "reference solver required non-finite repair");
        std::cout << "DVE Fluoddity solver tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE Fluoddity solver tests failed: " << exception.what() << '\n';
        return 1;
    }
}
