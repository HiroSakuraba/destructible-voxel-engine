#include "dve/runtime_fluoddity_world.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace dve {
namespace {

constexpr std::uint32_t kMaximumStepsPerFrame = 8U;
constexpr double kMaximumAcceptedDeltaSeconds = 0.25;

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

[[nodiscard]] bool valid_instance(const RuntimeFluoddityInstance& instance,
                                  std::string* error = nullptr) {
    if (instance.objectId == 0U || instance.assetId == 0U) {
        set_error(error, "runtime Fluoddity instance has an invalid identity");
        return false;
    }
    if (!std::isfinite(instance.simulationFrequencyHz) ||
        instance.simulationFrequencyHz < 1.0F || instance.simulationFrequencyHz > 1000.0F ||
        !std::isfinite(instance.timeScale) || instance.timeScale < 0.0F ||
        instance.timeScale > 16.0F || instance.quality.particleCapacity == 0U ||
        instance.quality.trailResolution == 0U || instance.quality.trailResolution > 2048U) {
        set_error(error, "runtime Fluoddity instance has invalid simulation settings");
        return false;
    }
    if (static_cast<std::uint8_t>(instance.renderMode) >
        static_cast<std::uint8_t>(FluoddityRenderMode::GaborCinematicExperimental)) {
        set_error(error, "runtime Fluoddity render mode is invalid");
        return false;
    }
    return true;
}

[[nodiscard]] std::uint32_t ceil_div(std::uint32_t value, std::uint32_t divisor) noexcept {
    return value / divisor + (value % divisor == 0U ? 0U : 1U);
}

} // namespace

bool RuntimeFluodditySceneSnapshot::validate(std::string* error) const {
    if (instances.size() != states.size()) {
        set_error(error, "runtime Fluoddity snapshot instance/state counts differ");
        return false;
    }
    std::set<RuntimeFluoddityAssetId> assetIds;
    for (const FluoddityRuleAsset& asset : assets) {
        std::string validation;
        if (!asset.validate(&validation) || asset.contentHash == 0U) {
            set_error(error, "invalid Fluoddity asset in snapshot: " + validation);
            return false;
        }
        if (!assetIds.insert(asset.contentHash).second) {
            set_error(error, "snapshot contains duplicate Fluoddity asset hashes");
            return false;
        }
    }
    std::set<RuntimeFluoddityObjectId> objectIds;
    for (std::size_t index = 0U; index < instances.size(); ++index) {
        const RuntimeFluoddityInstance& instance = instances[index];
        std::string validation;
        if (!valid_instance(instance, &validation) ||
            !objectIds.insert(instance.objectId).second) {
            set_error(error, validation.empty() ?
                "snapshot contains duplicate Fluoddity object IDs" : validation);
            return false;
        }
        if (!assetIds.contains(instance.assetId)) {
            set_error(error, "snapshot instance references an unavailable Fluoddity asset");
            return false;
        }
        const RuntimeFluoddityState& state = states[index];
        if (!std::isfinite(state.simulationTimeSeconds) ||
            !std::isfinite(state.fixedStepAccumulatorSeconds) ||
            state.simulationTimeSeconds < 0.0 || state.fixedStepAccumulatorSeconds < 0.0 ||
            state.trailReadIndex > 1U) {
            set_error(error, "snapshot contains invalid Fluoddity runtime state");
            return false;
        }
    }
    return true;
}

