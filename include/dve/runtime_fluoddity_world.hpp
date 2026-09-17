#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dve/fluoddity.hpp"
#include "dve/transform.hpp"

namespace dve {

using RuntimeFluoddityObjectId = std::uint64_t;
using RuntimeFluoddityAssetId = std::uint64_t;

enum class FluoddityRenderMode : std::uint8_t {
    ParticlePoints,
    SoftParticles,
    TrailVolume,
    Hybrid,
    GaborCinematicExperimental,
};

struct FluoddityInteractionOptions {
    bool collideWithVoxels{};
    bool collideWithPolygons{};
    bool reactToDestruction{};
    bool receiveWorldWind{true};
    bool writeGameplayDensity{};
};

struct RuntimeFluoddityInstance {
    RuntimeFluoddityObjectId objectId{};
    RuntimeFluoddityAssetId assetId{};
    RigidTransform worldTransform{};
    bool visible{true};
    bool running{true};
    bool selected{};
    float simulationFrequencyHz{60.0F};
    float timeScale{1.0F};
    FluoddityQualityProfile quality{};
    FluoddityRenderMode renderMode{FluoddityRenderMode::Hybrid};
    FluoddityInteractionOptions interaction{};
};

struct RuntimeFluoddityState {
    std::uint64_t simulationFrame{};
    double simulationTimeSeconds{};
    double fixedStepAccumulatorSeconds{};
    std::uint32_t pendingSingleSteps{};
    std::uint32_t resetGeneration{};
    std::uint8_t trailReadIndex{};
};

struct FluodditySimulationFramePlan {
    RuntimeFluoddityObjectId objectId{};
    RuntimeFluoddityAssetId assetId{};
    bool enabled{};
    bool budgetAccepted{};
    bool resetBeforeStep{};
    std::uint32_t stepCount{};
    std::uint32_t particleWorkgroupsPerStep{};
    std::array<std::uint32_t, 3> trailWorkgroupsPerStep{};
    std::uint32_t computePassesPerStep{};
    std::uint8_t initialTrailReadIndex{};
    std::uint8_t finalTrailReadIndex{};
    FluoddityMemoryEstimate memory{};
    std::string rejectionReason;
};

struct RuntimeFluodditySceneSnapshot {
    std::vector<FluoddityRuleAsset> assets;
    std::vector<RuntimeFluoddityInstance> instances;
    std::vector<RuntimeFluoddityState> states;
    std::uint64_t revision{};
    std::uint64_t globalGpuBudgetBytes{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
};

class RuntimeFluoddityWorld {
public:
    [[nodiscard]] RuntimeFluoddityObjectId create(
        FluoddityRuleAsset asset,
        RuntimeFluoddityInstance instance = {},
        RuntimeFluoddityObjectId requestedObjectId = 0,
        RuntimeFluoddityAssetId requestedAssetId = 0,
        std::string* error = nullptr);
    bool update_asset(RuntimeFluoddityObjectId objectId, FluoddityRuleAsset asset,
                      std::string* error = nullptr);
    bool update_instance(RuntimeFluoddityObjectId objectId,
                         const RuntimeFluoddityInstance& replacement,
                         std::string* error = nullptr);
    bool destroy(RuntimeFluoddityObjectId objectId) noexcept;

    bool play(RuntimeFluoddityObjectId objectId) noexcept;
    bool pause(RuntimeFluoddityObjectId objectId) noexcept;
    bool step_once(RuntimeFluoddityObjectId objectId) noexcept;
    bool reset(RuntimeFluoddityObjectId objectId) noexcept;

    void set_global_gpu_budget(std::uint64_t bytes) noexcept;
    [[nodiscard]] std::uint64_t global_gpu_budget() const noexcept { return globalGpuBudgetBytes_; }
    [[nodiscard]] std::vector<FluodditySimulationFramePlan> plan_frame(double deltaSeconds);

    [[nodiscard]] const RuntimeFluoddityInstance* find_instance(
        RuntimeFluoddityObjectId objectId) const noexcept;
    [[nodiscard]] const RuntimeFluoddityState* find_state(
        RuntimeFluoddityObjectId objectId) const noexcept;
    [[nodiscard]] const FluoddityRuleAsset* find_asset(
        RuntimeFluoddityAssetId assetId) const noexcept;
    [[nodiscard]] RuntimeFluodditySceneSnapshot snapshot() const;
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
    struct Entry {
        RuntimeFluoddityInstance instance;
        RuntimeFluoddityState state;
        std::uint32_t plannedResetGeneration{};
    };

    RuntimeFluoddityObjectId nextObjectId_{1U};
    std::map<RuntimeFluoddityObjectId, Entry> entries_;
    std::map<RuntimeFluoddityAssetId, FluoddityRuleAsset> assets_;
    std::map<RuntimeFluoddityAssetId, std::size_t> assetUsers_;
    std::uint64_t globalGpuBudgetBytes_{1024ULL * 1024ULL * 1024ULL};
    std::uint64_t revision_{};
};

[[nodiscard]] std::string fluoddity_render_mode_name(FluoddityRenderMode mode);

} // namespace dve
