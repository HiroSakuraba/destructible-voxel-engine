// Regression tests for the realtime preset/wavetable handoff:
//   1. latest-wins preset mailbox (the final set_preset is always the one heard,
//      no matter how many updates were pushed between audio blocks)
//   2. wavetable cooking happens on the caller (UI) thread, never in render()
//   3. radix-2 FFT band-limiting matches a double-precision reference DFT
//   4. cook cost is milliseconds, not seconds
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#include "dve/audio/synthesizer.hpp"
#include "dve/audio/wavetable.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

float settled_rms(Synthesizer& synth) {
    std::vector<float> buf(2 * 480);
    double acc = 0.0;
    int count = 0;
    for (int block = 0; block < 40; ++block) {
        synth.render(buf.data(), 480);
        if (block >= 20) for (float v : buf) { acc += static_cast<double>(v) * v; ++count; }
    }
    return static_cast<float>(std::sqrt(acc / count));
}

void test_latest_preset_wins() {
    for (int pushes : {1, 2, 8, 9, 20, 200}) {
        // Final preset silent: must be silent.
        {
            Synthesizer synth(48000);
            SynthPreset p = SynthPreset::make_default();
            std::vector<float> warm(2 * 480);
            synth.render(warm.data(), 480);
            for (int i = 0; i < pushes - 1; ++i) { p.masterGain = 1.0F; p.masterPan = 0.001F * i; synth.set_preset(p); }
            p.masterGain = 0.0F;
            synth.set_preset(p);
            synth.note_on(60, 1.0F);
            const float r = settled_rms(synth);
            if (!(r < 0.002F)) std::printf("  pushes=%d silent-final rms=%f\n", pushes, r);
            CHECK(r < 0.002F);
        }
        // Final preset loud after silent ones: must be audible.
        {
            Synthesizer synth(48000);
            SynthPreset p = SynthPreset::make_default();
            std::vector<float> warm(2 * 480);
            synth.render(warm.data(), 480);
            for (int i = 0; i < pushes - 1; ++i) { p.masterGain = 0.0F; p.masterPan = 0.001F * i; synth.set_preset(p); }
            p.masterGain = 1.0F;
            synth.set_preset(p);
            synth.note_on(60, 1.0F);
            const float r = settled_rms(synth);
            if (!(r > 0.008F)) std::printf("  pushes=%d loud-final rms=%f\n", pushes, r);
            CHECK(r > 0.008F);
        }
    }
    Synthesizer synth(48000);
    SynthPreset p = SynthPreset::make_default();
    for (int i = 0; i < 20; ++i) { p.masterPan = 0.01F * i; synth.set_preset(p); }
    CHECK(synth.coalesced_preset_count() >= 19U);
}

SynthPreset wavetable_preset() {
    SynthPreset p = SynthPreset::make_default();
    p.wavetable = WavetableBank::make_default();
    p.wavetable.enabled = true;
    p.oscillators[0].waveform = OscillatorWaveform::Wavetable;
    return p;
}

void test_no_cook_in_render() {
    Synthesizer synth(48000);
    SynthPreset p = wavetable_preset();
    synth.set_preset(p);
    std::vector<float> buf(2 * 256);
    synth.note_on(60, 1.0F);
    synth.render(buf.data(), 256);
    for (int edit = 0; edit < 5; ++edit) {
        p.wavetable.samples[10 + edit] += 0.05F;
        const auto before = synth.wavetable_cook_count();
        synth.set_preset(p);                        // cook happens here (UI thread)
        const auto afterSet = synth.wavetable_cook_count();
        CHECK(afterSet == before + 1U);
        const auto t0 = std::chrono::steady_clock::now();
        synth.render(buf.data(), 256);              // adopts the pre-cooked table
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(synth.wavetable_cook_count() == afterSet);
        // 256 frames at 48 kHz is a 5.33 ms budget; the old in-render cook took >1 s.
        if (!(ms < 50.0)) std::printf("  render after wavetable edit took %.2f ms\n", ms);
        CHECK(ms < 50.0);
    }
    for (float v : buf) CHECK(std::isfinite(v));
}

void test_fft_matches_reference_dft() {
    const std::size_t n = kHQWavetableSamples;
    std::vector<std::vector<float>> frames(3, std::vector<float>(n));
    unsigned seed = 12345U;
    for (auto& frame : frames)
        for (float& s : frame) { seed = seed * 1664525U + 1013904223U; s = static_cast<float>(seed >> 8) / 8388608.0F - 1.0F; }
    const CookedWavetable table = cook_wavetable("ref", frames);
    const double twoPi = 6.283185307179586;
    double worst = 0.0;
    for (std::size_t f = 0; f < frames.size(); ++f) {
        std::vector<std::complex<double>> spec(n);
        for (std::size_t k = 0; k < n; ++k) {
            std::complex<double> sum{};
            for (std::size_t t = 0; t < n; ++t) sum += static_cast<double>(frames[f][t]) * std::polar(1.0, -twoPi * double(k * t) / double(n));
            spec[k] = sum;
        }
        for (std::size_t mip = 0; mip < kHQWavetableMips; ++mip) {
            const float* cooked = table.frame_data(mip, f);
            if (mip == 0) {
                for (std::size_t t = 0; t < n; ++t) worst = std::max(worst, std::abs(double(cooked[t]) - frames[f][t]));
                continue;
            }
            const std::size_t maxH = n / 2 / (1U << mip);
            for (std::size_t t = 0; t < n; t += 7) {
                std::complex<double> sum{};
                for (std::size_t k = 0; k < n; ++k) {
                    if (k > maxH && k < n - maxH) continue;
                    sum += spec[k] * std::polar(1.0, twoPi * double(k * t) / double(n));
                }
                worst = std::max(worst, std::abs(double(cooked[t]) - sum.real() / double(n)));
            }
        }
    }
    std::printf("  FFT vs reference DFT worst abs error: %.3g\n", worst);
    CHECK(worst < 1e-4);
}

void test_cook_is_fast() {
    std::vector<std::vector<float>> frames(kHQWavetableFrames, std::vector<float>(kHQWavetableSamples));
    for (std::size_t f = 0; f < frames.size(); ++f)
        for (std::size_t i = 0; i < kHQWavetableSamples; ++i) frames[f][i] = std::sin(0.01F * float(i * (f + 1)));
    const auto t0 = std::chrono::steady_clock::now();
    const CookedWavetable table = cook_wavetable("speed", frames);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  cook_wavetable(64 frames x 512 x %zu mips): %.2f ms\n", kHQWavetableMips, ms);
    CHECK(table.valid());
    CHECK(ms < 250.0);  // was ~1270 ms with the O(n^2) DFT; generous for sanitizer/debug CI
}

} // namespace

int main() {
    test_latest_preset_wins();
    test_no_cook_in_render();
    test_fft_matches_reference_dft();
    test_cook_is_fast();
    if (g_failures == 0) std::printf("dve_audio_realtime_handoff_tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