RuntimeFluoddityObjectId RuntimeFluoddityWorld::create(
    FluoddityRuleAsset asset,
    RuntimeFluoddityInstance instance,
    RuntimeFluoddityObjectId requestedObjectId,
    RuntimeFluoddityAssetId requestedAssetId,
    std::string* error) {
    asset.recompute_hash();
    std::string validation;
    if (!asset.validate(&validation) || asset.contentHash == 0U) {
        set_error(error, validation.empty() ? "invalid Fluoddity asset hash" : validation);
        return 0U;
    }
    RuntimeFluoddityObjectId objectId = requestedObjectId;
    if (objectId == 0U) {
        while (nextObjectId_ == 0U || entries_.contains(nextObjectId_)) ++nextObjectId_;
        objectId = nextObjectId_++;
    }
    if (entries_.contains(objectId)) {
        set_error(error, "duplicate runtime Fluoddity object ID");
        return 0U;
    }
    if (requestedAssetId != 0U && requestedAssetId != asset.contentHash) {
        set_error(error, "requested runtime Fluoddity asset ID must equal the content hash");
        return 0U;
    }
    const RuntimeFluoddityAssetId assetId = asset.contentHash;
    instance.objectId = objectId;
    instance.assetId = assetId;
    if (instance.quality.particleCapacity == 0U || instance.quality.trailResolution == 0U) {
        instance.quality = fluoddity_quality_profile(FluoddityQuality::Medium);
    }
    if (!valid_instance(instance, &validation)) {
        set_error(error, validation);
        return 0U;
    }
    assets_.try_emplace(assetId, std::move(asset));
    ++assetUsers_[assetId];
    entries_.emplace(objectId, Entry{instance, {}, 0U});
    nextObjectId_ = std::max(nextObjectId_, objectId + 1U);
    ++revision_;
    return objectId;
}

bool RuntimeFluoddityWorld::update_asset(
    RuntimeFluoddityObjectId objectId, FluoddityRuleAsset asset, std::string* error) {
    auto entry = entries_.find(objectId);
    if (entry == entries_.end()) {
        set_error(error, "runtime Fluoddity object does not exist");
        return false;
    }
    asset.recompute_hash();
    std::string validation;
    if (!asset.validate(&validation) || asset.contentHash == 0U) {
        set_error(error, validation.empty() ? "invalid Fluoddity asset hash" : validation);
        return false;
    }
    const RuntimeFluoddityAssetId oldAssetId = entry->second.instance.assetId;
    const RuntimeFluoddityAssetId newAssetId = asset.contentHash;
    assets_.try_emplace(newAssetId, std::move(asset));
    ++assetUsers_[newAssetId];
    entry->second.instance.assetId = newAssetId;
    ++entry->second.state.resetGeneration;
    if (auto users = assetUsers_.find(oldAssetId); users != assetUsers_.end()) {
        if (--users->second == 0U) {
            assetUsers_.erase(users);
            assets_.erase(oldAssetId);
        }
    }
    ++revision_;
    return true;
}

bool RuntimeFluoddityWorld::update_instance(
    RuntimeFluoddityObjectId objectId,
    const RuntimeFluoddityInstance& replacement,
    std::string* error) {
    auto entry = entries_.find(objectId);
    if (entry == entries_.end()) {
        set_error(error, "runtime Fluoddity object does not exist");
        return false;
    }
    if (replacement.objectId != objectId ||
        replacement.assetId != entry->second.instance.assetId) {
        set_error(error, "runtime Fluoddity identity cannot be changed by update_instance");
        return false;
    }
    std::string validation;
    if (!valid_instance(replacement, &validation)) {
        set_error(error, validation);
        return false;
    }
    const bool allocationChanged =
        replacement.quality.particleCapacity != entry->second.instance.quality.particleCapacity ||
        replacement.quality.trailResolution != entry->second.instance.quality.trailResolution ||
        replacement.quality.accumulationMode != entry->second.instance.quality.accumulationMode;
    entry->second.instance = replacement;
    if (allocationChanged) ++entry->second.state.resetGeneration;
    ++revision_;
    return true;
}

