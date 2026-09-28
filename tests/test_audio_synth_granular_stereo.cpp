// Tests for Phase 4 granular Worker 3 (stereo scatter + CPU quality scaling):
// per-grain L/R source offsets (granular.hpp/cpp) and the granularQuality
// tiers. Synthetic sample data generated in-memory; no audio files on disk.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dve/audio/granular.hpp"
#include "dve/audio/synthesizer.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

constexpr std::uint32_t kRate = 48000U;
constexpr std::uint32_t kBankFrames = 4800U;  // 0.1 s at 48 kHz

bool near(float a, float b, float eps = 1.0e-5F) { return std::fabs(a - b) <= eps; }

void install_ramp_bank(std::vector<float>& bank) {
    bank.resize(kBankFrames);
    for (std::uint32_t i = 0; i < kBankFrames; ++i)
        bank[i] = static_cast<float>(i) / static_cast<float>(kBankFrames - 1U);
}

void install_sine_bank(std::vector<float>& bank, float frequencyHertz) {
    bank.resize(kBankFrames);
    for (std::uint32_t i = 0; i < kBankFrames; ++i)
        bank[i] = 0.9F * std::sin(2.0F * 3.14159265358979F * frequencyHertz *
                                  static_cast<float>(i) / static_cast<float>(kRate));
}

// Renders `frames` stereo samples; returns interleaved L/R.
std::vector<float> render_engine(GranularEngine& engine, const GranularSource& source,
                                 const GranularParameters& params, float positionMod,
                                 std::uint32_t frames, float* peakOut = nullptr) {
    std::vector<float> out(frames * 2U);
    float peak = 0.0F;
    for (std::uint32_t i = 0; i < frames; ++i) {
        const auto [left, right] = engine.render(source, params, positionMod);
        out[i * 2U] = left;
        out[i * 2U + 1U] = right;
        peak = std::max({peak, std::fabs(left), std::fabs(right)});
        if (!std::isfinite(left) || !std::isfinite(right)) break;
    }
    if (peakOut != nullptr) *peakOut = peak;
    return out;
}

double mean_abs_difference(const std::vector<float>& a, const std::vector<float>& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) sum += std::abs(static_cast<double>(a[i]) - b[i]);
    return sum / static_cast<double>(std::max<std::size_t>(1U, a.size()));
}

double rms(const std::vector<float>& audio) {
    double sum = 0.0;
    for (float sample : audio) sum += static_cast<double>(sample) * sample;
    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1U, audio.size())));
}

// Mono collapse of an interleaved stereo buffer.
std::vector<float> mono_sum(const std::vector<float>& stereo) {
    std::vector<float> mono(stereo.size() / 2U);
    for (std::size_t i = 0; i < mono.size(); ++i)
        mono[i] = 0.5F * (stereo[i * 2U] + stereo[i * 2U + 1U]);
    return mono;
}

// Mean |L - R| of an interleaved stereo buffer.
double mean_lr_difference(const std::vector<float>& stereo) {
    double sum = 0.0;
    const std::size_t frames = stereo.size() / 2U;
    for (std::size_t i = 0; i < frames; ++i)
        sum += std::abs(static_cast<double>(stereo[i * 2U]) - stereo[i * 2U + 1U]);
    return sum / static_cast<double>(std::max<std::size_t>(1U, frames));
}

GranularParameters stereo_params() {
    GranularParameters params;
    params.densityHz = 120.0F;
    params.durationMs = 90.0F;
    params.position01 = 0.5F;
    params.positionJitter01 = 0.1F;
    params.panScatter01 = 0.0F;       // isolate offset effects from pan
    params.reverseProbability01 = 0.0F;
    params.gain = 0.8F;
    return params;
}

}  // namespace

