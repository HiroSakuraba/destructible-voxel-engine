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
//     stored so presets and UI can carry them; their *behavior* is wave-2's.
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

// Per-grain amplitude window. Hann and Triangle are the core shapes;
// ExponentialDecay and PlanckTaper use standard textbook windows here and are
// owned (tuning/verification) by the wave-2 envelope-shape work.
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
    // Wave-2 macro controls: stored here so the header interface is stable;
    // behavior is implemented by wave-2 workers.
    float cloud01{0.5F};
    float scatter01{0.0F};
    float dust01{0.0F};
    float freeze01{0.0F};
    float freezePosition01{0.5F};
    float smear01{0.0F};
    float width01{0.5F};
    // Worker 3: CPU quality tier for the grain cloud. Follows the same
    // FilterQuality convention as the preset-level filterQuality. Flows from
    // the preset automatically (GranularParameters is copied whole into the
    // active preset). Eco throttles admission to kEcoMaxActiveGrains grains,
    // forces the cheapest envelope (Triangle), and skips the per-grain filter
    // when its drive is neutral; Standard/High/Offline admit the full pool
    // (High and Offline additionally document the 4000 Hz density ceiling as
    // permitted — Offline is intended for render, not realtime).
    FilterQuality granularQuality{FilterQuality::Standard};
};

// Non-owning view of the grain source (built from the resident
// RealtimeSampleBank by the caller; kept decoupled so the engine and its
// tests never depend on synthesizer.cpp internals).
struct GranularSource {
    const float* samples{};      // mono source frames; nullptr = no source
    std::uint32_t frameCount{};  // < 2 renders silence (counts a grain miss)
    std::uint32_t sampleRate{};  // source sample rate, for pitch-increment math
};

// One live grain. Source position is in source frames; pitchIncrement is
// source frames advanced per output sample (signed: negative = reverse).
struct Grain {
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

// Fixed-pool granular cloud: one instance per synth voice.
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

    // Renders one stereo sample. Spawns grains at control rate from
    // params.densityHz; grain parameters are sampled at spawn time. Active
    // grains are cubic-interpolated from the source and accumulated with
    // equal-power panning. Each grain additionally carries independent L/R
    // source-read offsets scaled by params.width01 (both exactly 0 at
    // width01 == 0, so the channels read bit-identical positions).
    //
    // positionMod01 is the GranularPosition modulation value: added to
    // params.position01 and clamped to 0..1 before mapping over the bank.
    //
    // When params.enabled is false, or the source is missing/empty, the output
    // is silence; each spawn attempt against a missing/empty source counts one
    // grainMiss instead of crashing.
    std::pair<float, float> render(const GranularSource& source,
                                   const GranularParameters& params,
                                   float positionMod01) noexcept;

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

private:
    std::uint32_t random_u32() noexcept;
    float random01() noexcept;
    void spawn_grain(const GranularSource& source, const GranularParameters& params,
                     float positionMod01) noexcept;

    std::array<Grain, kMaxGrains> grains_{};
    GranularCounters counters_{};
    std::uint32_t sampleRate_{48000U};
    std::uint32_t rngState_{1U};
    float spawnPhase_{0.0F};
};

}  // namespace dve::audio
