// Phase 3: generative conductor implementation.
#include "dve/audio/generative_conductor.hpp"

#include <algorithm>
#include <cmath>

#include "dve/audio/synthesizer.hpp"

namespace dve::audio {

GenerativeConductor::GenerativeConductor() = default;
GenerativeConductor::~GenerativeConductor() = default;

void GenerativeConductor::configure(bool enabled, const AttractorConfig& config) {
    if (hasConfig_ && enabled == configuredEnabled_ && config == configuredConfig_) return;
    configuredEnabled_ = enabled;
    configuredConfig_ = config;
    hasConfig_ = true;
    if (!enabled) return;
    attractor_.set_config(config);
    reset();
}

void GenerativeConductor::reset() {
    attractor_.reset();
    // Walk RNG is derived from the attractor seed so the whole generative
    // performance (phases + morph wander) is reproducible per preset.
    walkRng_ = configuredConfig_.seed ^ 0xD1B54A32D192ED03ULL;
    if (walkRng_ == 0) walkRng_ = 0x9E3779B97F4A7C15ULL;
    morphWalk_ = 0.5F;
}

SequencerScale GenerativeConductor::map_scale(const Scale& scale) noexcept {
    const std::string& name = scale.name;
    if (name == "pentatonic") return SequencerScale::PentatonicMajor;
    if (name == "dorian") return SequencerScale::Dorian;
    if (name == "minor") return SequencerScale::Minor;
    if (name == "major") return SequencerScale::Major;
    // phrygian / chromatic / unknown: no exact sequencer quantizer, fall back
    // to chromatic so no authored pitch is ever swallowed.
    return SequencerScale::Chromatic;
}

std::uint64_t GenerativeConductor::rng_next(std::uint64_t& state) noexcept {
    // splitmix64.
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

float GenerativeConductor::rng_unit(std::uint64_t& state) noexcept {
    return static_cast<float>(rng_next(state) >> 11) * (1.0F / 9007199254740992.0F);
}

float GenerativeConductor::walk_step(std::uint64_t& rngState, float current, float lo,
                                     float hi) noexcept {
    if (!(hi > lo)) return lo;  // degenerate range: park at the bound
    if (!(current >= lo && current <= hi)) current = lo + (hi - lo) * 0.5F;
    // ~1.5% of the range per block (~100 blocks/s): a full-range wander takes
    // on the order of a minute, smooth at audio-block granularity.
    const float step = (rng_unit(rngState) * 2.0F - 1.0F) * (hi - lo) * 0.015F;
    float next = current + step;
    if (next < lo) next = lo + (lo - next);  // reflect at the bounds
    if (next > hi) next = hi - (next - hi);
    return std::clamp(next, lo, hi);
}

void GenerativeConductor::process(Synthesizer& synth, double blockSeconds) {
    if (!enabled_) return;
    // Pick up the preset's attractor config published by set_preset(), so the
    // walk uses the right region even when render() hasn't run yet to apply
    // the queued PresetUpdate (e.g. a direct process() caller). configure()
    // is a no-op when nothing changed, so this never restarts the arc twice.
    if (auto pending = synth.take_pending_conductor_config())
        configure(pending->enabled, pending->config);
    const AttractorState state =
        blockSeconds > 0.0 ? attractor_.advance(blockSeconds) : attractor_.current();

    Sequencer& seq = synth.sequencer();
    // Scale/mode: keep the sequencer's current octave, move the pitch class.
    seq.set_scale(map_scale(state.scale));
    const int octaveBase = (static_cast<int>(seq.root_note()) / 12) * 12;
    seq.set_root_note(static_cast<std::uint8_t>(
        std::clamp(octaveBase + state.scale.rootSemitone, 0, 127)));
    // Rhythm density scales the probability lane's trigger gate (authored
    // step values are never rewritten).
    seq.set_probability_scale(0.15F + 0.85F * std::clamp(state.rhythmDensity01, 0.0F, 1.0F));
    // Mutation intensity drives the sequencer's own per-cycle note drift.
    seq.set_mutation_amount(std::clamp(state.mutationIntensity01, 0.0F, 1.0F));
    // Brightness becomes a cutoff multiplier: 0.5x at 0, 1x at 0.5, 2x at 1.
    const float brightness = std::clamp(state.brightness01, 0.0F, 1.0F);
    synth.set_conductor_cutoff_multiplier(std::exp2(-1.0F + 2.0F * brightness));
    // Morph wanders inside the state's morph region (seeded smooth walk).
    // Gated on morphEnabled so a non-morphing preset doesn't churn the
    // preset queue pointlessly. Lock-free: publishes through the synth's
    // realtime morph request; the render thread applies it to its cached
    // numeric presets without copying strings or allocating.
    if (synth.morph_enabled_rt()) {
        const float lo = std::min(state.morphMin, state.morphMax);
        const float hi = std::max(state.morphMin, state.morphMax);
        morphWalk_ = walk_step(walkRng_, morphWalk_, lo, hi);
        synth.request_morph_amount(morphWalk_);
    }
}

}  // namespace dve::audio
