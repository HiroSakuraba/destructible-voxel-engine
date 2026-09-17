#include "dve/render/simulation_gpu.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace dve::render {
namespace {

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

[[nodiscard]] std::uint32_t ceil_div(std::uint32_t value, std::uint32_t divisor) noexcept {
    return (value + divisor - 1U) / divisor;
}

[[nodiscard]] rhi::ComputePipelineHandle vfx_pipeline(
    const VfxGpuPipelines& pipelines, VfxGpuPassKind pass) noexcept {
    switch (pass) {
    case VfxGpuPassKind::ResetCounters: return pipelines.resetCounters;
    case VfxGpuPassKind::Spawn: return pipelines.spawn;
    case VfxGpuPassKind::Update: return pipelines.update;
    case VfxGpuPassKind::BuildEvents: return pipelines.buildEvents;
    case VfxGpuPassKind::Compact: return pipelines.compact;
    case VfxGpuPassKind::Sort: return pipelines.sort;
    case VfxGpuPassKind::BuildIndirectDraw: return pipelines.buildIndirectDraw;
    }
    return {};
}

[[nodiscard]] rhi::ComputePipelineHandle pbf_pipeline(
    const PbfGpuPipelines& pipelines, PbfGpuPassKind pass) noexcept {
    switch (pass) {
    case PbfGpuPassKind::Predict: return pipelines.predict;
    case PbfGpuPassKind::BuildSpatialKeys: return pipelines.buildSpatialKeys;
    case PbfGpuPassKind::RadixSort: return pipelines.radixSort;
    case PbfGpuPassKind::BuildCellRanges: return pipelines.buildCellRanges;
    case PbfGpuPassKind::ComputeDensityLambda: return pipelines.computeDensityLambda;
    case PbfGpuPassKind::CorrectPositions: return pipelines.correctPositions;
    case PbfGpuPassKind::ApplyBoundaries: return pipelines.applyBoundaries;
    case PbfGpuPassKind::UpdateVelocity: return pipelines.updateVelocity;
    case PbfGpuPassKind::ApplyViscosity: return pipelines.applyViscosity;
    case PbfGpuPassKind::BuildSurfaceData: return pipelines.buildSurfaceData;
    }
    return {};
}

[[nodiscard]] rhi::ComputePipelineHandle grid_fluid_pipeline(
    const GridFluidGpuPipelines& pipelines, GridFluidGpuPassKind pass) noexcept {
    switch (pass) {
    case GridFluidGpuPassKind::ApplyEmitters: return pipelines.applyEmitters;
    case GridFluidGpuPassKind::AdvectVelocityForward: return pipelines.advectVelocityForward;
    case GridFluidGpuPassKind::AdvectVelocityBackward: return pipelines.advectVelocityBackward;
    case GridFluidGpuPassKind::CorrectVelocity: return pipelines.correctVelocity;
    case GridFluidGpuPassKind::AdvectScalarsForward: return pipelines.advectScalarsForward;
    case GridFluidGpuPassKind::AdvectScalarsBackward: return pipelines.advectScalarsBackward;
    case GridFluidGpuPassKind::CorrectScalars: return pipelines.correctScalars;
    case GridFluidGpuPassKind::Combustion: return pipelines.combustion;
    case GridFluidGpuPassKind::ApplyForces: return pipelines.applyForces;
    case GridFluidGpuPassKind::VorticityConfinement: return pipelines.vorticityConfinement;
    case GridFluidGpuPassKind::EnforceBoundaries: return pipelines.enforceBoundaries;
    case GridFluidGpuPassKind::ComputeDivergence: return pipelines.computeDivergence;
    case GridFluidGpuPassKind::PressureSolve: return pipelines.pressureSolve;
    case GridFluidGpuPassKind::ProjectVelocity: return pipelines.projectVelocity;
    case GridFluidGpuPassKind::ParticleToGrid: return pipelines.particleToGrid;
    case GridFluidGpuPassKind::NormalizeParticleGrid: return pipelines.normalizeParticleGrid;
    case GridFluidGpuPassKind::GridToParticle: return pipelines.gridToParticle;
    case GridFluidGpuPassKind::BuildVolumeOutput: return pipelines.buildVolumeOutput;
    }
    return {};
}

#if defined(DVE_ENABLE_FLIP_LIQUIDS)
[[nodiscard]] rhi::ComputePipelineHandle flip_liquid_pipeline(
    const FlipLiquidGpuPipelines& pipelines, FlipLiquidGpuPassKind pass) noexcept {
    switch (pass) {
    case FlipLiquidGpuPassKind::ClearGrid: return pipelines.clearGrid;
    case FlipLiquidGpuPassKind::ParticleToGrid: return pipelines.particleToGrid;
    case FlipLiquidGpuPassKind::NormalizeGrid: return pipelines.normalizeGrid;
    case FlipLiquidGpuPassKind::SaveGridVelocity: return pipelines.saveGridVelocity;
    case FlipLiquidGpuPassKind::ClassifyCells: return pipelines.classifyCells;
    case FlipLiquidGpuPassKind::BuildParticleLevelSet: return pipelines.buildParticleLevelSet;
    case FlipLiquidGpuPassKind::ApplyForces: return pipelines.applyForces;
    case FlipLiquidGpuPassKind::EnforceBoundaries: return pipelines.enforceBoundaries;
    case FlipLiquidGpuPassKind::ComputeDivergence: return pipelines.computeDivergence;
    case FlipLiquidGpuPassKind::PressureSolve: return pipelines.pressureSolve;
    case FlipLiquidGpuPassKind::ProjectVelocity: return pipelines.projectVelocity;
    case FlipLiquidGpuPassKind::ExtrapolateVelocity: return pipelines.extrapolateVelocity;
    case FlipLiquidGpuPassKind::GridToParticle: return pipelines.gridToParticle;
    case FlipLiquidGpuPassKind::AdvectParticles: return pipelines.advectParticles;
    case FlipLiquidGpuPassKind::ReseedParticles: return pipelines.reseedParticles;
    case FlipLiquidGpuPassKind::BuildSurfaceOutput: return pipelines.buildSurfaceOutput;
    }
    return {};
}
#endif

} // namespace

