// Phase 4 granular generator core (SYN-014, plan section 10 "Generator 4").
//
// A dedicated granular cloud engine: a fixed pool of grains is spawned at
// control rate from a density parameter, each grain reads a short window of
// the resident sample bank with cubic interpolation and its own envelope,
// pitch increment, pan, and direction, and all active grains accumulate into
// a stereo output pair.
//
// Design rules (foundation for wave-2 workers):
//   - No allocation on the audio thread: one fixed std::array<Grain, kMaxGrains>
//     per engine. When the pool is full the oldest (nearest completion) grain
//     is stolen.
//   - The engine is decoupled from the synthesizer: the grain source is a
//     plain GranularSource view (built from RealtimeSampleBank by the caller).
//     An empty/disabled bank renders silence and counts a grain miss.
//   - Core parameters are implemented here. The six macro fields
//     (cloud01/scatter01/dust01/freeze01/freezePosition01/smear01/width01) are
//     resolved by apply_granular_macros() into GranularEffectiveParams at
//     render time; the per-grain random draws (smear pitch/duration, dust
//     envelope override) come from the engine's seeded RNG so fixed-seed
//     renders stay deterministic.
//   - Profiler counters are accumulated locally and drained by the owner so the
//     voice can forward them into Synthesizer::granular_profiler().
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace dve::audio {

// CPU quality tiers shared by the generator stages (moved here from
// synthesizer.hpp so the decoupled granular engine can use the same type):
// Eco throttles cost (fewer grains, cheapest windows), Standard is the
// realtime default, High permits the most expensive realtime path, and
// Offline is for non-realtime renders.
enum class FilterQuality : std::uint8_t { Eco, Standard, High, Offline };
// Editor CPU policy is independent of authored instrument parameters. Inherit preserves
// the legacy preset path. Changes affect new grains; already sounding grains finish normally.
enum class GranularRuntimeQuality : std::uint8_t { Inherit, Low, Medium, High, Ultra };
enum class GrainInterpolation : std::uint8_t { Linear, Cubic, Sinc8 };

// Per-grain amplitude window. All four shapes are normalized to peak 1 with
// endpoints at ~0 (no clicks when a grain expires):
//   - Hann / Triangle: classic symmetric windows, peak at phase 0.5.
//   - ExponentialDecay: fast attack / smooth tail (gamma window x*k*e*e^(-k*x)
//     with k=10: peak 1 at phase 0.1, tail ~1.2e-3 at phase 1). Musically a
//     percussive grain; the dust macro biases per-grain shapes toward it.
//   - PlanckTaper: flat-top window with a 10% Planck taper on each end —
//     near-rectangular grains without edge clicks.
enum class GranularEnvelopeShape : std::uint8_t {
    Hann,
    Triangle,
    ExponentialDecay,
    PlanckTaper,
};

// Preset-level granular generator parameters. Field names are the contract
// used by wave-2 workers, the preset layer, and the editor UI.
struct GranularParameters {
    bool enabled{true};
    float densityHz{20.0F};            // grain spawn rate (grains/second)
    float durationMs{120.0F};          // grain duration in milliseconds
    float pitchSemitones{0.0F};        // per-grain pitch offset (semitones)
    float position01{0.0F};            // grain source position, 0..1 over the bank
    float positionJitter01{0.1F};      // random spray around position01, 0..1
    float panScatter01{0.3F};          // random per-grain pan spread, 0..1
    float gain{0.8F};                  // overall generator gain
    float reverseProbability01{0.0F};  // chance a grain plays backwards, 0..1
    GranularEnvelopeShape envelopeShape{GranularEnvelopeShape::Hann};
    // Six musical macro controls, resolved by apply_granular_macros() into a
    // GranularEffectiveParams before spawning. The base fields above stay
    // intact; the macros are a pure layer on top.
    float cloud01{0.5F};        // density x(0.25 + 3c), duration x(1 + 0.5c)
    float scatter01{0.0F};      // position jitter += scatter x 0.5
    float dust01{0.0F};         // duration x(1 - 0.9d), gain x(1 + 0.5d), transient-envelope bias
    float freeze01{0.0F};       // position lerp toward freezePosition01, jitter x(1 - freeze)
    float freezePosition01{0.5F};
    float smear01{0.0F};        // per-grain pitch +/-(12 x smear) st, duration x(1 +/- 0.75 x smear)
    float width01{0.5F};        // pan scatter x(0.2 + 1.6 x width); scales L/R source decorrelation
    // CPU quality tier for the grain cloud. Follows the same
    // FilterQuality convention as the preset-level filterQuality. Flows from
    // the preset automatically (GranularParameters is copied whole into the
    // active preset). Eco throttles admission to kEcoMaxActiveGrains grains,
    // forces the cheapest envelope (Triangle), and skips the per-grain filter
    // when its drive is neutral; Standard/High/Offline admit the full pool
    // (High and Offline additionally document the 4000 Hz density ceiling as
    // permitted — Offline is intended for render, not realtime).
    FilterQuality granularQuality{FilterQuality::Standard};
};

// Macro-resolved spawn parameters: the output of apply_granular_macros().
// The engine spawns grains from these; the per-grain random draws inside
// (smear pitch/duration, dust envelope override) use the engine's seeded RNG
// so fixed-seed renders stay deterministic.
struct GranularEffectiveParams {
    float densityHz{20.0F};
    float durationMs{120.0F};
    float pitchSemitones{0.0F};
    float position01{0.0F};             // freeze lerp applied; positionMod01 folded in
    float positionJitter01{0.1F};       // scatter added, freeze scaled
    float panScatter01{0.3F};           // width scaled
    float gain{0.8F};                   // dust compensation applied
    float reverseProbability01{0.0F};
    GranularEnvelopeShape envelopeShape{GranularEnvelopeShape::Hann};
    // Per-grain random ranges drawn at spawn from the engine's RNG (only when
    // the corresponding macro is active, so neutral macros leave the random
    // stream bit-identical to the macro-free path for the same seed):
    float smearPitchSemitones{0.0F};    // uniform offset in +/- this (semitones)
    float smearDurationSpread01{0.0F};  // uniform duration multiplier in 1 +/- this
    float dustTransientOverride01{0.0F};  // probability a grain's envelope becomes ExponentialDecay
    // Passthroughs (not macro-modified): quality tier drives Eco throttling
    // and envelope forcing in spawn; width01 scales the per-grain L/R source
    // decorrelation offsets.
    FilterQuality granularQuality{FilterQuality::Standard};
    float width01{0.5F};
};

// Pure function: resolves the six macro knobs on top of the base parameters.
// positionMod01 (GranularPosition modulation) is folded into position01 before
// the freeze lerp. Additive macros (scatter/dust/freeze/smear) are no-ops at 0;
// width is the identity at 0.5 and cloud maps density x1.0 at 0.25.
GranularEffectiveParams apply_granular_macros(const GranularParameters& params,
                                              float positionMod01) noexcept;

// Non-owning view of the grain source (built from the resident
// RealtimeSampleBank by the caller; kept decoupled so the engine and its
// tests never depend on synthesizer.cpp internals).
struct GranularSource {
    const float* samples{};      // mono source frames; nullptr = no source
    std::uint32_t frameCount{};  // < 2 renders silence (counts a grain miss)
    std::uint32_t sampleRate{};  // source sample rate, for pitch-increment math
    // MIDI note the source was recorded at (middle C = 60 by default, matching
    // RealtimeSampleBank::rootNote). Grain pitch tracks the played note
    // relative to this reference: playing rootNote renders the source at its
    // recorded pitch; each octave up doubles the grain playback rate.
    std::uint8_t rootNote{60};
};

// One live grain. Source position is in source frames; pitchIncrement is
// source frames advanced per output sample (signed: negative = reverse).
struct Grain {
    GrainInterpolation interpolation{GrainInterpolation::Cubic};
    bool active{false};
    float sourcePositionFrames{0.0F};
    float durationFrames{1.0F};
    float pitchIncrement{1.0F};
    float pan{0.0F};  // -1 (left) .. +1 (right), equal-power law at render
    float gain{1.0F};
    GranularEnvelopeShape envelopeShape{GranularEnvelopeShape::Hann};
    bool reverse{false};
    float filterValue{0.0F};   // reserved: wave-2 per-grain filter drive
    std::uint32_t seed{0U};    // xorshift state captured at spawn (debugging)
    float ageFrames{0.0F};     // output samples elapsed since spawn
    // Worker 3: true stereo grain placement. Independent L/R source-read
    // offsets in frames, derived from the grain's seed at spawn and scaled by
    // width01 (see spawn_grain). Both are exactly 0 at width01 == 0, so the
    // two channels read bit-identical source positions (mono-compatible).
    float sourceOffsetL{0.0F};
    float sourceOffsetR{0.0F};
};

// Counters accumulated on the audio thread. The owner drains them (draining
// zeroes) and forwards into the synthesizer-level profiler atomics.
struct GranularCounters {
    std::uint64_t requestedGrains{};  // spawn attempts
    std::uint64_t admittedGrains{};   // grains actually started (incl. steals)
    std::uint64_t grainSteals{};      // spawn attempts that stole a live grain
    // Spawn attempts with no usable source, plus Eco-quality admission
    // refusals (a spawn refused because kEcoMaxActiveGrains grains are
    // already live). Shared by design so quality throttling is visible in the
    // synth-level profiler without extra plumbing.
    std::uint64_t grainMisses{};
};

// Fixed-pool granular cloud: one instance per voice oscillator.
class GranularEngine {
public:
    static constexpr std::size_t kMaxGrains = 64;
    // Worker 3: Eco quality admits at most this many concurrent grains; extra
    // spawn attempts are refused (counted in GranularCounters::grainMisses).
    static constexpr std::size_t kEcoMaxActiveGrains = 16;
    // Worker 3: maximum L/R source-offset magnitude at width01 == 1, in
    // seconds of source audio. 1 ms keeps the antisymmetric pair's mono
    // collapse within a few percent on tonal material (the mono error is
    // second order in the offset) while still fully decorrelating L/R at
    // high widths (the L/R difference is first order in the offset).
    static constexpr float kMaxStereoOffsetSeconds = 0.001F;

    GranularEngine() noexcept = default;
    GranularEngine(const GranularEngine&) = default;
    GranularEngine& operator=(const GranularEngine&) = default;

    // Output sample rate in Hz (drives spawn scheduling and pitch increments).
    void set_sample_rate(std::uint32_t sampleRate) noexcept;
    // Seeds the deterministic xorshift random stream (grain jitter, pan,
    // reverse decisions). Same seed + same params + same source = bit-identical
    // output, which the determinism tests rely on.
    void set_seed(std::uint32_t seed) noexcept;
    // Full reset: clears grains, spawn phase, and counters.
    void reset() noexcept;
    // Deactivates all grains immediately without touching counters or the
    // random stream (voice kill / all-sound-off path).
    void kill_grains() noexcept;

    // Renders one stereo sample. Spawns grains at control rate from the
    // macro-resolved effective density; grain parameters are sampled at spawn
    // time. Active grains are cubic-interpolated from the source and
    // accumulated with equal-power panning. Each grain additionally carries
    // independent L/R source-read offsets scaled by width01 (both exactly 0
    // at width01 == 0, so the channels read bit-identical positions).
    //
    // positionMod01 is the GranularPosition modulation value: folded into the
    // macro-resolved position (before the freeze lerp) and clamped to 0..1.
    //
    // When params.enabled is false, or the source is missing/empty, the output
    // is silence; each spawn attempt against a missing/empty source counts one
    // grainMiss instead of crashing.
    //
    // The 3-argument form renders with the source's reference pitch (ratio
    // exactly 1.0); the 4-argument form tracks the played note: each grain's
    // playback rate is scaled by frequencyHertz / referenceFrequency, where
    // referenceFrequency = 440 * 2^((source.rootNote - 69)/12). A non-finite
    // or non-positive frequencyHertz falls back to the reference (ratio 1.0).
    std::pair<float, float> render(const GranularSource& source,
                                   const GranularParameters& params,
                                   float positionMod01) noexcept;
    std::pair<float, float> render(const GranularSource& source,
                                   const GranularParameters& params,
                                   float positionMod01,
                                   float frequencyHertz) noexcept;
    std::pair<float, float> render(const GranularSource& source, const GranularParameters& params,
                                  float positionMod01, float frequencyHertz,
                                  GranularRuntimeQuality quality) noexcept;

    [[nodiscard]] std::uint32_t active_grain_count() const noexcept;
    [[nodiscard]] const GranularCounters& counters() const noexcept { return counters_; }
    // Returns the accumulated counters and zeroes them.
    GranularCounters drain_counters() noexcept;

    // Read-only view of the pool (tests, telemetry, wave-2 workers).
    [[nodiscard]] const std::array<Grain, kMaxGrains>& grains() const noexcept { return grains_; }

    // Grain window value at normalized phase 0..1 (clamped).
    static float envelope_value(GranularEnvelopeShape shape, float phase01) noexcept;
    // Semitone offset -> playback ratio (12 = 2x, -12 = 0.5x).
    static float semitones_to_ratio(float semitones) noexcept;
    // Catmull-Rom cubic interpolation over the source; position is in frames
    // and is clamped to the valid range (same pattern as sampler_cubic_sample).
    static float cubic_sample(const float* source, std::uint32_t frameCount, float position) noexcept;
    static float interpolated_sample(const float* source, std::uint32_t frameCount, float position,
                                     GrainInterpolation interpolation, float increment = 1.0F) noexcept;

private:
    std::uint32_t random_u32() noexcept;
    float random01() noexcept;
    void spawn_grain(const GranularSource& source, const GranularEffectiveParams& effective,
                     float noteRatio, GrainInterpolation interpolation) noexcept;

    std::array<Grain, kMaxGrains> grains_{};
    GranularCounters counters_{};
    std::uint32_t sampleRate_{48000U};
    std::uint32_t rngState_{1U};
    float spawnPhase_{0.0F};
};

}  // namespace dve::audio
