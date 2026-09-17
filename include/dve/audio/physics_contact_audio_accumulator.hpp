#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dve/audio/destruction_runtime.hpp"

namespace dve::audio {

struct PhysicsContactSample {
    std::uint64_t bodyA{};
    std::uint64_t bodyB{};
    std::uint16_t materialA{};
    std::uint16_t materialB{};
    AudioVec3 position{};
    AudioVec3 relativeVelocity{};
    AudioVec3 contactNormal{0.0F, 1.0F, 0.0F};
    float normalImpulse{};
    float effectiveMass{};
    bool persistent{};
    double timeSeconds{};
};

struct PhysicsContactAccumulatorSettings {
    float spatialCellMeters{0.5F};
    double aggregationWindowSeconds{0.08};
    std::size_t capacity{256};
};

struct PhysicsContactAccumulatorMetrics {
    std::uint64_t submittedSamples{};
    std::uint64_t emittedContacts{};
    std::uint64_t rejectedSamples{};
    std::uint64_t estimatedImpulses{};
    std::size_t activeBuckets{};
};

// Fixed-capacity physics-thread accumulator. Samples are grouped by unordered body pair,
// corresponding material pair, spatial cell, and time window. The class never allocates while
// recording contacts; draining writes compact engine-neutral PhysicsContactAudio records.
class PhysicsContactAudioAccumulator {
public:
    static constexpr std::size_t kMaximumBuckets = 256;

    explicit PhysicsContactAudioAccumulator(
        PhysicsContactAccumulatorSettings settings = {}) noexcept;

    [[nodiscard]] bool record(const PhysicsContactSample& sample) noexcept;
    std::size_t drain(double nowSeconds, std::vector<PhysicsContactAudio>& output,
                      bool flushAll = false);
    void reset() noexcept;
    [[nodiscard]] PhysicsContactAccumulatorMetrics metrics() const noexcept;

private:
    struct Bucket {
        bool active{};
        std::uint64_t bodyA{};
        std::uint64_t bodyB{};
        std::uint16_t materialA{};
        std::uint16_t materialB{};
        std::int32_t cellX{};
        std::int32_t cellY{};
        std::int32_t cellZ{};
        std::int64_t window{};
        AudioVec3 weightedPosition{};
        AudioVec3 weightedVelocity{};
        float accumulatedImpulse{};
        float maximumMass{};
        float weight{};
        bool persistent{};
        double latestTime{};
    };

    [[nodiscard]] bool valid_sample(const PhysicsContactSample& sample) const noexcept;
    [[nodiscard]] Bucket* find_or_create(const PhysicsContactSample& sample,
                                         std::int32_t cellX, std::int32_t cellY,
                                         std::int32_t cellZ, std::int64_t window) noexcept;
    [[nodiscard]] PhysicsContactAudio emit(const Bucket& bucket) const noexcept;

    PhysicsContactAccumulatorSettings settings_{};
    std::array<Bucket, kMaximumBuckets> buckets_{};
    PhysicsContactAccumulatorMetrics metrics_{};
};

} // namespace dve::audio