FluoddityGpuConstants make_fluoddity_gpu_constants(
    const FluoddityRuleAsset& asset, const FluoddityReferenceSettings& settings,
    std::uint32_t particleCount) noexcept {
    FluoddityGpuConstants constants;
    constants.simulation = {particleCount, settings.trailResolution, settings.frameNumber,
                            static_cast<std::uint32_t>(asset.boundaryMode)};
    constants.integration = {settings.deltaSeconds, settings.fixedPointScale,
                             settings.maximumSpeed, settings.depositStrength};
    const auto dragIndex = static_cast<std::size_t>(FluoddityParameter::Drag);
    constants.boundsAndDrag = {settings.boundsHalfExtent.x, settings.boundsHalfExtent.y,
                              settings.boundsHalfExtent.z, asset.parameters[dragIndex].value};
    const auto persistenceIndex = static_cast<std::size_t>(FluoddityParameter::TrailPersistence);
    const auto diffusionIndex = static_cast<std::size_t>(FluoddityParameter::TrailDiffusion);
    constants.field = {asset.gravityForce, asset.gravityStrafe,
                       asset.parameters[persistenceIndex].value,
                       asset.parameters[diffusionIndex].value};
    constants.flags = {asset.cohortCount, static_cast<std::uint32_t>(asset.trailMode),
                       asset.parameterSweepsEnabled ? 1U : 0U, asset.disableSymmetry ? 1U : 0U};
    for (std::size_t index = 0U; index < asset.parameters.size(); ++index) {
        const auto& parameter = asset.parameters[index];
        constants.parameterData[index * 2U] = {parameter.value, parameter.minimum,
                                               parameter.maximum, parameter.xSweep};
        constants.parameterData[index * 2U + 1U] = {parameter.ySweep, parameter.cohortSweep,
                                                    parameter.jitter, 0.0F};
    }
    for (std::size_t center = 0U; center < asset.rule.size(); ++center) {
        constants.ruleData[center * 3U] = asset.rule[center].frequency;
        constants.ruleData[center * 3U + 1U] = asset.rule[center].amplitude;
        constants.ruleData[center * 3U + 2U] = {asset.rule[center].frequencyExtension[0],
                                               asset.rule[center].frequencyExtension[1],
                                               asset.rule[center].amplitudeExtension[0],
                                               asset.rule[center].amplitudeExtension[1]};
    }
    return constants;
}

