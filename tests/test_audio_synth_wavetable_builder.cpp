// Tests for audio-to-wavetable builder.
#include <cmath>
#include <cstdio>
#include <vector>

#include "dve/audio/wavetable_builder.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    // Generate a test tone: 220 Hz sine with harmonics.
    const float sampleRate = 44100.0F;
    const float freq = 220.0F;
    const std::size_t count = 44100; // 1 second
    std::vector<float> audio(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / sampleRate;
        audio[i] = std::sin(6.2831853F * freq * t) * 0.6F +
                   std::sin(6.2831853F * 2 * freq * t) * 0.3F +
                   std::sin(6.2831853F * 3 * freq * t) * 0.1F;
    }

    // Test pitch estimation.
    {
        const float est = estimate_fundamental_frequency(audio.data(), count, sampleRate);
        std::printf("estimated: %.1f Hz (expected %.1f)\n", est, freq);
        CHECK(std::abs(est - freq) < 5.0F);
    }

    // Test full build.
    {
        WavetableBuildOptions opts;
        opts.sampleRate = sampleRate;
        opts.targetFrames = 16;
        auto result = build_wavetable_from_audio("Test Tone", audio.data(), count, opts);
        CHECK(result.success);
        CHECK(result.table.valid());
        CHECK(result.cyclesFound > 0);
        CHECK(result.cyclesKept > 0);
        CHECK(result.cyclesKept <= 16);
        std::printf("cycles: found=%zu kept=%zu freq=%.1f\n",
                    result.cyclesFound, result.cyclesKept, result.estimatedFrequencyHertz);
        // The built table should produce sound.
        float sum = 0.0F;
        for (int i = 0; i < 100; ++i)
            sum += std::abs(sample_wavetable(result.table, static_cast<float>(i)/100.0F, 0.5F, 0));
        CHECK(sum > 1.0F);
    }

    // Test failure modes.
    {
        auto r1 = build_wavetable_from_audio("empty", nullptr, 0);
        CHECK(!r1.success);
        // Noise (no clear pitch) should fail gracefully.
        std::vector<float> noise(44100);
        for (std::size_t i = 0; i < noise.size(); ++i)
            noise[i] = static_cast<float>(rand()) / RAND_MAX * 2.0F - 1.0F;
        auto r2 = build_wavetable_from_audio("noise", noise.data(), noise.size());
        // May succeed or fail; just shouldn't crash.
        std::printf("noise build: success=%d\n", r2.success);
    }

    if (g_failures == 0) std::printf("wavetable builder tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
