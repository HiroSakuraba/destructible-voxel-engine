#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dve/grid_fluid.hpp"

namespace dve {

enum class FlipLiquidCellType : std::uint8_t {
    Air,
    Liquid,
    Solid,
};

enum class FlipLiquidTransferMode : std::uint8_t {
    Pic,
    Flip,
    FlipApic,
};

struct FlipLiquidParticle {
    Float3 position{};
    Float3 velocity{};
    // Rows of the APIC affine velocity matrix C.  Each row is the gradient of
    // one velocity component with respect to world position.
    Float3 affineX{};
    Float3 affineY{};
    Float3 affineZ{};
    std::uint32_t persistentId{};
};

struct FlipLiquidSettings {
    GridFluidDimensions dimensions{20U, 24U, 16U};
    float cellSize{0.08F};
    float particleRadius{0.025F};
    float particleDensity{1000.0F};
    Float3 gravity{0.0F, -9.81F, 0.0F};

    FlipLiquidTransferMode transferMode{FlipLiquidTransferMode::FlipApic};
    float flipRatio{0.95F};
    float apicRatio{1.0F};
    float affineClamp{40.0F};

    float maximumCfl{2.0F};
    float maximumSubstepSeconds{1.0F / 30.0F};
    std::uint32_t maximumSubsteps{8U};
    std::uint32_t pressureMaximumIterations{160U};
    float pressureRelativeTolerance{1.0e-5F};
    std::uint32_t velocityExtrapolationLayers{4U};
    std::uint32_t levelSetBandCells{3U};

    bool reseedingEnabled{true};
    std::uint32_t minimumParticlesPerCell{4U};
    std::uint32_t targetParticlesPerCell{8U};
    std::uint32_t maximumParticlesPerCell{16U};
    std::uint32_t maximumParticles{1'048'576U};
    std::uint64_t deterministicSeed{0x464C49505F445645ULL};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct FlipLiquidState {
    FlipLiquidSettings settings{};
    GridFluidMacGrid velocity;
    GridFluidMacGrid previousVelocity;
    std::vector<float> pressure;
    std::vector<float> divergence;
    std::vector<float> levelSet;
    std::vector<FlipLiquidCellType> cellTypes;
    std::vector<FlipLiquidParticle> particles;
    std::uint64_t frameIndex{};
    std::uint32_t nextParticleId{1U};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct FlipLiquidTelemetry {
    std::uint64_t substeps{};
    bool substepBudgetClamped{};
    std::uint64_t particleToGridContributions{};
    std::uint64_t gridToParticleUpdates{};
    std::uint64_t pressureIterations{};
    std::uint64_t extrapolatedFaces{};
    std::uint64_t particlesSpawned{};
    std::uint64_t particlesRemoved{};
    std::uint64_t particleBoundaryContacts{};
    std::uint64_t particleSolidContacts{};
    std::uint64_t nonFiniteCorrections{};
    std::uint32_t liquidCells{};
    std::uint32_t surfaceCells{};
    float maximumSpeedBefore{};
    float maximumSpeedAfter{};
    float maximumDivergenceBeforeProjection{};
    float maximumDivergenceAfterProjection{};
    float pressureResidual{};
    float estimatedLiquidVolume{};
};

[[nodiscard]] FlipLiquidState make_flip_liquid_state(
    const FlipLiquidSettings& settings,
    std::string* error = nullptr);

[[nodiscard]] bool add_flip_liquid_particle(FlipLiquidState& state,
                                             Float3 position,
                                             Float3 velocity = {},
                                             std::string* error = nullptr);
[[nodiscard]] std::uint32_t add_flip_liquid_box(FlipLiquidState& state,
                                                 Float3 minimum,
                                                 Float3 maximum,
                                                 float spacing,
                                                 Float3 velocity = {},
                                                 std::string* error = nullptr);
void clear_flip_liquid(FlipLiquidState& state) noexcept;
void clear_flip_liquid_solids(FlipLiquidState& state) noexcept;
[[nodiscard]] bool set_flip_liquid_solid_box(FlipLiquidState& state,
                                               Float3 minimum,
                                               Float3 maximum,
                                               std::string* error = nullptr);

[[nodiscard]] SimulationStepPlan plan_flip_liquid_step(float frameDeltaSeconds,
                                                         const FlipLiquidState& state);
[[nodiscard]] FlipLiquidTelemetry step_flip_liquid(FlipLiquidState& state,
                                                    float frameDeltaSeconds);

[[nodiscard]] Float3 sample_flip_liquid_velocity(const FlipLiquidState& state,
                                                  Float3 worldPosition) noexcept;
[[nodiscard]] float sample_flip_liquid_level_set(const FlipLiquidState& state,
                                                  Float3 worldPosition) noexcept;
[[nodiscard]] std::uint64_t estimate_flip_liquid_bytes(
    const FlipLiquidSettings& settings) noexcept;

struct FlipLiquidSurfaceSnapshot {
    GridFluidDimensions dimensions{};
    float cellSize{};
    std::uint64_t sourceFrame{};
    std::vector<float> levelSet;
    std::vector<FlipLiquidCellType> cellTypes;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

[[nodiscard]] FlipLiquidSurfaceSnapshot build_flip_liquid_surface_snapshot(
    const FlipLiquidState& state);

enum class FlipLiquidGpuPassKind : std::uint8_t {
    ClearGrid,
    ParticleToGrid,
    NormalizeGrid,
    SaveGridVelocity,
    ClassifyCells,
    BuildParticleLevelSet,
    ApplyForces,
    EnforceBoundaries,
    ComputeDivergence,
    PressureSolve,
    ProjectVelocity,
    ExtrapolateVelocity,
    GridToParticle,
    AdvectParticles,
    ReseedParticles,
    BuildSurfaceOutput,
};

struct FlipLiquidGpuDispatch {
    FlipLiquidGpuPassKind pass{FlipLiquidGpuPassKind::ClearGrid};
    std::uint32_t iteration{};
    std::uint32_t groupsX{1U};
    std::uint32_t groupsY{1U};
    std::uint32_t groupsZ{1U};
};

struct FlipLiquidGpuFramePlan {
    bool enabled{};
    GridFluidDimensions dimensions{};
    std::uint32_t particleCount{};
    std::uint32_t substeps{};
    std::uint32_t pressureIterations{};
    std::uint32_t extrapolationLayers{};
    bool usesApic{};
    bool reseeds{};
    std::uint64_t estimatedBytes{};
    std::vector<FlipLiquidGpuDispatch> dispatches;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

[[nodiscard]] FlipLiquidGpuFramePlan plan_flip_liquid_gpu_frame(
    const FlipLiquidSettings& settings,
    std::uint32_t particleCount,
    std::uint32_t substeps,
    bool buildSurfaceOutput);

} // namespace dve