bool FluoddityGpuStepPlan::validate(std::string* error) const {
    if (!enabled) return true;
    if (steps == 0U || particleGroups == 0U || trailGroups[0] == 0U ||
        trailGroups[1] == 0U || trailGroups[2] == 0U || initialTrailReadIndex > 1U ||
        finalTrailReadIndex > 1U || finalTrailReadIndex !=
            static_cast<std::uint8_t>(initialTrailReadIndex ^ (steps & 1U))) {
        set_error(error, "enabled Fluoddity GPU step plan is invalid");
        return false;
    }
    return true;
}

FluoddityGpuSimulation::~FluoddityGpuSimulation() { clear(); }

bool FluoddityGpuSimulation::initialize(const FluoddityQualityProfile& quality,
                                        const FluoddityGpuShaderBytecode& shaders,
                                        std::string* error) {
    clear();
    if (quality.particleCapacity == 0U || quality.trailResolution == 0U) {
        set_error(error, "Fluoddity GPU quality profile is invalid");
        return false;
    }
    if (device_.capabilities().maxComputeInvocations < 64U) {
        set_error(error, "device does not support the Fluoddity 64-thread particle group");
        return false;
    }
    const std::uint64_t particleBytes64 = static_cast<std::uint64_t>(quality.particleCapacity) *
                                          sizeof(FluoddityParticleState);
    const std::uint64_t voxelCount = static_cast<std::uint64_t>(quality.trailResolution) *
                                     quality.trailResolution * quality.trailResolution;
    const std::uint64_t accumulationBytes64 = voxelCount * sizeof(FluoddityFixedPointVoxel);
    if (particleBytes64 > std::numeric_limits<std::size_t>::max() ||
        accumulationBytes64 > std::numeric_limits<std::size_t>::max()) {
        set_error(error, "Fluoddity GPU allocation exceeds addressable memory");
        return false;
    }
    std::string localError;
    resources_.constants = device_.create_buffer({
        4096U, rhi::BufferUsage::Constant | rhi::BufferUsage::CopyDestination,
        rhi::MemoryDomain::Upload, "Fluoddity constants", rhi::ResourceState::ShaderRead},
        &localError);
    resources_.particles = device_.create_buffer({
        static_cast<std::size_t>(particleBytes64),
        rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDestination |
            rhi::BufferUsage::CopySource,
        rhi::MemoryDomain::Upload, "Fluoddity particle state", rhi::ResourceState::ShaderWrite},
        &localError);
    resources_.fixedPointAccumulation = device_.create_buffer({
        static_cast<std::size_t>(accumulationBytes64),
        rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDestination |
            rhi::BufferUsage::CopySource,
        rhi::MemoryDomain::Upload, "Fluoddity fixed-point accumulation",
        rhi::ResourceState::ShaderWrite}, &localError);
    const rhi::TextureDesc trailDesc{
        rhi::TextureDimension::Texture3D, rhi::TextureFormat::RGBA16Float,
        quality.trailResolution, quality.trailResolution, quality.trailResolution, 1U, 1U,
        rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled,
        rhi::ResourceState::ShaderWrite, "Fluoddity trail"};
    resources_.trailA = device_.create_texture(trailDesc, &localError);
    resources_.trailB = device_.create_texture(trailDesc, &localError);
    resources_.trailViewA = device_.create_texture_view(
        {resources_.trailA, 0U, 1U, 0U, 1U, "Fluoddity trail A view"}, &localError);
    resources_.trailViewB = device_.create_texture_view(
        {resources_.trailB, 0U, 1U, 0U, 1U, "Fluoddity trail B view"}, &localError);
    if (!resources_.constants || !resources_.particles || !resources_.fixedPointAccumulation ||
        !resources_.trailA || !resources_.trailB || !resources_.trailViewA ||
        !resources_.trailViewB) {
        destroy_resources(nullptr);
        set_error(error, localError.empty() ? "Fluoddity GPU resource allocation failed" : localError);
        return false;
    }
    rhi::BindGroupLayoutDesc layout;
    layout.debugName = "Fluoddity simulation resources";
    layout.bindings = {
        {0U, rhi::BindingType::UniformBuffer, rhi::ShaderStage::Compute},
        {1U, rhi::BindingType::StorageBufferReadWrite, rhi::ShaderStage::Compute},
        {2U, rhi::BindingType::StorageBufferReadWrite, rhi::ShaderStage::Compute},
        {3U, rhi::BindingType::StorageTexture, rhi::ShaderStage::Compute},
        {4U, rhi::BindingType::StorageTexture, rhi::ShaderStage::Compute}};
    resources_.layout = device_.create_bind_group_layout(layout, &localError);
    auto makeGroup = [&](rhi::TextureViewHandle read, rhi::TextureViewHandle write,
                         const char* name) {
        rhi::BindGroupDesc group;
        group.layout = resources_.layout;
        group.debugName = name;
        group.entries = {
            {0U, resources_.constants, {}, 0U, 4096U},
            {1U, resources_.particles, {}, 0U, static_cast<std::size_t>(particleBytes64)},
            {2U, resources_.fixedPointAccumulation, {}, 0U,
             static_cast<std::size_t>(accumulationBytes64)},
            {3U, {}, read, 0U, 0U},
            {4U, {}, write, 0U, 0U}};
        return device_.create_bind_group(group, &localError);
    };
    resources_.readAWriteB = makeGroup(resources_.trailViewA, resources_.trailViewB,
                                       "Fluoddity read A write B");
    resources_.readBWriteA = makeGroup(resources_.trailViewB, resources_.trailViewA,
                                       "Fluoddity read B write A");
    auto makePipeline = [&](const char* name, const std::vector<std::byte>& bytecode,
                            std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        rhi::ComputePipelineDesc pipeline;
        pipeline.debugName = name;
        pipeline.bytecode = bytecode;
        pipeline.bindGroupLayouts.push_back(resources_.layout);
        pipeline.threadsX = x;
        pipeline.threadsY = y;
        pipeline.threadsZ = z;
        return device_.create_compute_pipeline(pipeline, &localError);
    };
    resources_.clearAccumulation = makePipeline("Fluoddity clear accumulation",
                                                shaders.clearAccumulation, 64U, 1U, 1U);
    resources_.senseMoveDeposit = makePipeline("Fluoddity sense move deposit",
                                               shaders.senseMoveDeposit, 64U, 1U, 1U);
    resources_.resolveDiffuse = makePipeline("Fluoddity resolve diffuse",
                                             shaders.resolveDiffuse, 4U, 4U, 4U);
    if (!resources_.layout || !resources_.readAWriteB || !resources_.readBWriteA ||
        !resources_.clearAccumulation || !resources_.senseMoveDeposit ||
        !resources_.resolveDiffuse) {
        destroy_resources(nullptr);
        set_error(error, localError.empty() ? "Fluoddity GPU pipeline creation failed" : localError);
        return false;
    }
    resources_.particleCapacity = quality.particleCapacity;
    resources_.trailResolution = quality.trailResolution;
    resources_.residentBytes = particleBytes64 + accumulationBytes64 + voxelCount * 16U;
    return true;
}

