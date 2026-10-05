// Fix verification tests for the Phase 4 audio critique.
// Covers:
//   1. audio-thread preset race (hammer set_preset/set_morph_amount/preset vs render)
//   2. NaN hardening (validate rejects; engine falls back to finite defaults)
//   3. per-oscillator granular engines (independent clouds, osc-0 determinism)
//   4. .dvesynth granular key round-trip + old-file compatibility
//   6. wavetable cook gating (no re-cook on repeat set_preset / morph walk)
//   7. grain pitch tracking (MIDI 60 vs 72 an octave apart)
//   8. binary patch oscillator waveform/enabled IDs round-trip
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

#include "dve/audio/granular.hpp"
#include "dve/audio/synth_patch.hpp"
#include "dve/audio/synthesizer.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

// Naive DFT peak-bin finder for the pitch test (test sizes only).
std::size_t peak_bin(const std::vector<float>& samples, std::size_t start, std::size_t n,
                     float sampleRate, float minHz, float maxHz) {
    std::size_t best = 0;
    double bestMag = -1.0;
    const std::size_t binLo = static_cast<std::size_t>(minHz * n / sampleRate);
    const std::size_t binHi = static_cast<std::size_t>(maxHz * n / sampleRate);
    for (std::size_t k = binLo; k <= binHi && k < n / 2; ++k) {
        double re = 0.0, im = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double angle = 2.0 * 3.141592653589793 * k * i / n;
            re += samples[start + i] * std::cos(angle);
            im -= samples[start + i] * std::sin(angle);
        }
        const double mag = re * re + im * im;
        if (mag > bestMag) { bestMag = mag; best = k; }
    }
    return best;
}

SynthPreset granular_test_preset(float densityHz) {
    SynthPreset p = SynthPreset::make_default();
    p.name = "CritiqueFixGranular";
    // Resident bank: 0.34 s sine at 220 Hz, recorded as MIDI 60.
    p.sampleBank.enabled = true;
    p.sampleBank.sampleRate = 48000;
    p.sampleBank.rootNote = 60;
    p.sampleBank.frameCount = kSynthSampleMaxFrames;
    for (std::size_t i = 0; i < kSynthSampleMaxFrames; ++i)
        p.sampleBank.samples[i] =
            std::sin(2.0F * 3.14159265358979F * 220.0F * static_cast<float>(i) / 48000.0F);
    for (auto& osc : p.oscillators) osc.enabled = false;
    p.oscillators[0].enabled = true;
    p.oscillators[0].waveform = OscillatorWaveform::Granular;
    p.oscillators[1].enabled = true;
    p.oscillators[1].waveform = OscillatorWaveform::Granular;
    p.granular.densityHz = densityHz;
    p.granular.durationMs = 120.0F;
    p.granular.positionJitter01 = 0.0F;
    p.granular.panScatter01 = 0.0F;
    p.granular.smear01 = 0.0F;
    p.granular.width01 = 0.0F;
    p.granular.reverseProbability01 = 0.0F;
    p.granular.gain = 1.0F;
    return p;
}

}  // namespace

void test_thread_hammer() {
    // Fix 1: set_preset() on the UI thread while render() runs on the audio
    // thread must not race, deadlock, or crash. (Also run under TSAN.)
    Synthesizer synth(48000);
    SynthPreset a = SynthPreset::make_default(); a.name = "HammerA";
    SynthPreset b = SynthPreset::make_default(); b.name = "HammerB";
    b.filter.cutoffHertz = 8000.0F;
    synth.set_preset(a);
    synth.note_on(69, 1.0F);

    std::atomic<bool> stop{false};
    std::atomic<int> renderBlocks{0};
    std::thread audio([&] {
        std::vector<float> buf(256 * 2);
        while (!stop.load()) { synth.render(buf.data(), 256); ++renderBlocks; }
    });
    std::thread t1([&] { for (int i = 0; i < 300; ++i) synth.set_preset(i % 2 ? a : b); });
    std::thread t2([&] { for (int i = 0; i < 300; ++i) synth.set_morph_amount((i % 101) / 100.0F); });
    std::thread t3([&] { for (int i = 0; i < 300; ++i) { volatile auto p = synth.preset(); (void)p; } });
    t1.join(); t2.join(); t3.join();
    stop.store(true);
    audio.join();
    CHECK(renderBlocks.load() > 0);
    // The synth is still usable afterwards; last write wins on the UI copy.
    synth.set_preset(a);
    std::vector<float> buf(512 * 2, 0.0F);
    synth.render(buf.data(), 512);
    bool finite = true;
    for (float s : buf) finite = finite && std::isfinite(s);
    CHECK(finite);
    std::printf("thread hammer: OK (%d render blocks, no crash/deadlock)\n", renderBlocks.load());
}

