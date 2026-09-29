// Tests for the Phase 5 spectral resynthesis oscillator (SYN-015):
// include/dve/audio/spectral.hpp + src/audio/spectral.cpp.
//
// The test asset is cooked with Worker A's offline analyzer
// (analyze_spectrum from include/dve/audio/spectral_asset.hpp) and converted
// to the engine's view with SpectralAssetViewData. Synthetic data only; no
// audio files on disk.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

#include "dve/audio/spectral.hpp"
#include "dve/audio/synthesizer.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

constexpr std::uint32_t kRate = 48000U;
constexpr float kTwoPi = std::numbers::pi_v<float> * 2.0F;

bool near(float a, float b, float eps = 1.0e-5F) { return std::fabs(a - b) <= eps; }

// ---------------------------------------------------------------------------
// Allocation watchdog: counts global operator new calls inside a window.
// The spectral render path must perform zero heap allocations.
// ---------------------------------------------------------------------------
std::atomic<long> g_allocCount{0};
thread_local bool g_countingAllocs = false;

struct AllocScope {
    AllocScope() { g_allocCount.store(0); g_countingAllocs = true; }
    ~AllocScope() { g_countingAllocs = false; }
    long count() const { return g_allocCount.load(); }
};

}  // namespace

void* operator new(std::size_t n) {
    if (g_countingAllocs) g_allocCount.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc{};
    return p;
}
void* operator new[](std::size_t n) {
    if (g_countingAllocs) g_allocCount.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc{};
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

// ---------------------------------------------------------------------------
// Test asset: a sine cooked with Worker A's offline analyzer, converted to
// the engine's magnitude/phase view with SpectralAssetViewData.
// ---------------------------------------------------------------------------
struct TestAsset {
    SpectralAsset asset;        // Worker's A cooked asset (owns the frames)
    SpectralAssetViewData data;  // converted magnitude/phase view (owns the view)
    const SpectralAssetView* view = nullptr;

    void build_sine(float freqHz, float amplitude, float seconds) {
        const std::uint32_t totalSamples = static_cast<std::uint32_t>(seconds * sampleRate());
        std::vector<float> audio(totalSamples);
        for (std::uint32_t i = 0; i < totalSamples; ++i)
            audio[i] = amplitude * std::sin(kTwoPi * freqHz * static_cast<float>(i) /
                                            static_cast<float>(sampleRate()));
        SpectralAnalyzerConfig config;
        config.fftSize = 1024U;
        config.hopSize = 256U;
        asset = analyze_spectrum(audio.data(), totalSamples, sampleRate(), config);
        CHECK(!asset.empty());
        CHECK(asset.numBins == 513U);
        CHECK(data.convert(asset));
        CHECK(data.valid());
        view = &data.view();
    }

    static constexpr std::uint32_t sampleRate() { return kRate; }
};

SpectralParameters default_params() {
    SpectralParameters params;
    params.gain = 0.8F;
    params.spectralQuality = FilterQuality::Standard;
    return params;
}

// Renders `frames` samples through an oscillator; returns interleaved stereo.
std::vector<float> render_osc(SpectralOscillator& osc, const SpectralAssetView& asset,
                              const SpectralParameters& params, float pitchHz,
                              std::uint32_t frames, float* peakOut = nullptr) {
    std::vector<float> out(frames * 2U);
    float peak = 0.0F;
    for (std::uint32_t i = 0; i < frames; ++i) {
        const auto [left, right] = osc.render(asset, params, pitchHz);
        out[i * 2U] = left;
        out[i * 2U + 1U] = right;
        peak = std::max({peak, std::fabs(left), std::fabs(right)});
        if (!std::isfinite(left) || !std::isfinite(right)) break;
    }
    if (peakOut != nullptr) *peakOut = peak;
    return out;
}

// Hann-windowed peak bin of a mono buffer (naive DFT, test-only).
std::uint32_t peak_bin(const float* x, std::uint32_t n, float* magOut = nullptr) {
    std::uint32_t best = 1U;
    double bestMag = 0.0;
    for (std::uint32_t k = 1U; k < n / 2U; ++k) {
        double re = 0.0, im = 0.0;
        for (std::uint32_t i = 0U; i < n; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / (n - 1U));
            const double ang = 2.0 * 3.14159265358979 * k * i / n;
            re += x[i] * w * std::cos(ang);
            im -= x[i] * w * std::sin(ang);
        }
        const double mag = std::sqrt(re * re + im * im);
        if (mag > bestMag) {
            bestMag = mag;
            best = k;
        }
    }
    if (magOut != nullptr) *magOut = static_cast<float>(bestMag);
    return best;
}

