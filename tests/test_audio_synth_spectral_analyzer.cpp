// Tests for the Phase 5 offline spectral analyzer (SYN-015), Worker A:
// include/dve/audio/spectral_asset.hpp + src/audio/spectral_analyzer.cpp.
// All signals are synthesized in-memory; no audio files on disk.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/audio/spectral_asset.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

constexpr std::uint32_t kRate = 48000U;
constexpr float kTwoPi = 6.28318530717958647692F;

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

std::vector<float> make_sine(float freqHz, std::uint32_t frames, float amp = 0.9F) {
    std::vector<float> out(frames);
    for (std::uint32_t i = 0; i < frames; ++i)
        out[i] = amp * std::sin(kTwoPi * freqHz * static_cast<float>(i) / static_cast<float>(kRate));
    return out;
}

std::uint32_t peak_bin(const SpectralAsset& asset, std::uint32_t frame) {
    const float* d = asset.frame_data(frame);
    std::uint32_t best = 0;
    float bestMag = -1.0F;
    for (std::uint32_t b = 1; b < asset.numBins; ++b) {  // skip DC
        const float m = SpectralAsset::bin_magnitude(d + 2U * b);
        if (m > bestMag) { bestMag = m; best = b; }
    }
    return best;
}

float max_abs_error(const std::vector<float>& a, const std::vector<float>& b,
                    std::uint32_t trim) {
    float worst = 0.0F;
    const std::uint32_t n = static_cast<std::uint32_t>(a.size());
    for (std::uint32_t i = trim; i + trim < n && i + trim < b.size(); ++i) {
        const float e = std::fabs(a[i] - b[i]);
        if (e > worst) worst = e;
    }
    return worst;
}

void test_sine_peak_bin() {
    // 440 Hz sine at 48 kHz, fftSize 2048: expected bin = 440*2048/48000 ~= 18.77.
    SpectralAnalyzerConfig cfg;
    cfg.fftSize = 2048U;
    cfg.hopSize = 512U;
    const auto sig = make_sine(440.0F, kRate);  // 1 s
    SpectralAsset asset = analyze_spectrum(sig.data(), static_cast<std::uint32_t>(sig.size()), kRate, cfg);
    CHECK(asset.numBins == 1025U);
    CHECK(asset.numFrames > 4U);
    const std::uint32_t expected = 19U;  // round(18.77)
    for (std::uint32_t f = 1; f + 1 < asset.numFrames; ++f) {
        const std::uint32_t pb = peak_bin(asset, f);
        const std::uint32_t dist = (pb > expected) ? pb - expected : expected - pb;
        if (dist > 1U) {
            std::printf("  frame %u peak bin %u, expected ~%u\n", f, pb, expected);
            CHECK(false);
            break;
        }
    }
    // Pitch hint should land near 440 Hz for a clean sine.
    CHECK(asset.estimatedPitchHz > 400.0F && asset.estimatedPitchHz < 480.0F);
}

void test_istft_roundtrip_sine() {
    SpectralAnalyzerConfig cfg;
    cfg.fftSize = 2048U;
    cfg.hopSize = 512U;
    const auto sig = make_sine(440.0F, kRate / 2U);
    SpectralAsset asset = analyze_spectrum(sig.data(), static_cast<std::uint32_t>(sig.size()), kRate, cfg);
    std::vector<float> recon;
    resynthesize_spectral_asset(asset, recon);
    CHECK(recon.size() >= sig.size());
    const float err = max_abs_error(sig, recon, cfg.fftSize);
    std::printf("  sine ISTFT max abs error (edge-trimmed): %g\n", err);
    CHECK(err < 1.0e-3F);
}

