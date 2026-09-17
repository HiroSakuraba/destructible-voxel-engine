#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dve/fluoddity_solver.hpp"
#include "dve/grid_fluid.hpp"
#if defined(DVE_ENABLE_FLIP_LIQUIDS)
#include "dve/flip_liquid.hpp"
#endif
#include "dve/liquid_pbf.hpp"
#include "dve/rhi/device.hpp"
#include "dve/vfx_particle_graph.hpp"

namespace dve::render {


struct alignas(16) FluoddityGpuConstants {
    std::array<std::uint32_t, 4> simulation{}; // particle count, trail resolution, frame, boundary
    std::array<float, 4> integration{};        // dt, fixed scale, max speed, deposit strength
    std::array<float, 4> boundsAndDrag{};      // half extent xyz, drag
    std::array<float, 4> field{};              // gravity, gravity strafe, persistence, diffusion
    std::array<std::uint32_t, 4> flags{};      // cohorts, trail mode, sweeps, disable symmetry
    std::array<std::array<float, 4>, kFluoddityParameterCount * 2U> parameterData{};
    std::array<std::array<float, 4>, kFluoddityFourierCenterCount * 3U> ruleData{};
};
static_assert(sizeof(FluoddityGpuConstants) == 944U);

[[nodiscard]] FluoddityGpuConstants make_fluoddity_gpu_constants(
    const FluoddityRuleAsset& asset,
    const FluoddityReferenceSettings& settings,
    std::uint32_t particleCount) noexcept;

struct FluoddityGpuShaderBytecode {
    std::vector<std::byte> clearAccumulation;
    std::vector<std::byte> senseMoveDeposit;
    std::vector<std::byte> resolveDiffuse;
};

struct FluoddityGpuResources {
    rhi::BufferHandle constants;
    rhi::BufferHandle particles;
    rhi::BufferHandle fixedPointAccumulation;
    rhi::TextureHandle trailA;
    rhi::TextureHandle trailB;
    rhi::TextureViewHandle trailViewA;
    rhi::TextureViewHandle trailViewB;
    rhi::BindGroupLayoutHandle layout;
    rhi::BindGroupHandle readAWriteB;
    rhi::BindGroupHandle readBWriteA;
    rhi::ComputePipelineHandle clearAccumulation;
    rhi::ComputePipelineHandle senseMoveDeposit;
    rhi::ComputePipelineHandle resolveDiffuse;
    std::uint32_t particleCapacity{};
    std::uint32_t trailResolution{};
    std::uint64_t residentBytes{};
};

struct FluoddityGpuStepPlan {
    bool enabled{};
    bool reset{};
    std::uint32_t steps{};
    std::uint32_t particleGroups{};
    std::array<std::uint32_t, 3> trailGroups{};
    std::uint8_t initialTrailReadIndex{};
    std::uint8_t finalTrailReadIndex{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

class FluoddityGpuSimulation {
public:
    explicit FluoddityGpuSimulation(rhi::IDevice& device) : device_(device) {}
    ~FluoddityGpuSimulation();
    FluoddityGpuSimulation(const FluoddityGpuSimulation&) = delete;
    FluoddityGpuSimulation& operator=(const FluoddityGpuSimulation&) = delete;

    bool initialize(const FluoddityQualityProfile& quality,
                    const FluoddityGpuShaderBytecode& shaders,
                    std::string* error = nullptr);
    bool upload_particles(std::span<const FluoddityParticleState> particles,
                          std::string* error = nullptr);
    bool upload_constants(std::span<const std::byte> constants,
                          std::string* error = nullptr);
    bool record(rhi::CommandListHandle commands, const FluoddityGpuStepPlan& plan,
                std::string* error = nullptr);
    void clear() noexcept;

    [[nodiscard]] const FluoddityGpuResources& resources() const noexcept { return resources_; }

private:
    bool destroy_resources(std::string* error) noexcept;
    rhi::IDevice& device_;
    FluoddityGpuResources resources_{};
};

struct VfxGpuPipelines {
    rhi::ComputePipelineHandle resetCounters;
    rhi::ComputePipelineHandle spawn;
    rhi::ComputePipelineHandle update;
    rhi::ComputePipelineHandle buildEvents;
    rhi::ComputePipelineHandle compact;
    rhi::ComputePipelineHandle sort;
    rhi::ComputePipelineHandle buildIndirectDraw;
};

bool record_vfx_gpu_frame(rhi::IDevice& device, rhi::CommandListHandle commands,
                          rhi::BindGroupHandle resources,
                          const VfxGpuPipelines& pipelines,
                          const VfxGpuFramePlan& plan,
                          std::string* error = nullptr);

struct PbfGpuPipelines {
    rhi::ComputePipelineHandle predict;
    rhi::ComputePipelineHandle buildSpatialKeys;
    rhi::ComputePipelineHandle radixSort;
    rhi::ComputePipelineHandle buildCellRanges;
    rhi::ComputePipelineHandle computeDensityLambda;
    rhi::ComputePipelineHandle correctPositions;
    rhi::ComputePipelineHandle applyBoundaries;
    rhi::ComputePipelineHandle updateVelocity;
    rhi::ComputePipelineHandle applyViscosity;
    rhi::ComputePipelineHandle buildSurfaceData;
};

bool record_pbf_gpu_frame(rhi::IDevice& device, rhi::CommandListHandle commands,
                          rhi::BindGroupHandle resources,
                          const PbfGpuPipelines& pipelines,
                          const PbfGpuFramePlan& plan,
                          std::string* error = nullptr);

struct GridFluidGpuPipelines {
    rhi::ComputePipelineHandle applyEmitters;
    rhi::ComputePipelineHandle advectVelocityForward;
    rhi::ComputePipelineHandle advectVelocityBackward;
    rhi::ComputePipelineHandle correctVelocity;
    rhi::ComputePipelineHandle advectScalarsForward;
    rhi::ComputePipelineHandle advectScalarsBackward;
    rhi::ComputePipelineHandle correctScalars;
    rhi::ComputePipelineHandle combustion;
    rhi::ComputePipelineHandle applyForces;
    rhi::ComputePipelineHandle vorticityConfinement;
    rhi::ComputePipelineHandle enforceBoundaries;
    rhi::ComputePipelineHandle computeDivergence;
    rhi::ComputePipelineHandle pressureSolve;
    rhi::ComputePipelineHandle projectVelocity;
    rhi::ComputePipelineHandle particleToGrid;
    rhi::ComputePipelineHandle normalizeParticleGrid;
    rhi::ComputePipelineHandle gridToParticle;
    rhi::ComputePipelineHandle buildVolumeOutput;
};

bool record_grid_fluid_gpu_frame(
    rhi::IDevice& device, rhi::CommandListHandle commands,
    rhi::BindGroupHandle resources, const GridFluidGpuPipelines& pipelines,
    const GridFluidGpuFramePlan& plan, std::string* error = nullptr);

#if defined(DVE_ENABLE_FLIP_LIQUIDS)
struct FlipLiquidGpuPipelines {
    rhi::ComputePipelineHandle clearGrid;
    rhi::ComputePipelineHandle particleToGrid;
    rhi::ComputePipelineHandle normalizeGrid;
    rhi::ComputePipelineHandle saveGridVelocity;
    rhi::ComputePipelineHandle classifyCells;
    rhi::ComputePipelineHandle buildParticleLevelSet;
    rhi::ComputePipelineHandle applyForces;
    rhi::ComputePipelineHandle enforceBoundaries;
    rhi::ComputePipelineHandle computeDivergence;
    rhi::ComputePipelineHandle pressureSolve;
    rhi::ComputePipelineHandle projectVelocity;
    rhi::ComputePipelineHandle extrapolateVelocity;
    rhi::ComputePipelineHandle gridToParticle;
    rhi::ComputePipelineHandle advectParticles;
    rhi::ComputePipelineHandle reseedParticles;
    rhi::ComputePipelineHandle buildSurfaceOutput;
};

bool record_flip_liquid_gpu_frame(
    rhi::IDevice& device, rhi::CommandListHandle commands,
    rhi::BindGroupHandle resources, const FlipLiquidGpuPipelines& pipelines,
    const FlipLiquidGpuFramePlan& plan, std::string* error = nullptr);
#endif

} // namespace dve::render
