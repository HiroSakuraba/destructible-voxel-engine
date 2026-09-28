// Phase 4 granular generator core (SYN-014). See granular.hpp for the
// interface contract and the wave-1/wave-2 division of labor.
#include "dve/audio/granular.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dve::audio {
namespace {

inline float clamp01(float v) noexcept { return std::clamp(v, 0.0F, 1.0F); }

}  // namespace

void GranularEngine::set_sample_rate(std::uint32_t sampleRate) noexcept {
    sampleRate_ = sampleRate == 0U ? 48000U : sampleRate;
}

void GranularEngine::set_seed(std::uint32_t seed) noexcept {
    rngState_ = seed == 0U ? 1U : seed;
}

void GranularEngine::reset() noexcept {
    grains_ = {};
    counters_ = {};
    spawnPhase_ = 0.0F;
}

void GranularEngine::kill_grains() noexcept {
    for (auto& grain : grains_) grain.active = false;
}

std::uint32_t GranularEngine::random_u32() noexcept {
    std::uint32_t s = rngState_;
    s ^= s << 13U;
    s ^= s >> 17U;
    s ^= s << 5U;
    rngState_ = s;
    return s;
}

float GranularEngine::random01() noexcept {
    // 24-bit mantissa precision, [0, 1).
    return static_cast<float>(random_u32() >> 8U) * (1.0F / 16777216.0F);
}

float GranularEngine::semitones_to_ratio(float semitones) noexcept {
    return std::exp2(semitones / 12.0F);
}

float GranularEngine::envelope_value(GranularEnvelopeShape shape, float phase01) noexcept {
    const float phase = clamp01(phase01);
    switch (shape) {
        case GranularEnvelopeShape::Hann:
            return 0.5F - 0.5F * std::cos(2.0F * 3.141592653589793F * phase);
        case GranularEnvelopeShape::Triangle:
            return 1.0F - std::fabs(phase * 2.0F - 1.0F);
        case GranularEnvelopeShape::ExponentialDecay:
            // Wave-2 owns tuning: standard exponential decay window.
            return std::exp(-4.0F * phase);
        case GranularEnvelopeShape::PlanckTaper: {
            // Wave-2 owns tuning: standard Planck taper, epsilon = 0.1.
            constexpr float epsilon = 0.1F;
            if (phase < epsilon) {
                const float x = std::max(phase, 1.0e-6F);
                return 1.0F / (1.0F + std::exp(epsilon / x - epsilon / (epsilon - x)));
            }
            if (phase > 1.0F - epsilon) {
                const float x = std::max(1.0F - phase, 1.0e-6F);
                return 1.0F / (1.0F + std::exp(epsilon / x - epsilon / (epsilon - x)));
            }
            return 1.0F;
        }
    }
    return 1.0F;
}

float GranularEngine::cubic_sample(const float* source, std::uint32_t frameCount,
                                   float position) noexcept {
    if (source == nullptr || frameCount == 0U) return 0.0F;
    const float frames = static_cast<float>(frameCount);
    const float bounded = std::clamp(position, 0.0F, frames - 1.0F);
    const std::int32_t center = static_cast<std::int32_t>(bounded);
    const float fraction = bounded - static_cast<float>(center);
    const std::uint32_t last = frameCount - 1U;
    const std::uint32_t i0 = static_cast<std::uint32_t>(std::max<std::int32_t>(center - 1, 0));
    const std::uint32_t i1 = static_cast<std::uint32_t>(center);
    const std::uint32_t i2 = std::min(i1 + 1U, last);
    const std::uint32_t i3 = std::min(i1 + 2U, last);
    const float p0 = source[i0];
    const float p1 = source[i1];
    const float p2 = source[i2];
    const float p3 = source[i3];
    const float f2 = fraction * fraction;
    const float f3 = f2 * fraction;
    // Catmull-Rom (same pattern as the sampler's cubic interpolation).
    return 0.5F * (2.0F * p1 + (p2 - p0) * fraction +
                   (2.0F * p0 - 5.0F * p1 + 4.0F * p2 - p3) * f2 +
                   (3.0F * (p1 - p2) + p3 - p0) * f3);
}

