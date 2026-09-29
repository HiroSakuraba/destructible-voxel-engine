// Phase 6 audio-to-synth search (SYN-016, plan section 12 "Generator 6").
//
// CONTRACT HEADER owned by the Phase 6 coordinator. Workers A (feature
// extractor), B (offline render evaluator), C (evolutionary search) and D
// (candidate browser) all build against this file; do not rename, remove,
// or reorder fields without coordinating with all of them. Appending new
// fields at the end is allowed.
//
// The pipeline: Worker A turns a target recording into an AudioFeatureVector.
// Worker B renders candidate SynthPresets offline and scores them with
// feature_distance() against the target. Worker C evolves a population of
// presets toward the target. Worker D lets the user browse ranked candidates.
//
// Layout contract:
//   - AudioFeatureVector: plain struct of doubles, all finite for finite
//     input (0.0 when the quantity is undefined, e.g. pitch of silence).
//     melBands holds 16 log mean energies of triangular mel bands from
//     0..Nyquist, each normalized to roughly [-1, 1] (0 = average band).
//   - FeatureWeights: per-field weights for feature_distance(); all default
//     to 1.0. A weight of 0.0 excludes the field.
//   - feature_distance() is a weighted root-mean-square over the fields,
//     with melBands contributing as one averaged term. Lower = more similar.
//     It must be deterministic and symmetric.
//   - extract_audio_features() takes interleaved float samples (numChannels
//     1 or 2), finite input, and never returns NaN/inf.

#pragma once

#include <cstddef>

namespace dve::audio {

// Feature vector describing a recording (or an offline render) for the
// audio-to-synth search. All fields are doubles; undefined quantities are
// 0.0, never NaN/inf for finite input.
struct AudioFeatureVector {
    double rms = 0.0;                 // overall RMS level (linear)
    double peak = 0.0;                // overall peak amplitude (linear)
    double spectralCentroidHz = 0.0;  // brightness, mean over frames
    double spectralRolloffHz = 0.0;   // freq below which 85% of energy sits
    double spectralFlatness = 0.0;    // 0 = tonal, 1 = noise-like
    double zeroCrossingRate = 0.0;    // mean zero crossings per second
    double estimatedPitchHz = 0.0;    // monophonic pitch estimate, 0 = none
    double pitchConfidence = 0.0;     // 0..1 confidence in the pitch estimate
    double harmonicEnergyRatio = 0.0; // 0..1 energy at harmonic partials
    double attackSeconds = 0.0;       // 10%..90% rise time of amplitude env
    double decaySeconds = 0.0;        // time from peak to sustain level
    double sustainLevel = 0.0;        // sustain amplitude relative to peak
    double releaseSeconds = 0.0;      // sustain end to -60dB (or buffer end)
    double stereoWidth = 0.0;         // 0 = mono, 1 = fully wide
    double melBands[16] = {};         // log mel-band energies, ~[-1, 1]
};

// Per-field weights for feature_distance(). Default 1.0; 0.0 excludes.
struct FeatureWeights {
    double rms = 1.0;
    double peak = 1.0;
    double spectralCentroidHz = 1.0;
    double spectralRolloffHz = 1.0;
    double spectralFlatness = 1.0;
    double zeroCrossingRate = 1.0;
    double estimatedPitchHz = 1.0;
    double pitchConfidence = 1.0;
    double harmonicEnergyRatio = 1.0;
    double attackSeconds = 1.0;
    double decaySeconds = 1.0;
    double sustainLevel = 1.0;
    double releaseSeconds = 1.0;
    double stereoWidth = 1.0;
    double melBands = 1.0;  // single weight for the averaged mel term
};

// Weighted RMS distance between two feature vectors. Deterministic,
// symmetric, >= 0. Lower means more similar.
[[nodiscard]] double feature_distance(const AudioFeatureVector& a,
                                      const AudioFeatureVector& b,
                                      const FeatureWeights& weights = FeatureWeights{}) noexcept;

// Analyze interleaved float samples (numChannels = 1 or 2) at sampleRate Hz.
// Returns a fully finite vector (0.0 for undefined quantities).
[[nodiscard]] AudioFeatureVector extract_audio_features(const float* samples,
                                                       std::size_t numFrames,
                                                       int numChannels,
                                                       double sampleRate) noexcept;

}  // namespace dve::audio
