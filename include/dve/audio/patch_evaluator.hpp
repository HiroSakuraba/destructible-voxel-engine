// Phase 6 audio-to-synth search (SYN-016, Worker B: offline render evaluator).
//
// Renders a SynthPreset headlessly (no realtime, no wall-clock, fixed RNG
// state via a fresh Synthesizer per call) and scores it against a target
// AudioFeatureVector using the contract API in audio_features.hpp.
//
// Threading: all functions are reentrant; each call builds its own
// Synthesizer, so concurrent evaluations on different threads are safe.
// The search should evaluate populations in parallel: one evaluation costs
// ~6 s on a 2-CPU VM (2026-09-29 measurement, 1 s render @ 48 kHz, under
// heavy build contention), dominated by Synthesizer construction, which
// cooks the HQ wavetable mipmaps with naive O(n^2) DFTs. The DSP render
// itself is ~150 ms/s of audio; feature extraction ~13 ms.

#pragma once

#include "dve/audio/audio_features.hpp"
#include "dve/audio/synthesizer.hpp"

namespace dve::audio {

// What to render for one evaluation. All fields are sanitized (clamped to
// sane ranges) inside the evaluator, so any RenderConfig is safe to pass.
struct RenderConfig {
    double seconds = 2.0;       // total render length; clamped to [0.25, 30]
    double sampleRate = 48000.0;  // clamped to [8000, 192000]
    int midiNote = 69;          // clamped to [0, 127]
    float velocity = 0.8F;      // clamped to [0, 1]
    bool stereo = true;         // false renders stereo then downmixes to mono
};

// Score returned by score_preset() when the preset fails validation.
// Large but finite: it ranks below every valid preset without poisoning
// the search with NaN/inf.
inline constexpr double kInvalidPresetScore = 1.0e9;

// Render the preset headlessly with the given config and extract its
// feature vector via the contract API. Deterministic: the same preset and
// config always produce bit-identical audio (fresh Synthesizer per call,
// note-on at frame 0, note-off at 75% of the buffer to capture release).
// An invalid preset is not rendered; a zero vector is returned instead.
[[nodiscard]] AudioFeatureVector render_preset_features(const SynthPreset& preset,
                                                       const RenderConfig& cfg);

// Score a preset against a target feature vector: the contract
// feature_distance() between the preset's rendered features and the target.
// Lower = closer to the target. Invalid presets return kInvalidPresetScore
// (never NaN/inf, never throws).
[[nodiscard]] double score_preset(const SynthPreset& preset,
                                 const AudioFeatureVector& target,
                                 const FeatureWeights& weights,
                                 const RenderConfig& cfg);

}  // namespace dve::audio