bool FluoddityGpuSimulation::upload_particles(
    std::span<const FluoddityParticleState> particles, std::string* error) {
    if (!resources_.particles || particles.size() > resources_.particleCapacity) {
        set_error(error, "Fluoddity particle upload exceeds initialized capacity");
        return false;
    }
    return device_.write_buffer(resources_.particles, 0U, std::as_bytes(particles), error);
}

bool FluoddityGpuSimulation::upload_constants(std::span<const std::byte> constants,
                                              std::string* error) {
    if (!resources_.constants || constants.size() > 4096U) {
        set_error(error, "Fluoddity constants exceed the 4096-byte contract");
        return false;
    }
    return device_.write_buffer(resources_.constants, 0U, constants, error);
}

bool FluoddityGpuSimulation::record(rhi::CommandListHandle commands,
                                    const FluoddityGpuStepPlan& plan,
                                    std::string* error) {
    std::string validation;
    if (!plan.validate(&validation)) {
        set_error(error, validation);
        return false;
    }
    if (!plan.enabled) return true;
    std::uint8_t readIndex = plan.initialTrailReadIndex;
    const std::uint64_t voxelCount = static_cast<std::uint64_t>(resources_.trailResolution) *
                                     resources_.trailResolution * resources_.trailResolution;
    const std::uint32_t clearGroups = ceil_div(
        static_cast<std::uint32_t>(std::min<std::uint64_t>(
            voxelCount, std::numeric_limits<std::uint32_t>::max())), 64U);
    for (std::uint32_t step = 0U; step < plan.steps; ++step) {
        const rhi::BindGroupHandle group = readIndex == 0U ? resources_.readAWriteB
                                                           : resources_.readBWriteA;
        if (!device_.bind_compute_bind_group(commands, 0U, group, error) ||
            !device_.dispatch(commands, resources_.clearAccumulation,
                              std::max(1U, clearGroups), 1U, 1U, error) ||
            !device_.dispatch(commands, resources_.senseMoveDeposit,
                              plan.particleGroups, 1U, 1U, error) ||
            !device_.dispatch(commands, resources_.resolveDiffuse,
                              plan.trailGroups[0], plan.trailGroups[1],
                              plan.trailGroups[2], error)) {
            return false;
        }
        readIndex ^= 1U;
    }
    return readIndex == plan.finalTrailReadIndex;
}

