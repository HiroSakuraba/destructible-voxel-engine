// Phase 3: generative conductor — maps AttractorSequencer state onto the
// live Synthesizer once per render block.
//
// The conductor is purely additive: when disabled (the default) process() is
// a no-op and existing presets render bit-identically. When enabled it owns
// four mappings, all derived from the current AttractorState:
//
//   scale            -> sequencer quantizer scale + root note (octave kept)
//   rhythmDensity01  -> probability-lane trigger-gate multiplier
//   mutationIntensity01 -> sequencer per-cycle mutation amount (all lanes)
//   morphMin/Max     -> smooth seeded random walk driving request_morph_amount()
//   brightness01     -> filter cutoff multiplier around the base cutoff
//
// The morph walk only runs when the preset has morphing enabled; otherwise
// the walk state is parked so enabling morph later starts mid-range.
#pragma once

#include <cstdint>

#include "dve/audio/attractor.hpp"
#include "dve/audio/sequencer.hpp"

namespace dve::audio {

class Synthesizer;

class GenerativeConductor {
  public:
    GenerativeConductor();
    ~GenerativeConductor();

    GenerativeConductor(const GenerativeConductor&) = delete;
    GenerativeConductor& operator=(const GenerativeConductor&) = delete;

    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    // Installs the preset-owned attractor settings. Resets the phase machine
    // (back to HOME, morph walk re-seeded) only when enabled-flag or config
    // actually changed, so per-block set_preset() calls (e.g. from the morph
    // walk itself) never restart the arc.
    void configure(bool enabled, const AttractorConfig& config);

    // Returns to HOME with the walk parked at mid-range.
    void reset();

    // Advances the attractor by blockSeconds of audio time and applies the
    // state mapping to synth. No-op when disabled. Non-positive blockSeconds
    // still applies the current state's mapping without advancing time.
    void process(Synthesizer& synth, double blockSeconds);

    // --- introspection (editor UI, tests) ---
    [[nodiscard]] AttractorPhase phase() const noexcept { return attractor_.phase(); }
    [[nodiscard]] double phase_progress() const noexcept { return attractor_.phase_progress(); }
    [[nodiscard]] double bars_elapsed() const noexcept { return attractor_.bars_elapsed(); }
    [[nodiscard]] float morph_walk_value() const noexcept { return morphWalk_; }
    [[nodiscard]] const AttractorConfig& config() const noexcept { return attractor_.config(); }

  private:
    // Maps an attractor scale onto the closest sequencer quantizer.
    [[nodiscard]] static SequencerScale map_scale(const Scale& scale) noexcept;
    // One smooth random-walk step inside [lo, hi] with reflection at bounds.
    [[nodiscard]] static float walk_step(std::uint64_t& rngState, float current, float lo,
                                         float hi) noexcept;
    [[nodiscard]] static std::uint64_t rng_next(std::uint64_t& state) noexcept;
    [[nodiscard]] static float rng_unit(std::uint64_t& state) noexcept;

    bool enabled_{false};
    AttractorSequencer attractor_;
    bool configuredEnabled_{false};
    AttractorConfig configuredConfig_{};
    bool hasConfig_{false};
    std::uint64_t walkRng_{0x9E3779B97F4A7C15ULL};
    float morphWalk_{0.5F};
};

}  // namespace dve::audio
