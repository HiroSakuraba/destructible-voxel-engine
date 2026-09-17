#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace dve::audio {

enum class ReflectionQualityTier : std::uint8_t { Disabled, Low, Medium, High };

struct ReflectionQualityProfile {
    ReflectionQualityTier tier{ReflectionQualityTier::Medium};
    std::size_t sourceCount{2};
    std::int32_t rays{4096};
    std::int32_t bounces{8};
    float impulseDurationSeconds{1.0F};
    std::int32_t ambisonicOrder{1};
};

[[nodiscard]] ReflectionQualityProfile reflection_profile(ReflectionQualityTier tier) noexcept;

struct ReflectionBudgetSettings {
    double workerBudgetMilliseconds{1.5};
    std::uint32_t overloadSamplesToDegrade{2};
    std::uint32_t underBudgetSamplesToRecover{120};
};

struct ReflectionBudgetMetrics {
    std::uint64_t samples{};
    std::uint64_t overloadSamples{};
    std::uint64_t degradations{};
    std::uint64_t recoveries{};
    double lastWorkerMilliseconds{};
    ReflectionQualityTier activeTier{ReflectionQualityTier::Medium};
};

// Worker-side quality controller. It degrades quickly when the measured reflections work exceeds
// its budget and recovers slowly to avoid oscillation. No callback thread calls this class.
class ReflectionBudgetController {
public:
    explicit ReflectionBudgetController(ReflectionBudgetSettings settings = {}) noexcept;
    void set_requested_tier(ReflectionQualityTier tier) noexcept;
    [[nodiscard]] ReflectionQualityTier requested_tier() const noexcept;
    [[nodiscard]] ReflectionQualityTier active_tier() const noexcept;
    [[nodiscard]] ReflectionQualityProfile profile() const noexcept;
    void record_worker_sample(double milliseconds) noexcept;
    [[nodiscard]] ReflectionBudgetMetrics metrics() const noexcept;

private:
    ReflectionBudgetSettings settings_{};
    ReflectionQualityTier requested_{ReflectionQualityTier::Medium};
    ReflectionQualityTier active_{ReflectionQualityTier::Medium};
    std::uint32_t overloadStreak_{};
    std::uint32_t underBudgetStreak_{};
    ReflectionBudgetMetrics metrics_{};
};

struct ReflectionFieldSnapshot {
    std::uint64_t generation{};
    ReflectionQualityProfile profile{};
    std::size_t simulatedSources{};
    std::array<float, 3> reverbTimesSeconds{};
    std::array<float, 3> equalization{1.0F, 1.0F, 1.0F};
    bool parametricFallback{};
    double workerMilliseconds{};
};

struct ReflectionFieldMetrics {
    std::uint64_t acceptedGenerations{};
    std::uint64_t rejectedGenerations{};
    std::uint64_t currentGeneration{};
};

// Atomic immutable publication boundary between the reflection worker and audio callback.
class ReflectionFieldPublisher {
public:
    bool publish(ReflectionFieldSnapshot snapshot) noexcept;
    [[nodiscard]] std::shared_ptr<const ReflectionFieldSnapshot> snapshot() const noexcept;
    [[nodiscard]] ReflectionFieldMetrics metrics() const noexcept;

private:
    std::atomic<std::shared_ptr<const ReflectionFieldSnapshot>> published_{};
    std::atomic<std::uint64_t> generation_{};
    std::atomic<std::uint64_t> accepted_{};
    std::atomic<std::uint64_t> rejected_{};
};

} // namespace dve::audio