void GranularEngine::spawn_grain(const GranularSource& source, const GranularParameters& params,
                                float positionMod01) noexcept {
    // Find a free slot; when the pool is full steal the oldest grain (nearest
    // completion), which is the least audible disruption.
    Grain* target = nullptr;
    for (auto& grain : grains_) {
        if (!grain.active) {
            target = &grain;
            break;
        }
    }
    if (target == nullptr) {
        target = &*std::max_element(grains_.begin(), grains_.end(), [](const Grain& a, const Grain& b) {
            return (a.ageFrames / std::max(a.durationFrames, 1.0F)) <
                   (b.ageFrames / std::max(b.durationFrames, 1.0F));
        });
        ++counters_.grainSteals;
    }

    const float lastFrame = static_cast<float>(source.frameCount - 1U);
    const float base01 = clamp01(params.position01 + positionMod01);
    const float jitter = (random01() * 2.0F - 1.0F) * clamp01(params.positionJitter01);
    const float position01 = clamp01(base01 + jitter);
    const float ratio = semitones_to_ratio(params.pitchSemitones);
    const float sourceRate = source.sampleRate == 0U ? static_cast<float>(sampleRate_)
                                                     : static_cast<float>(source.sampleRate);
    float increment = ratio * sourceRate / static_cast<float>(sampleRate_);
    const bool reverse = random01() < clamp01(params.reverseProbability01);
    if (reverse) increment = -increment;
    const float durationFrames =
        std::max(1.0F, std::max(params.durationMs, 0.0F) * 0.001F * static_cast<float>(sampleRate_));

    float startPosition = position01 * lastFrame;
    if (reverse) {
        // Pre-advance so the grain sweeps backwards through [position, position + length].
        startPosition = std::min(startPosition + durationFrames * std::abs(increment), lastFrame);
    }
    const float pan = std::clamp((random01() * 2.0F - 1.0F) * clamp01(params.panScatter01), -1.0F, 1.0F);

    target->active = true;
    target->sourcePositionFrames = startPosition;
    target->durationFrames = durationFrames;
    target->pitchIncrement = increment;
    target->pan = pan;
    target->gain = 1.0F;
    target->envelopeShape = params.envelopeShape;
    target->reverse = reverse;
    target->filterValue = 0.0F;  // reserved for wave-2 per-grain filtering
    target->seed = random_u32();
    target->ageFrames = 0.0F;
    ++counters_.admittedGrains;
}

std::pair<float, float> GranularEngine::render(const GranularSource& source,
                                              const GranularParameters& params,
                                              float positionMod01) noexcept {
    if (!params.enabled) return {0.0F, 0.0F};
    const bool bankUsable =
        source.samples != nullptr && source.frameCount >= 2U;

    // Grain spawn runs at control rate from densityHz: a fractional accumulator
    // spawns whole grains, so fractional densities stay exact over time.
    const float density = std::clamp(params.densityHz, 0.0F, 4000.0F);
    spawnPhase_ += density / static_cast<float>(sampleRate_);
    while (spawnPhase_ >= 1.0F) {
        spawnPhase_ -= 1.0F;
        ++counters_.requestedGrains;
        if (!bankUsable) {
            // No usable grain source: count the miss, stay silent, never crash.
            ++counters_.grainMisses;
            continue;
        }
        spawn_grain(source, params, positionMod01);
    }
    if (!bankUsable) return {0.0F, 0.0F};

    const float masterGain = params.gain;
    float left = 0.0F;
    float right = 0.0F;
    for (auto& grain : grains_) {
        if (!grain.active) continue;
        grain.ageFrames += 1.0F;
        const float phase = grain.ageFrames / grain.durationFrames;
        if (phase >= 1.0F) {
            grain.active = false;
            continue;
        }
        const float envelope = envelope_value(grain.envelopeShape, phase);
        const float sample = cubic_sample(source.samples, source.frameCount, grain.sourcePositionFrames);
        grain.sourcePositionFrames += grain.pitchIncrement;
        const float contribution = sample * envelope * grain.gain * masterGain;
        // Equal-power panning.
        const float panLeft = std::sqrt(0.5F * (1.0F - grain.pan));
        const float panRight = std::sqrt(0.5F * (1.0F + grain.pan));
        left += contribution * panLeft;
        right += contribution * panRight;
    }
    return {left, right};
}

std::uint32_t GranularEngine::active_grain_count() const noexcept {
    std::uint32_t count = 0U;
    for (const auto& grain : grains_) {
        if (grain.active) ++count;
    }
    return count;
}

GranularCounters GranularEngine::drain_counters() noexcept {
    const GranularCounters drained = counters_;
    counters_ = {};
    return drained;
}

}  // namespace dve::audio
