// Phase 3 (SYN-013): seeded patch mutation and breeding for SynthPreset.
//
// Semantics follow the Phase 1 morph precedent: tuning snaps to musical
// intervals, envelope times move multiplicatively, filter cutoff moves in the
// log domain, resonance/gains/mixes move linearly, topology/mode changes are
// rare discrete jumps, and everything is clamped so no NaN or out-of-range
// value is ever produced.
#include "dve/audio/patch_genetics.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <string>

namespace dve::audio {
namespace {

// Seeded RNG. Draws happen in a fixed code order, so the same
// (preset, intensity, seed, locks) always yields the same result.
class GeneRng {
public:
    explicit GeneRng(std::uint64_t seed) : gen_(seed) {}
    float uniform01() { return std::uniform_real_distribution<float>(0.0F, 1.0F)(gen_); }
    float symmetric() { return uniform01() * 2.0F - 1.0F; }  // [-1, 1)
    int intRange(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(gen_); }
    bool chance(float p) { return uniform01() < p; }

private:
    std::mt19937_64 gen_;
};

[[nodiscard]] bool groupLocked(std::uint8_t mask, GeneGroup group) noexcept {
    return (mask & gene_group_bit(group)) != 0U;
}

// Max multiplicative swing for ratio-based mutations (envelope times, cutoff,
// delay times, drive, ...), in octaves: ±(0.5 + 2.5 * intensity).
[[nodiscard]] float maxMutationOctaves(float intensity) noexcept {
    return 0.5F + 2.5F * intensity;
}

// Per-gene hit chance: continuous parameters move often, discrete topology /
// mode / source changes are rare. Both scale with intensity.
[[nodiscard]] float continuousChance(float intensity) noexcept { return 0.45F + 0.55F * intensity; }
[[nodiscard]] float discreteChance(float intensity) noexcept { return 0.12F * intensity; }

// Linear drift within [lo, hi]: never NaN, never out of range.
float driftLinear(GeneRng& rng, float value, float span, float lo, float hi, float intensity) {
    const float next = value + rng.symmetric() * span * intensity;
    return std::clamp(next, lo, hi);
}

// Multiplicative (log-domain) drift: value * 2^(+/-octaves), clamped to [lo, hi].
// Never negative, never NaN for finite input.
float driftRatio(GeneRng& rng, float value, float maxOctaves, float lo, float hi) {
    const float ratio = std::exp2(rng.symmetric() * maxOctaves);
    return std::clamp(value * ratio, lo, hi);
}

constexpr int kOscillatorWaveformCount = 16;  // Sine .. ModalResonator
constexpr int kLfoWaveformCount = 6;          // Sine .. SmoothRandom

void mutateOscillators(GeneRng& rng, float intensity, SynthPreset& result) {
    const float cont = continuousChance(intensity);
    const float disc = discreteChance(intensity);
    for (auto& osc : result.oscillators) {
        if (rng.chance(disc))
            osc.waveform = static_cast<OscillatorWaveform>(rng.intRange(0, kOscillatorWaveformCount - 1));
        if (rng.chance(cont)) {
            // Tuning snaps to musical intervals: semitone steps of
            // +/-(1 + 6*intensity), occasionally a perfect fifth or an octave.
            int steps = 0;
            if (rng.chance(0.15F)) {
                const int leaps[2] = {7, 12};
                steps = leaps[rng.intRange(0, 1)] * (rng.chance(0.5F) ? 1 : -1);
            } else {
                const int magnitude = 1 + static_cast<int>(rng.uniform01() * 6.0F * intensity);
                steps = magnitude * (rng.chance(0.5F) ? 1 : -1);
            }
            osc.semitones = std::clamp(std::round(osc.semitones + static_cast<float>(steps)),
                                       -96.0F, 96.0F);
        }
        if (rng.chance(cont)) osc.gain = driftLinear(rng, osc.gain, 0.3F, 0.0F, 2.0F, intensity);
        if (rng.chance(cont)) osc.shape = driftLinear(rng, osc.shape, 0.3F, 0.0F, 1.0F, intensity);
        if (rng.chance(cont))
            osc.pulseWidth = driftLinear(rng, osc.pulseWidth, 0.2F, 0.03F, 0.97F, intensity);
        if (rng.chance(cont)) osc.pwmDepth = driftLinear(rng, osc.pwmDepth, 0.3F, 0.0F, 1.0F, intensity);
        if (rng.chance(cont))
            osc.pwmRateHertz = driftRatio(rng, osc.pwmRateHertz, maxMutationOctaves(intensity),
                                          0.01F, 40.0F);
        if (rng.chance(cont))
            osc.subOscillatorLevel = driftLinear(rng, osc.subOscillatorLevel, 0.3F, 0.0F, 1.0F, intensity);
        if (rng.chance(disc)) osc.subOscillatorOctaves = static_cast<std::uint8_t>(rng.intRange(1, 3));
        if (rng.chance(disc * 0.5F)) osc.enabled = !osc.enabled;
        if (rng.chance(disc * 0.5F)) osc.keySync = !osc.keySync;
    }
}

void mutateSpectralShape(GeneRng& rng, float intensity, SynthPreset& result) {
    const float cont = continuousChance(intensity);
    const float disc = discreteChance(intensity);
    for (auto& osc : result.oscillators) {
        if (rng.chance(cont))
            osc.wavetablePosition = driftLinear(rng, osc.wavetablePosition, 0.3F, 0.0F, 1.0F, intensity);
        // Modal resonator globals (mutated unconditionally for a stable draw
        // order; only audible when waveform == ModalResonator).
        auto& resonator = osc.modalResonator;
        if (rng.chance(cont))
            resonator.damping = driftRatio(rng, resonator.damping, maxMutationOctaves(intensity),
                                           0.01F, 8.0F);
        if (rng.chance(cont))
            resonator.inharmonicity = driftLinear(rng, resonator.inharmonicity, 0.3F, 0.0F, 1.0F, intensity);
        if (rng.chance(cont))
            resonator.brightness = driftLinear(rng, resonator.brightness, 0.3F, 0.0F, 1.0F, intensity);
        if (rng.chance(cont))
            resonator.excitationLevel = driftLinear(rng, resonator.excitationLevel, 0.5F, 0.0F, 4.0F, intensity);
    }
    auto& sampler = result.sampler;
    if (rng.chance(cont))
        sampler.startOffsetSeconds = driftLinear(rng, sampler.startOffsetSeconds, 0.2F, 0.0F, 3600.0F, intensity);
    if (rng.chance(cont)) sampler.gain = driftLinear(rng, sampler.gain, 0.3F, 0.0F, 2.0F, intensity);
    if (rng.chance(cont))
        sampler.loopStartSeconds = driftLinear(rng, sampler.loopStartSeconds, 0.2F, 0.0F, 3600.0F, intensity);
    if (rng.chance(cont))
        sampler.loopEndSeconds = driftLinear(rng, sampler.loopEndSeconds, 0.2F, 0.0F, 3600.0F, intensity);
    if (rng.chance(disc))
        sampler.playbackMode = (sampler.playbackMode == SamplerPlaybackMode::OneShot)
                                   ? SamplerPlaybackMode::Loop
                                   : SamplerPlaybackMode::OneShot;
    if (rng.chance(disc))
        sampler.direction = (sampler.direction == SamplerDirection::Forward)
                                ? SamplerDirection::Reverse
                                : SamplerDirection::Forward;
}

void mutateFilters(GeneRng& rng, float intensity, SynthPreset& result) {
    const float cont = continuousChance(intensity);
    const float disc = discreteChance(intensity);
    auto& filter = result.filter;
    // Cutoff moves in the log domain (ratio-based), like the Phase 1 morph.
    if (rng.chance(cont))
        filter.cutoffHertz = driftRatio(rng, filter.cutoffHertz, maxMutationOctaves(intensity),
                                        18.0F, 24000.0F);
    if (rng.chance(cont)) filter.resonance = driftLinear(rng, filter.resonance, 0.4F, 0.0F, 1.0F, intensity);
    if (rng.chance(cont))
        filter.drive = driftRatio(rng, filter.drive, maxMutationOctaves(intensity), 0.05F, 24.0F);
    if (rng.chance(cont))
        filter.envelopeAmountOctaves = driftLinear(rng, filter.envelopeAmountOctaves, 3.0F, -12.0F, 12.0F, intensity);
    if (rng.chance(cont)) filter.keyTrack = driftLinear(rng, filter.keyTrack, 0.5F, -2.0F, 2.0F, intensity);
    if (rng.chance(cont))
        filter.bassCompensation = driftLinear(rng, filter.bassCompensation, 0.3F, 0.0F, 1.0F, intensity);
    if (rng.chance(cont)) filter.morph = driftLinear(rng, filter.morph, 0.3F, 0.0F, 1.0F, intensity);
    if (rng.chance(cont))
        filter.ms20HighPassCutoffHertz = driftRatio(rng, filter.ms20HighPassCutoffHertz,
                                                    maxMutationOctaves(intensity), 12.0F, 18000.0F);
    if (rng.chance(cont))
        filter.selfOscillation = driftLinear(rng, filter.selfOscillation, 0.2F, 0.5F, 1.35F, intensity);
    // Rare discrete topology / mode jumps.
    if (rng.chance(disc))
        filter.topology = static_cast<FilterTopology>(rng.intRange(0, 5));  // .. Formant
    if (rng.chance(disc)) filter.mode = static_cast<FilterMode>(rng.intRange(0, 3));
    if (rng.chance(cont)) filter.comb.damping = driftLinear(rng, filter.comb.damping, 0.3F, 0.0F, 1.0F, intensity);
    if (rng.chance(cont)) filter.comb.mix = driftLinear(rng, filter.comb.mix, 0.3F, 0.0F, 1.0F, intensity);
    if (rng.chance(cont))
        filter.comb.feedbackScale = driftLinear(rng, filter.comb.feedbackScale, 0.3F, 0.0F, 1.5F, intensity);
    if (rng.chance(cont))
        filter.formant.dryMix = driftLinear(rng, filter.formant.dryMix, 0.3F, 0.0F, 1.0F, intensity);
    for (std::size_t i = 0; i < FormantParameters::kBandCount; ++i) {
        if (rng.chance(cont))
            filter.formant.frequencyHertz[i] = driftRatio(rng, filter.formant.frequencyHertz[i],
                                                          maxMutationOctaves(intensity), 50.0F, 12000.0F);
        if (rng.chance(cont))
            filter.formant.gains[i] = driftLinear(rng, filter.formant.gains[i], 0.3F, 0.0F, 2.0F, intensity);
    }
}

void mutateAdsr(GeneRng& rng, float intensity, AdsrParameters& envelope) {
    const float cont = continuousChance(intensity);
    const float disc = discreteChance(intensity);
    const float octaves = maxMutationOctaves(intensity);
    // Time parameters move multiplicatively (never negative), like the morph.
    auto mutateTime = [&](float& time) {
        if (rng.chance(cont)) time = driftRatio(rng, std::max(time, 0.001F), octaves, 0.0F, 60.0F);
    };
    mutateTime(envelope.attackSeconds);
    mutateTime(envelope.decaySeconds);
    mutateTime(envelope.releaseSeconds);
    mutateTime(envelope.delaySeconds);
    mutateTime(envelope.holdSeconds);
    if (rng.chance(cont))
        envelope.sustainLevel = driftLinear(rng, envelope.sustainLevel, 0.3F, 0.0F, 1.0F, intensity);
    if (rng.chance(disc * 0.5F))
        envelope.curve = (envelope.curve == EnvelopeCurve::Linear) ? EnvelopeCurve::Exponential
                                                                   : EnvelopeCurve::Linear;
}

void mutateModulation(GeneRng& rng, float intensity, SynthPreset& result) {
    const float cont = continuousChance(intensity);
    const float disc = discreteChance(intensity);
    for (auto& slot : result.modulation) {
        if (rng.chance(cont)) slot.amount = driftLinear(rng, slot.amount, 0.4F, -1.0F, 1.0F, intensity);
        if (rng.chance(cont * 0.5F)) slot.bias = driftLinear(rng, slot.bias, 0.3F, -1.0F, 1.0F, intensity);
        if (rng.chance(disc))
            slot.source = static_cast<ModulationSource>(
                rng.intRange(0, static_cast<int>(ModulationSource::Lorenz)));
        if (rng.chance(disc))
            slot.destination = static_cast<ModulationDestination>(
                rng.intRange(0, static_cast<int>(ModulationDestination::SamplerStartPosition)));
        if (rng.chance(disc * 0.5F)) slot.enabled = !slot.enabled;
    }
    for (auto& lfo : result.lfos) {
        // LFO rate morphs in log domain in the Phase 1 morph; same here.
        if (rng.chance(cont))
            lfo.rateHertz = driftRatio(rng, lfo.rateHertz, maxMutationOctaves(intensity), 0.001F, 100.0F);
        if (rng.chance(cont)) lfo.depth = driftLinear(rng, lfo.depth, 0.3F, 0.0F, 1.0F, intensity);
        if (rng.chance(disc))
            lfo.waveform = static_cast<LfoWaveform>(rng.intRange(0, kLfoWaveformCount - 1));
    }
    for (auto& macro : result.macros.values) {
        if (rng.chance(cont)) macro = driftLinear(rng, macro, 0.3F, 0.0F, 1.0F, intensity);
    }
}

void mutateStereo(GeneRng& rng, float intensity, SynthPreset& result) {
    const float cont = continuousChance(intensity);
    const float disc = discreteChance(intensity);
    for (auto& osc : result.oscillators) {
        if (rng.chance(cont))
            osc.stereoDivergence = driftLinear(rng, osc.stereoDivergence, 0.3F, 0.0F, 1.0F, intensity);
        if (rng.chance(cont)) osc.pan = driftLinear(rng, osc.pan, 0.4F, -1.0F, 1.0F, intensity);
        if (rng.chance(cont * 0.5F))
            osc.grainStereoSpread = driftLinear(rng, osc.grainStereoSpread, 0.3F, 0.0F, 1.0F, intensity);
    }
    if (rng.chance(cont)) result.masterPan = driftLinear(rng, result.masterPan, 0.4F, -1.0F, 1.0F, intensity);
    auto& unison = result.unison;
    if (rng.chance(cont))
        unison.detuneCents = driftLinear(rng, unison.detuneCents, 20.0F, 0.0F, 100.0F, intensity);
    if (rng.chance(cont))
        unison.stereoSpread = driftLinear(rng, unison.stereoSpread, 0.3F, 0.0F, 1.0F, intensity);
    if (rng.chance(cont))
        unison.phaseSpread = driftLinear(rng, unison.phaseSpread, 0.3F, 0.0F, 1.0F, intensity);
    if (rng.chance(disc))
        unison.voices = static_cast<std::uint8_t>(
            rng.intRange(1, static_cast<int>(kSynthUnisonMax)));
    if (rng.chance(disc * 0.5F)) unison.enabled = !unison.enabled;
}

void mutateEffects(GeneRng& rng, float intensity, SynthPreset& result) {
    const float cont = continuousChance(intensity);
    const float disc = discreteChance(intensity);
    const float octaves = maxMutationOctaves(intensity);
    // FX mix parameters move linearly within [0, 1].
    auto mutateMix = [&](float& mix) {
        if (rng.chance(cont)) mix = driftLinear(rng, mix, 0.3F, 0.0F, 1.0F, intensity);
    };
    if (rng.chance(cont))
        result.distortion.drive = driftRatio(rng, result.distortion.drive, octaves, 0.05F, 32.0F);
    mutateMix(result.distortion.mix);
    if (rng.chance(disc))
        result.distortion.mode = static_cast<DistortionMode>(rng.intRange(0, 3));
    mutateMix(result.bitcrusher.mix);
    if (rng.chance(disc))
        result.bitcrusher.bits = static_cast<std::uint8_t>(rng.intRange(1, 16));
    if (rng.chance(disc))
        result.bitcrusher.downsample = static_cast<std::uint8_t>(rng.intRange(1, 64));
    if (rng.chance(cont))
        result.harmonizer.subLevel = driftLinear(rng, result.harmonizer.subLevel, 0.3F, 0.0F, 1.0F, intensity);
    if (rng.chance(cont))
        result.harmonizer.upLevel = driftLinear(rng, result.harmonizer.upLevel, 0.3F, 0.0F, 1.0F, intensity);
    mutateMix(result.harmonizer.mix);
    if (rng.chance(cont))
        result.eq.lowGainDb = driftLinear(rng, result.eq.lowGainDb, 6.0F, -24.0F, 24.0F, intensity);
    if (rng.chance(cont))
        result.eq.midGainDb = driftLinear(rng, result.eq.midGainDb, 6.0F, -24.0F, 24.0F, intensity);
    if (rng.chance(cont))
        result.eq.highGainDb = driftLinear(rng, result.eq.highGainDb, 6.0F, -24.0F, 24.0F, intensity);
    if (rng.chance(cont))
        result.chorus.rateHertz = driftRatio(rng, result.chorus.rateHertz, octaves, 0.01F, 20.0F);
    if (rng.chance(cont))
        result.chorus.depthMilliseconds =
            driftLinear(rng, result.chorus.depthMilliseconds, 6.0F, 0.0F, 30.0F, intensity);
    mutateMix(result.chorus.mix);
    if (rng.chance(cont))
        result.flanger.rateHertz = driftRatio(rng, result.flanger.rateHertz, octaves, 0.01F, 20.0F);
    if (rng.chance(cont))
        result.flanger.depthMilliseconds =
            driftLinear(rng, result.flanger.depthMilliseconds, 2.0F, 0.0F, 10.0F, intensity);
    if (rng.chance(cont))
        result.flanger.feedback = driftLinear(rng, result.flanger.feedback, 0.3F, -0.92F, 0.92F, intensity);
    mutateMix(result.flanger.mix);
    mutateMix(result.ensemble.mix);
    if (rng.chance(disc)) result.ensemble.mode = static_cast<EnsembleMode>(rng.intRange(0, 2));
    if (rng.chance(cont))
        result.phaser.rateHertz = driftRatio(rng, result.phaser.rateHertz, octaves, 0.01F, 20.0F);
    if (rng.chance(cont))
        result.phaser.depth = driftLinear(rng, result.phaser.depth, 0.3F, 0.0F, 1.0F, intensity);
    if (rng.chance(cont))
        result.phaser.feedback = driftLinear(rng, result.phaser.feedback, 0.3F, -0.95F, 0.95F, intensity);
    mutateMix(result.phaser.mix);
    if (rng.chance(cont))
        result.delay.timeSeconds = driftRatio(rng, result.delay.timeSeconds, octaves, 0.01F, 1.95F);
    if (rng.chance(cont))
        result.delay.feedback = driftLinear(rng, result.delay.feedback, 0.2F, 0.0F, 0.94F, intensity);
    mutateMix(result.delay.mix);
    if (rng.chance(cont))
        result.diffusionDelay.timeSeconds =
            driftRatio(rng, result.diffusionDelay.timeSeconds, octaves, 0.01F, 1.95F);
    if (rng.chance(cont))
        result.diffusionDelay.feedback =
            driftLinear(rng, result.diffusionDelay.feedback, 0.2F, 0.0F, 0.94F, intensity);
    mutateMix(result.diffusionDelay.mix);
    if (rng.chance(cont))
        result.diffusionDelay.diffusion =
            driftLinear(rng, result.diffusionDelay.diffusion, 0.3F, 0.0F, 1.0F, intensity);
    if (rng.chance(cont))
        result.reverb.roomSize = driftLinear(rng, result.reverb.roomSize, 0.3F, 0.0F, 1.0F, intensity);
    if (rng.chance(cont))
        result.reverb.damping = driftLinear(rng, result.reverb.damping, 0.3F, 0.0F, 0.98F, intensity);
    if (rng.chance(cont))
        result.reverb.width = driftLinear(rng, result.reverb.width, 0.3F, 0.0F, 1.0F, intensity);
    mutateMix(result.reverb.mix);
    if (rng.chance(cont))
        result.compressor.thresholdDb =
            driftLinear(rng, result.compressor.thresholdDb, 12.0F, -60.0F, 0.0F, intensity);
    if (rng.chance(cont))
        result.compressor.ratio = driftLinear(rng, result.compressor.ratio, 6.0F, 1.0F, 30.0F, intensity);
    if (rng.chance(cont))
        result.compressor.makeupDb =
            driftLinear(rng, result.compressor.makeupDb, 6.0F, -24.0F, 24.0F, intensity);
    if (rng.chance(cont))
        result.limiter.ceilingDb = driftLinear(rng, result.limiter.ceilingDb, 6.0F, -24.0F, 0.0F, intensity);
}

// ---- Breed helpers: copy the fields each gene group owns. ----

void copyOscillatorGroup(SynthPreset& dst, const SynthPreset& src) {
    for (std::size_t i = 0; i < dst.oscillators.size(); ++i) {
        auto& d = dst.oscillators[i];
        const auto& s = src.oscillators[i];
        d.enabled = s.enabled;
        d.waveform = s.waveform;
        d.gain = s.gain;
        d.semitones = s.semitones;
        d.pulseWidth = s.pulseWidth;
        d.pwmDepth = s.pwmDepth;
        d.pwmRateHertz = s.pwmRateHertz;
        d.shape = s.shape;
        d.subOscillatorLevel = s.subOscillatorLevel;
        d.subOscillatorOctaves = s.subOscillatorOctaves;
        d.keySync = s.keySync;
        // pan, stereoDivergence, grainStereoSpread -> Stereo; wavetablePosition,
        // modalResonator -> SpectralShape.
    }
}

void copySpectralShapeGroup(SynthPreset& dst, const SynthPreset& src) {
    for (std::size_t i = 0; i < dst.oscillators.size(); ++i) {
        dst.oscillators[i].wavetablePosition = src.oscillators[i].wavetablePosition;
        dst.oscillators[i].modalResonator = src.oscillators[i].modalResonator;
    }
    dst.sampler = src.sampler;
}

void copyFilterGroup(SynthPreset& dst, const SynthPreset& src) {
    // filter.envelope belongs to the Envelopes group; keep the child's own.
    const AdsrParameters savedEnvelope = dst.filter.envelope;
    dst.filter = src.filter;
    dst.filter.envelope = savedEnvelope;
}

void copyEnvelopeGroup(SynthPreset& dst, const SynthPreset& src) {
    dst.ampEnvelope = src.ampEnvelope;
    dst.filter.envelope = src.filter.envelope;
}

void copyModulationGroup(SynthPreset& dst, const SynthPreset& src) {
    dst.modulation = src.modulation;
    dst.lfos = src.lfos;
    dst.macros = src.macros;
}

void copyStereoGroup(SynthPreset& dst, const SynthPreset& src) {
    for (std::size_t i = 0; i < dst.oscillators.size(); ++i) {
        dst.oscillators[i].pan = src.oscillators[i].pan;
        dst.oscillators[i].stereoDivergence = src.oscillators[i].stereoDivergence;
        dst.oscillators[i].grainStereoSpread = src.oscillators[i].grainStereoSpread;
    }
    dst.masterPan = src.masterPan;
    dst.unison = src.unison;
}

void copySequencerGroup(SynthPreset& dst, const SynthPreset& src) {
    dst.arpeggiator = src.arpeggiator;
}

void copyEffectsGroup(SynthPreset& dst, const SynthPreset& src) {
    dst.distortion = src.distortion;
    dst.bitcrusher = src.bitcrusher;
    dst.harmonizer = src.harmonizer;
    dst.eq = src.eq;
    dst.chorus = src.chorus;
    dst.flanger = src.flanger;
    dst.ensemble = src.ensemble;
    dst.phaser = src.phaser;
    dst.delay = src.delay;
    dst.diffusionDelay = src.diffusionDelay;
    dst.reverb = src.reverb;
    dst.compressor = src.compressor;
    dst.limiter = src.limiter;
}

}  // namespace

SynthPreset mutate_preset(const SynthPreset& p, float intensity, std::uint64_t seed,
                          std::uint8_t lockedGroups) {
    if (!std::isfinite(intensity) || intensity <= 0.0F) return p;
    const float t = std::clamp(intensity, 0.0F, 1.0F);
    SynthPreset result = p;
    GeneRng rng(seed);
    if (!groupLocked(lockedGroups, GeneGroup::Oscillators)) mutateOscillators(rng, t, result);
    if (!groupLocked(lockedGroups, GeneGroup::SpectralShape)) mutateSpectralShape(rng, t, result);
    if (!groupLocked(lockedGroups, GeneGroup::Filters)) mutateFilters(rng, t, result);
    if (!groupLocked(lockedGroups, GeneGroup::Envelopes)) {
        mutateAdsr(rng, t, result.ampEnvelope);
        mutateAdsr(rng, t, result.filter.envelope);
    }
    if (!groupLocked(lockedGroups, GeneGroup::Modulation)) mutateModulation(rng, t, result);
    if (!groupLocked(lockedGroups, GeneGroup::Stereo)) mutateStereo(rng, t, result);
    // Sequencer: label exists for API stability; mutation body lands later.
    if (!groupLocked(lockedGroups, GeneGroup::Effects)) mutateEffects(rng, t, result);
    return result;
}

SynthPreset breed_presets(const SynthPreset& a, const SynthPreset& b, std::uint8_t groupsFromB,
                          std::uint64_t seed) {
    SynthPreset child = a;
    if ((groupsFromB & gene_group_bit(GeneGroup::Oscillators)) != 0U) copyOscillatorGroup(child, b);
    if ((groupsFromB & gene_group_bit(GeneGroup::SpectralShape)) != 0U) copySpectralShapeGroup(child, b);
    if ((groupsFromB & gene_group_bit(GeneGroup::Filters)) != 0U) copyFilterGroup(child, b);
    if ((groupsFromB & gene_group_bit(GeneGroup::Envelopes)) != 0U) copyEnvelopeGroup(child, b);
    if ((groupsFromB & gene_group_bit(GeneGroup::Modulation)) != 0U) copyModulationGroup(child, b);
    if ((groupsFromB & gene_group_bit(GeneGroup::Stereo)) != 0U) copyStereoGroup(child, b);
    if ((groupsFromB & gene_group_bit(GeneGroup::Sequencer)) != 0U) copySequencerGroup(child, b);
    if ((groupsFromB & gene_group_bit(GeneGroup::Effects)) != 0U) copyEffectsGroup(child, b);
    // Deterministic name (kept within the 128-char validate() bound).
    std::string name = "Breed: " + a.name + " x " + b.name;
    if (name.size() > 128U) name.resize(128U);
    child.name = name;
    // Small post-breed mutation for organic feel: low fixed intensity, no locks,
    // seed derived from the breed seed so the whole operation stays reproducible.
    return mutate_preset(child, 0.15F, seed ^ 0x9E3779B97F4A7C15ULL, 0U);
}

}  // namespace dve::audio
