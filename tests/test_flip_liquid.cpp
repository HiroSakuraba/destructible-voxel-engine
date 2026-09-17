#include "dve/flip_liquid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool finite_particle(const dve::FlipLiquidParticle& particle) {
    const auto finite3 = [](dve::Float3 value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    return finite3(particle.position) && finite3(particle.velocity) &&
           finite3(particle.affineX) && finite3(particle.affineY) &&
           finite3(particle.affineZ);
}

} // namespace

int main() {
    using namespace dve;

    FlipLiquidSettings settings;
    settings.dimensions = {14U, 18U, 12U};
    settings.cellSize = 0.08F;
    settings.particleRadius = 0.022F;
    settings.maximumSubsteps = 6U;
    settings.pressureMaximumIterations = 100U;
    settings.pressureRelativeTolerance = 2.0e-5F;
    settings.velocityExtrapolationLayers = 3U;
    settings.levelSetBandCells = 3U;
    settings.minimumParticlesPerCell = 3U;
    settings.targetParticlesPerCell = 6U;
    settings.maximumParticlesPerCell = 12U;
    settings.maximumParticles = 80'000U;
    settings.transferMode = FlipLiquidTransferMode::FlipApic;
    settings.flipRatio = 0.92F;

    std::string error;
    require(settings.validate(&error), "FLIP/APIC settings validate");
    FlipLiquidState state = make_flip_liquid_state(settings, &error);
    require(state.validate(&error), "FLIP/APIC state validates");

    require(set_flip_liquid_solid_box(state,
                                      {0.48F, 0.08F, 0.34F},
                                      {0.64F, 0.48F, 0.62F}, &error),
            "solid obstacle is accepted");
    const std::uint32_t initialParticles = add_flip_liquid_box(
        state, {0.12F, 0.18F, 0.18F}, {0.42F, 0.82F, 0.68F}, 0.045F,
        {0.35F, 0.0F, 0.0F}, &error);
    require(initialParticles > 500U, "liquid box creates a useful particle sample");

    const SimulationStepPlan stepPlan = plan_flip_liquid_step(1.0F / 30.0F, state);
    require(stepPlan.validate(&error) && stepPlan.substeps >= 1U,
            "FLIP/APIC adaptive step validates");

    std::uint64_t pressureIterations = 0U;
    std::uint64_t extrapolatedFaces = 0U;
    std::uint64_t reseeded = 0U;
    float firstVolume = 0.0F;
    float lastVolume = 0.0F;
    for (int frame = 0; frame < 8; ++frame) {
        const FlipLiquidTelemetry telemetry = step_flip_liquid(state, 1.0F / 30.0F);
        require(telemetry.substeps >= 1U, "liquid frame uses bounded substeps");
        require(telemetry.particleToGridContributions > state.particles.size(),
                "APIC particle-to-grid transfer executes");
        require(telemetry.gridToParticleUpdates > 0U,
                "grid-to-particle transfer executes");
        require(telemetry.pressureIterations > 0U,
                "free-surface pressure projection iterates");
        require(std::isfinite(telemetry.pressureResidual),
                "pressure residual remains finite");
        if (!(telemetry.maximumDivergenceAfterProjection <=
              telemetry.maximumDivergenceBeforeProjection + 2.5e-2F)) {
            std::cerr << "div before=" << telemetry.maximumDivergenceBeforeProjection
                      << " after=" << telemetry.maximumDivergenceAfterProjection
                      << " residual=" << telemetry.pressureResidual
                      << " iterations=" << telemetry.pressureIterations << '\n';
        }
        require(telemetry.maximumDivergenceAfterProjection <=
                    telemetry.maximumDivergenceBeforeProjection + 2.5e-2F,
                "projection does not materially increase liquid divergence");
        require(telemetry.liquidCells > 0U && telemetry.surfaceCells > 0U,
                "liquid and surface cells are classified");
        require(telemetry.nonFiniteCorrections == 0U,
                "reference liquid solver needs no non-finite repair");
        if (frame == 0) firstVolume = telemetry.estimatedLiquidVolume;
        lastVolume = telemetry.estimatedLiquidVolume;
        pressureIterations += telemetry.pressureIterations;
        extrapolatedFaces += telemetry.extrapolatedFaces;
        reseeded += telemetry.particlesSpawned;
    }

    require(state.particles.size() <= settings.maximumParticles,
            "particle budget remains bounded");
    require(std::all_of(state.particles.begin(), state.particles.end(), finite_particle),
            "all FLIP/APIC particles remain finite");
    require(pressureIterations > 0U && extrapolatedFaces > 0U,
            "pressure and velocity extrapolation both perform work");
    require(reseeded > 0U, "deterministic particle reseeding activates");
    require(firstVolume > 0.0F && lastVolume > 0.0F &&
                lastVolume > firstVolume * 0.45F && lastVolume < firstVolume * 1.8F,
            "coarse classified liquid volume remains bounded");

    const FlipLiquidSurfaceSnapshot surface = build_flip_liquid_surface_snapshot(state);
    require(surface.validate(&error) && surface.sourceFrame == state.frameIndex,
            "surface snapshot validates and tracks the source frame");
    require(std::any_of(surface.levelSet.begin(), surface.levelSet.end(),
                        [](float phi) { return phi < 0.0F; }),
            "particle level set contains an interior");
    require(std::any_of(surface.cellTypes.begin(), surface.cellTypes.end(),
                        [](auto type) { return type == FlipLiquidCellType::Liquid; }),
            "surface snapshot contains liquid cells");

    const Float3 velocity = sample_flip_liquid_velocity(state, state.particles.front().position);
    const float phi = sample_flip_liquid_level_set(state, state.particles.front().position);
    require(std::isfinite(velocity.x) && std::isfinite(velocity.y) &&
                std::isfinite(velocity.z) && std::isfinite(phi),
            "liquid velocity and level-set sampling are finite");

    const FlipLiquidGpuFramePlan gpuPlan = plan_flip_liquid_gpu_frame(
        settings, static_cast<std::uint32_t>(state.particles.size()), 2U, true);
    require(gpuPlan.validate(&error), "FLIP/APIC GPU plan validates");
    require(gpuPlan.usesApic && gpuPlan.reseeds,
            "GPU plan retains APIC and reseeding features");
    require(gpuPlan.dispatches.front().pass == FlipLiquidGpuPassKind::ClearGrid,
            "GPU plan starts by clearing the grid");
    require(gpuPlan.dispatches.back().pass == FlipLiquidGpuPassKind::BuildSurfaceOutput,
            "GPU plan ends with surface output");
    require(std::count_if(gpuPlan.dispatches.begin(), gpuPlan.dispatches.end(),
                          [](const auto& dispatch) {
                              return dispatch.pass == FlipLiquidGpuPassKind::PressureSolve;
                          }) == static_cast<std::ptrdiff_t>(
                              settings.pressureMaximumIterations * gpuPlan.substeps),
            "GPU plan includes pressure iterations for each substep");

    const std::uint64_t bytes = estimate_flip_liquid_bytes(settings);
    require(bytes > state.particles.size() * sizeof(FlipLiquidParticle),
            "memory estimate includes grid and particle storage");

    const std::size_t finalParticles = state.particles.size();
    clear_flip_liquid(state);
    require(state.particles.empty(), "clear removes all liquid particles");
    clear_flip_liquid_solids(state);
    require(std::none_of(state.cellTypes.begin(), state.cellTypes.end(),
                         [](auto type) { return type == FlipLiquidCellType::Solid; }),
            "solid clear removes all liquid obstacles");

    std::cout << "FLIP/APIC tests passed: initial_particles=" << initialParticles
              << " final_particles=" << finalParticles
              << " pressure_iterations=" << pressureIterations
              << " extrapolated_faces=" << extrapolatedFaces
              << " gpu_dispatches=" << gpuPlan.dispatches.size() << '\n';
    return 0;
}