bool RuntimeFluoddityWorld::destroy(RuntimeFluoddityObjectId objectId) noexcept {
    const auto entry = entries_.find(objectId);
    if (entry == entries_.end()) return false;
    const RuntimeFluoddityAssetId assetId = entry->second.instance.assetId;
    entries_.erase(entry);
    if (auto users = assetUsers_.find(assetId); users != assetUsers_.end()) {
        if (--users->second == 0U) {
            assetUsers_.erase(users);
            assets_.erase(assetId);
        }
    }
    ++revision_;
    return true;
}

bool RuntimeFluoddityWorld::play(RuntimeFluoddityObjectId objectId) noexcept {
    auto entry = entries_.find(objectId);
    if (entry == entries_.end()) return false;
    if (!entry->second.instance.running) {
        entry->second.instance.running = true;
        ++revision_;
    }
    return true;
}

bool RuntimeFluoddityWorld::pause(RuntimeFluoddityObjectId objectId) noexcept {
    auto entry = entries_.find(objectId);
    if (entry == entries_.end()) return false;
    if (entry->second.instance.running) {
        entry->second.instance.running = false;
        ++revision_;
    }
    return true;
}

bool RuntimeFluoddityWorld::step_once(RuntimeFluoddityObjectId objectId) noexcept {
    auto entry = entries_.find(objectId);
    if (entry == entries_.end()) return false;
    if (entry->second.state.pendingSingleSteps < kMaximumStepsPerFrame) {
        ++entry->second.state.pendingSingleSteps;
    }
    ++revision_;
    return true;
}

bool RuntimeFluoddityWorld::reset(RuntimeFluoddityObjectId objectId) noexcept {
    auto entry = entries_.find(objectId);
    if (entry == entries_.end()) return false;
    RuntimeFluoddityState& state = entry->second.state;
    state.simulationFrame = 0U;
    state.simulationTimeSeconds = 0.0;
    state.fixedStepAccumulatorSeconds = 0.0;
    state.pendingSingleSteps = 0U;
    state.trailReadIndex = 0U;
    ++state.resetGeneration;
    ++revision_;
    return true;
}

void RuntimeFluoddityWorld::set_global_gpu_budget(std::uint64_t bytes) noexcept {
    globalGpuBudgetBytes_ = bytes;
    ++revision_;
}

std::vector<FluodditySimulationFramePlan> RuntimeFluoddityWorld::plan_frame(double deltaSeconds) {
    const double boundedDelta = std::clamp(
        std::isfinite(deltaSeconds) ? deltaSeconds : 0.0, 0.0, kMaximumAcceptedDeltaSeconds);
    std::vector<FluodditySimulationFramePlan> plans;
    plans.reserve(entries_.size());
    std::uint64_t reservedBytes{};
    for (auto& [objectId, entry] : entries_) {
        FluodditySimulationFramePlan plan;
        plan.objectId = objectId;
        plan.assetId = entry.instance.assetId;
        plan.enabled = entry.instance.visible;
        const FluoddityRuleAsset& asset = assets_.at(entry.instance.assetId);
        plan.memory = estimate_fluoddity_memory(entry.instance.quality, asset.cohortCount);
        if (plan.memory.totalBytes > globalGpuBudgetBytes_ -
            std::min(reservedBytes, globalGpuBudgetBytes_)) {
            plan.budgetAccepted = false;
            plan.enabled = false;
            plan.rejectionReason = "global Fluoddity GPU budget exceeded";
            plans.push_back(std::move(plan));
            continue;
        }
        reservedBytes += plan.memory.totalBytes;
        plan.budgetAccepted = true;
        plan.resetBeforeStep = entry.plannedResetGeneration != entry.state.resetGeneration;
        entry.plannedResetGeneration = entry.state.resetGeneration;
        plan.initialTrailReadIndex = entry.state.trailReadIndex;

        const double fixedStep = 1.0 / static_cast<double>(entry.instance.simulationFrequencyHz);
        if (entry.instance.running && entry.instance.timeScale > 0.0F) {
            entry.state.fixedStepAccumulatorSeconds +=
                boundedDelta * static_cast<double>(entry.instance.timeScale);
        }
        std::uint32_t automaticSteps{};
        if (fixedStep > 0.0) {
            automaticSteps = static_cast<std::uint32_t>(std::min<double>(
                std::floor(entry.state.fixedStepAccumulatorSeconds / fixedStep),
                static_cast<double>(kMaximumStepsPerFrame)));
        }
        if (automaticSteps > 0U) {
            entry.state.fixedStepAccumulatorSeconds -= fixedStep * automaticSteps;
        }
        const std::uint32_t availableForManual = kMaximumStepsPerFrame - automaticSteps;
        const std::uint32_t manualSteps =
            std::min(entry.state.pendingSingleSteps, availableForManual);
        entry.state.pendingSingleSteps -= manualSteps;
        plan.stepCount = automaticSteps + manualSteps;
        plan.particleWorkgroupsPerStep = ceil_div(entry.instance.quality.particleCapacity, 64U);
        const std::uint32_t trailGroups = ceil_div(entry.instance.quality.trailResolution, 4U);
        plan.trailWorkgroupsPerStep = {trailGroups, trailGroups, trailGroups};
        plan.computePassesPerStep = entry.instance.quality.accumulationMode ==
                                            FluoddityAccumulationMode::FixedPointSigned
                                        ? 3U
                                        : 2U;
        for (std::uint32_t step = 0U; step < plan.stepCount; ++step) {
            entry.state.trailReadIndex ^= 1U;
        }
        entry.state.simulationFrame += plan.stepCount;
        entry.state.simulationTimeSeconds += fixedStep * plan.stepCount;
        plan.finalTrailReadIndex = entry.state.trailReadIndex;
        plans.push_back(std::move(plan));
    }
    return plans;
}

