// Tests for the Phase 6 feature extractor (SYN-016, Worker A):
// include/dve/audio/audio_features.hpp + src/audio/audio_features.cpp.
// All signals are synthesized in-memory; no audio files on disk.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

#include "dve/audio/audio_features.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

constexpr double kRate = 48000.0;
constexpr float kTwoPi = 6.28318530717958647692F;

bool all_finite(const AudioFeatureVector& v) {
    const double* p = &v.rms;
    for (int i = 0; i < 14; ++i)
        if (!std::isfinite(p[i])) return false;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(v.melBands[i])) return false;
    return true;
}

bool all_zero(const AudioFeatureVector& v) {
    const double* p = &v.rms;
    for (int i = 0; i < 14; ++i)
        if (p[i] != 0.0) return false;
    for (int i = 0; i < 16; ++i)
        if (v.melBands[i] != 0.0) return false;
    return true;
}

bool vec_equal(const AudioFeatureVector& a, const AudioFeatureVector& b) {
    const double* pa = &a.rms;
    const double* pb = &b.rms;
    for (int i = 0; i < 14; ++i)
        if (pa[i] != pb[i]) return false;
    for (int i = 0; i < 16; ++i)
        if (a.melBands[i] != b.melBands[i]) return false;
    return true;
}

// Interleaved stereo/mono sine.
std::vector<float> make_sine(double freqHz, std::uint32_t frames, float amp, int channels,
                             float rightScale = 1.0F) {
    std::vector<float> out(frames * static_cast<std::uint32_t>(channels));
    for (std::uint32_t i = 0; i < frames; ++i) {
        const float s = amp * std::sin(kTwoPi * static_cast<float>(freqHz) *
                                       static_cast<float>(i) / static_cast<float>(kRate));
        out[i * static_cast<std::uint32_t>(channels)] = s;
        if (channels == 2) out[i * 2U + 1U] = s * rightScale;
    }
    return out;
}

std::vector<float> make_noise(std::uint32_t frames, float amp, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-amp, amp);
    std::vector<float> out(frames);
    for (auto& s : out) s = dist(rng);
    return out;
}

}  // namespace

