// Tests for Phase 1 high-quality wavetable oscillator.
#include <cmath>
#include <cstdio>
#include <vector>

#include "dve/audio/synthesizer.hpp"
#include "dve/audio/wavetable.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    // Test 1: factory tables cook correctly.
    {
        auto table = make_basic_morph_table();
        CHECK(table.valid());
        CHECK(table.samples.size() == kHQWavetableMips * kHQWavetableFrames * kHQWavetableSamples);
        // Frame 0 should be sine-like, last frame square-like.
        const float* f0 = table.frame_data(0, 0);
        const float* fLast = table.frame_data(0, kHQWavetableFrames - 1);
        CHECK(f0 != nullptr && fLast != nullptr);
        // Sine at phase 0.25 should be ~0.8 (peak).
        const float sinePeak = f0[kHQWavetableSamples / 4];
        CHECK(std::abs(sinePeak - 0.8F) < 0.1F);
        // Square should be near +/-0.8 across the frame.
        CHECK(std::abs(std::abs(fLast[10]) - 0.8F) < 0.1F);
    }

    // Test 2: mip selection.
    {
        // Low freq -> mip 0, high freq -> higher mip.
        CHECK(wavetable_mip_for_frequency(100.0F, 44100.0F) == 0);
        const std::size_t highMip = wavetable_mip_for_frequency(8000.0F, 44100.0F);
        CHECK(highMip > 0 && highMip < kHQWavetableMips);
    }

    // Test 3: sampling produces sound.
    {
        auto table = make_basic_morph_table();
        float sum = 0.0F;
        for (int i = 0; i < 100; ++i) {
            const float phase = static_cast<float>(i) / 100.0F;
            sum += std::abs(sample_wavetable(table, phase, 0.5F, 0));
        }
        CHECK(sum > 1.0F); // should produce audible output
    }

    // Test 4: wavetable oscillator in the synth.
    {
        SynthPreset preset = SynthPreset::make_default();
        preset.name = "Wavetable Test";
        preset.oscillators[0].waveform = OscillatorWaveform::Wavetable;
        preset.oscillators[0].gain = 0.8F;
        preset.wavetable.enabled = true;
        preset.wavetable.frameCount = 4;
        // Fill with simple sine frames.
        for (std::size_t f = 0; f < 4; ++f) {
            for (std::size_t i = 0; i < kWavetableSampleCount; ++i) {
                const float phase = static_cast<float>(i) / static_cast<float>(kWavetableSampleCount);
                preset.wavetable.samples[f * kWavetableSampleCount + i] = std::sin(6.2831853F * phase) * 0.8F;
            }
        }
        Synthesizer synth(44100.0);
        synth.set_preset(preset);
        synth.note_on(0, 69, 100);
        std::vector<float> buffer(4410 * 2);
        synth.render(buffer.data(), 4410);
        double sum = 0;
        for (float s : buffer) sum += std::abs(s);
        const double meanAbs = sum / buffer.size();
        std::printf("wavetable mean abs: %.6f\n", meanAbs);
        CHECK(meanAbs > 0.001); // should produce sound
        synth.note_off(0, 69);
    }

    // Test 5: wavetable position modulation.
    {
        SynthPreset preset = SynthPreset::make_default();
        preset.oscillators[0].waveform = OscillatorWaveform::Wavetable;
        preset.oscillators[0].gain = 0.8F;
        preset.wavetable.enabled = true;
        preset.wavetable.frameCount = 4;
        for (std::size_t f = 0; f < 4; ++f) {
            for (std::size_t i = 0; i < kWavetableSampleCount; ++i) {
                const float phase = static_cast<float>(i) / static_cast<float>(kWavetableSampleCount);
                // Different harmonic content per frame.
                preset.wavetable.samples[f * kWavetableSampleCount + i] =
                    (std::sin(6.2831853F * phase) + 0.5F * std::sin(6.2831853F * (f+1) * phase)) * 0.5F;
            }
        }
        // Modulate position with LFO.
        preset.lfos[0].enabled = true;
        preset.lfos[0].rateHertz = 2.0F;
        preset.modulation[0] = {true, ModulationSource::Lfo1,
                                ModulationDestination::WavetablePosition,
                                1.0F, 0.0F,
                                ModulationCurve::Linear, ModulationPolarity::Bipolar, 0.0F};
        Synthesizer synth(44100.0);
        synth.set_preset(preset);
        // Should validate and render without crashing.
        synth.note_on(0, 69, 100);
        std::vector<float> buffer(4410 * 2);
        synth.render(buffer.data(), 4410);
        double sum = 0;
        for (float s : buffer) sum += std::abs(s);
        CHECK(sum / buffer.size() > 0.001);
        synth.note_off(0, 69);
    }

    if (g_failures == 0) std::printf("wavetable tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
