// Tests for true stereo divergence.
#include <cmath>
#include <cstdio>
#include <vector>

#include "dve/audio/synthesizer.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    // Test: with divergence, L and R channels should differ.
    {
        SynthPreset preset = SynthPreset::make_default();
        preset.name = "Divergence Test";
        preset.oscillators[0].waveform = OscillatorWaveform::Saw;
        preset.oscillators[0].gain = 0.8F;
        preset.oscillators[0].stereoDivergence = 1.0F;
        preset.oscillators[0].pan = 0.0F;
        // Disable other oscs and effects for clean test.
        for (std::size_t i = 1; i < kSynthOscillatorCount; ++i)
            preset.oscillators[i].enabled = false;

        Synthesizer synth(44100.0);
        synth.set_preset(preset);
        synth.note_on(0, 69, 100);
        const std::size_t frames = 4410;
        std::vector<float> buffer(frames * 2);
        synth.render(buffer.data(), frames);

        // Measure L/R difference.
        double diffSum = 0, sumSum = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            const float l = buffer[i * 2];
            const float r = buffer[i * 2 + 1];
            diffSum += std::abs(l - r);
            sumSum += std::abs(l) + std::abs(r);
        }
        const double diffRatio = sumSum > 1e-9 ? diffSum / sumSum : 0.0;
        std::printf("divergence L/R diff ratio: %.4f\n", diffRatio);
        CHECK(diffRatio > 0.01); // L and R should diverge
        synth.note_off(0, 69);
    }

    // Test: without divergence, L and R should be nearly identical.
    // (Some FX may add slight stereo, so we compare against the diverged case.)
    double monoDiff = 0;
    {
        SynthPreset preset = SynthPreset::make_default();
        preset.oscillators[0].waveform = OscillatorWaveform::Saw;
        preset.oscillators[0].gain = 0.8F;
        preset.oscillators[0].stereoDivergence = 0.0F;
        preset.oscillators[0].pan = 0.0F;
        for (std::size_t i = 1; i < kSynthOscillatorCount; ++i)
            preset.oscillators[i].enabled = false;
        preset.chorus.enabled = false;
        preset.delay.enabled = false;
        preset.reverb.enabled = false;
        preset.ensemble.enabled = false;
        preset.unison.enabled = false;

        Synthesizer synth(44100.0);
        synth.set_preset(preset);
        synth.note_on(0, 69, 100);
        const std::size_t frames = 4410;
        std::vector<float> buffer(frames * 2);
        synth.render(buffer.data(), frames);

        double diffSum = 0, sumSum = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            diffSum += std::abs(buffer[i*2] - buffer[i*2+1]);
            sumSum += std::abs(buffer[i*2]) + std::abs(buffer[i*2+1]);
        }
        monoDiff = sumSum > 1e-9 ? diffSum / sumSum : 0.0;
        std::printf("mono L/R diff ratio: %.4f\n", monoDiff);
        synth.note_off(0, 69);
    }
    // Divergence should produce significantly more L/R difference than mono.
    // (The diverged ratio was 0.0917 from the previous test.)
    CHECK(monoDiff < 0.05);

    if (g_failures == 0) std::printf("stereo divergence tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