int main() {
    constexpr std::uint32_t kFrames = 48000U;  // 1 s at 48 kHz

    // ---- 440 Hz sine ----------------------------------------------------
    {
        auto sine = make_sine(440.0, kFrames, 0.9F, 1);
        const AudioFeatureVector v =
            extract_audio_features(sine.data(), kFrames, 1, kRate);
        CHECK(all_finite(v));
        CHECK(v.spectralCentroidHz > 418.0 && v.spectralCentroidHz < 462.0);  // +-5%
        CHECK(v.estimatedPitchHz > 431.2 && v.estimatedPitchHz < 448.8);      // +-2%
        CHECK(v.pitchConfidence > 0.7);
        CHECK(v.harmonicEnergyRatio > 0.8);
        CHECK(v.spectralFlatness < 0.3);
        CHECK(v.rms > 0.5 && v.rms < 0.8);  // 0.9/sqrt(2) ~= 0.636
        CHECK(v.stereoWidth == 0.0);        // mono input
    }

    // ---- White noise -----------------------------------------------------
    {
        auto noise = make_noise(kFrames, 0.9F, 12345U);
        const AudioFeatureVector v =
            extract_audio_features(noise.data(), kFrames, 1, kRate);
        CHECK(all_finite(v));
        CHECK(v.spectralFlatness > 0.7);
        CHECK(v.pitchConfidence < 0.4);
        CHECK(v.estimatedPitchHz == 0.0);  // unpitched -> 0/0
    }

    // ---- Silence: every field 0.0 and finite ------------------------------
    {
        std::vector<float> silence(kFrames, 0.0F);
        const AudioFeatureVector v =
            extract_audio_features(silence.data(), kFrames, 1, kRate);
        CHECK(all_finite(v));
        CHECK(all_zero(v));
        std::vector<float> silenceStereo(kFrames * 2U, 0.0F);
        const AudioFeatureVector vs =
            extract_audio_features(silenceStereo.data(), kFrames, 2, kRate);
        CHECK(all_finite(vs));
        CHECK(all_zero(vs));
    }

    // ---- Impulse click: tiny attack ---------------------------------------
    {
        std::vector<float> click(kFrames, 0.0F);
        click[kFrames / 10U] = 1.0F;
        const AudioFeatureVector v =
            extract_audio_features(click.data(), kFrames, 1, kRate);
        CHECK(all_finite(v));
        CHECK(v.attackSeconds < 0.01);
        CHECK(v.peak == 1.0);
    }

    // ---- Stereo width ------------------------------------------------------
    {
        auto mono2 = make_sine(440.0, kFrames, 0.9F, 2, 1.0F);   // L == R
        const AudioFeatureVector w0 =
            extract_audio_features(mono2.data(), kFrames, 2, kRate);
        CHECK(all_finite(w0));
        CHECK(w0.stereoWidth < 0.05);

        auto wide = make_sine(440.0, kFrames, 0.9F, 2, -1.0F);  // L == -R
        const AudioFeatureVector w1 =
            extract_audio_features(wide.data(), kFrames, 2, kRate);
        CHECK(all_finite(w1));
        CHECK(w1.stereoWidth > 0.95);
    }

    // ---- Determinism: bit-identical vectors --------------------------------
    {
        auto sine = make_sine(440.0, kFrames, 0.9F, 1);
        const AudioFeatureVector a =
            extract_audio_features(sine.data(), kFrames, 1, kRate);
        const AudioFeatureVector b =
            extract_audio_features(sine.data(), kFrames, 1, kRate);
        CHECK(vec_equal(a, b));
        auto noise = make_noise(kFrames, 0.9F, 777U);
        const AudioFeatureVector c =
            extract_audio_features(noise.data(), kFrames, 1, kRate);
        const AudioFeatureVector d =
            extract_audio_features(noise.data(), kFrames, 1, kRate);
        CHECK(vec_equal(c, d));
    }

    // ---- Distance sanity ----------------------------------------------------
    {
        auto s440 = make_sine(440.0, kFrames, 0.9F, 1);
        auto s445 = make_sine(445.0, kFrames, 0.9F, 1);
        auto s880 = make_sine(880.0, kFrames, 0.9F, 1);
        const AudioFeatureVector a =
            extract_audio_features(s440.data(), kFrames, 1, kRate);
        const AudioFeatureVector b =
            extract_audio_features(s445.data(), kFrames, 1, kRate);
        const AudioFeatureVector c =
            extract_audio_features(s880.data(), kFrames, 1, kRate);
        CHECK(feature_distance(a, a) == 0.0);
        CHECK(feature_distance(a, b) >= 0.0);
        CHECK(feature_distance(a, c) > feature_distance(a, b));  // 880 farther than 445
        CHECK(feature_distance(a, c) == feature_distance(c, a));  // symmetry
        FeatureWeights noCentroid;
        noCentroid.spectralCentroidHz = 0.0;
        const double dFull = feature_distance(a, c);
        const double dNoCent = feature_distance(a, c, noCentroid);
        CHECK(dNoCent < dFull);  // weights respected
        CHECK(dNoCent > 0.0);    // still differs (pitch, rolloff, mel, ...)
        FeatureWeights none;
        none.rms = none.peak = none.spectralCentroidHz = none.spectralRolloffHz =
            none.spectralFlatness = none.zeroCrossingRate = none.estimatedPitchHz =
                none.pitchConfidence = none.harmonicEnergyRatio = none.attackSeconds =
                    none.decaySeconds = none.sustainLevel = none.releaseSeconds =
                        none.stereoWidth = none.melBands = 0.0;
        CHECK(feature_distance(a, c, none) == 0.0);  // no weights -> 0
    }

    // ---- Degenerate input: finite, all-zero ----------------------------------
    {
        std::vector<float> buf(100U, 0.5F);
        CHECK(all_zero(extract_audio_features(nullptr, 100U, 1, kRate)));
        CHECK(all_zero(extract_audio_features(buf.data(), 0U, 1, kRate)));
        CHECK(all_zero(extract_audio_features(buf.data(), 100U, 3, kRate)));
        CHECK(all_zero(extract_audio_features(buf.data(), 100U, 1, 0.0)));
        CHECK(all_zero(extract_audio_features(buf.data(), 100U, 0, kRate)));
        std::vector<float> nan(100U, 0.0F);
        nan[10] = std::numeric_limits<float>::quiet_NaN();
        nan[20] = std::numeric_limits<float>::infinity();
        const AudioFeatureVector v =
            extract_audio_features(nan.data(), 100U, 1, kRate);
        CHECK(all_finite(v));  // sanitized, never NaN/inf
        // Very short input (shorter than one FFT frame) still works.
        std::vector<float> tiny(64U, 0.5F);
        const AudioFeatureVector t =
            extract_audio_features(tiny.data(), 64U, 1, kRate);
        CHECK(all_finite(t));
        CHECK(t.rms > 0.0);
    }

    if (g_failures == 0) {
        std::printf("test_audio_synth_features: ALL PASS\n");
        return 0;
    }
    std::printf("test_audio_synth_features: %d FAILURES\n", g_failures);
    return 1;
}
