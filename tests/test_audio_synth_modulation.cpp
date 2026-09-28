// Tests for Phase 0 modulation quick wins: MPE sources + per-route bias.
#include <cmath>
#include <cstdio>
#include <vector>

#include "dve/audio/synthesizer.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static std::vector<float> render_note(SynthPreset preset, int frames = 4410) {
    Synthesizer synth(44100.0);
    synth.set_preset(preset);
    synth.note_on(0, 69, 100);
    std::vector<float> buffer(static_cast<std::size_t>(frames) * 2);
    synth.render(buffer.data(), frames);
    synth.note_off(0, 69);
    return buffer;
}

static double mean_abs(const std::vector<float>& buf) {
    double sum = 0;
    for (float s : buf) sum += std::abs(s);
    return sum / buf.size();
}

int main() {
    // Test 1: bias shifts the routed value even with amount=0.
    {
        SynthPreset base = SynthPreset::make_default();
        base.name = "Bias Test";
        SynthPreset biased = base;
        biased.modulation[0] = {true, ModulationSource::Velocity,
                                ModulationDestination::VoiceGain,
                                0.0F, 0.5F,  // amount=0, bias=0.5
                                ModulationCurve::Linear, ModulationPolarity::Unipolar, 0.0F};
        const double baseLevel = mean_abs(render_note(base));
        const double biasedLevel = mean_abs(render_note(biased));
        std::printf("bias test: base=%.6f biased=%.6f\n", baseLevel, biasedLevel);
        // Bias of 0.5 on VoiceGain should make it louder.
        CHECK(biasedLevel > baseLevel * 1.2);
    }

    // Test 2: new source enum values exist and are distinct.
    {
        CHECK(static_cast<unsigned>(ModulationSource::Timbre) > static_cast<unsigned>(ModulationSource::Macro4));
        CHECK(static_cast<unsigned>(ModulationSource::NotePitchBend) > static_cast<unsigned>(ModulationSource::Timbre));
        CHECK(static_cast<unsigned>(ModulationSource::ReleaseVelocity) > static_cast<unsigned>(ModulationSource::NotePitchBend));
    }

    // Test 3: bias survives preset serialization round-trip.
    {
        SynthPreset preset = SynthPreset::make_default();
        preset.modulation[0] = {true, ModulationSource::Lfo1,
                                ModulationDestination::FilterCutoff,
                                0.5F, 0.25F,
                                ModulationCurve::Linear, ModulationPolarity::Bipolar, 8.0F};
        const std::string serialized = preset.serialize();
        std::string error;
        auto restored = SynthPreset::parse(serialized, &error);
        CHECK(restored.has_value());
        if (restored) {
            CHECK(std::abs(restored->modulation[0].bias - 0.25F) < 1e-6F);
            CHECK(restored->modulation[0].source == ModulationSource::Lfo1);
        }
    }

    // Test 4: new sources survive serialization round-trip.
    {
        SynthPreset preset = SynthPreset::make_default();
        preset.modulation[0].enabled = true;
        preset.modulation[0].source = ModulationSource::Timbre;
        preset.modulation[0].destination = ModulationDestination::FilterCutoff;
        preset.modulation[0].amount = 0.5F;
        const std::string serialized = preset.serialize();
        std::string error;
        auto restored = SynthPreset::parse(serialized, &error);
        CHECK(restored.has_value());
        if (restored) {
            CHECK(restored->modulation[0].source == ModulationSource::Timbre);
        }
    }

    if (g_failures == 0) std::printf("modulation quick-win tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
