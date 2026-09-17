#include "dve/grid_fluid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool finite_vector(const std::vector<float>& values) {
    return std::all_of(values.begin(), values.end(), [](float value) {
        return std::isfinite(value);
    });
}

} // namespace

int main() {
    using namespace dve;

    GridFluidSettings settings;
    settings.dimensions = {18U, 24U, 16U};
    settings.cellSize = 0.08F;
    settings.maximumCfl = 1.5F;
    settings.maximumSubstepSeconds = 1.0F / 60.0F;
    settings.maximumSubsteps = 6U;
    settings.scalarAdvection = GridFluidAdvection::MacCormack;
    settings.velocityAdvection = GridFluidAdvection::SemiLagrangian;
    settings.gravity = {0.0F, -0.15F, 0.0F};
    settings.temperatureBuoyancy = 1.2F;
    settings.smokeWeight = 0.04F;
    settings.vorticityConfinement = 0.12F;
    settings.pressureMaximumIterations = 100U;
    settings.pressureRelativeTolerance = 2.0e-5F;
    require(settings.validate(), "grid-fluid settings validate");

    std::string error;
    GridFluidState state = make_grid_fluid_state(settings, &error);
    require(error.empty() && state.validate(&error), "grid-fluid state validates");
    require(estimate_grid_fluid_bytes(settings) > 0U, "memory estimate is nonzero");

    const Float3 obstacleMinimum{0.58F, 0.42F, 0.48F};
    const Float3 obstacleMaximum{0.86F, 0.72F, 0.80F};
    require(set_grid_fluid_solid_box(state, obstacleMinimum, obstacleMaximum, &error),
            "solid obstacle is accepted");

    GridFluidEmitterSample emitter;
    emitter.center = {0.38F, 0.22F, 0.58F};
    emitter.radius = 0.18F;
    emitter.density = 0.8F;
    emitter.temperature = 1.4F;
    emitter.fuel = 0.7F;
    emitter.flame = 0.15F;
    emitter.velocity = {0.12F, 0.85F, 0.04F};
    require(inject_grid_fluid_sphere(state, emitter, &error),
            "smoke/fire emitter is accepted");

    const float initialDensity = std::accumulate(state.density.begin(),
                                                  state.density.end(), 0.0F);
    const float initialFuel = std::accumulate(state.fuel.begin(), state.fuel.end(), 0.0F);
    require(initialDensity > 0.0F && initialFuel > 0.0F,
            "emitter populates density and fuel");

    const SimulationStepPlan stepPlan = plan_grid_fluid_step(1.0F / 30.0F, state);
    require(stepPlan.validate(&error) && stepPlan.substeps >= 1U,
            "adaptive grid-fluid step validates");

    GridFluidTelemetry aggregate;
    for (int frame = 0; frame < 5; ++frame) {
        require(inject_grid_fluid_sphere(state, emitter, &error),
                "repeated emitter injection succeeds");
        const GridFluidTelemetry telemetry = step_grid_fluid(state, 1.0F / 30.0F);
        require(telemetry.substeps >= 1U, "grid-fluid frame uses substeps");
        require(telemetry.scalarCellsAdvected > 0U && telemetry.velocityFacesAdvected > 0U,
                "advection executes");
        require(telemetry.pressureIterations > 0U,
                "pressure projection iterates");
        require(std::isfinite(telemetry.pressureResidual),
                "pressure residual is finite");
        require(telemetry.maximumDivergenceAfterProjection <=
                    telemetry.maximumDivergenceBeforeProjection + 1.0e-3F,
                "projection does not increase maximum divergence materially");
        aggregate.pressureIterations += telemetry.pressureIterations;
        aggregate.combustionCells += telemetry.combustionCells;
    }

    require(aggregate.pressureIterations > 0U, "aggregate pressure work is recorded");
    require(aggregate.combustionCells > 0U, "combustion consumes injected fuel");
    require(finite_vector(state.density) && finite_vector(state.temperature) &&
            finite_vector(state.fuel) && finite_vector(state.flame) &&
            finite_vector(state.pressure) && finite_vector(state.divergence),
            "all grid-fluid fields remain finite");
    require(std::all_of(state.density.begin(), state.density.end(),
                        [](float value) { return value >= 0.0F; }),
            "MacCormack scalar correction preserves nonnegative density");
    require(std::all_of(state.fuel.begin(), state.fuel.end(),
                        [](float value) { return value >= 0.0F; }),
            "fuel remains nonnegative");

    const GridFluidVolumeSnapshot volume = build_grid_fluid_volume_snapshot(state, 2.0F);
    require(volume.validate(&error) && volume.sourceFrame == state.frameIndex,
            "volume snapshot validates and tracks its source frame");
    require(volume.voxels.size() == state.density.size(),
            "volume snapshot preserves grid resolution");
    require(std::any_of(volume.voxels.begin(), volume.voxels.end(), [](const auto& voxel) {
        return voxel.density > 0.0F || voxel.emission > 0.0F;
    }), "volume snapshot contains visible smoke or fire");

    const Float3 samplePoint{0.38F, 0.55F, 0.58F};
    const float sampledDensity = sample_grid_fluid_scalar(state, state.density, samplePoint);
    const Float3 sampledVelocity = sample_grid_fluid_velocity(state, samplePoint);
    require(std::isfinite(sampledDensity) && sampledDensity >= 0.0F,
            "density sampling is finite");
    require(std::isfinite(sampledVelocity.x) && std::isfinite(sampledVelocity.y) &&
            std::isfinite(sampledVelocity.z), "MAC velocity sampling is finite");

    std::vector<GridFluidParticle> particles;
    for (std::uint32_t z = 0U; z < 3U; ++z) {
        for (std::uint32_t y = 0U; y < 3U; ++y) {
            for (std::uint32_t x = 0U; x < 3U; ++x) {
                particles.push_back({
                    {0.20F + 0.04F * static_cast<float>(x),
                     0.35F + 0.04F * static_cast<float>(y),
                     0.35F + 0.04F * static_cast<float>(z)},
                    {0.05F, 0.4F, 0.02F},
                });
            }
        }
    }
    const auto deposit = deposit_grid_fluid_particles_to_mac(state, particles);
    require(deposit.particlesDeposited == particles.size() &&
            deposit.faceContributions >= particles.size() * 3U,
            "PIC/FLIP particle-to-grid transfer deposits all particles");

    GridFluidParticleTransferSettings transfer;
    transfer.mode = GridFluidTransferMode::FlipBlend;
    transfer.flipRatio = 0.95F;
    require(transfer.validate(), "particle transfer settings validate");
    const auto update = update_grid_fluid_particles_from_mac(
        state, particles, transfer, 1.0F / 60.0F);
    require(update.particlesUpdated == particles.size(),
            "grid-to-particle transfer updates all particles");
    require(std::all_of(particles.begin(), particles.end(), [](const auto& particle) {
        return std::isfinite(particle.position.x) && std::isfinite(particle.position.y) &&
               std::isfinite(particle.position.z) && std::isfinite(particle.velocity.x) &&
               std::isfinite(particle.velocity.y) && std::isfinite(particle.velocity.z);
    }), "transferred particles remain finite");

    const GridFluidGpuFramePlan gpuPlan = plan_grid_fluid_gpu_frame(
        settings, 2U, true, true, true);
    require(gpuPlan.validate(&error), "grid-fluid GPU plan validates");
    require(gpuPlan.usesMacCormackScalars && gpuPlan.usesParticles,
            "GPU plan preserves scalar and particle features");
    require(gpuPlan.dispatches.front().pass == GridFluidGpuPassKind::ApplyEmitters,
            "GPU plan starts with emitters");
    require(gpuPlan.dispatches.back().pass == GridFluidGpuPassKind::BuildVolumeOutput,
            "GPU plan ends with volume output");
    require(std::count_if(gpuPlan.dispatches.begin(), gpuPlan.dispatches.end(),
                          [](const GridFluidGpuDispatch& dispatch) {
                              return dispatch.pass == GridFluidGpuPassKind::PressureSolve;
                          }) == static_cast<std::ptrdiff_t>(
                              settings.pressureMaximumIterations * gpuPlan.substeps),
            "GPU plan includes bounded pressure iterations for every substep");

    clear_grid_fluid(state);
    require(std::accumulate(state.density.begin(), state.density.end(), 0.0F) == 0.0F,
            "clear resets smoke density");
    clear_grid_fluid_obstacles(state);
    require(std::all_of(state.cellTypes.begin(), state.cellTypes.end(), [](auto type) {
        return type == GridFluidCellType::Fluid;
    }), "obstacle clear restores fluid cells");

    std::cout << "Grid-fluid tests passed: cells=" << settings.dimensions.cell_count()
              << " memory=" << estimate_grid_fluid_bytes(settings)
              << " gpu_dispatches=" << gpuPlan.dispatches.size()
              << " pressure_iterations=" << aggregate.pressureIterations << '\n';
    return 0;
}