int main() {
    setbuf(stdout, nullptr);
    std::vector<float> bank;

    // Test 1: mono-compatibility — at width01 == 0 the L/R source offsets are
    // exactly 0, so with centered pan the channels are bit-identical.
    {
        install_sine_bank(bank, 220.0F);
        const GranularSource source{bank.data(), kBankFrames, kRate};
        GranularEngine engine;
        engine.set_sample_rate(kRate);
        engine.set_seed(1001U);
        GranularParameters params = stereo_params();
        params.width01 = 0.0F;
        const auto audio = render_engine(engine, source, params, 0.0F, 4800U);
        bool identical = true;
        for (std::size_t i = 0; i < audio.size(); i += 2U) {
            if (audio[i] != audio[i + 1U]) { identical = false; break; }
        }
        CHECK(identical);
        // The spawned grains carry exactly-zero offsets at width 0.
        for (const auto& grain : engine.grains()) {
            if (!grain.active) continue;
            CHECK(grain.sourceOffsetL == 0.0F);
            CHECK(grain.sourceOffsetR == 0.0F);
        }
        CHECK(engine.active_grain_count() > 0U);
    }

    // Test 2: width decorrelates L/R monotonically. On a ramp bank the
    // |L - R| difference is exactly linear in the offset (Catmull-Rom
    // reproduces linear content), so the mechanism itself must be monotone.
    {
        install_ramp_bank(bank);
        const GranularSource source{bank.data(), kBankFrames, kRate};
        const float widths[] = {0.0F, 0.25F, 0.5F, 0.75F, 1.0F};
        double diffs[5] = {};
        for (int w = 0; w < 5; ++w) {
            GranularEngine engine;
            engine.set_sample_rate(kRate);
            engine.set_seed(777U);
            GranularParameters params = stereo_params();
            params.width01 = widths[w];
            const auto audio = render_engine(engine, source, params, 0.0F, 9600U);
            diffs[w] = mean_lr_difference(audio);
        }
        CHECK(diffs[0] == 0.0);
        for (int w = 1; w < 5; ++w) {
            CHECK(diffs[w] > diffs[w - 1]);
        }
        // Sanity on musical content: width 1 audibly separates the channels.
        install_sine_bank(bank, 220.0F);
        const GranularSource sineSource{bank.data(), kBankFrames, kRate};
        auto lr_at = [&](float width) {
            GranularEngine engine;
            engine.set_sample_rate(kRate);
            engine.set_seed(777U);
            GranularParameters params = stereo_params();
            params.width01 = width;
            return mean_lr_difference(render_engine(engine, sineSource, params, 0.0F, 9600U));
        };
        CHECK(lr_at(0.0F) == 0.0);
        CHECK(lr_at(1.0F) > 1.0e-3);
        CHECK(lr_at(1.0F) > lr_at(0.5F));
    }

    // Test 3: mono-sum stability. The offsets are antisymmetric around the
    // grain base position, so on linear content the mono collapse is exact
    // (first-order terms cancel); on tonal content the error is second order
    // in the offset and must stay small at moderate width.
    {
        install_ramp_bank(bank);
        const GranularSource source{bank.data(), kBankFrames, kRate};
        auto render_mono = [&](float width) {
            GranularEngine engine;
            engine.set_sample_rate(kRate);
            engine.set_seed(31337U);
            GranularParameters params = stereo_params();
            params.width01 = width;
            return mono_sum(render_engine(engine, source, params, 0.0F, 9600U));
        };
        const auto mono0 = render_mono(0.0F);
        const auto monoHalf = render_mono(0.5F);
        // Ramp: antisymmetry gives exact mono preservation up to fp noise.
        // (A one-sided offset design would differ by ~width * slope here.)
        CHECK(mean_abs_difference(mono0, monoHalf) < 1.0e-4);

        install_sine_bank(bank, 220.0F);
        const GranularSource sineSource{bank.data(), kBankFrames, kRate};
        auto render_sine_mono = [&](float width) {
            GranularEngine engine;
            engine.set_sample_rate(kRate);
            engine.set_seed(31337U);
            GranularParameters params = stereo_params();
            params.width01 = width;
            return mono_sum(render_engine(engine, sineSource, params, 0.0F, 9600U));
        };
        const auto sineMono0 = render_sine_mono(0.0F);
        const auto sineMono03 = render_sine_mono(0.3F);
        const double allowed = 0.05 * rms(sineMono0);  // small: second-order effect
        const double measured = mean_abs_difference(sineMono0, sineMono03);
        std::printf("info: sine mono-sum drift at width 0.3: %.6f (rms %.6f, allowed %.6f)\n",
                    measured, rms(sineMono0), allowed);
        CHECK(measured < allowed);
    }

    // Test 4: CPU quality scaling — Eco admits fewer grains than High under
    // saturating density, and the throttled admissions are visible in the
    // counters (grainMisses, shared with no-source misses by design).
    {
        install_sine_bank(bank, 220.0F);
        const GranularSource source{bank.data(), kBankFrames, kRate};
        auto saturate = [&](FilterQuality quality) {
            GranularEngine engine;
            engine.set_sample_rate(kRate);
            engine.set_seed(555U);
            GranularParameters params = stereo_params();
            params.densityHz = 2000.0F;   // ~1000 concurrent wanted
            params.durationMs = 500.0F;
            params.granularQuality = quality;
            std::uint32_t maxActive = 0U;
            float peak = 0.0F;
            for (std::uint32_t i = 0; i < kRate; ++i) {
                const auto [left, right] = engine.render(source, params, 0.0F);
                peak = std::max({peak, std::fabs(left), std::fabs(right)});
                maxActive = std::max(maxActive, engine.active_grain_count());
                if (!std::isfinite(left) || !std::isfinite(right)) break;
            }
            return std::make_pair(maxActive, engine.counters());
        };
        const auto [highMax, highCounters] = saturate(FilterQuality::High);
        const auto [ecoMax, ecoCounters] = saturate(FilterQuality::Eco);
        std::printf("info: max active grains — High %u, Eco %u\n", highMax, ecoMax);
        CHECK(highMax == GranularEngine::kMaxGrains);  // pool saturated
        CHECK(ecoMax <= GranularEngine::kEcoMaxActiveGrains);
        CHECK(ecoMax < highMax);
        CHECK(ecoCounters.grainMisses > 0U);  // throttled admissions visible
        CHECK(ecoCounters.admittedGrains < ecoCounters.requestedGrains);
        CHECK(highCounters.grainMisses == 0U);  // High never throttles
        CHECK(highCounters.admittedGrains == highCounters.requestedGrains);
        // Standard behaves like the wave-1 core: full pool, no throttling.
        const auto [stdMax, stdCounters] = saturate(FilterQuality::Standard);
        CHECK(stdMax == GranularEngine::kMaxGrains);
        CHECK(stdCounters.grainMisses == 0U);
        // Offline matches High (documented: for render, not realtime).
        const auto [offMax, offCounters] = saturate(FilterQuality::Offline);
        CHECK(offMax == highMax);
        CHECK(offCounters.grainMisses == 0U);
    }

    // Test 5: Eco forces the cheapest envelope (Triangle) regardless of the
    // requested shape; other tiers honor the requested shape.
    {
        install_sine_bank(bank, 220.0F);
        const GranularSource source{bank.data(), kBankFrames, kRate};
        auto spawned_shape = [&](FilterQuality quality) {
            GranularEngine engine;
            engine.set_sample_rate(kRate);
            engine.set_seed(2024U);
            GranularParameters params = stereo_params();
            params.densityHz = 4000.0F;
            params.envelopeShape = GranularEnvelopeShape::Hann;
            params.granularQuality = quality;
            const Grain* found = nullptr;
            for (int s = 0; s < 64 && found == nullptr; ++s) {
                engine.render(source, params, 0.0F);
                for (const auto& grain : engine.grains()) {
                    if (grain.active) { found = &grain; break; }
                }
            }
            CHECK(found != nullptr);
            return found != nullptr ? found->envelopeShape : GranularEnvelopeShape::Hann;
        };
        CHECK(spawned_shape(FilterQuality::Eco) == GranularEnvelopeShape::Triangle);
        CHECK(spawned_shape(FilterQuality::Standard) == GranularEnvelopeShape::Hann);
        CHECK(spawned_shape(FilterQuality::High) == GranularEnvelopeShape::Hann);
        CHECK(spawned_shape(FilterQuality::Offline) == GranularEnvelopeShape::Hann);
    }

    // Test 6: quality switches mid-stream don't click — no NaN/inf, bounded.
    {
        install_sine_bank(bank, 330.0F);
        const GranularSource source{bank.data(), kBankFrames, kRate};
        GranularEngine engine;
        engine.set_sample_rate(kRate);
        engine.set_seed(909U);
        GranularParameters params = stereo_params();
        params.densityHz = 300.0F;
        float peak = 0.0F;
        const FilterQuality sequence[] = {FilterQuality::Standard, FilterQuality::Eco,
                                          FilterQuality::High, FilterQuality::Eco,
                                          FilterQuality::Offline, FilterQuality::Standard};
        for (FilterQuality quality : sequence) {
            params.granularQuality = quality;
            for (std::uint32_t i = 0; i < 2400U; ++i) {
                const auto [left, right] = engine.render(source, params, 0.0F);
                CHECK(std::isfinite(left) && std::isfinite(right));
                peak = std::max({peak, std::fabs(left), std::fabs(right)});
            }
        }
        CHECK(peak < 10.0F);
        CHECK(peak > 0.001F);  // still producing audio through the switches
    }

    // Test 7: determinism with fixed seed — stereo offsets included.
    {
        install_sine_bank(bank, 220.0F);
        const GranularSource source{bank.data(), kBankFrames, kRate};
        auto run = [&]() {
            GranularEngine engine;
            engine.set_sample_rate(kRate);
            engine.set_seed(424242U);
            GranularParameters params = stereo_params();
            params.width01 = 1.0F;
            params.panScatter01 = 0.6F;
            params.reverseProbability01 = 0.2F;
            return render_engine(engine, source, params, 0.15F, 4096U);
        };
        const auto first = run();
        const auto second = run();
        CHECK(first == second);
    }

    if (g_failures == 0) std::printf("ALL GRANULAR STEREO/QUALITY TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
