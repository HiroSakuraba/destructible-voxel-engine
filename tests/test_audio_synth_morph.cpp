// Tests for semantic patch morphing (A/B states).
#include <cmath>
#include <cstdio>
#include <vector>

#include "dve/audio/synthesizer.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    // Test 1: morph_synth_presets interpolates.
    {
        SynthPreset a = SynthPreset::make_default();
        SynthPreset b = SynthPreset::make_default();
        a.filter.cutoffHertz = 200.0F;
        b.filter.cutoffHertz = 8000.0F;
        a.oscillators[0].gain = 0.1F;
        b.oscillators[0].gain = 0.9F;

        auto m0 = morph_synth_presets(a, b, 0.0F);
        auto m1 = morph_synth_presets(a, b, 1.0F);
        auto m05 = morph_synth_presets(a, b, 0.5F);

        // At 0, should equal A; at 1, should equal B.
        CHECK(std::abs(m0.filter.cutoffHertz - 200.0F) < 1.0F);
        CHECK(std::abs(m1.filter.cutoffHertz - 8000.0F) < 1.0F);
        // At 0.5, cutoff should be geometric mean (log domain): sqrt(200*8000) = 1264.
        const float expected = std::sqrt(200.0F * 8000.0F);
        std::printf("morph 0.5 cutoff: %.1f (expected %.1f)\n", m05.filter.cutoffHertz, expected);
        CHECK(std::abs(m05.filter.cutoffHertz - expected) < 10.0F);
        // Gain should be linear: 0.5.
        CHECK(std::abs(m05.oscillators[0].gain - 0.5F) < 0.01F);
    }

    // Test 2: envelope times morph in log domain.
    {
        SynthPreset a = SynthPreset::make_default();
        SynthPreset b = SynthPreset::make_default();
        a.ampEnvelope.attackSeconds = 0.01F;
        b.ampEnvelope.attackSeconds = 1.0F;
        auto m05 = morph_synth_presets(a, b, 0.5F);
        // Geometric mean: sqrt(0.01*1.0) = 0.1
        std::printf("morph 0.5 attack: %.4f (expected 0.1)\n", m05.ampEnvelope.attackSeconds);
        CHECK(std::abs(m05.ampEnvelope.attackSeconds - 0.1F) < 0.01F);
    }

    // Test 3: Synthesizer A/B morph via API.
    {
        SynthPreset a = SynthPreset::make_default();
        a.name = "Morph A";
        a.oscillators[0].waveform = OscillatorWaveform::Saw;
        a.oscillators[0].gain = 0.8F;
        a.morphEnabled = true;
        a.morphAmount = 0.0F;

        SynthPreset b = SynthPreset::make_default();
        b.name = "Morph B";
        b.oscillators[0].waveform = OscillatorWaveform::Square;
        b.oscillators[0].gain = 0.2F;

        Synthesizer synth(44100.0);
        synth.set_morph_preset_b(b);
        synth.set_preset(a);

        CHECK(synth.has_morph_preset_b());
        // At amount 0, should sound like A (saw, loud).
        synth.note_on(0, 69, 100);
        std::vector<float> bufferA(4410 * 2);
        synth.render(bufferA.data(), 4410);
        double sumA = 0;
        for (float s : bufferA) sumA += std::abs(s);
        synth.note_off(0, 69);

        // Morph to B.
        synth.set_morph_amount(1.0F);
        synth.note_on(0, 69, 100);
        std::vector<float> bufferB(4410 * 2);
        synth.render(bufferB.data(), 4410);
        double sumB = 0;
        for (float s : bufferB) sumB += std::abs(s);
        synth.note_off(0, 69);

        std::printf("morph A level: %.4f, B level: %.4f\n", sumA / bufferA.size(), sumB / bufferB.size());
        // B has lower gain, so should be quieter.
        CHECK(sumB < sumA);
    }

    if (g_failures == 0) std::printf("patch morphing tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
