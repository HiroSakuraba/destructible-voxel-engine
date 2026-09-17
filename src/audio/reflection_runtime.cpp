#include "dve/audio/reflection_runtime.hpp"

#include <algorithm>
#include <cmath>

namespace dve::audio {
namespace {

int tier_value(ReflectionQualityTier tier) noexcept { return static_cast<int>(tier); }
ReflectionQualityTier lower_tier(ReflectionQualityTier tier) noexcept {
    return static_cast<ReflectionQualityTier>(std::max(0, tier_value(tier) - 1));
}
ReflectionQualityTier higher_tier(ReflectionQualityTier tier) noexcept {
    return static_cast<ReflectionQualityTier>(std::min(tier_value(ReflectionQualityTier::High),
                                                       tier_value(tier) + 1));
}

} // namespace

ReflectionQualityProfile reflection_profile(ReflectionQualityTier tier) noexcept {
    switch (tier) {
        case ReflectionQualityTier::Disabled: return {tier, 0, 0, 0, 0.0F, 0};
        case ReflectionQualityTier::Low: return {tier, 1, 2048, 4, 0.5F, 0};
        case ReflectionQualityTier::Medium: return {tier, 2, 4096, 8, 1.0F, 1};
        case ReflectionQualityTier::High: return {tier, 4, 8192, 16, 1.5F, 2};
    }
    return {};
}

ReflectionBudgetController::ReflectionBudgetController(ReflectionBudgetSettings settings) noexcept
    : settings_(settings) {
    if (!(settings_.workerBudgetMilliseconds > 0.0) ||
        !std::isfinite(settings_.workerBudgetMilliseconds)) settings_.workerBudgetMilliseconds = 1.5;
    settings_.overloadSamplesToDegrade = std::max(1U, settings_.overloadSamplesToDegrade);
    settings_.underBudgetSamplesToRecover = std::max(1U, settings_.underBudgetSamplesToRecover);
    metrics_.activeTier = active_;
}

void ReflectionBudgetController::set_requested_tier(ReflectionQualityTier tier) noexcept {
    requested_ = tier;
    if (tier_value(active_) > tier_value(requested_)) active_ = requested_;
    overloadStreak_ = underBudgetStreak_ = 0;
    metrics_.activeTier = active_;
}
ReflectionQualityTier ReflectionBudgetController::requested_tier() const noexcept { return requested_; }
ReflectionQualityTier ReflectionBudgetController::active_tier() const noexcept { return active_; }
ReflectionQualityProfile ReflectionBudgetController::profile() const noexcept { return reflection_profile(active_); }

void ReflectionBudgetController::record_worker_sample(double milliseconds) noexcept {
    ++metrics_.samples;
    metrics_.lastWorkerMilliseconds = milliseconds;
    if (!std::isfinite(milliseconds) || milliseconds > settings_.workerBudgetMilliseconds) {
        ++metrics_.overloadSamples;
        ++overloadStreak_;
        underBudgetStreak_ = 0;
        if (overloadStreak_ >= settings_.overloadSamplesToDegrade &&
            active_ != ReflectionQualityTier::Disabled) {
            active_ = lower_tier(active_);
            overloadStreak_ = 0;
            ++metrics_.degradations;
        }
    } else {
        overloadStreak_ = 0;
        ++underBudgetStreak_;
        if (underBudgetStreak_ >= settings_.underBudgetSamplesToRecover &&
            tier_value(active_) < tier_value(requested_)) {
            active_ = higher_tier(active_);
            underBudgetStreak_ = 0;
            ++metrics_.recoveries;
        }
    }
    metrics_.activeTier = active_;
}

ReflectionBudgetMetrics ReflectionBudgetController::metrics() const noexcept { return metrics_; }

bool ReflectionFieldPublisher::publish(ReflectionFieldSnapshot snapshotValue) noexcept {
    const std::uint64_t current = generation_.load(std::memory_order_acquire);
    if (snapshotValue.generation == 0U || snapshotValue.generation <= current ||
        !std::isfinite(snapshotValue.workerMilliseconds)) {
        rejected_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    for (float value : snapshotValue.reverbTimesSeconds)
        if (!std::isfinite(value) || value < 0.0F) {
            rejected_.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
    auto owned = std::make_shared<const ReflectionFieldSnapshot>(std::move(snapshotValue));
    published_.store(owned, std::memory_order_release);
    generation_.store(owned->generation, std::memory_order_release);
    accepted_.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
std::shared_ptr<const ReflectionFieldSnapshot> ReflectionFieldPublisher::snapshot() const noexcept {
    return published_.load(std::memory_order_acquire);
}
ReflectionFieldMetrics ReflectionFieldPublisher::metrics() const noexcept {
    return {accepted_.load(std::memory_order_relaxed), rejected_.load(std::memory_order_relaxed),
            generation_.load(std::memory_order_acquire)};
}

} // namespace dve::audio
