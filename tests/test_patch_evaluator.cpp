// Phase 6 (SYN-016) Worker B tests: offline render evaluator.
//
// Covers: self-score ~ 0, ranking of different presets, bit-identical
// determinism, invalid-preset handling, and FeatureWeights honoring.
// Links against the real feature extractor (audio_features.cpp, Worker A);
// these tests were developed against a stub with the same contract API and
// must pass against both.

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "dve/audio/audio_features.hpp"
#include "dve/audio/patch_evaluator.hpp"
#include "dve/audio/synthesizer.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void disable_effects(dve::audio::SynthPreset& preset) {
    preset.distortion.enabled = false;
    preset.eq.enabled = false;
    preset.chorus.enabled = false;
    preset.phaser.enabled = false;
    preset.delay.enabled = false;
    preset.reverb.enabled = false;
    preset.compressor.enabled = false;
    preset.limiter.enabled = true;
}

// Dark, soft sub bass: sine osc, slow attack, lowpass-ish dark character.
dve::audio::SynthPreset sub_bass_preset() {
    using namespace dve::audio;
    SynthPreset preset = SynthPreset::make_default();
    preset.name = "Phase6 Test Sub Bass";
    for (auto& oscillator : preset.oscillators) oscillator.enabled = false;
    preset.oscillators[0].enabled = true;
    preset.oscillators[0].gain = 0.5F;
    preset.oscillators[0].waveform = OscillatorWaveform::Sine;
    preset.oscillators[0].semitones = -12.0F;
    preset.ampEnvelope = {0.05F, 0.10F, 0.8F, 0.25F, EnvelopeCurve::Linear};
    preset.filter.enabled = false;
    preset.sequencer.enabled = false;
    preset.attractor.enabled = false;
    preset.masterGain = 0.55F;
    preset.masterPan = 0.0F;
    preset.tuning.analogDriftCents = 0.0F;
    disable_effects(preset);
    return preset;
}

// Bright, snappy lead: saw osc, fast attack, wide stereo divergence.
dve::audio::SynthPreset bright_lead_preset() {
    using namespace dve::audio;
    SynthPreset preset = SynthPreset::make_default();
    preset.name = "Phase6 Test Bright Lead";
    for (auto& oscillator : preset.oscillators) oscillator.enabled = false;
    preset.oscillators[0].enabled = true;
    preset.oscillators[0].gain = 0.4F;
    preset.oscillators[0].waveform = OscillatorWaveform::Saw;
    preset.oscillators[0].stereoDivergence = 0.8F;
    preset.ampEnvelope = {0.001F, 0.005F, 1.0F, 0.03F, EnvelopeCurve::Linear};
    preset.filter.enabled = false;
    preset.sequencer.enabled = false;
    preset.attractor.enabled = false;
    preset.masterGain = 0.55F;
    preset.masterPan = 0.0F;
    preset.tuning.analogDriftCents = 0.0F;
    disable_effects(preset);
    return preset;
}

dve::audio::RenderConfig fast_config() {
    dve::audio::RenderConfig cfg;
    cfg.seconds = 1.0;  // shorter than the default: keeps the suite fast
    cfg.sampleRate = 48000.0;
    cfg.midiNote = 69;
    cfg.velocity = 0.8F;
    cfg.stereo = true;
    return cfg;
}

void test_self_score_near_zero() {
    using namespace dve::audio;
    const SynthPreset bass = sub_bass_preset();
    require(bass.validate(), "test bass preset failed validation");
    const RenderConfig cfg = fast_config();
    const AudioFeatureVector target = render_preset_features(bass, cfg);
    const double score = score_preset(bass, target, FeatureWeights{}, cfg);
    require(score >= 0.0 && score < 1.0e-6,
            "preset scored against its own features was not ~0");
}

void test_ranking_distinguishes_presets() {
    using namespace dve::audio;
    const SynthPreset bass = sub_bass_preset();
    const SynthPreset lead = bright_lead_preset();
    require(lead.validate(), "test lead preset failed validation");
    const RenderConfig cfg = fast_config();
    const AudioFeatureVector bassTarget = render_preset_features(bass, cfg);
    const double bassScore = score_preset(bass, bassTarget, FeatureWeights{}, cfg);
    const double leadScore = score_preset(lead, bassTarget, FeatureWeights{}, cfg);
    require(std::isfinite(bassScore) && std::isfinite(leadScore), "scores not finite");
    require(bassScore < leadScore,
            "matching preset did not score strictly lower than the different preset");
    // And symmetrically against the lead's own features.
    const AudioFeatureVector leadTarget = render_preset_features(lead, cfg);
    require(score_preset(lead, leadTarget, FeatureWeights{}, cfg) <
                score_preset(bass, leadTarget, FeatureWeights{}, cfg),
            "symmetric ranking failed");
}