bool FluoddityGpuSimulation::destroy_resources(std::string* error) noexcept {
    bool success = true;
    auto destroyPipeline = [&](rhi::ComputePipelineHandle& handle) {
        if (handle && !device_.destroy_compute_pipeline(handle, error)) success = false;
        handle = {};
    };
    destroyPipeline(resources_.clearAccumulation);
    destroyPipeline(resources_.senseMoveDeposit);
    destroyPipeline(resources_.resolveDiffuse);
    if (resources_.readAWriteB && !device_.destroy_bind_group(resources_.readAWriteB, error)) success = false;
    if (resources_.readBWriteA && !device_.destroy_bind_group(resources_.readBWriteA, error)) success = false;
    resources_.readAWriteB = {};
    resources_.readBWriteA = {};
    if (resources_.layout && !device_.destroy_bind_group_layout(resources_.layout, error)) success = false;
    resources_.layout = {};
    if (resources_.trailViewA && !device_.destroy_texture_view(resources_.trailViewA, error)) success = false;
    if (resources_.trailViewB && !device_.destroy_texture_view(resources_.trailViewB, error)) success = false;
    resources_.trailViewA = {};
    resources_.trailViewB = {};
    if (resources_.trailA && !device_.destroy_texture(resources_.trailA, error)) success = false;
    if (resources_.trailB && !device_.destroy_texture(resources_.trailB, error)) success = false;
    resources_.trailA = {};
    resources_.trailB = {};
    if (resources_.fixedPointAccumulation &&
        !device_.destroy_buffer(resources_.fixedPointAccumulation, error)) success = false;
    if (resources_.particles && !device_.destroy_buffer(resources_.particles, error)) success = false;
    if (resources_.constants && !device_.destroy_buffer(resources_.constants, error)) success = false;
    resources_.fixedPointAccumulation = {};
    resources_.particles = {};
    resources_.constants = {};
    resources_.particleCapacity = 0U;
    resources_.trailResolution = 0U;
    resources_.residentBytes = 0U;
    return success;
}

