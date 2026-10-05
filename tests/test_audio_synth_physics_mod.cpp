// Tests for physics modulation sources.
#include <cmath>
#include <cstdio>
#include <vector>

#include "dve/audio/synthesizer.hpp"
#include "dve/audio/physics_modulation.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    // Test spring: excited, oscillates, decays.
    {
        SpringModulator s;
        s.excite(5.0F);
        float first = s.step(1.0F/44100.0F);
        CHECK(std::abs(first) > 0.0F); // should move
        // Step for 5 seconds, should decay.
        for (int i = 0; i < 44100 * 5; ++i) s.step(1.0F/44100.0F);
        CHECK(std::abs(s.position) < 0.1F); // decayed
    }

    // Test pendulum: oscillates.
    {
        PendulumModulator p;
        p.excite(3.0F);
        float sum = 0.0F;
        for (int i = 0; i < 4410; ++i) sum += std::abs(p.step(1.0F/44100.0F));
        CHECK(sum > 1.0F); // produced movement
    }

    // Test orbiter: orbits.
    {
        OrbiterModulator o;
        float sum = 0.0F;
        for (int i = 0; i < 4410; ++i) sum += std::abs(o.step(1.0F/44100.0F));
        CHECK(sum > 1.0F);
    }

    // Test Lorenz: chaotic, bounded, non-repeating.
    {
        LorenzModulator l;
        float prev = l.step(0.01F);
        bool changed = false;
        for (int i = 0; i < 100; ++i) {
            float v = l.step(0.01F);
            CHECK(v >= -1.0F && v <= 1.0F); // bounded
            if (std::abs(v - prev) > 0.001F) changed = true;
            prev = v;
        }
        CHECK(changed); // evolving
    }

    // Test bank: note_on excites, step advances.
    {
        PhysicsModulationBank bank;
        bank.note_on(1.0F);
        CHECK(std::abs(bank.spring.velocity) > 0.0F);
        bank.step(1.0F/44100.0F);
        // Spring should have moved.
        CHECK(std::abs(bank.spring.position) > 0.0F);
    }

    // Test in synth: spring modulates filter cutoff.
    {
        SynthPreset preset = SynthPreset::make_default();
        preset.oscillators[0].waveform = OscillatorWaveform::Saw;
        preset.oscillators[0].gain = 0.8F;
        preset.modulation[0] = {true, ModulationSource::Spring,
                                ModulationDestination::FilterCutoff,
                                0.8F, 0.0F,
                                ModulationCurve::Linear, ModulationPolarity::Bipolar, 0.0F};
        Synthesizer synth(44100.0);
        synth.set_preset(preset);
        synth.note_on(0, 69, 100);
        std::vector<float> buffer(4410 * 2);
        synth.render(buffer.data(), 4410);
        double sum = 0;
        for (float s : buffer) sum += std::abs(s);
        CHECK(sum / buffer.size() > 0.001);
        synth.note_off(0, 69);
        std::printf("physics mod test: mean abs %.6f\n", sum / buffer.size());
    }

    if (g_failures == 0) std::printf("physics modulation tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
