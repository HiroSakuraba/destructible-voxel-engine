#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "dve/liquid_pbf.hpp"

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
}

int main() {
    try {
        dve::PbfLiquidSettings settings;
        settings.maximumParticles = 512U;
        settings.particleRadius = 0.035F;
        settings.kernelRadius = 0.11F;
        settings.solverIterations = 4U;
        settings.vorticityConfinement = 0.05F;
        settings.maximumNeighbors = 96U;
        settings.boundsMinimum = {-0.5F, 0.0F, -0.5F};
        settings.boundsMaximum = {0.5F, 1.0F, 0.5F};
        std::string error;
        require(settings.validate(&error), error);
        dve::PbfLiquidWorld world(settings);
        const auto added = world.add_box({-0.12F, 0.35F, -0.12F},
                                         {0.12F, 0.59F, 0.12F}, 0.08F, {}, &error);
        require(added >= 27U, "PBF box emitter created too few particles");
        const float initialY = world.particles().front().position.y;
        dve::PbfLiquidTelemetry telemetry{};
        for (int step = 0; step < 8; ++step) telemetry = world.step(1.0F / 120.0F);
        require(telemetry.particlesIntegrated >= added &&
                telemetry.particlesIntegrated % added == 0U,
                "PBF did not integrate every particle in each substep");
        require(telemetry.substeps >= 1U, "PBF adaptive step planner was not used");
        require(telemetry.densityConstraints > 0U && telemetry.positionCorrections > 0U,
                "PBF constraints were not solved");
        require(world.particles().front().position.y < initialY,
                "PBF particles did not respond to gravity");
        for (const auto& particle : world.particles()) {
            require(std::isfinite(particle.position.x) && std::isfinite(particle.position.y) &&
                    std::isfinite(particle.position.z), "PBF produced non-finite state");
            require(particle.position.y >= settings.boundsMinimum.y + settings.particleRadius - 1.0e-4F,
                    "PBF particle escaped lower boundary");
        }
        const auto temporalPlan = dve::plan_spatiotemporal_flip_samples(
            added, 4U, 2U, 0.25F, 7U);
        require(temporalPlan.validate(&error) &&
                temporalPlan.samples.size() == static_cast<std::size_t>(added) * 4U,
                "ST-FLIP sampling plan is invalid");
        const auto plan = dve::plan_pbf_gpu_frame(settings, added, true);
        require(plan.validate(&error), error);
        require(plan.solverIterations == settings.solverIterations &&
                plan.dispatches.back().pass == dve::PbfGpuPassKind::BuildSurfaceData,
                "PBF GPU frame plan is incomplete");
        std::cout << "DVE PBF liquid tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE PBF liquid tests failed: " << exception.what() << '\n';
        return 1;
    }
}
