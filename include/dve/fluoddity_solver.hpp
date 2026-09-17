#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "dve/fluoddity.hpp"
#include "dve/simulation_quality.hpp"
#include "dve/transform.hpp"

namespace dve {

struct alignas(16) FluoddityParticleState {
    Float3 position{};
    float padding0{};
    Float3 velocity{};
    float hue{};
    float size{1.0F};
    std::uint32_t cohort{};
    std::uint32_t age{};
    std::uint32_t padding1{};
};
static_assert(sizeof(FluoddityParticleState) == 48U);

struct FluoddityTrailVoxel {
    Float3 velocity{};
    float density{};
};
static_assert(sizeof(FluoddityTrailVoxel) == 16U);

struct FluoddityFixedPointVoxel {
    std::array<std::int32_t, 4> channels{};
};
static_assert(sizeof(FluoddityFixedPointVoxel) == 16U);

enum class FluoddityTrailSampling : std::uint8_t { Nearest, Trilinear };

struct FluoddityReferenceSettings {
    std::uint32_t trailResolution{16U};
    Float3 boundsHalfExtent{1.0F, 1.0F, 1.0F};
    float deltaSeconds{1.0F / 60.0F};
    float fixedPointScale{1'048'576.0F};
    float maximumSpeed{4.0F};
    float depositStrength{1.0F};
    std::uint32_t frameNumber{};
    FluoddityTrailSampling trailSampling{FluoddityTrailSampling::Trilinear};
    float maximumDisplacementFraction{0.4F};
    std::uint32_t maximumSubsteps{8U};
};

struct FluoddityReferenceStepTelemetry {
    std::uint64_t movedParticles{};
    std::uint64_t resetParticles{};
    std::uint64_t depositedParticles{};
    std::uint64_t saturatedAtomicAdds{};
    std::uint64_t nonFiniteCorrections{};
    float maximumObservedSpeed{};
};

struct FluoddityReferenceState {
    std::vector<FluoddityParticleState> particles;
    std::vector<FluoddityTrailVoxel> trailA;
    std::vector<FluoddityTrailVoxel> trailB;
    std::vector<FluoddityFixedPointVoxel> accumulation;
    std::uint8_t trailReadIndex{};

    [[nodiscard]] bool validate(const FluoddityReferenceSettings& settings,
                                std::string* error = nullptr) const;
};

[[nodiscard]] FluoddityReferenceState make_fluoddity_reference_state(
    const FluoddityRuleAsset& asset,
    std::uint32_t particleCount,
    const FluoddityReferenceSettings& settings);

[[nodiscard]] FluoddityReferenceStepTelemetry step_fluoddity_reference(
    const FluoddityRuleAsset& asset,
    const FluoddityReferenceSettings& settings,
    FluoddityReferenceState& state);


[[nodiscard]] FluoddityTrailVoxel sample_fluoddity_trail(
    const std::vector<FluoddityTrailVoxel>& trail,
    const FluoddityReferenceSettings& settings,
    Float3 position) noexcept;

[[nodiscard]] SimulationStepPlan plan_fluoddity_reference_step(
    const FluoddityReferenceSettings& settings,
    const FluoddityReferenceState& state);

[[nodiscard]] const std::vector<FluoddityTrailVoxel>& fluoddity_read_trail(
    const FluoddityReferenceState& state) noexcept;

} // namespace dve