void test_nan_hardening() {
    // Fix 2: validate() rejects NaN granular params; the engine itself falls
    // back to finite defaults and renders without crashing or OOB reads.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    SynthPreset p = SynthPreset::make_default();
    p.granular.position01 = nan;
    std::string error;
    CHECK(!p.validate(&error));
    std::printf("nan validate rejected: %s\n", error.c_str());

    GranularEngine engine;
    engine.set_sample_rate(48000);
    engine.set_seed(1234);
    GranularParameters gp{};
    gp.densityHz = nan; gp.durationMs = nan; gp.pitchSemitones = nan;
    gp.position01 = nan; gp.positionJitter01 = nan; gp.panScatter01 = nan;
    gp.gain = nan; gp.reverseProbability01 = nan;
    gp.cloud01 = nan; gp.scatter01 = nan; gp.dust01 = nan;
    gp.freeze01 = nan; gp.freezePosition01 = nan; gp.smear01 = nan; gp.width01 = nan;
    constexpr std::size_t kFrames = 4096;
    std::vector<float> src(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i)
        src[i] = std::sin(2.0F * 3.14159265358979F * 440.0F * static_cast<float>(i) / 48000.0F);
    const GranularSource source{src.data(), static_cast<std::uint32_t>(kFrames), 48000, 60};
    for (int i = 0; i < 4800; ++i) {
        const auto [l, r] = engine.render(source, gp, nan, nan);
        CHECK(std::isfinite(l) && std::isfinite(r));
    }
    CHECK(GranularEngine::cubic_sample(src.data(), static_cast<std::uint32_t>(kFrames), nan) == 0.0F);
    CHECK(GranularEngine::cubic_sample(src.data(), static_cast<std::uint32_t>(kFrames),
                                       std::numeric_limits<float>::infinity()) == 0.0F);
    std::printf("nan hardening: OK (finite defaults, no crash/OOB)\n");
}

void test_per_osc_engines() {
    // Fix 3: each oscillator owns a grain pool. Two granular oscillators at
    // the same density spawn ~2x the grains of one; oscillator 0 keeps the
    // legacy seed so single-osc renders stay bit-identical across runs.
    const auto render_grains = [](bool secondOsc) {
        Synthesizer synth(48000);
        SynthPreset p = granular_test_preset(100.0F);
        if (!secondOsc) p.oscillators[1].enabled = false;
        synth.set_preset(p);
        synth.note_on(69, 1.0F);
        std::vector<float> buf(512 * 2);
        for (int i = 0; i < 94; ++i) synth.render(buf.data(), 512);  // ~1 s
        return synth.granular_profiler().requestedGrains;
    };
    const auto single = render_grains(false);
    const auto dual = render_grains(true);
    std::printf("per-osc engines: single=%llu dual=%llu\n",
                (unsigned long long)single, (unsigned long long)dual);
    // Two independent clouds: dual admits ~2x the grains of single (ratio-based,
    // robust to density scaling from the Cloud macro).
    CHECK(single > 50);
    CHECK(dual > single * 1.8 && dual < single * 2.2);

    // Osc-0 determinism: two fresh synths, same seed path, bit-identical.
    const auto render_once = [] {
        Synthesizer synth(48000);
        SynthPreset p = granular_test_preset(40.0F);
        p.oscillators[1].enabled = false;
        synth.set_preset(p);
        synth.note_on(69, 1.0F);
        std::vector<float> buf(2048 * 2);
        synth.render(buf.data(), 2048);
        return buf;
    };
    const auto x = render_once();
    const auto y = render_once();
    CHECK(x.size() == y.size());
    for (std::size_t i = 0; i < x.size(); ++i) CHECK(x[i] == y[i]);
    std::printf("per-osc engines: OK (independent clouds, osc-0 bit-identical)\n");
}

