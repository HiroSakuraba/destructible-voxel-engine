// Phase 3 (SYN-013): patch mutation and breeding for the DVE advanced synthesizer.
//
// Gene groups split a SynthPreset into musical regions so mutation and
// breeding can operate on one region at a time. All operations are seeded
// with std::mt19937_64: the same (preset, intensity, seed, locks) always
// produces the same result.
//
// Parameter semantics follow the Phase 1 morph precedent (see
// morph_synth_presets in src/audio/synthesizer.cpp):
//   - oscillator tuning snaps to musical intervals (semitone steps,
//     occasionally perfect fifths or octaves) — never microtonal drift;
//   - envelope times move multiplicatively (x/div, never negative);
//   - filter cutoff moves in the log domain (ratio-based);
//   - resonance, gains and mix levels move linearly within their valid ranges;
//   - topology/mode/source changes are rare discrete jumps (probability
//     scaled by intensity);
//   - everything is clamped; no NaN or out-of-range value is ever produced.
#pragma once

#include <cstdint>

#include "dve/audio/synthesizer.hpp"

namespace dve::audio {

// Gene groups for patch mutation and breeding. The Sequencer group mutates
// the sequencer's authored config (step values, lane lengths, directions).
enum class GeneGroup : std::uint8_t {
    Oscillators,    // waveform, tuning, gain, shape
    SpectralShape,  // wavetable position, sampler and modal-resonator parameters
    Filters,        // cutoff, resonance, topology/mode, drive
    Envelopes,      // attack/decay/sustain/release (+ delay/hold)
    Modulation,     // slot amounts/sources, LFO rate/depth, macros
    Stereo,         // divergence, pan, width
    Sequencer,      // sequencer authored config (step values, lengths, directions)
    Effects,        // FX mix parameters
};

// Bit position of a gene group inside the lockedGroups / groupsFromB masks.
[[nodiscard]] constexpr std::uint8_t gene_group_bit(GeneGroup group) noexcept {
    return static_cast<std::uint8_t>(1U << static_cast<unsigned>(group));
}

// Mask with every gene group selected.
inline constexpr std::uint8_t kAllGeneGroups = 0xFFU;

// Returns a mutated copy of p. intensity in [0, 1]: 0 returns the patch
// unchanged; 1 applies wide but still musical changes. lockedGroups is a
// bitmask over GeneGroup (see gene_group_bit); locked groups are copied
// verbatim. seed makes the mutation exactly reproducible.
[[nodiscard]] SynthPreset mutate_preset(const SynthPreset& p, float intensity,
                                        std::uint64_t seed, std::uint8_t lockedGroups);

// Breeds a child from two parents: gene groups in groupsFromB come from
// parent b, the rest from parent a. A small post-breed mutation (fixed
// low intensity, derived from seed) adds organic variation. seed makes
// the breeding exactly reproducible.
[[nodiscard]] SynthPreset breed_presets(const SynthPreset& a, const SynthPreset& b,
                                       std::uint8_t groupsFromB, std::uint64_t seed);

}  // namespace dve::audio
