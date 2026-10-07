// Phase 4 granular generator core (SYN-014). See granular.hpp for the
// interface contract. Includes the wave-2 grain-envelope tuning and the
// musical macro layer (apply_granular_macros).
#include "dve/audio/granular.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dve::audio {
namespace {

inline float clamp01(float v) noexcept { return std::clamp(v, 0.0F, 1.0F); }

// Per-grain filter hook. Wave-2 owns the filter design; today the drive is
// always neutral (0.0) at spawn, so this is the identity. The Eco bypass in
// render() is the documented call site where the stage gets skipped.
inline float apply_grain_filter(float sample, float /*filterValue*/) noexcept { return sample; }

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
        case GranularEnvelopeShape::ExponentialDecay: {
            // Gamma window x*k*e*e^(-k*x): exact 0 at the attack (no click),
            // peak 1 at x = 1/k (fast attack), smooth decaying tail.
            // k = 10 puts the peak at 10% of the grain and leaves a tail of
            // 10/e^9 ~= 1.2e-3 at the grain end — inaudible, no release click.
            constexpr float kRate = 10.0F;
            constexpr float kPeakNorm = 2.718281828459045F * kRate;  // k*e
            return phase * kPeakNorm * std::exp(-kRate * phase);
        }
        case GranularEnvelopeShape::PlanckTaper: {
            // Planck taper with a 10% taper fraction on each end: flat-top 1
            // across the middle, C-infinity-smooth rolloff to ~0 at both ends.
            constexpr float epsilon = 0.10F;
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
    // NaN hardening: a non-finite read position renders silence instead of
    // indexing out of bounds (NaN would survive std::clamp and poison the
    // cast below).
    if (!std::isfinite(position)) return 0.0F;
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

GranularEffectiveParams apply_granular_macros(const GranularParameters& params,
                                              float positionMod01) noexcept {
    // NaN hardening: non-finite inputs fall back to neutral defaults so they
    // can never poison the grain pool. validate() still rejects such presets
    // at the API boundary; this is the last line of defense for values that
    // reach the engine directly (e.g. NaN modulation).
    const auto finiteOr = [](float v, float fallback) noexcept {
        return std::isfinite(v) ? v : fallback;
    };
    const float cloud = clamp01(finiteOr(params.cloud01, 0.5F));
    const float scatter = clamp01(finiteOr(params.scatter01, 0.0F));
    const float dust = clamp01(finiteOr(params.dust01, 0.0F));
    const float freeze = clamp01(finiteOr(params.freeze01, 0.0F));
    const float smear = clamp01(finiteOr(params.smear01, 0.0F));
    const float width = clamp01(finiteOr(params.width01, 0.5F));
    const float densityIn = finiteOr(params.densityHz, 20.0F);
    const float durationIn = finiteOr(params.durationMs, 120.0F);
    const float pitchIn = finiteOr(params.pitchSemitones, 0.0F);
    const float positionIn = finiteOr(params.position01, 0.0F);
    const float jitterIn = finiteOr(params.positionJitter01, 0.1F);
    const float panScatterIn = finiteOr(params.panScatter01, 0.3F);
    const float gainIn = finiteOr(params.gain, 0.8F);
    const float reverseIn = finiteOr(params.reverseProbability01, 0.0F);
    const float freezePositionIn = finiteOr(params.freezePosition01, 0.5F);
    const float positionMod = finiteOr(positionMod01, 0.0F);

    GranularEffectiveParams eff;
    // Cloud: sparse <-> dense. density x(0.25 + 3c); duration stretches
    // slightly with cloud so denser clouds overlap more.
    eff.densityHz = std::clamp(densityIn * (0.25F + 3.0F * cloud), 0.0F, 4000.0F);
    // Dust: transient emphasis. Duration collapses toward 5 ms, gain is nudged
    // up so the shorter grains stay audible, and per-grain envelopes bias
    // toward the fast-attack ExponentialDecay shape.
    float durationMs = durationIn * (1.0F - 0.9F * dust);
    durationMs = std::max(durationMs, 5.0F);
    durationMs *= 1.0F + 0.5F * cloud;
    eff.durationMs = durationMs;
    eff.gain = gainIn * (1.0F + 0.5F * dust);
    eff.dustTransientOverride01 = dust;
    // Freeze: position converges on the freeze point; jitter dies out with it.
    const float basePosition = clamp01(positionIn + positionMod);
    eff.position01 = basePosition * (1.0F - freeze) + clamp01(freezePositionIn) * freeze;
    eff.positionJitter01 =
        clamp01(clamp01(jitterIn + scatter * 0.5F) * (1.0F - freeze));
    // Smear: per-grain random pitch (+/-(12 x smear) semitones) and duration
    // (1 +/-(0.75 x smear)) dispersion, drawn from the engine RNG at spawn.
    eff.smearPitchSemitones = smear * 12.0F;
    eff.smearDurationSpread01 = smear * 0.75F;
    // Width: widens the pan distribution and scales the per-grain L/R source
    // decorrelation offsets (true stereo placement; mono-compatible at 0).
    eff.panScatter01 = clamp01(panScatterIn * (0.2F + 1.6F * width));
    eff.width01 = width;
    // Passthrough core fields.
    eff.pitchSemitones = pitchIn;
    eff.reverseProbability01 = clamp01(reverseIn);
    eff.envelopeShape = params.envelopeShape;
    eff.granularQuality = params.granularQuality;
    return eff;
}

void GranularEngine::spawn_grain(const GranularSource& source,
                                 const GranularEffectiveParams& eff,
                                 float noteRatio, GrainInterpolation interpolation) noexcept {
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
    const float jitter = (random01() * 2.0F - 1.0F) * eff.positionJitter01;
    const float position01 = clamp01(eff.position01 + jitter);
    // Smear pitch dispersion: per-grain random offset around the base pitch.
    // The draw is skipped when smear is neutral so the random stream stays
    // bit-identical to the macro-free path for the same seed.
    float smearPitch = 0.0F;
    if (eff.smearPitchSemitones > 0.0F)
        smearPitch = (random01() * 2.0F - 1.0F) * eff.smearPitchSemitones;
    const float ratio = semitones_to_ratio(eff.pitchSemitones + smearPitch);
    const float sourceRate = source.sampleRate == 0U ? static_cast<float>(sampleRate_)
                                                     : static_cast<float>(source.sampleRate);
    // Note tracking (fix: grain pitch follows the played note). Guarded to a
    // finite positive ratio so a hostile frequencyHertz can never poison the
    // grain pool with NaN/inf increments.
    const float safeNoteRatio =
        (std::isfinite(noteRatio) && noteRatio > 0.0F) ? noteRatio : 1.0F;
    float increment = ratio * sourceRate / static_cast<float>(sampleRate_) * safeNoteRatio;
    const bool reverse = random01() < eff.reverseProbability01;
    if (reverse) increment = -increment;
    // Smear duration dispersion: per-grain random multiplier around the
    // (macro-resolved) duration. Draw skipped when smear is neutral.
    float durationMul = 1.0F;
    if (eff.smearDurationSpread01 > 0.0F)
        durationMul = 1.0F + (random01() * 2.0F - 1.0F) * eff.smearDurationSpread01;
    const float durationFrames =
        std::max(1.0F, std::max(eff.durationMs, 0.0F) * 0.001F *
                           static_cast<float>(sampleRate_) * std::max(durationMul, 0.05F));

    float startPosition = position01 * lastFrame;
    if (reverse) {
        // Pre-advance so the grain sweeps backwards through [position, position + length].
        startPosition = std::min(startPosition + durationFrames * std::abs(increment), lastFrame);
    }
    const float pan = std::clamp((random01() * 2.0F - 1.0F) * eff.panScatter01, -1.0F, 1.0F);
    // Dust transient bias: per-grain chance of the fast-attack ExponentialDecay
    // envelope instead of the preset's shape. Draw skipped when dust is neutral
    // so the random stream stays bit-identical to the macro-free path.
    // Eco quality forces the cheapest correct window (Triangle: one abs, no
    // trig) instead of the requested/overridden shape.
    GranularEnvelopeShape shape = eff.envelopeShape;
    if (eff.granularQuality == FilterQuality::Eco) {
        shape = GranularEnvelopeShape::Triangle;
    } else if (eff.dustTransientOverride01 > 0.0F && random01() < eff.dustTransientOverride01) {
        shape = GranularEnvelopeShape::ExponentialDecay;
    }

    target->active = true;
    target->interpolation = interpolation;
    target->sourcePositionFrames = startPosition;
    target->durationFrames = durationFrames;
    target->pitchIncrement = increment;
    target->pan = pan;
    target->gain = 1.0F;
    target->envelopeShape = shape;
    target->reverse = reverse;
    target->filterValue = 0.0F;  // reserved for wave-2 per-grain filtering
    const std::uint32_t seed = random_u32();
    target->seed = seed;
    // True stereo grain placement (Worker 3). The L/R offsets are
    // antisymmetric around the grain's base position: offsetL = -delta,
    // offsetR = +delta with one per-grain delta. Mono-sum stability policy:
    // (L + R) reads positions base +/- delta, so the mono collapse differs
    // from the width01 == 0 render only to second order in delta — at
    // moderate widths the collapsed timbre is effectively unchanged, while
    // the channels decorrelate progressively as width01 rises. Delta is
    // derived from the grain's own seed (fixed-seed renders stay
    // reproducible), is exactly 0 at width01 == 0 (bit-identical L/R reads),
    // and tops out at kMaxStereoOffsetSeconds of source audio.
    const float width = clamp01(eff.width01);
    const float seedFrac = static_cast<float>(seed >> 8U) * (1.0F / 16777216.0F);
    const float maxOffsetFrames = kMaxStereoOffsetSeconds * static_cast<float>(sampleRate_);
    const float delta = width * maxOffsetFrames * (0.25F + 0.75F * seedFrac);
    target->sourceOffsetL = -delta;
    target->sourceOffsetR = delta;
    target->ageFrames = 0.0F;
    ++counters_.admittedGrains;
}

std::pair<float, float> GranularEngine::render(const GranularSource& source,
                                              const GranularParameters& params,
                                              float positionMod01) noexcept {
    // Reference pitch of the source: ratio exactly 1.0, so this form renders
    // bit-identically to the pre-note-tracking engine.
    const float reference = 440.0F * std::exp2((static_cast<float>(source.rootNote) - 69.0F) / 12.0F);
    return render(source, params, positionMod01, reference);
}

std::pair<float, float> GranularEngine::render(const GranularSource& source,
                                              const GranularParameters& params,
                                              float positionMod01,
                                              float frequencyHertz) noexcept {
    return render(source, params, positionMod01, frequencyHertz, GranularRuntimeQuality::Inherit);
}

float GranularEngine::interpolated_sample(const float* source, std::uint32_t frames, float position,
                                        GrainInterpolation interpolation, float increment) noexcept {
    if (!source || frames == 0U || !std::isfinite(position)) return 0.0F;
    position = std::clamp(position, 0.0F, static_cast<float>(frames - 1U));
    if (interpolation == GrainInterpolation::Cubic) return cubic_sample(source, frames, position);
    const auto base = static_cast<std::uint32_t>(position);
    const float fraction = position - static_cast<float>(base);
    if (interpolation == GrainInterpolation::Linear)
        return source[base] + (source[std::min(base + 1U, frames - 1U)] - source[base]) * fraction;
    // Eight-tap windowed sinc. Scale its cutoff for grains that read faster than
    // source rate, reducing aliasing. Fixed storage and bounded work per sample.
    constexpr float pi = 3.14159265358979323846F;
    const float cutoff = 1.0F / std::max(1.0F, std::fabs(increment));
    float sum = 0.0F, weightSum = 0.0F;
    for (int tap = -3; tap <= 4; ++tap) {
        const float distance = static_cast<float>(tap) - fraction;
        const float argument = pi * distance * cutoff;
        const float sinc = std::fabs(argument) < 1.0e-6F ? 1.0F : std::sin(argument) / argument;
        const float weight = sinc * (0.5F + 0.5F * std::cos(pi * distance / 4.0F));
        const auto index = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            static_cast<std::int64_t>(base) + tap, 0, static_cast<std::int64_t>(frames) - 1));
        sum += source[index] * weight;
        weightSum += weight;
    }
    return std::fabs(weightSum) > 1.0e-6F ? sum / weightSum : source[base];
}

