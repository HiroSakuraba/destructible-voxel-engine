// Attractor sequencer prototype implementation.
#include "dve/audio/attractor.hpp"

#include <algorithm>
#include <cmath>

namespace dve::audio {
namespace {

float smooth01(float x) noexcept {
    x = std::clamp(x, 0.0F, 1.0F);
    return x * x * (3.0F - 2.0F * x);  // smoothstep: zero slope at both ends
}

float lerp01(float a, float b, float t) noexcept { return a + (b - a) * t; }

}  // namespace

// ---- Scale factories -------------------------------------------------------

Scale Scale::major(int rootSemitone) {
    return {rootSemitone, {0, 2, 4, 5, 7, 9, 11}, "major"};
}
Scale Scale::minor(int rootSemitone) {
    return {rootSemitone, {0, 2, 3, 5, 7, 8, 10}, "minor"};
}
Scale Scale::pentatonic(int rootSemitone) {
    return {rootSemitone, {0, 2, 4, 7, 9}, "pentatonic"};
}
Scale Scale::dorian(int rootSemitone) {
    return {rootSemitone, {0, 2, 3, 5, 7, 9, 10}, "dorian"};
}
Scale Scale::phrygian(int rootSemitone) {
    return {rootSemitone, {0, 1, 3, 5, 7, 8, 10}, "phrygian"};
}
Scale Scale::chromatic(int rootSemitone) {
    return {rootSemitone, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, "chromatic"};
}

// ---- AttractorSequencer ----------------------------------------------------

AttractorSequencer::AttractorSequencer(AttractorConfig config) : config_(config) { reset(); }

AttractorSequencer::~AttractorSequencer() = default;

void AttractorSequencer::reset() {
    phase_ = AttractorPhase::Home;
    progressBars_ = 0.0;
    barsElapsed_ = 0.0;
    rngState_ = config_.seed ^ 0x9E3779B97F4A7C15ULL;
    currentScale_ = draw_scale(AttractorPhase::Home);
}

void AttractorSequencer::set_config(const AttractorConfig& config) {
    config_ = config;
    reset();  // timing/scale draws depend on the config, so restart cleanly
}

AttractorPhase AttractorSequencer::next_phase(AttractorPhase p) const {
    return static_cast<AttractorPhase>((static_cast<int>(p) + 1) % kPhaseCount);
}

AttractorPhase AttractorSequencer::prev_phase(AttractorPhase p) {
    return static_cast<AttractorPhase>((static_cast<int>(p) + kPhaseCount - 1) % kPhaseCount);
}

double AttractorSequencer::phase_bars(AttractorPhase p) const {
    switch (p) {
        case AttractorPhase::Home: return config_.barsHome;
        case AttractorPhase::Rise: return config_.barsRise;
        case AttractorPhase::Tension: return config_.barsTension;
        case AttractorPhase::Peak: return config_.barsPeak;
        case AttractorPhase::Fall: return config_.barsFall;
    }
    return 0.0;
}

// Anchor bundle for a phase: the continuous values reached at its end.
// Discrete fields (phase, scale) are filled in by build_state().
AttractorState AttractorSequencer::anchor(AttractorPhase p) const {
    AttractorState s;
    s.phase = p;
    switch (p) {
        case AttractorPhase::Home:
            s.rhythmDensity01 = 0.15F;
            s.morphMin = 0.00F;
            s.morphMax = 0.25F;
            s.mutationIntensity01 = 0.10F;
            s.brightness01 = 0.25F;
            break;
        case AttractorPhase::Rise:
            s.rhythmDensity01 = 0.38F;
            s.morphMin = 0.20F;
            s.morphMax = 0.50F;
            s.mutationIntensity01 = 0.30F;
            s.brightness01 = 0.50F;
            break;
        case AttractorPhase::Tension:
            s.rhythmDensity01 = 0.65F;
            s.morphMin = 0.55F;
            s.morphMax = 0.85F;
            s.mutationIntensity01 = 0.60F;
            s.brightness01 = 0.65F;
            break;
        case AttractorPhase::Peak:
            s.rhythmDensity01 = 0.95F;
            s.morphMin = 0.00F;
            s.morphMax = 1.00F;
            s.mutationIntensity01 = 1.00F;
            s.brightness01 = 1.00F;
            break;
        case AttractorPhase::Fall:
            s.rhythmDensity01 = 0.35F;
            s.morphMin = 0.15F;
            s.morphMax = 0.40F;
            s.mutationIntensity01 = 0.30F;
            s.brightness01 = 0.40F;
            break;
    }
    return s;
}

std::uint64_t AttractorSequencer::rng_next() {
    // splitmix64
    std::uint64_t z = (rngState_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Each phase draws its scale from the seeded RNG when the machine enters it,
// from a candidate pool that fits the phase's character. Because draws happen
// only at deterministic phase boundaries, the same seed always yields the same
// scale sequence regardless of how advance() calls are chunked.
Scale AttractorSequencer::draw_scale(AttractorPhase p) {
    const int root = static_cast<int>(rng_next() % 12ULL);
    switch (p) {
        case AttractorPhase::Home:
            return Scale::pentatonic(root);
        case AttractorPhase::Rise:
            return (rng_next() & 1ULL) ? Scale::major(root) : Scale::pentatonic(root);
        case AttractorPhase::Tension: {
            const std::uint64_t v = rng_next() % 3ULL;
            if (v == 0) return Scale::dorian(root);
            if (v == 1) return Scale::phrygian(root);
            return Scale::minor(root);
        }
        case AttractorPhase::Peak: {
            const std::uint64_t v = rng_next() % 3ULL;
            if (v == 0) return Scale::chromatic(root);
            if (v == 1) return Scale::phrygian(root);
            return Scale::dorian(root);
        }
        case AttractorPhase::Fall: {
            const std::uint64_t v = rng_next() % 3ULL;
            if (v == 0) return Scale::minor(root);
            if (v == 1) return Scale::dorian(root);
            return Scale::pentatonic(root);
        }
    }
    return Scale::pentatonic(root);
}

AttractorState AttractorSequencer::build_state() const {
    const AttractorPhase prev = prev_phase(phase_);
    const AttractorState a = anchor(prev);
    const AttractorState b = anchor(phase_);
    const double dur = phase_bars(phase_);
    const double rawP = (dur > 0.0) ? std::min(1.0, progressBars_ / dur) : 0.0;
    const float t = smooth01(static_cast<float>(rawP));

    AttractorState s;
    s.phase = phase_;
    s.scale = currentScale_;
    s.rhythmDensity01 = lerp01(a.rhythmDensity01, b.rhythmDensity01, t);
    s.morphMin = lerp01(a.morphMin, b.morphMin, t);
    s.morphMax = lerp01(a.morphMax, b.morphMax, t);
    s.mutationIntensity01 = lerp01(a.mutationIntensity01, b.mutationIntensity01, t);
    s.brightness01 = lerp01(a.brightness01, b.brightness01, t);
    s.phaseProgress01 = static_cast<float>(rawP);
    return s;
}

AttractorState AttractorSequencer::current() const { return build_state(); }

AttractorState AttractorSequencer::advance_bars(double bars) {
    if (!(bars > 0.0)) return current();  // ignore non-positive/NaN input

    // Total cycle length; if every duration is zero, hold in HOME.
    double cycle = 0.0;
    for (int i = 0; i < kPhaseCount; ++i)
        cycle += phase_bars(static_cast<AttractorPhase>(i));
    barsElapsed_ += bars;
    if (!(cycle > 0.0)) return current();

    double remaining = bars;
    while (remaining > 0.0) {
        const double dur = phase_bars(phase_);
        if (!(dur > 0.0)) {
            // Zero-duration phase: skip through it instantly.
            phase_ = next_phase(phase_);
            currentScale_ = draw_scale(phase_);
            continue;
        }
        const double left = dur - progressBars_;
        if (remaining < left) {
            progressBars_ += remaining;
            remaining = 0.0;
        } else {
            remaining -= left;
            progressBars_ = 0.0;
            phase_ = next_phase(phase_);
            currentScale_ = draw_scale(phase_);
        }
    }
    // Skip through zero-duration phases we landed exactly on (a zero-duration
    // phase is never an observable resting state).
    for (int guard = 0; guard < kPhaseCount; ++guard) {
        if (phase_bars(phase_) > 0.0) break;
        phase_ = next_phase(phase_);
        currentScale_ = draw_scale(phase_);
    }
    return current();
}

AttractorState AttractorSequencer::advance(double blockSeconds) {
    if (!(blockSeconds > 0.0) || !(config_.bpm > 0.0)) return current();
    const double bars = blockSeconds * config_.bpm / 240.0;  // 4 beats per bar
    return advance_bars(bars);
}

}  // namespace dve::audio