void test_istft_roundtrip_complex() {
    // Sample-bank-ish: sum of sines + deterministic noise.
    std::mt19937 rng(1234U);
    std::uniform_real_distribution<float> uni(-0.05F, 0.05F);
    std::vector<float> sig(kRate / 2U);
    for (std::uint32_t i = 0; i < sig.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kRate);
        sig[i] = 0.5F * std::sin(kTwoPi * 220.0F * t)
               + 0.3F * std::sin(kTwoPi * 554.37F * t + 0.7F)
               + 0.2F * std::sin(kTwoPi * 1318.5F * t + 2.1F)
               + uni(rng);
    }
    SpectralAnalyzerConfig cfg;
    cfg.fftSize = 2048U;
    cfg.hopSize = 512U;
    SpectralAsset asset = analyze_spectrum(sig.data(), static_cast<std::uint32_t>(sig.size()), kRate, cfg);
    std::vector<float> recon;
    resynthesize_spectral_asset(asset, recon);
    const float err = max_abs_error(sig, recon, cfg.fftSize);
    std::printf("  complex ISTFT max abs error (edge-trimmed): %g\n", err);
    CHECK(err < 0.05F);
}

void test_silence_and_empty() {
    SpectralAnalyzerConfig cfg;
    std::vector<float> silence(kRate / 4U, 0.0F);
    SpectralAsset asset = analyze_spectrum(silence.data(),
                                           static_cast<std::uint32_t>(silence.size()), kRate, cfg);
    CHECK(!asset.empty());
    for (float v : asset.frames) CHECK(std::isfinite(v));
    for (float v : asset.frameEnergy) CHECK(std::isfinite(v) && v == 0.0F);
    CHECK(asset.estimatedPitchHz == 0.0F);

    // Empty input: zero frames, no crash, resynthesis yields nothing.
    SpectralAsset empty = analyze_spectrum(nullptr, 0U, kRate, cfg);
    CHECK(empty.empty());
    CHECK(empty.frames.empty());
    CHECK(empty.frameEnergy.empty());
    std::vector<float> recon;
    resynthesize_spectral_asset(empty, recon);
    CHECK(recon.empty());
}

void test_fft_roundtrip() {
    std::mt19937 rng(42U);
    std::uniform_real_distribution<float> uni(-1.0F, 1.0F);
    for (std::uint32_t size : {256U, 1024U, 2048U, 4096U}) {
        SpectralFft fft(size);
        std::vector<float> x(size), back(size);
        std::vector<float> spec((size / 2U + 1U) * 2U);
        for (auto& v : x) v = uni(rng);
        fft.forward(x.data(), spec.data());
        fft.inverse(spec.data(), back.data());
        float worst = 0.0F;
        for (std::uint32_t i = 0; i < size; ++i)
            worst = std::max(worst, std::fabs(x[i] - back[i]));
        std::printf("  FFT size %u round-trip max error: %g\n", size, worst);
        CHECK(worst < 1.0e-5F);
    }
    // Rejected sizes throw.
    bool threw = false;
    try { SpectralFft bad(1000U); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { SpectralFft bad(128U); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

void test_short_input_pads_to_one_frame() {
    SpectralAnalyzerConfig cfg;
    cfg.fftSize = 1024U;
    cfg.hopSize = 256U;
    const auto sig = make_sine(880.0F, 100U);  // shorter than fftSize
    SpectralAsset asset = analyze_spectrum(sig.data(), static_cast<std::uint32_t>(sig.size()), kRate, cfg);
    CHECK(asset.numFrames == 1U);
    for (float v : asset.frames) CHECK(std::isfinite(v));
    std::vector<float> recon;
    resynthesize_spectral_asset(asset, recon);
    CHECK(recon.size() == cfg.fftSize);
}

}  // namespace

int main() {
    test_sine_peak_bin();
    test_istft_roundtrip_sine();
    test_istft_roundtrip_complex();
    test_silence_and_empty();
    test_fft_roundtrip();
    test_short_input_pads_to_one_frame();
    if (g_failures == 0) std::printf("ALL SPECTRAL ANALYZER TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