// Per-bin magnitudes of a mono buffer (for the freeze stationarity test).
std::vector<double> spectrum_mags(const float* x, std::uint32_t n) {
    std::vector<double> mags(n / 2U, 0.0);
    for (std::uint32_t k = 0U; k < n / 2U; ++k) {
        double re = 0.0, im = 0.0;
        for (std::uint32_t i = 0U; i < n; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / (n - 1U));
            const double ang = 2.0 * 3.14159265358979 * k * i / n;
            re += x[i] * w * std::cos(ang);
            im -= x[i] * w * std::sin(ang);
        }
        mags[k] = std::sqrt(re * re + im * im);
    }
    return mags;
}

float max_abs_diff_lr(const std::vector<float>& interleaved) {
    float worst = 0.0F;
    for (std::size_t i = 0; i + 1U < interleaved.size(); i += 2U)
        worst = std::max(worst, std::fabs(interleaved[i] - interleaved[i + 1U]));
    return worst;
}

}  // namespace

int main() {
    {
        // T0: the engine's realtime transform (fft_inplace) agrees with Worker
        // A's offline SpectralFft, and A's inverse inverts its forward.
        // (A's transform allocates scratch per call, so it stays off the
        // audio thread; the two are cross-validated here instead.)
        constexpr std::uint32_t kN = 1024U;
        std::vector<float> time(kN), reA(kN), imA(kN), reB(kN), imB(kN);
        std::uint32_t s = 0xC0FFEEU;
        auto rnd = [&] {
            s = s * 1664525U + 1013904223U;
            return static_cast<float>(s) / 4294967296.0F * 2.0F - 1.0F;
        };
        for (std::uint32_t i = 0; i < kN; ++i) time[i] = rnd();
        SpectralFft fft(kN);
        std::vector<float> interleaved(kN + 2U);
        fft.forward(time.data(), interleaved.data());
        for (std::uint32_t i = 0; i < kN; ++i) {
            reA[i] = time[i];
            imA[i] = 0.0F;
        }
        SpectralOscillator::fft_inplace(reA.data(), imA.data(), kN, false);
        float worstFwd = 0.0F;
        for (std::uint32_t k = 0; k <= kN / 2U; ++k) {
            worstFwd = std::max(worstFwd, std::fabs(reA[k] - interleaved[2U * k]));
            worstFwd = std::max(worstFwd, std::fabs(imA[k] - interleaved[2U * k + 1U]));
        }
        CHECK(worstFwd < 1.0e-3F);
        std::vector<float> back(kN);
        fft.inverse(interleaved.data(), back.data());
        float worstInv = 0.0F;
        for (std::uint32_t i = 0; i < kN; ++i)
            worstInv = std::max(worstInv, std::fabs(back[i] - time[i]));
        CHECK(worstInv < 1.0e-4F);
    }

    // A 440 Hz sine asset cooked by Worker A's analyzer (1024-pt FFT, 256 hop).
    TestAsset asset;
    asset.build_sine(440.0F, 0.9F, 2.0F);
    CHECK(asset.view->frameCount >= 300U);
    // The analyzer's pitch hint should land near the sine's true frequency;
    // the oscillator uses it as the pitch-ratio reference (baseFrequencyHz).
    const float estimated = asset.view->baseFrequencyHz;
    CHECK(estimated > 400.0F && estimated < 480.0F);
    // The adaptor must reject degenerate assets.
    {
        SpectralAssetViewData bad;
        CHECK(!bad.convert(SpectralAsset{}));
        SpectralAsset tooBig = asset.asset;
        tooBig.fftSize = 8192U;
        CHECK(!bad.convert(tooBig));
    }

    {
        // T1: determinism — same asset + seed + params -> bit-identical output.
        SpectralOscillator a, b;
        a.set_sample_rate(kRate);
        b.set_sample_rate(kRate);
        a.reset();
        b.reset();
        a.set_seed(0x12345678U);
        b.set_seed(0x12345678U);
        const SpectralParameters params = default_params();
        const auto outA = render_osc(a, *asset.view, params, 440.0F, 24000U);
        const auto outB = render_osc(b, *asset.view, params, 440.0F, 24000U);
        CHECK(outA.size() == outB.size());
        CHECK(std::memcmp(outA.data(), outB.data(), outA.size() * sizeof(float)) == 0);
    }

    {
        // T2: renders non-silent, NaN-free output from a real asset.
        SpectralOscillator osc;
        osc.set_sample_rate(kRate);
        osc.reset();
        osc.set_seed(7U);
        float peak = 0.0F;
        const auto out = render_osc(osc, *asset.view, default_params(), 440.0F, 48000U, &peak);
        bool finite = true;
        for (float v : out) finite = finite && std::isfinite(v);
        CHECK(finite);
        CHECK(peak > 0.1F);  // raised at Phase 5 merge: 1/N scaling bug fixed, amplitude now correct
        const SpectralCounters c = osc.drain_counters();
        CHECK(c.framesRendered > 0U);
        CHECK(c.binsRendered == c.framesRendered * 512U);  // Standard tier = 512 bins
        const SpectralCounters c2 = osc.drain_counters();
        CHECK(c2.framesRendered == 0U);  // drain zeroes
    }

    {
        // T11: amplitude round-trip — a 0.5-amplitude sine cooked by Worker A's
        // analyzer resynthesizes at ~0.5 (within 10%). Regression test for the
        // 1/N scaling bug: the single-sided magnitudes were fed straight into
        // the 1/N-normalized inverse FFT, so the output was 1/N of correct.
        // Rendered at the asset's own base frequency (pitch ratio exactly 1);
        // the measurement window skips the fade-in and stays clear of the
        // asset's loop-wrap point.
        TestAsset rt;
        rt.build_sine(440.0F, 0.5F, 2.0F);
        for (FilterQuality tier :
             {FilterQuality::Eco, FilterQuality::Standard, FilterQuality::High,
              FilterQuality::Offline}) {
            SpectralOscillator osc;
            osc.set_sample_rate(kRate);
            osc.reset();
            osc.set_seed(7U);
            SpectralParameters params;
            params.gain = 1.0F;
            params.spectralQuality = tier;
            const auto out = render_osc(osc, *rt.view, params, rt.view->baseFrequencyHz,
                                        48000U);
            float peak = 0.0F;
            for (std::uint32_t i = 16384U; i < 40000U; ++i)
                peak = std::max({peak, std::fabs(out[i * 2U]), std::fabs(out[i * 2U + 1U])});
            CHECK(peak > 0.45F && peak < 0.55F);
        }
    }

    {
        // T3: freeze — with freeze on, the output spectrum is stationary.
        SpectralOscillator osc;
        osc.set_sample_rate(kRate);
        osc.reset();
        osc.set_seed(7U);
        SpectralParameters params = default_params();
        params.freeze01 = 1.0F;
        const auto out = render_osc(osc, *asset.view, params, 440.0F, 96000U);
        // Two adjacent 2048-sample windows from the settled tail (mono sum).
        constexpr std::uint32_t kWin = 2048U;
        const std::size_t tail = out.size() / 2U - 2U * kWin;
        std::vector<float> winA(kWin), winB(kWin);
        for (std::uint32_t i = 0; i < kWin; ++i) {
            winA[i] = 0.5F * (out[(tail + i) * 2U] + out[(tail + i) * 2U + 1U]);
            winB[i] = 0.5F * (out[(tail + kWin + i) * 2U] + out[(tail + kWin + i) * 2U + 1U]);
        }
        const auto specA = spectrum_mags(winA.data(), kWin);
        const auto specB = spectrum_mags(winB.data(), kWin);
        double num = 0.0, den = 0.0;
        for (std::size_t k = 0; k < specA.size(); ++k) {
            const double d = specA[k] - specB[k];
            num += d * d;
            den += specA[k] * specA[k];
        }
        const double relDiff = den > 0.0 ? std::sqrt(num / den) : 1.0;
        CHECK(relDiff < 0.02);
    }

    {
        // T4: time stretch 2x consumes asset frames at half speed, pitch unchanged.
        SpectralOscillator s1, s2;
        s1.set_sample_rate(kRate);
        s2.set_sample_rate(kRate);
        s1.reset();
        s2.reset();
        s1.set_seed(7U);
        s2.set_seed(7U);
        SpectralParameters p1 = default_params();
        SpectralParameters p2 = default_params();
        p2.timeStretch = 2.0F;
        const auto out1 = render_osc(s1, *asset.view, p1, 440.0F, 48000U);
        const auto out2 = render_osc(s2, *asset.view, p2, 440.0F, 48000U);
        const double adv1 = s1.total_frames_advanced();
        const double adv2 = s2.total_frames_advanced();
        CHECK(adv1 > 100.0);
        CHECK(near(static_cast<float>(adv1 / adv2), 2.0F, 0.05F));
        // Peak-bin stability on the sine asset (skip the fade-in region).
        constexpr std::uint32_t kWin = 2048U;
        std::vector<float> w1(kWin), w2(kWin);
        for (std::uint32_t i = 0; i < kWin; ++i) {
            w1[i] = 0.5F * (out1[(24000U + i) * 2U] + out1[(24000U + i) * 2U + 1U]);
            w2[i] = 0.5F * (out2[(24000U + i) * 2U] + out2[(24000U + i) * 2U + 1U]);
        }
        const std::uint32_t b1 = peak_bin(w1.data(), kWin);
        const std::uint32_t b2 = peak_bin(w2.data(), kWin);
        CHECK(std::abs(static_cast<int>(b1) - static_cast<int>(b2)) <= 1);
    }

    {
        // T5: pitch input tracks — 440 Hz vs 220 Hz renders an octave apart.
        SpectralOscillator osc;
        osc.set_sample_rate(kRate);
        osc.reset();
        osc.set_seed(7U);
        const auto hi = render_osc(osc, *asset.view, default_params(), 440.0F, 48000U);
        osc.reset();
        osc.set_seed(7U);
        const auto lo = render_osc(osc, *asset.view, default_params(), 220.0F, 48000U);
        constexpr std::uint32_t kWin = 2048U;
        std::vector<float> wHi(kWin), wLo(kWin);
        for (std::uint32_t i = 0; i < kWin; ++i) {
            wHi[i] = 0.5F * (hi[(24000U + i) * 2U] + hi[(24000U + i) * 2U + 1U]);
            wLo[i] = 0.5F * (lo[(24000U + i) * 2U] + lo[(24000U + i) * 2U + 1U]);
        }
        const std::uint32_t bHi = peak_bin(wHi.data(), kWin);
        const std::uint32_t bLo = peak_bin(wLo.data(), kWin);
        CHECK(bHi > 0U && bLo > 0U);
        CHECK(near(static_cast<float>(bHi) / static_cast<float>(bLo), 2.0F, 0.3F));
    }

    {
        // T6: null/degenerate asset renders exact silence, never a crash.
        SpectralOscillator osc;
        osc.set_sample_rate(kRate);
        osc.reset();
        osc.set_seed(7U);
        const SpectralAssetView nullAsset{};
        float peak = 1.0F;
        const auto out = render_osc(osc, nullAsset, default_params(), 440.0F, 8192U, &peak);
        CHECK(peak == 0.0F);
        CHECK(osc.counters().silentFrames > 0U);
        // Degenerate: one frame only.
        SpectralAssetView oneFrame = *asset.view;
        oneFrame.frameCount = 1U;
        float peak2 = 1.0F;
        render_osc(osc, oneFrame, default_params(), 440.0F, 1024U, &peak2);
        CHECK(peak2 == 0.0F);
    }

    {
        // T7: Eco always wins — extreme musical settings are forced off in Eco
        // (L/R bit-identical = mono-compatible); Standard honours spread.
        SpectralParameters spicy = default_params();
        spicy.stereoSpread01 = 1.0F;
        spicy.spectralBlur01 = 1.0F;
        spicy.spectralTiltDbPerOct = 12.0F;
        spicy.formantShiftSemitones = 7.0F;
        spicy.phaseRandom01 = 1.0F;
        spicy.spectralQuality = FilterQuality::Eco;
        SpectralOscillator eco;
        eco.set_sample_rate(kRate);
        eco.reset();
        eco.set_seed(7U);
        const auto ecoOut = render_osc(eco, *asset.view, spicy, 440.0F, 24000U);
        CHECK(max_abs_diff_lr(ecoOut) == 0.0F);
        CHECK(eco.counters().binsRendered == eco.counters().framesRendered * 256U);

        SpectralParameters std = spicy;
        std.spectralQuality = FilterQuality::Standard;
        SpectralOscillator stdOsc;
        stdOsc.set_sample_rate(kRate);
        stdOsc.reset();
        stdOsc.set_seed(7U);
        const auto stdOut = render_osc(stdOsc, *asset.view, std, 440.0F, 24000U);
        CHECK(max_abs_diff_lr(stdOut) > 1.0e-6F);
    }

    {
        // T8: no heap allocation during render; bounded runtime.
        SpectralOscillator osc;
        osc.set_sample_rate(kRate);
        osc.reset();
        osc.set_seed(7U);
        const SpectralParameters params = default_params();
        // Warm up outside the counting window (primes fade/OLA state).
        render_osc(osc, *asset.view, params, 440.0F, 4096U);
        // The sink is allocated BEFORE the counting window opens.
        std::vector<float> sink(48000U * 2U, 0.0F);
        const auto t0 = std::clock();
        long allocs = 0;
        bool finite = true;
        {
            AllocScope scope;
            for (std::uint32_t i = 0U; i < 48000U; ++i) {
                const auto [left, right] = osc.render(*asset.view, params, 440.0F);
                sink[i * 2U] = left;
                sink[i * 2U + 1U] = right;
                finite = finite && std::isfinite(left) && std::isfinite(right);
            }
            allocs = scope.count();
        }
        const double seconds = static_cast<double>(std::clock() - t0) / CLOCKS_PER_SEC;
        CHECK(finite);
        CHECK(allocs == 0L);
        CHECK(seconds < 5.0);
        std::printf("spectral render 1s: %.3f s wall, %ld allocs\n", seconds, allocs);
    }

    {
        // T9: all four quality tiers render finite audio.
        for (FilterQuality q : {FilterQuality::Eco, FilterQuality::Standard,
                                FilterQuality::High, FilterQuality::Offline}) {
            SpectralOscillator osc;
            osc.set_sample_rate(kRate);
            osc.reset();
            osc.set_seed(7U);
            SpectralParameters params = default_params();
            params.spectralQuality = q;
            float peak = 0.0F;
            const auto out = render_osc(osc, *asset.view, params, 440.0F, 12000U, &peak);
            bool finite = true;
            for (float v : out) finite = finite && std::isfinite(v);
            CHECK(finite);
            CHECK(peak > 0.1F);  // raised at Phase 5 merge: 1/N scaling bug fixed, amplitude now correct
        }
    }

    {
        // T10: waveform plumbing — name round-trip and voice-level render.
        CHECK(oscillator_waveform_name(OscillatorWaveform::Spectral) == "Spectral");
        Synthesizer synth(kRate);
        SynthPreset preset;
        preset.oscillators[0].waveform = OscillatorWaveform::Spectral;
        for (std::size_t i = 1U; i < preset.oscillators.size(); ++i)
            preset.oscillators[i].enabled = false;  // isolate the Spectral voice
        preset.spectralAsset = asset.view;
        synth.set_preset(preset);
        CHECK(synth.note_on(69, 0.9F));
        std::vector<float> audio(48000U * 2U, 0.0F);
        synth.render(audio);
        float peak = 0.0F;
        bool finite = true;
        for (float v : audio) {
            finite = finite && std::isfinite(v);
            peak = std::max(peak, std::fabs(v));
        }
        CHECK(finite);
        CHECK(peak > 1.0e-6F);
        // Null asset through the voice path: silence, no crash (fresh synth so
        // no earlier voice is still sustaining).
        Synthesizer silentSynth(kRate);
        SynthPreset silent = preset;
        silent.spectralAsset = nullptr;
        silentSynth.set_preset(silent);
        CHECK(silentSynth.note_on(69, 0.9F));
        std::vector<float> audio2(8192U * 2U, 0.0F);
        silentSynth.render(audio2);
        float peak2 = 0.0F;
        for (float v : audio2) peak2 = std::max(peak2, std::fabs(v));
        CHECK(peak2 == 0.0F);
    }

    if (g_failures == 0) std::printf("ALL SPECTRAL TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
