// Phase 6 audio-to-synth search (SYN-016, Worker B: offline render evaluator).
//
// Renders candidate SynthPresets headlessly and scores them against a target
// AudioFeatureVector. Each evaluation constructs a fresh Synthesizer, which
// resets every internal RNG to its fixed default seed, so the render is
// bit-identical for the same preset + config (no wall-clock is read anywhere
// on the render path).

#include "dve/audio/patch_evaluator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace dve::audio {
namespace {

// Clamp the config to sane ranges so every RenderConfig is safe.
RenderConfig sanitize(RenderConfig cfg) {
    cfg.seconds = std::clamp(cfg.seconds, 0.25, 30.0);
    cfg.sampleRate = std::clamp(cfg.sampleRate, 8000.0, 192000.0);
    cfg.midiNote = std::clamp(cfg.midiNote, 0, 127);
    cfg.velocity = std::clamp(cfg.velocity, 0.0F, 1.0F);
    return cfg;
}

}  // namespace

AudioFeatureVector render_preset_features(const SynthPreset& preset, const RenderConfig& rawCfg) {
    const RenderConfig cfg = sanitize(rawCfg);
    AudioFeatureVector empty{};
    std::string error;
    if (!preset.validate(&error)) return empty;  // invalid: no render, zero vector

    const std::uint32_t sampleRate = static_cast<std::uint32_t>(cfg.sampleRate);
    const std::size_t totalFrames =
        static_cast<std::size_t>(cfg.seconds * cfg.sampleRate);
    const std::size_t noteOffFrame = totalFrames * 3U / 4U;  // capture release

    // Fresh synthesizer per call: resets all voice/conductor/grain RNG state
    // to fixed seeds, so this render is bit-identical for identical inputs.
    Synthesizer synth(sampleRate);
    synth.set_preset(preset);
    synth.note_on(static_cast<std::uint8_t>(cfg.midiNote), cfg.velocity);

    std::vector<float> interleaved(totalFrames * 2U, 0.0F);
    synth.render(interleaved.data(), noteOffFrame);
    synth.note_off(static_cast<std::uint8_t>(cfg.midiNote));
    synth.render(interleaved.data() + noteOffFrame * 2U, totalFrames - noteOffFrame);

    if (cfg.stereo) {
        return extract_audio_features(interleaved.data(), totalFrames, 2, cfg.sampleRate);
    }
    // Downmix to mono for the feature extractor.
    std::vector<float> mono(totalFrames, 0.0F);
    for (std::size_t i = 0; i < totalFrames; ++i)
        mono[i] = 0.5F * (interleaved[i * 2U] + interleaved[i * 2U + 1U]);
    return extract_audio_features(mono.data(), totalFrames, 1, cfg.sampleRate);
}

double score_preset(const SynthPreset& preset, const AudioFeatureVector& target,
                    const FeatureWeights& weights, const RenderConfig& cfg) {
    std::string error;
    if (!preset.validate(&error)) return kInvalidPresetScore;
    const double d = feature_distance(render_preset_features(preset, cfg), target, weights);
    // The contract guarantees a finite distance for finite input; guard
    // anyway so a search loop can never see NaN/inf from this entry point.
    if (!std::isfinite(d)) return kInvalidPresetScore;
    return d;
}

}  // namespace dve::audio
