#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dve/transform.hpp"
#include "dve/simulation_quality.hpp"

namespace dve {

struct PbfLiquidParticle {
    Float3 position{};
    Float3 predictedPosition{};
    Float3 velocity{};
    float density{};
    float lambda{};
};

struct PbfLiquidSettings {
    float particleRadius{0.04F};
    float kernelRadius{0.12F};
    float restDensity{1000.0F};
    float particleMass{1.0F};
    float constraintCompliance{1.0e-7F};
    float lambdaRelaxation{100.0F};
    float artificialPressure{0.001F};
    float artificialPressureRadiusRatio{0.3F};
    float viscosity{0.01F};
    float vorticityConfinement{};
    Float3 gravity{0.0F, -9.81F, 0.0F};
    Float3 boundsMinimum{-1.0F, 0.0F, -1.0F};
    Float3 boundsMaximum{1.0F, 2.0F, 1.0F};
    std::uint32_t solverIterations{4U};
    std::uint32_t maximumSubsteps{8U};
    std::uint32_t maximumNeighbors{128U};
    float maximumDisplacementFraction{0.4F};
    float correctionClampRatio{0.25F};
    bool rebuildNeighborsEachIteration{true};
    std::uint32_t maximumParticles{262'144U};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

struct PbfLiquidTelemetry {
    std::uint64_t particlesIntegrated{};
    std::uint64_t neighborPairs{};
    std::uint64_t densityConstraints{};
    std::uint64_t positionCorrections{};
    std::uint64_t boundaryContacts{};
    std::uint64_t nonFiniteCorrections{};
    std::uint32_t maximumNeighbors{};
    std::uint64_t neighborOverflow{};
    std::uint64_t vorticityUpdates{};
    std::uint64_t substeps{};
    bool substepBudgetClamped{};
    float maximumDensityError{};
};

class PbfLiquidWorld {
public:
    explicit PbfLiquidWorld(PbfLiquidSettings settings = {});
    [[nodiscard]] bool set_settings(PbfLiquidSettings settings,
                                    std::string* error = nullptr);
    [[nodiscard]] bool add_particle(Float3 position, Float3 velocity = {},
                                    std::string* error = nullptr);
    [[nodiscard]] std::uint32_t add_box(Float3 minimum, Float3 maximum, float spacing,
                                        Float3 initialVelocity = {},
                                        std::string* error = nullptr);
    void clear() noexcept;
    [[nodiscard]] PbfLiquidTelemetry step(float deltaSeconds);
    [[nodiscard]] SimulationStepPlan plan_step(float deltaSeconds) const;

    [[nodiscard]] const PbfLiquidSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] const std::vector<PbfLiquidParticle>& particles() const noexcept {
        return particles_;
    }

private:
    PbfLiquidSettings settings_;
    std::vector<PbfLiquidParticle> particles_;
};

enum class PbfGpuPassKind : std::uint8_t {
    Predict,
    BuildSpatialKeys,
    RadixSort,
    BuildCellRanges,
    ComputeDensityLambda,
    CorrectPositions,
    ApplyBoundaries,
    UpdateVelocity,
    ApplyViscosity,
    BuildSurfaceData,
};

struct PbfGpuDispatch {
    PbfGpuPassKind pass{PbfGpuPassKind::Predict};
    std::uint32_t iteration{};
    std::uint32_t groupsX{1U};
};

struct PbfGpuFramePlan {
    bool enabled{};
    std::uint32_t particleCount{};
    std::uint32_t paddedSortCount{};
    std::uint32_t solverIterations{};
    std::uint64_t estimatedBytes{};
    std::vector<PbfGpuDispatch> dispatches;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

[[nodiscard]] PbfGpuFramePlan plan_pbf_gpu_frame(const PbfLiquidSettings& settings,
                                                  std::uint32_t particleCount,
                                                  bool buildSurfaceData);

struct SpatiotemporalFlipSample {
    std::uint32_t particleIndex{};
    float normalizedTime{};
    float weight{};
};

struct SpatiotemporalFlipPlan {
    bool enabled{};
    std::uint32_t temporalSamples{1U};
    std::uint32_t timeSlabs{1U};
    float jitterAmplitude{0.0F};
    std::vector<SpatiotemporalFlipSample> samples;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

[[nodiscard]] SpatiotemporalFlipPlan plan_spatiotemporal_flip_samples(
    std::uint32_t particleCount,
    std::uint32_t temporalSamples,
    std::uint32_t timeSlabs,
    float jitterAmplitude,
    std::uint64_t seed);

} // namespace dve