void test_dvesynth_granular_roundtrip() {
    // Fix 4: granular.* keys round-trip through the text format; files written
    // before the fix (no granular keys) still parse with default values.
    SynthPreset p = SynthPreset::make_default();
    p.granular.enabled = true;
    p.granular.densityHz = 123.0F;
    p.granular.durationMs = 321.0F;
    p.granular.pitchSemitones = -7.0F;
    p.granular.position01 = 0.7F;
    p.granular.positionJitter01 = 0.2F;
    p.granular.panScatter01 = 0.9F;
    p.granular.gain = 1.5F;
    p.granular.reverseProbability01 = 0.25F;
    p.granular.envelopeShape = GranularEnvelopeShape::PlanckTaper;
    p.granular.cloud01 = 0.8F;
    p.granular.scatter01 = 0.6F;
    p.granular.dust01 = 0.4F;
    p.granular.freeze01 = 0.3F;
    p.granular.freezePosition01 = 0.65F;
    p.granular.smear01 = 0.5F;
    p.granular.width01 = 0.75F;
    p.granular.granularQuality = FilterQuality::High;

    const std::string text = p.serialize();
    std::string error;
    const auto parsed = SynthPreset::parse(text, &error);
    CHECK(parsed.has_value());
    if (!parsed.has_value()) { std::printf("parse failed: %s\n", error.c_str()); return; }
    const auto& g = parsed->granular;
    CHECK(g.enabled);
    CHECK(std::abs(g.densityHz - 123.0F) < 1e-4F);
    CHECK(std::abs(g.durationMs - 321.0F) < 1e-4F);
    CHECK(std::abs(g.pitchSemitones + 7.0F) < 1e-4F);
    CHECK(std::abs(g.position01 - 0.7F) < 1e-4F);
    CHECK(std::abs(g.positionJitter01 - 0.2F) < 1e-4F);
    CHECK(std::abs(g.panScatter01 - 0.9F) < 1e-4F);
    CHECK(std::abs(g.gain - 1.5F) < 1e-4F);
    CHECK(std::abs(g.reverseProbability01 - 0.25F) < 1e-4F);
    CHECK(g.envelopeShape == GranularEnvelopeShape::PlanckTaper);
    CHECK(std::abs(g.cloud01 - 0.8F) < 1e-4F);
    CHECK(std::abs(g.scatter01 - 0.6F) < 1e-4F);
    CHECK(std::abs(g.dust01 - 0.4F) < 1e-4F);
    CHECK(std::abs(g.freeze01 - 0.3F) < 1e-4F);
    CHECK(std::abs(g.freezePosition01 - 0.65F) < 1e-4F);
    CHECK(std::abs(g.smear01 - 0.5F) < 1e-4F);
    CHECK(std::abs(g.width01 - 0.75F) < 1e-4F);
    CHECK(g.granularQuality == FilterQuality::High);

    // Old-file compatibility: strip every granular.* line; the parse must
    // succeed and leave make_default() granular values in place.
    std::string legacy;
    for (std::size_t pos = 0; pos < text.size();) {
        const std::size_t eol = text.find('\n', pos);
        const std::string line = text.substr(pos, eol == std::string::npos ? eol : eol - pos);
        if (line.rfind("granular.", 0) != 0) { legacy += line; legacy += '\n'; }
        pos = eol == std::string::npos ? text.size() : eol + 1;
    }
    const auto legacyParsed = SynthPreset::parse(legacy, &error);
    CHECK(legacyParsed.has_value());
    if (legacyParsed.has_value()) {
        const auto& lg = legacyParsed->granular;
        const auto& dg = SynthPreset::make_default().granular;
        CHECK(lg.densityHz == dg.densityHz);
        CHECK(lg.position01 == dg.position01);
        CHECK(lg.envelopeShape == dg.envelopeShape);
        CHECK(lg.granularQuality == dg.granularQuality);
        CHECK(lg.width01 == dg.width01);
    }
    std::printf("dvesynth granular round-trip: OK (incl. legacy files)\n");
}