const RuntimeFluoddityInstance* RuntimeFluoddityWorld::find_instance(
    RuntimeFluoddityObjectId objectId) const noexcept {
    const auto entry = entries_.find(objectId);
    return entry == entries_.end() ? nullptr : &entry->second.instance;
}

const RuntimeFluoddityState* RuntimeFluoddityWorld::find_state(
    RuntimeFluoddityObjectId objectId) const noexcept {
    const auto entry = entries_.find(objectId);
    return entry == entries_.end() ? nullptr : &entry->second.state;
}

const FluoddityRuleAsset* RuntimeFluoddityWorld::find_asset(
    RuntimeFluoddityAssetId assetId) const noexcept {
    const auto asset = assets_.find(assetId);
    return asset == assets_.end() ? nullptr : &asset->second;
}

RuntimeFluodditySceneSnapshot RuntimeFluoddityWorld::snapshot() const {
    RuntimeFluodditySceneSnapshot snapshot;
    snapshot.revision = revision_;
    snapshot.globalGpuBudgetBytes = globalGpuBudgetBytes_;
    snapshot.assets.reserve(assets_.size());
    snapshot.instances.reserve(entries_.size());
    snapshot.states.reserve(entries_.size());
    for (const auto& [assetId, asset] : assets_) {
        (void)assetId;
        snapshot.assets.push_back(asset);
    }
    for (const auto& [objectId, entry] : entries_) {
        (void)objectId;
        snapshot.instances.push_back(entry.instance);
        snapshot.states.push_back(entry.state);
    }
    return snapshot;
}

std::string fluoddity_render_mode_name(FluoddityRenderMode mode) {
    switch (mode) {
    case FluoddityRenderMode::ParticlePoints: return "Particle Points";
    case FluoddityRenderMode::SoftParticles: return "Soft Particles";
    case FluoddityRenderMode::TrailVolume: return "Trail Volume";
    case FluoddityRenderMode::Hybrid: return "Hybrid";
    case FluoddityRenderMode::GaborCinematicExperimental: return "Gabor Cinematic (Experimental)";
    }
    return "Unknown";
}

} // namespace dve
