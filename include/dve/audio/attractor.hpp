// Attractor sequencer prototype (Phase 3 of the DVE Advanced Synthesizer plan).
//
// Gives generative patches DIRECTION instead of pure randomness: a small state
// machine cycles HOME -> RISE -> TENSION -> PEAK -> FALL -> HOME on musical
// time (bars, tempo-synced). Each state selects a bundle of musical parameters
// (rhythm density, scale/mode, morph region, mutation intensity) and all
// continuous parameters are ramped smoothly across the state's bars, so there
// are no jumps at state boundaries or mid-state.
//
// This unit is deliberately dependency-free: it knows nothing about the
// sequencer lanes or Synthesizer. A later integration step maps AttractorState
// onto real sequencer lanes and Synthesizer::set_morph_amount.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dve::audio {

// Musical arc phases, in cycle order.
enum class AttractorPhase : std::uint8_t {
    Home = 0,  // sparse, calm, consonant
    Rise,      // density and brightness grow
    Tension,   // high density, tenser mode, morph pushed toward B
    Peak,      // maximum density/brightness, full morph sweep, max mutation
    Fall,     // decay back toward HOME
};

// A musical scale: a root semitone (0-11, C=0) plus ascending semitone offsets
// from the root (always starting at 0). The sequencer worker owns its own
// quantization; this struct only expresses "which scale/mode this state wants".
struct Scale {
    int rootSemitone = 0;              // 0-11
    std::vector<int> intervals;        // semitone offsets, ascending, intervals[0] == 0
    std::string name;                  // e.g. "pentatonic", "dorian"

    bool operator==(const Scale& other) const {
        return rootSemitone == other.rootSemitone && intervals == other.intervals &&
               name == other.name;
    }
    bool operator!=(const Scale& other) const { return !(*this == other); }

    static Scale major(int rootSemitone);
    static Scale minor(int rootSemitone);
    static Scale pentatonic(int rootSemitone);  // major pentatonic
    static Scale dorian(int rootSemitone);
    static Scale phrygian(int rootSemitone);
    static Scale chromatic(int rootSemitone);

    // Simple brightness metric: number of distinct pitch classes per octave.
    int pitchClassCount() const { return static_cast<int>(intervals.size()); }
};

// Parameter bundle selected by the current attractor phase. Continuous fields
// are ramped smoothly across the state's bars (no jumps mid-state or at
// boundaries): at progress p through state S, value = lerp(anchor(prev(S)),
// anchor(S), smoothstep(p)).
struct AttractorState {
    AttractorPhase phase = AttractorPhase::Home;
    Scale scale;                       // scale/mode this state wants
    float rhythmDensity01 = 0.15F;     // 0 = sparse, 1 = max density
    float morphMin = 0.0F;             // morph region lower bound (0 = patch A)
    float morphMax = 0.25F;            // morph region upper bound (1 = patch B)
    float mutationIntensity01 = 0.1F;  // how much the sequencer may mutate notes
    float brightness01 = 0.25F;        // target tonal brightness (0 dark .. 1 bright)
    float phaseProgress01 = 0.0F;      // 0..1 position within the current state's bars
};

// Per-state durations in bars plus the RNG seed. Durations must be >= 0;
// a zero-duration phase is skipped instantly. If every duration is zero the
// machine holds in HOME.
struct AttractorConfig {
    double bpm = 120.0;                // tempo used to convert seconds -> bars
    double barsHome = 4.0;
    double barsRise = 4.0;
    double barsTension = 4.0;
    double barsPeak = 2.0;
    double barsFall = 4.0;
    std::uint64_t seed = 0x1234ABCDEULL;
};

class AttractorSequencer {
  public:
    explicit AttractorSequencer(AttractorConfig config = {});
    ~AttractorSequencer();

    AttractorSequencer(const AttractorSequencer&) = delete;
    AttractorSequencer& operator=(const AttractorSequencer&) = delete;

    // Return to HOME with zero elapsed bars; the scale draw sequence restarts
    // (same seed => same scales as a fresh instance).
    void reset();

    void set_config(const AttractorConfig& config);
    const AttractorConfig& config() const { return config_; }

    // Advance by seconds of wall/audio time (converted to bars via config.bpm)
    // and return the resulting state. May cross several phase boundaries in
    // one call. Non-positive input is ignored.
    AttractorState advance(double blockSeconds);
    // Advance directly by a number of bars. May cross several boundaries.
    AttractorState advance_bars(double bars);

    // Current state without advancing.
    AttractorState current() const;

    AttractorPhase phase() const { return phase_; }
    // 0..1 position within the current phase's bars.
    double phase_progress() const { return progressBars_ / phase_bars(phase_); }
    // Bars elapsed since reset().
    double bars_elapsed() const { return barsElapsed_; }

  private:
    static constexpr int kPhaseCount = 5;

    AttractorPhase next_phase(AttractorPhase p) const;
    static AttractorPhase prev_phase(AttractorPhase p);
    double phase_bars(AttractorPhase p) const;
    // Anchor parameter bundle for a phase (the values reached at its end).
    AttractorState anchor(AttractorPhase p) const;
    // Draw this phase's scale/mode from the seeded RNG.
    Scale draw_scale(AttractorPhase p);
    // Build the interpolated state for the current phase/progress.
    AttractorState build_state() const;
    std::uint64_t rng_next();

    AttractorConfig config_;
    AttractorPhase phase_ = AttractorPhase::Home;
    double progressBars_ = 0.0;   // bars elapsed within the current phase
    double barsElapsed_ = 0.0;    // bars elapsed since reset()
    std::uint64_t rngState_ = 0;  // splitmix64 state
    Scale currentScale_ = Scale::pentatonic(0);
};

}  // namespace dve::audio
