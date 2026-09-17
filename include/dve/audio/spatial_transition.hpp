#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "dve/audio/spatializer.hpp"

namespace dve::audio {

struct SpatializationTransitionMetrics {
    std::uint64_t acceptedTargets{};
    std::uint64_t rejectedTargets{};
    std::uint64_t activatedTargets{};
    std::uint64_t completedTargets{};
    std::uint64_t publishedGeneration{};
    std::uint64_t activeGeneration{};
};

// Control-thread targets are published as immutable snapshots. The audio thread calls advance()
// once per rendered source block; it performs no allocation, lock, logging, or middleware work.
class SpatializationTransition {
public:
    explicit SpatializationTransition(std::uint32_t sampleRate = 48000U) noexcept;

    void reset(std::uint64_t generation, SpatializationResult value) noexcept;
    [[nodiscard]] bool publish_target(std::uint64_t generation,
                                      SpatializationResult value,
                                      float transitionSeconds) noexcept;
    [[nodiscard]] SpatializationResult advance(std::size_t renderedFrames) noexcept;
    [[nodiscard]] SpatializationResult current() const noexcept { return current_; }
    [[nodiscard]] SpatializationTransitionMetrics metrics() const noexcept;

private:
    struct Target {
        std::uint64_t generation{};
        SpatializationResult value{};
        std::uint64_t transitionFrames{};
    };

    [[nodiscard]] static bool finite(SpatializationResult value) noexcept;
    [[nodiscard]] static SpatializationResult interpolate(SpatializationResult from,
                                                          SpatializationResult to,
                                                          float amount) noexcept;

    std::uint32_t sampleRate_{};
    std::atomic<std::shared_ptr<const Target>> pending_{};
    std::atomic<std::uint64_t> publishedGeneration_{};
    std::atomic<std::uint64_t> acceptedTargets_{};
    std::atomic<std::uint64_t> rejectedTargets_{};
    std::atomic<std::uint64_t> activatedTargets_{};
    std::atomic<std::uint64_t> completedTargets_{};

    SpatializationResult start_{};
    SpatializationResult target_{};
    SpatializationResult current_{};
    std::uint64_t activeGeneration_{};
    std::uint64_t elapsedFrames_{};
    std::uint64_t transitionFrames_{};
};

} // namespace dve::audio
