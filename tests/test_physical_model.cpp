#include "dve/audio/synthesizer.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dve::audio;
namespace fs = std::filesystem;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

SynthPreset physical_test_preset(float size, float tension, float pickup) {
    SynthPreset preset = SynthPreset::make_default();
    preset.name = "Physical tuning invariant";
    for (auto& osc : preset.oscillators) osc.enabled = false;
    auto& osc = preset.oscillators[0];
    osc.enabled = true;
    osc.waveform = OscillatorWaveform::PhysicalModel;
    osc.gain = 0.8F;
    osc.physicalModel = PhysicalModelType::WaveguideString;
    osc.physicalMaterial = PhysicalMaterial::String;
    osc.physicalExcitation = PhysicalExcitation::Pluck;
    osc.physicalSizeMeters = size;
    osc.physicalTension = tension;
    osc.physicalStiffness = 0.08F;
    osc.physicalDamping = 0.08F;
    osc.physicalBrightness = 0.72F;
    osc.physicalExcitationPosition = 0.24F;
    osc.physicalHardness = 0.5F;
    osc.physicalPickupPosition = pickup;
    osc.physicalBodyAmount = 0.0F;
    osc.physicalContinuousAmount = 0.0F;
    preset.ampEnvelope.attackSeconds = 0.001F;
    preset.ampEnvelope.decaySeconds = 2.0F;
    preset.ampEnvelope.sustainLevel = 1.0F;
    preset.ampEnvelope.releaseSeconds = 0.2F;
    preset.filter.enabled = false;
    preset.distortion.enabled = false;
    preset.eq.enabled = false;
    preset.chorus.enabled = false;
    preset.phaser.enabled = false;
    preset.delay.enabled = false;
    preset.reverb.enabled = false;
    preset.compressor.enabled = false;
    preset.limiter.enabled = false;
    preset.masterGain = 0.75F;
    preset.tuning.analogDriftCents = 0.0F;
    return preset;
}

std::vector<float> render(const SynthPreset& preset, std::size_t frames = 24000U) {
    Synthesizer synth(48000);
    synth.set_preset(preset);
    require(synth.note_on(60, 0.85F), "physical note-on failed");
    std::vector<float> audio(frames * 2U);
    synth.render(audio);
    require(std::all_of(audio.begin(), audio.end(), [](float value) { return std::isfinite(value); }),
            "physical model produced non-finite output");
    return audio;
}

double peak(std::span<const float> audio) {
    double result = 0.0;
    for (float value : audio) result = std::max(result, std::abs(static_cast<double>(value)));
    return result;
}

double estimate_pitch(std::span<const float> stereo, double expectedHz) {
    constexpr double sampleRate = 48000.0;
    const std::size_t first = 2400U;
    const std::size_t count = std::min<std::size_t>(12000U, stereo.size() / 2U - first);
    std::vector<double> x(count);
    double mean = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        x[i] = 0.5 * (stereo[2U * (first + i)] + stereo[2U * (first + i) + 1U]);
        mean += x[i];
    }
    mean /= static_cast<double>(x.size());
    for (double& value : x) value -= mean;
    const std::size_t minLag = static_cast<std::size_t>(sampleRate / (expectedHz * 1.08));
    const std::size_t maxLag = static_cast<std::size_t>(sampleRate / (expectedHz * 0.92));
    double best = -1.0;
    std::size_t bestLag = 0;
    for (std::size_t lag = minLag; lag <= maxLag; ++lag) {
        long double cross = 0.0, aa = 0.0, bb = 0.0;
        for (std::size_t i = 0; i + lag < x.size(); ++i) {
            cross += x[i] * x[i + lag];
            aa += x[i] * x[i];
            bb += x[i + lag] * x[i + lag];
        }
        const double denominator = std::sqrt(static_cast<double>(aa * bb));
        const double correlation = denominator > 1.0e-20 ? static_cast<double>(cross) / denominator : 0.0;
        if (correlation > best) { best = correlation; bestLag = lag; }
    }
    require(bestLag != 0 && best > 0.45, "physical tuning signal was not periodic enough to audit");
    return sampleRate / static_cast<double>(bestLag);
}

double cents_between(double a, double b) { return 1200.0 * std::log2(a / b); }

void test_pitch_invariants() {
    constexpr double expected = 261.6255653005986;
    const double base = estimate_pitch(render(physical_test_preset(0.65F, 0.72F, 0.25F)), expected);
    const double otherPickup = estimate_pitch(render(physical_test_preset(0.65F, 0.72F, 0.82F)), expected);
    const double extremeGeometry = estimate_pitch(render(physical_test_preset(2.8F, 0.12F, 0.25F)), expected);
    require(std::abs(cents_between(base, expected)) < 35.0, "physical model root pitch is not MIDI-centered");
    require(std::abs(cents_between(otherPickup, base)) < 20.0, "pickup position retuned the waveguide");
    require(std::abs(cents_between(extremeGeometry, base)) < 20.0, "size/tension retuned the waveguide");
}

void test_validation_and_roundtrip() {
    SynthPreset preset = physical_test_preset(0.65F, 0.7F, 0.4F);
    preset.oscillators[0].physicalMaterial = PhysicalMaterial::Brass;
    std::string error;
    require(preset.validate(&error), error);
    const std::string serialized = preset.serialize();
    require(serialized.starts_with("DVE_SYNTH_PRESET=5\n"), "physical fields did not bump the preset format");
    auto parsed = SynthPreset::parse(serialized, &error);
    require(parsed.has_value(), error);
    require(parsed->oscillators[0].physicalMaterial == PhysicalMaterial::Brass,
            "brass physical material did not round trip");
    parsed->oscillators[0].physicalSizeMeters = 0.0F;
    require(!parsed->validate(&error), "invalid physical size was accepted");
    parsed = SynthPreset::parse(serialized, &error);
    parsed->oscillators[0].physicalThroatQ = 20.0F;
    require(!parsed->validate(&error), "invalid physical throat Q was accepted");
}

void test_shipped_presets() {
#ifdef DVE_SOURCE_DIR
    const fs::path root = fs::path(DVE_SOURCE_DIR) / "assets/audio/presets/Physical";
#else
    const fs::path root = "assets/audio/presets/Physical";
#endif
    std::vector<fs::path> paths;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".dvesynth") paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    require(paths.size() == 78U, "expected the merged set of 78 physical presets");
    for (const auto& path : paths) {
        std::string error;
        auto preset = SynthPreset::load(path, &error);
        require(preset.has_value(), path.string() + ": " + error);
        const auto audio = render(*preset, 8192U);
        require(peak(audio) > 1.0e-5, path.string() + ": rendered silence");
        require(peak(audio) < 1.01, path.string() + ": exceeded normalized output range");
    }
}
}

int main() {
    try {
        test_pitch_invariants();
        test_validation_and_roundtrip();
        test_shipped_presets();
        std::cout << "dve_physical_model_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_physical_model_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
