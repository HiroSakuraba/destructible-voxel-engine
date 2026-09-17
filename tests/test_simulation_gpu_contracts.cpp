#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "dve/fluoddity.hpp"
#include "dve/render/simulation_gpu.hpp"
#include "dve/rhi/null_device.hpp"

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
}

int main() {
    try {
        dve::rhi::NullDevice device;
        dve::render::FluoddityGpuSimulation simulation(device);
        dve::FluoddityQualityProfile quality;
        quality.quality = dve::FluoddityQuality::Custom;
        quality.particleCapacity = 128U;
        quality.trailResolution = 8U;
        dve::render::FluoddityGpuShaderBytecode bytecode;
        std::string error;
        require(simulation.initialize(quality, bytecode, &error), error);
        std::vector<dve::FluoddityParticleState> particles(64U);
        require(simulation.upload_particles(particles, &error), error);
        dve::FluoddityRuleAsset asset;
        asset.cohortCount = 4U;
        for (auto& parameter : asset.parameters) {
            parameter.minimum = -100.0F;
            parameter.maximum = 100.0F;
        }
        asset.parameters[static_cast<std::size_t>(dve::FluoddityParameter::TrailPersistence)].value = 0.98F;
        asset.parameters[static_cast<std::size_t>(dve::FluoddityParameter::TrailDiffusion)].value = 0.1F;
        dve::FluoddityReferenceSettings referenceSettings;
        referenceSettings.trailResolution = quality.trailResolution;
        const auto constants = dve::render::make_fluoddity_gpu_constants(
            asset, referenceSettings, static_cast<std::uint32_t>(particles.size()));
        require(simulation.upload_constants(std::as_bytes(std::span{&constants, 1U}), &error), error);
        const auto commands = device.begin_commands(dve::rhi::QueueKind::Compute,
                                                     "simulation contract", &error);
        require(static_cast<bool>(commands), error);
        dve::render::FluoddityGpuStepPlan plan;
        plan.enabled = true;
        plan.steps = 2U;
        plan.particleGroups = 2U;
        plan.trailGroups = {2U, 2U, 2U};
        plan.initialTrailReadIndex = 0U;
        plan.finalTrailReadIndex = 0U;
        require(simulation.record(commands, plan, &error), error);
        const auto fence = device.submit(commands, &error);
        require(static_cast<bool>(fence), error);
        require(device.wait(fence, &error), error);
        require(simulation.resources().residentBytes > 0U,
                "Fluoddity GPU resource accounting is empty");

        dve::rhi::ComputePipelineDesc genericGridPipelineDesc;
        genericGridPipelineDesc.debugName = "grid-fluid contract pipeline";
        genericGridPipelineDesc.bindGroupLayouts.push_back(simulation.resources().layout);
        genericGridPipelineDesc.threadsX = 4U;
        genericGridPipelineDesc.threadsY = 4U;
        genericGridPipelineDesc.threadsZ = 4U;
        const auto genericGridPipeline = device.create_compute_pipeline(
            genericGridPipelineDesc, &error);
        require(static_cast<bool>(genericGridPipeline), error);
        dve::render::GridFluidGpuPipelines gridPipelines;
        gridPipelines.applyEmitters = genericGridPipeline;
        gridPipelines.advectVelocityForward = genericGridPipeline;
        gridPipelines.advectVelocityBackward = genericGridPipeline;
        gridPipelines.correctVelocity = genericGridPipeline;
        gridPipelines.advectScalarsForward = genericGridPipeline;
        gridPipelines.advectScalarsBackward = genericGridPipeline;
        gridPipelines.correctScalars = genericGridPipeline;
        gridPipelines.combustion = genericGridPipeline;
        gridPipelines.applyForces = genericGridPipeline;
        gridPipelines.vorticityConfinement = genericGridPipeline;
        gridPipelines.enforceBoundaries = genericGridPipeline;
        gridPipelines.computeDivergence = genericGridPipeline;
        gridPipelines.pressureSolve = genericGridPipeline;
        gridPipelines.projectVelocity = genericGridPipeline;
        gridPipelines.particleToGrid = genericGridPipeline;
        gridPipelines.normalizeParticleGrid = genericGridPipeline;
        gridPipelines.gridToParticle = genericGridPipeline;
        gridPipelines.buildVolumeOutput = genericGridPipeline;
        dve::GridFluidSettings gridSettings;
        gridSettings.dimensions = {8U, 8U, 8U};
        gridSettings.pressureMaximumIterations = 2U;
        gridSettings.scalarAdvection = dve::GridFluidAdvection::SemiLagrangian;
        gridSettings.velocityAdvection = dve::GridFluidAdvection::SemiLagrangian;
        gridSettings.combustionEnabled = false;
        gridSettings.vorticityConfinement = 0.0F;
        const auto gridPlan = dve::plan_grid_fluid_gpu_frame(
            gridSettings, 1U, false, false, false);
        require(gridPlan.validate(&error), error);
        const auto gridCommands = device.begin_commands(
            dve::rhi::QueueKind::Compute, "grid-fluid simulation contract", &error);
        require(static_cast<bool>(gridCommands), error);
        require(dve::render::record_grid_fluid_gpu_frame(
            device, gridCommands, simulation.resources().readAWriteB, gridPipelines,
            gridPlan, &error), error);
        const auto gridFence = device.submit(gridCommands, &error);
        require(static_cast<bool>(gridFence) && device.wait(gridFence, &error), error);

#if defined(DVE_ENABLE_FLIP_LIQUIDS)
        dve::render::FlipLiquidGpuPipelines flipPipelines;
        flipPipelines.clearGrid = genericGridPipeline;
        flipPipelines.particleToGrid = genericGridPipeline;
        flipPipelines.normalizeGrid = genericGridPipeline;
        flipPipelines.saveGridVelocity = genericGridPipeline;
        flipPipelines.classifyCells = genericGridPipeline;
        flipPipelines.buildParticleLevelSet = genericGridPipeline;
        flipPipelines.applyForces = genericGridPipeline;
        flipPipelines.enforceBoundaries = genericGridPipeline;
        flipPipelines.computeDivergence = genericGridPipeline;
        flipPipelines.pressureSolve = genericGridPipeline;
        flipPipelines.projectVelocity = genericGridPipeline;
        flipPipelines.extrapolateVelocity = genericGridPipeline;
        flipPipelines.gridToParticle = genericGridPipeline;
        flipPipelines.advectParticles = genericGridPipeline;
        flipPipelines.reseedParticles = genericGridPipeline;
        flipPipelines.buildSurfaceOutput = genericGridPipeline;
        dve::FlipLiquidSettings flipSettings;
        flipSettings.dimensions = {8U, 8U, 8U};
        flipSettings.pressureMaximumIterations = 2U;
        flipSettings.velocityExtrapolationLayers = 2U;
        const auto flipPlan = dve::plan_flip_liquid_gpu_frame(
            flipSettings, 128U, 1U, true);
        require(flipPlan.validate(&error), error);
        const auto flipCommands = device.begin_commands(
            dve::rhi::QueueKind::Compute, "FLIP liquid simulation contract", &error);
        require(static_cast<bool>(flipCommands), error);
        require(dve::render::record_flip_liquid_gpu_frame(
            device, flipCommands, simulation.resources().readAWriteB, flipPipelines,
            flipPlan, &error), error);
        const auto flipFence = device.submit(flipCommands, &error);
        require(static_cast<bool>(flipFence) && device.wait(flipFence, &error), error);
#endif
        require(device.destroy_compute_pipeline(genericGridPipeline, &error), error);

        std::cout << "DVE simulation GPU contract tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE simulation GPU contract tests failed: " << exception.what() << '\n';
        return 1;
    }
}