void test_determinism_bit_identical() {
    using namespace dve::audio;
    const SynthPreset lead = bright_lead_preset();
    const RenderConfig cfg = fast_config();
    const AudioFeatureVector target = render_preset_features(lead, cfg);
    const double first = score_preset(lead, target, FeatureWeights{}, cfg);
    const double second = score_preset(lead, target, FeatureWeights{}, cfg);
    require(first == second, "same preset+config did not give bit-identical scores");
    // Rendered features must be bit-identical too.
    const AudioFeatureVector again = render_preset_features(lead, cfg);
    require(feature_distance(target, again, FeatureWeights{}) == 0.0,
            "repeated render did not produce identical features");
}

void test_invalid_preset_graceful() {
    using namespace dve::audio;
    SynthPreset broken = sub_bass_preset();
    broken.name.clear();  // empty name fails SynthPreset::validate()
    require(!broken.validate(), "broken preset unexpectedly validated");
    const RenderConfig cfg = fast_config();
    const AudioFeatureVector target = render_preset_features(sub_bass_preset(), cfg);
    const double score = score_preset(broken, target, FeatureWeights{}, cfg);
    require(std::isfinite(score), "invalid preset score was NaN/inf");
    require(score >= kInvalidPresetScore, "invalid preset score was not large");
    // render_preset_features on an invalid preset must also stay finite.
    const AudioFeatureVector v = render_preset_features(broken, cfg);
    require(std::isfinite(v.rms) && std::isfinite(v.spectralCentroidHz),
            "invalid preset render produced non-finite features");
}

void test_zero_weights_give_zero() {
    using namespace dve::audio;
    const SynthPreset bass = sub_bass_preset();
    const SynthPreset lead = bright_lead_preset();
    const RenderConfig cfg = fast_config();
    const AudioFeatureVector target = render_preset_features(bass, cfg);
    FeatureWeights zero{};
    zero.rms = 0.0; zero.peak = 0.0; zero.spectralCentroidHz = 0.0;
    zero.spectralRolloffHz = 0.0; zero.spectralFlatness = 0.0;
    zero.zeroCrossingRate = 0.0; zero.estimatedPitchHz = 0.0;
    zero.pitchConfidence = 0.0; zero.harmonicEnergyRatio = 0.0;
    zero.attackSeconds = 0.0; zero.decaySeconds = 0.0; zero.sustainLevel = 0.0;
    zero.releaseSeconds = 0.0; zero.stereoWidth = 0.0; zero.melBands = 0.0;
    require(score_preset(bass, target, zero, cfg) == 0.0,
            "zero weights did not give exactly 0 for the matching preset");
    require(score_preset(lead, target, zero, cfg) == 0.0,
            "zero weights did not give exactly 0 for a different preset");
}

void test_config_sanitization() {
    using namespace dve::audio;
    const SynthPreset bass = sub_bass_preset();
    RenderConfig wild;
    wild.seconds = -5.0;
    wild.sampleRate = 1.0e9;
    wild.midiNote = 300;
    wild.velocity = 50.0F;
    wild.stereo = false;  // exercises the mono downmix path
    const AudioFeatureVector v = render_preset_features(bass, wild);
    require(std::isfinite(v.rms) && std::isfinite(v.spectralCentroidHz) &&
                std::isfinite(v.stereoWidth),
            "wild config produced non-finite features");
    require(v.stereoWidth == 0.0, "mono render should report zero stereo width");
}

void test_measure_evaluation_cost() {
    using namespace dve::audio;
    const SynthPreset bass = sub_bass_preset();
    const RenderConfig cfg = fast_config();
    const AudioFeatureVector target = render_preset_features(bass, cfg);
    const auto start = std::chrono::steady_clock::now();
    constexpr int kIters = 5;
    for (int i = 0; i < kIters; ++i) {
        const double s = score_preset(bass, target, FeatureWeights{}, cfg);
        require(std::isfinite(s), "timed evaluation produced non-finite score");
    }
    const auto end = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(end - start).count() / kIters;
    std::cout << "  [timing] mean score_preset (1s render @48kHz): " << ms << " ms\n";
}

}  // namespace

int main() {
    try {
        test_self_score_near_zero();
        test_ranking_distinguishes_presets();
        test_determinism_bit_identical();
        test_invalid_preset_graceful();
        test_zero_weights_give_zero();
        test_config_sanitization();
        test_measure_evaluation_cost();
        std::cout << "dve_patch_evaluator_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_patch_evaluator_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