std::pair<float, float> GranularEngine::render(const GranularSource& source,
                                              const GranularParameters& params,
                                              float positionMod01, float frequencyHertz,
                                              GranularRuntimeQuality quality) noexcept {
    if (!params.enabled) return {0.0F, 0.0F};
    // Note tracking: scale grain playback by the played note relative to the
    // source's recorded root note. Non-finite/non-positive input falls back
    // to the reference pitch (ratio 1.0) rather than poisoning the pool.
    const float reference = 440.0F * std::exp2((static_cast<float>(source.rootNote) - 69.0F) / 12.0F);
    float noteRatio = 1.0F;
    if (std::isfinite(frequencyHertz) && frequencyHertz > 0.0F && std::isfinite(reference) &&
        reference > 0.0F)
        noteRatio = frequencyHertz / reference;
    const bool bankUsable =
        source.samples != nullptr && source.frameCount >= 2U;

    // Resolve the musical macro layer once per sample: pure function of the
    // preset params plus the position modulation value.
    const GranularEffectiveParams eff = apply_granular_macros(params, positionMod01);

    // Grain spawn runs at control rate from the effective density: a fractional
    // accumulator spawns whole grains, so fractional densities stay exact over
    // time. The 4000 Hz ceiling from wave 1 is kept (High/Offline document it
    // as permitted; Standard keeps it unchanged).
    const float density = std::clamp(eff.densityHz, 0.0F, 4000.0F);
    spawnPhase_ += density / static_cast<float>(sampleRate_);
    const bool inherited = quality == GranularRuntimeQuality::Inherit;
    const std::size_t admissionLimit = quality == GranularRuntimeQuality::Low ? 16U
        : quality == GranularRuntimeQuality::Medium ? 32U
        : inherited && eff.granularQuality == FilterQuality::Eco ? kEcoMaxActiveGrains : kMaxGrains;
    const GrainInterpolation interpolation = quality == GranularRuntimeQuality::Low
        ? GrainInterpolation::Linear : quality == GranularRuntimeQuality::Ultra
        ? GrainInterpolation::Sinc8 : GrainInterpolation::Cubic;
    while (spawnPhase_ >= 1.0F) {
        spawnPhase_ -= 1.0F;
        ++counters_.requestedGrains;
        if (!bankUsable) {
            // No usable grain source: count the miss, stay silent, never crash.
            ++counters_.grainMisses;
            continue;
        }
        if (admissionLimit < kMaxGrains && active_grain_count() >= admissionLimit) {
            // Eco CPU scaling: refuse the admission instead of stealing or
            // growing past the cap. Counted in grainMisses so the throttle is
            // visible in the synth-level profiler.
            ++counters_.grainMisses;
            continue;
        }
        spawn_grain(source, eff, noteRatio, interpolation);
    }
    if (!bankUsable) return {0.0F, 0.0F};

    const float masterGain = eff.gain;
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
        // True stereo placement: L and R read from independent source
        // positions (grain.sourceOffsetL/R, both 0 at width01 == 0).
        const float basePosition = grain.sourcePositionFrames;
        float sampleL = interpolated_sample(source.samples, source.frameCount,
            basePosition + grain.sourceOffsetL, grain.interpolation, grain.pitchIncrement);
        float sampleR = interpolated_sample(source.samples, source.frameCount,
            basePosition + grain.sourceOffsetR, grain.interpolation, grain.pitchIncrement);
        grain.sourcePositionFrames = basePosition + grain.pitchIncrement;
        // Eco CPU scaling: skip the per-grain filter stage entirely when its
        // drive is neutral. Non-Eco tiers always run it (identity today;
        // wave-2 implements the filter behind apply_grain_filter).
        if (params.granularQuality != FilterQuality::Eco || grain.filterValue != 0.0F) {
            sampleL = apply_grain_filter(sampleL, grain.filterValue);
            sampleR = apply_grain_filter(sampleR, grain.filterValue);
        }
        // Equal-power panning (unchanged by the stereo source offsets).
        const float panLeft = std::sqrt(0.5F * (1.0F - grain.pan));
        const float panRight = std::sqrt(0.5F * (1.0F + grain.pan));
        left += sampleL * envelope * grain.gain * masterGain * panLeft;
        right += sampleR * envelope * grain.gain * masterGain * panRight;
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