void FluoddityGpuSimulation::clear() noexcept { (void)destroy_resources(nullptr); }

bool record_vfx_gpu_frame(rhi::IDevice& device, rhi::CommandListHandle commands,
                          rhi::BindGroupHandle resources, const VfxGpuPipelines& pipelines,
                          const VfxGpuFramePlan& plan, std::string* error) {
    std::string validation;
    if (!plan.validate(&validation)) {
        set_error(error, validation);
        return false;
    }
    if (!plan.enabled) return true;
    if (!device.bind_compute_bind_group(commands, 0U, resources, error)) return false;
    for (const auto& dispatch : plan.dispatches) {
        const auto pipeline = vfx_pipeline(pipelines, dispatch.pass);
        if (!pipeline) {
            set_error(error, "VFX GPU frame is missing a required pipeline");
            return false;
        }
        if (!device.dispatch(commands, pipeline, dispatch.groupsX, dispatch.groupsY,
                             dispatch.groupsZ, error)) return false;
    }
    return true;
}

bool record_pbf_gpu_frame(rhi::IDevice& device, rhi::CommandListHandle commands,
                          rhi::BindGroupHandle resources, const PbfGpuPipelines& pipelines,
                          const PbfGpuFramePlan& plan, std::string* error) {
    std::string validation;
    if (!plan.validate(&validation)) {
        set_error(error, validation);
        return false;
    }
    if (!plan.enabled) return true;
    if (!device.bind_compute_bind_group(commands, 0U, resources, error)) return false;
    for (const auto& dispatch : plan.dispatches) {
        const auto pipeline = pbf_pipeline(pipelines, dispatch.pass);
        if (!pipeline) {
            set_error(error, "PBF GPU frame is missing a required pipeline");
            return false;
        }
        if (!device.dispatch(commands, pipeline, dispatch.groupsX, 1U, 1U, error)) return false;
    }
    return true;
}

bool record_grid_fluid_gpu_frame(
    rhi::IDevice& device, rhi::CommandListHandle commands,
    rhi::BindGroupHandle resources, const GridFluidGpuPipelines& pipelines,
    const GridFluidGpuFramePlan& plan, std::string* error) {
    std::string validation;
    if (!plan.validate(&validation)) {
        set_error(error, validation);
        return false;
    }
    if (!plan.enabled) return true;
    if (!device.bind_compute_bind_group(commands, 0U, resources, error)) return false;
    for (const GridFluidGpuDispatch& dispatch : plan.dispatches) {
        const auto pipeline = grid_fluid_pipeline(pipelines, dispatch.pass);
        if (!pipeline) {
            set_error(error, "grid-fluid GPU frame is missing a required pipeline");
            return false;
        }
        if (!device.dispatch(commands, pipeline, dispatch.groupsX, dispatch.groupsY,
                             dispatch.groupsZ, error)) return false;
    }
    return true;
}

#if defined(DVE_ENABLE_FLIP_LIQUIDS)
bool record_flip_liquid_gpu_frame(
    rhi::IDevice& device, rhi::CommandListHandle commands,
    rhi::BindGroupHandle resources, const FlipLiquidGpuPipelines& pipelines,
    const FlipLiquidGpuFramePlan& plan, std::string* error) {
    std::string validation;
    if (!plan.validate(&validation)) {
        set_error(error, validation);
        return false;
    }
    if (!plan.enabled) return true;
    if (!device.bind_compute_bind_group(commands, 0U, resources, error)) return false;
    for (const FlipLiquidGpuDispatch& dispatch : plan.dispatches) {
        const auto pipeline = flip_liquid_pipeline(pipelines, dispatch.pass);
        if (!pipeline) {
            set_error(error, "FLIP liquid GPU frame is missing a required pipeline");
            return false;
        }
        if (!device.dispatch(commands, pipeline, dispatch.groupsX, dispatch.groupsY,
                             dispatch.groupsZ, error)) return false;
    }
    return true;
}
#endif

} // namespace dve::render
