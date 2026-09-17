#include "dve/audio/spatial_transition.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dve::audio {
namespace {
float blend(float from, float to, float amount) noexcept { return from + (to - from) * amount; }
}

SpatializationTransition::SpatializationTransition(std::uint32_t sampleRate) noexcept
    : sampleRate_(std::clamp(sampleRate, 8000U, 384000U)) {}

bool SpatializationTransition::finite(SpatializationResult value) noexcept {
    return std::isfinite(value.leftGain) && std::isfinite(value.rightGain) &&
           std::isfinite(value.distanceGain) && std::isfinite(value.dopplerRatio) &&
           std::isfinite(value.lowPassHertz) && std::isfinite(value.reverbSend) &&
           std::isfinite(value.propagationDelaySeconds);
}

SpatializationResult SpatializationTransition::interpolate(SpatializationResult from,
                                                            SpatializationResult to,
                                                            float amount) noexcept {
    amount = std::clamp(amount, 0.0F, 1.0F);
    // Smoothstep removes the slope discontinuity at both scene-generation boundaries.
    amount = amount * amount * (3.0F - 2.0F * amount);
    return {blend(from.leftGain, to.leftGain, amount),
            blend(from.rightGain, to.rightGain, amount),
            blend(from.distanceGain, to.distanceGain, amount),
            blend(from.dopplerRatio, to.dopplerRatio, amount),
            blend(from.lowPassHertz, to.lowPassHertz, amount),
            blend(from.reverbSend, to.reverbSend, amount),
            blend(from.propagationDelaySeconds, to.propagationDelaySeconds, amount)};
}

void SpatializationTransition::reset(std::uint64_t generation,
                                     SpatializationResult value) noexcept {
    if (!finite(value)) value = {};
    start_ = value;
    target_ = value;
    current_ = value;
    activeGeneration_ = generation;
    elapsedFrames_ = 0;
    transitionFrames_ = 0;
    publishedGeneration_.store(generation, std::memory_order_release);
    pending_.store({}, std::memory_order_release);
}

bool SpatializationTransition::publish_target(std::uint64_t generation,
                                              SpatializationResult value,
                                              float transitionSeconds) noexcept {
    if (generation == 0U || !finite(value) || !std::isfinite(transitionSeconds) ||
        transitionSeconds < 0.0F) {
        rejectedTargets_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    std::uint64_t observed = publishedGeneration_.load(std::memory_order_acquire);
    while (generation > observed) {
        if (publishedGeneration_.compare_exchange_weak(observed, generation,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
            const double frames = static_cast<double>(sampleRate_) *
                                  static_cast<double>(transitionSeconds);
            const auto transitionFrames = static_cast<std::uint64_t>(std::clamp(
                std::ceil(frames), 0.0,
                static_cast<double>(std::numeric_limits<std::uint64_t>::max())));
            try {
                pending_.store(std::make_shared<const Target>(Target{generation, value,
                                                                     transitionFrames}),
                               std::memory_order_release);
            } catch (...) {
                // Restore only if no later producer publication superseded this generation.
                std::uint64_t expected = generation;
                (void)publishedGeneration_.compare_exchange_strong(expected, observed,
                                                                    std::memory_order_acq_rel);
                rejectedTargets_.fetch_add(1U, std::memory_order_relaxed);
                return false;
            }
            acceptedTargets_.fetch_add(1U, std::memory_order_relaxed);
            return true;
        }
    }
    rejectedTargets_.fetch_add(1U, std::memory_order_relaxed);
    return false;
}

SpatializationResult SpatializationTransition::advance(std::size_t renderedFrames) noexcept {
    const auto pending = pending_.load(std::memory_order_acquire);
    if (pending && pending->generation > activeGeneration_) {
        start_ = current_;
        target_ = pending->value;
        activeGeneration_ = pending->generation;
        elapsedFrames_ = 0;
        transitionFrames_ = pending->transitionFrames;
        activatedTargets_.fetch_add(1U, std::memory_order_relaxed);
        if (transitionFrames_ == 0U) {
            current_ = target_;
            completedTargets_.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    if (transitionFrames_ > 0U && elapsedFrames_ < transitionFrames_) {
        elapsedFrames_ = std::min<std::uint64_t>(transitionFrames_,
            elapsedFrames_ + static_cast<std::uint64_t>(renderedFrames));
        const float amount = static_cast<float>(static_cast<double>(elapsedFrames_) /
                                                static_cast<double>(transitionFrames_));
        current_ = interpolate(start_, target_, amount);
        if (elapsedFrames_ == transitionFrames_)
            completedTargets_.fetch_add(1U, std::memory_order_relaxed);
    }
    return current_;
}

SpatializationTransitionMetrics SpatializationTransition::metrics() const noexcept {
    return {acceptedTargets_.load(std::memory_order_relaxed),
            rejectedTargets_.load(std::memory_order_relaxed),
            activatedTargets_.load(std::memory_order_relaxed),
            completedTargets_.load(std::memory_order_relaxed),
            publishedGeneration_.load(std::memory_order_acquire), activeGeneration_};
}

} // namespace dve::audio
