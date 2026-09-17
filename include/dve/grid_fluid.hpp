#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dve/simulation_quality.hpp"
#include "dve/transform.hpp"

namespace dve {

enum class GridFluidAdvection : std::uint8_t {
    SemiLagrangian,
    MacCormack,
};

enum class GridFluidCellType : std::uint8_t {
    Fluid,
    Solid,
};

enum class GridFluidTransferMode : std::uint8_t {
    Pic,
    FlipBlend,
};

struct GridFluidDimensions {
    std::uint32_t x{24U};
    std::uint32_t y{32U};
    std::uint32_t z{24U};

    [[nodiscard]] std::uint64_t cell_count() const noexcept;
    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct GridFluidSettings {
    GridFluidDimensions dimensions{};
    float cellSize{0.1F};
    float maximumCfl{2.0F};
    float maximumSubstepSeconds{1.0F / 30.0F};
    std::uint32_t maximumSubsteps{8U};
    GridFluidAdvection scalarAdvection{GridFluidAdvection::MacCormack};
    GridFluidAdvection velocityAdvection{GridFluidAdvection::SemiLagrangian};
    std::uint32_t traceOrder{2U};

    Float3 gravity{};
    float ambientTemperature{0.0F};
    float temperatureBuoyancy{1.0F};
    float smokeWeight{0.05F};
    float vorticityConfinement{0.15F};

    float densityDissipation{0.05F};
    float temperatureDissipation{0.2F};
    float fuelDissipation{0.02F};
    float flameDissipation{1.5F};

    bool combustionEnabled{true};
    float ignitionTemperature{0.5F};
    float burnRate{1.5F};
    float heatRelease{2.5F};
    float smokeYield{0.8F};
    float flameYield{1.0F};

    std::uint32_t pressureMaximumIterations{120U};
    float pressureRelativeTolerance{1.0e-5F};
    bool warmStartPressure{true};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct GridFluidMacGrid {
    GridFluidDimensions dimensions{};
    std::vector<float> u;
    std::vector<float> v;
    std::vector<float> w;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    void clear() noexcept;
};

struct GridFluidState {
    GridFluidSettings settings{};
    GridFluidMacGrid velocity;
    GridFluidMacGrid previousVelocity;
    std::vector<float> density;
    std::vector<float> temperature;
    std::vector<float> fuel;
    std::vector<float> flame;
    std::vector<float> pressure;
    std::vector<float> divergence;
    std::vector<GridFluidCellType> cellTypes;
    std::uint64_t frameIndex{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct GridFluidEmitterSample {
    Float3 center{};
    float radius{0.25F};
    float density{1.0F};
    float temperature{1.0F};
    float fuel{};
    float flame{};
    Float3 velocity{};
};

struct GridFluidTelemetry {
    std::uint64_t substeps{};
    bool substepBudgetClamped{};
    std::uint64_t scalarCellsAdvected{};
    std::uint64_t velocityFacesAdvected{};
    std::uint64_t combustionCells{};
    std::uint64_t solidFacesClamped{};
    std::uint64_t pressureIterations{};
    std::uint64_t nonFiniteCorrections{};
    float maximumSpeedBefore{};
    float maximumSpeedAfter{};
    float maximumDivergenceBeforeProjection{};
    float maximumDivergenceAfterProjection{};
    float pressureResidual{};
    float totalDensity{};
    float totalFuel{};
};

[[nodiscard]] GridFluidState make_grid_fluid_state(const GridFluidSettings& settings,
                                                     std::string* error = nullptr);
[[nodiscard]] SimulationStepPlan plan_grid_fluid_step(float frameDeltaSeconds,
                                                       const GridFluidState& state);
[[nodiscard]] GridFluidTelemetry step_grid_fluid(GridFluidState& state,
                                                  float frameDeltaSeconds);

void clear_grid_fluid(GridFluidState& state) noexcept;
void clear_grid_fluid_obstacles(GridFluidState& state) noexcept;
[[nodiscard]] bool set_grid_fluid_solid_box(GridFluidState& state,
                                             Float3 minimum,
                                             Float3 maximum,
                                             std::string* error = nullptr);
[[nodiscard]] bool inject_grid_fluid_sphere(GridFluidState& state,
                                             const GridFluidEmitterSample& emitter,
                                             std::string* error = nullptr);

[[nodiscard]] float sample_grid_fluid_scalar(const GridFluidState& state,
                                              const std::vector<float>& field,
                                              Float3 worldPosition) noexcept;
[[nodiscard]] Float3 sample_grid_fluid_velocity(const GridFluidState& state,
                                                Float3 worldPosition) noexcept;
[[nodiscard]] float maximum_grid_fluid_divergence(const GridFluidState& state) noexcept;
[[nodiscard]] std::uint64_t estimate_grid_fluid_bytes(const GridFluidSettings& settings) noexcept;

struct GridFluidVolumeVoxel {
    float density{};
    float temperature{};
    float emission{};
    float fuel{};
};

struct GridFluidVolumeSnapshot {
    GridFluidDimensions dimensions{};
    float cellSize{};
    std::uint64_t sourceFrame{};
    std::vector<GridFluidVolumeVoxel> voxels;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

[[nodiscard]] GridFluidVolumeSnapshot build_grid_fluid_volume_snapshot(
    const GridFluidState& state,
    float emissionScale = 1.0F);

struct GridFluidParticle {
    Float3 position{};
    Float3 velocity{};
};

struct GridFluidParticleTransferSettings {
    GridFluidTransferMode mode{GridFluidTransferMode::FlipBlend};
    float flipRatio{0.95F};
    bool advectParticles{true};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct GridFluidParticleTransferTelemetry {
    std::uint64_t particlesDeposited{};
    std::uint64_t faceContributions{};
    std::uint64_t particlesUpdated{};
    std::uint64_t particlesClamped{};
    std::uint64_t particlesInsideSolids{};
};

[[nodiscard]] GridFluidParticleTransferTelemetry deposit_grid_fluid_particles_to_mac(
    GridFluidState& state,
    const std::vector<GridFluidParticle>& particles);
[[nodiscard]] GridFluidParticleTransferTelemetry update_grid_fluid_particles_from_mac(
    const GridFluidState& state,
    std::vector<GridFluidParticle>& particles,
    const GridFluidParticleTransferSettings& settings,
    float deltaSeconds);

enum class GridFluidGpuPassKind : std::uint8_t {
    ApplyEmitters,
    AdvectVelocityForward,
    AdvectVelocityBackward,
    CorrectVelocity,
    AdvectScalarsForward,
    AdvectScalarsBackward,
    CorrectScalars,
    Combustion,
    ApplyForces,
    VorticityConfinement,
    EnforceBoundaries,
    ComputeDivergence,
    PressureSolve,
    ProjectVelocity,
    ParticleToGrid,
    NormalizeParticleGrid,
    GridToParticle,
    BuildVolumeOutput,
};

struct GridFluidGpuDispatch {
    GridFluidGpuPassKind pass{GridFluidGpuPassKind::ApplyEmitters};
    std::uint32_t iteration{};
    std::uint32_t groupsX{1U};
    std::uint32_t groupsY{1U};
    std::uint32_t groupsZ{1U};
};

struct GridFluidGpuFramePlan {
    bool enabled{};
    GridFluidDimensions dimensions{};
    std::uint32_t substeps{};
    std::uint32_t pressureIterations{};
    bool usesMacCormackScalars{};
    bool usesMacCormackVelocity{};
    bool usesParticles{};
    std::uint64_t estimatedBytes{};
    std::vector<GridFluidGpuDispatch> dispatches;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

[[nodiscard]] GridFluidGpuFramePlan plan_grid_fluid_gpu_frame(
    const GridFluidSettings& settings,
    std::uint32_t substeps,
    bool hasEmitters,
    bool useParticles,
    bool buildVolumeOutput);

} // namespace dve