void test_wavetable_cook_gating() {
    // Fix 6: the HQ wavetable is re-cooked only when its content changes —
    // not on every set_preset, and not on morph walks that leave the content
    // alone (the old code re-cooked + reallocated per block).
    SynthPreset p = SynthPreset::make_default();
    p.name = "CookGate";
    p.wavetable.enabled = true;
    p.wavetable.frameCount = 4;
    for (std::size_t f = 0; f < 4; ++f)
        for (std::size_t i = 0; i < kWavetableSampleCount; ++i)
            p.wavetable.samples[f * kWavetableSampleCount + i] =
                std::sin(2.0F * 3.14159265358979F * static_cast<float>(f + 1) *
                         static_cast<float>(i) / static_cast<float>(kWavetableSampleCount));
    p.morphEnabled = true;

    Synthesizer synth(48000);
    synth.set_preset(p);
    std::vector<float> buf(512 * 2);
    synth.render(buf.data(), 512);
    const auto cooksAfterFirst = synth.wavetable_cook_count();
    CHECK(cooksAfterFirst >= 1);

    // Re-setting the identical preset must not re-cook.
    for (int i = 0; i < 20; ++i) { synth.set_preset(p); synth.render(buf.data(), 512); }
    CHECK(synth.wavetable_cook_count() == cooksAfterFirst);

    // Morph walk between presets with identical wavetable content: still flat.
    SynthPreset b = p;
    b.name = "CookGateB";
    b.filter.cutoffHertz = 9000.0F;
    synth.set_morph_preset_b(b);
    for (int i = 0; i <= 40; ++i) {
        synth.set_morph_amount(i / 40.0F);
        synth.render(buf.data(), 512);
    }
    CHECK(synth.wavetable_cook_count() == cooksAfterFirst);

    // A genuinely different wavetable still cooks exactly once.
    SynthPreset c = p;
    c.name = "CookGateC";
    c.wavetable.samples[0] += 0.5F;
    synth.set_preset(c);
    synth.render(buf.data(), 512);
    CHECK(synth.wavetable_cook_count() == cooksAfterFirst + 1);
    std::printf("wavetable cook gating: OK (cooks=%llu, flat across walk)\n",
                (unsigned long long)synth.wavetable_cook_count());
}

void test_grain_pitch_tracking() {
    // Fix 7: grain pitch tracks the played note. Bank holds a 220 Hz sine
    // recorded as MIDI 60; playing MIDI 60 renders ~220 Hz, MIDI 72 renders
    // ~440 Hz (peak bins an octave apart).
    constexpr float kRate = 48000.0F;
    SynthPreset p = SynthPreset::make_default();
    p.name = "PitchTrack";
    p.sampleBank.enabled = true;
    p.sampleBank.sampleRate = 48000;
    p.sampleBank.rootNote = 60;
    p.sampleBank.frameCount = kSynthSampleMaxFrames;
    for (std::size_t i = 0; i < kSynthSampleMaxFrames; ++i)
        p.sampleBank.samples[i] =
            std::sin(2.0F * 3.14159265358979F * 220.0F * static_cast<float>(i) / kRate);
    for (auto& osc : p.oscillators) osc.enabled = false;
    p.oscillators[0].enabled = true;
    p.oscillators[0].waveform = OscillatorWaveform::Granular;
    p.granular.densityHz = 30.0F;
    p.granular.durationMs = 100.0F;  // fits in the 341 ms source; no end-clamp
    p.granular.position01 = 0.0F;
    p.granular.positionJitter01 = 0.0F;
    p.granular.panScatter01 = 0.0F;
    p.granular.smear01 = 0.0F;
    p.granular.width01 = 0.0F;
    p.granular.reverseProbability01 = 0.0F;
    p.granular.gain = 1.0F;
    p.filter.enabled = false;
    p.reverb.mix = 0.0F;
    p.delay.mix = 0.0F;

    const auto render_note = [&](std::uint8_t midi) {
        Synthesizer synth(48000);
        synth.set_preset(p);
        synth.note_on(midi, 1.0F);
        std::vector<float> buf(48000 * 2);  // 1 s stereo
        synth.render(buf.data(), 48000);
        std::vector<float> mono(48000);
        for (std::size_t i = 0; i < 48000; ++i) mono[i] = 0.5F * (buf[2 * i] + buf[2 * i + 1]);
        return mono;
    };
    const auto mono60 = render_note(60);
    const auto mono72 = render_note(72);
    constexpr std::size_t kN = 16384;
    const std::size_t bin60 = peak_bin(mono60, 24000, kN, kRate, 100.0F, 400.0F);
    const std::size_t bin72 = peak_bin(mono72, 24000, kN, kRate, 200.0F, 800.0F);
    const double hz60 = bin60 * kRate / kN;
    const double hz72 = bin72 * kRate / kN;
    std::printf("pitch tracking: note60 peak %.1f Hz (bin %zu), note72 peak %.1f Hz (bin %zu)\n",
                hz60, bin60, hz72, bin72);
    // Fundamental in the right ballpark (220 Hz source; measurement has a few
    // percent slop from overlapping-grain interference), and the octave
    // relationship is exact: grain pitch tracks the played note.
    CHECK(hz60 > 180.0 && hz60 < 260.0);
    CHECK(std::abs(hz72 / hz60 - 2.0) < 0.08);
    std::printf("pitch tracking: OK (octave apart)\n");
}

void test_binary_patch_waveform_ids() {
    // Fix 8: oscillator waveform/enabled survive the binary patch format.
    SynthPreset p = SynthPreset::make_default();
    p.name = "WaveformIds";
    p.oscillators[0].waveform = OscillatorWaveform::Wavetable;
    p.oscillators[3].waveform = OscillatorWaveform::Granular;
    p.oscillators[5].enabled = false;
    // Binary patch stores sample-bank metadata (enabled/root/rate), not the
    // frames themselves; keep the bank disabled so the frame-count validator
    // passes. Waveform/enabled IDs are what this test exercises.
    p.sampleBank.enabled = false;
    p.sampleBank.rootNote = 69;
    p.sampleBank.sampleRate = 44100;

    const SynthPatchProgram program = compile_patch(p);
    std::string error;
    const auto loaded = load_patch_program(program.bytes.data(), program.bytes.size(), &error);
    CHECK(loaded.has_value());
    if (!loaded.has_value()) { std::printf("load failed: %s\n", error.c_str()); return; }
    CHECK(loaded->oscillators[0].waveform == OscillatorWaveform::Wavetable);
    CHECK(loaded->oscillators[3].waveform == OscillatorWaveform::Granular);
    CHECK(loaded->oscillators[1].waveform == p.oscillators[1].waveform);
    CHECK(!loaded->oscillators[5].enabled);
    CHECK(loaded->oscillators[0].enabled);
    CHECK(!loaded->sampleBank.enabled);  // bank disabled in this test (see above)
    CHECK(loaded->sampleBank.rootNote == 69);
    CHECK(loaded->sampleBank.sampleRate == 44100);
    std::printf("binary patch waveform IDs: OK\n");
}

int main() {
    test_thread_hammer();
    test_nan_hardening();
    test_per_osc_engines();
    test_dvesynth_granular_roundtrip();
    test_wavetable_cook_gating();
    test_grain_pitch_tracking();
    test_binary_patch_waveform_ids();
    if (g_failures == 0) std::printf("audio critique fix tests: all passed\n");
    else std::printf("audio critique fix tests: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
