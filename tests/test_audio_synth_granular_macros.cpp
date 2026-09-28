// Tests for the Phase 4 granular wave-2 work (Worker 2):
//   1. Grain envelope tuning — all four GranularEnvelopeShape windows have
//      endpoints ~= 0 (no clicks), peak ~= 1, and their documented characters
//      (Hann symmetry, Planck flat-top, exponential-decay fast attack).
//   2. Musical macro layer — apply_granular_macros() is a pure function; each
//      macro's mapping is verified directly and through engine spawns.
// Synthetic sample data is generated in-memory; no audio files on disk.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "dve/audio/granular.hpp"

using namespace dve::audio;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

constexpr std::uint32_t kRate = 48000U;
constexpr std::uint32_t kBankFrames = 4800U;  // 0.1 s at 48 kHz

bool near(float a, float b, float eps = 1.0e-5F) { return std::fabs(a - b) <= eps; }

void install_sine_bank(std::vector<float>& bank, float frequencyHertz) {
    bank.resize(kBankFrames);
    for (std::uint32_t i = 0; i < kBankFrames; ++i)
        bank[i] = 0.9F * std::sin(2.0F * 3.14159265358979F * frequencyHertz *
                                  static_cast<float>(i) / static_cast<float>(kRate));
}

// Base params with every macro neutralized (cloud at its x1.0 point 0.25,
// width at its identity point 0.5, additive macros at 0).
GranularParameters neutral_params() {
    GranularParameters params;
    params.densityHz = 40.0F;
    params.durationMs = 120.0F;
    params.gain = 0.8F;
    params.cloud01 = 0.25F;  // 0.25 + 3 x 0.25 = 1.0 -> density unchanged
    params.scatter01 = 0.0F;
    params.dust01 = 0.0F;
    params.freeze01 = 0.0F;
    params.smear01 = 0.0F;
    params.width01 = 0.5F;  // 0.2 + 1.6 x 0.5 = 1.0 -> pan scatter unchanged
    return params;
}

std::vector<float> render_engine(GranularEngine& engine, const GranularSource& source,
                                 const GranularParameters& params, float positionMod,
                                 std::uint32_t frames) {
    std::vector<float> out(frames * 2U);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const auto [left, right] = engine.render(source, params, positionMod);
        out[i * 2U] = left;
        out[i * 2U + 1U] = right;
        if (!std::isfinite(left) || !std::isfinite(right)) break;
    }
    return out;
}

double stddev(const std::vector<float>& values) {
    if (values.empty()) return 0.0;
    double mean = 0.0;
    for (float v : values) mean += v;
    mean /= static_cast<double>(values.size());
    double var = 0.0;
    for (float v : values) var += (v - mean) * (v - mean);
    return std::sqrt(var / static_cast<double>(values.size()));
}

double mean_of(const std::vector<float>& values) {
    if (values.empty()) return 0.0;
    double sum = 0.0;
    for (float v : values) sum += v;
    return sum / static_cast<double>(values.size());
}

// Spawns a pool of grains and recovers each grain's spawn-time parameters.
// Recovered spawn position = current position - age x increment (exact, since
// the read head advances linearly).
struct SpawnedStats {
    std::vector<float> spawnPositionFrames;
    std::vector<float> pitchIncrement;
    std::vector<float> pan;
    std::vector<float> durationFrames;
    std::vector<GranularEnvelopeShape> shapes;
    std::uint32_t grainCount{0};
};

SpawnedStats collect_spawns(const GranularParameters& params, std::uint32_t seed,
                            std::uint32_t renderSamples) {
    std::vector<float> bank;
    install_sine_bank(bank, 220.0F);
    const GranularSource source{bank.data(), kBankFrames, kRate};
    GranularEngine engine;
    engine.set_sample_rate(kRate);
    engine.set_seed(seed);
    render_engine(engine, source, params, 0.0F, renderSamples);
    SpawnedStats stats;
    for (const auto& grain : engine.grains()) {
        if (!grain.active) continue;
        stats.spawnPositionFrames.push_back(grain.sourcePositionFrames -
                                            grain.ageFrames * grain.pitchIncrement);
        stats.pitchIncrement.push_back(grain.pitchIncrement);
        stats.pan.push_back(grain.pan);
        stats.durationFrames.push_back(grain.durationFrames);
        stats.shapes.push_back(grain.envelopeShape);
    }
    stats.grainCount = static_cast<std::uint32_t>(stats.spawnPositionFrames.size());
    return stats;
}

}  // namespace

int main() {
    setbuf(stdout, nullptr);

    // Test 1: envelope endpoints ~= 0 (no clicks) and peaks ~= 1.
    {
        using S = GranularEnvelopeShape;
        for (int i = 0; i < 4; ++i) {
            const auto shape = static_cast<S>(i);
            CHECK(GranularEngine::envelope_value(shape, 0.0F) < 5.0e-3F);
            CHECK(GranularEngine::envelope_value(shape, 1.0F) < 5.0e-3F);
            float peak = 0.0F;
            for (int s = 0; s <= 1000; ++s)
                peak = std::max(peak, GranularEngine::envelope_value(shape, s / 1000.0F));
            CHECK(near(peak, 1.0F, 1.0e-3F));
        }
        // Exact peaks at their documented locations.
        CHECK(near(GranularEngine::envelope_value(S::Hann, 0.5F), 1.0F));
        CHECK(near(GranularEngine::envelope_value(S::Triangle, 0.5F), 1.0F));
        CHECK(near(GranularEngine::envelope_value(S::ExponentialDecay, 0.1F), 1.0F, 1.0e-5F));
        CHECK(near(GranularEngine::envelope_value(S::PlanckTaper, 0.5F), 1.0F));
    }

    // Test 2: Hann and Triangle are symmetric.
    {
        using S = GranularEnvelopeShape;
        for (int s = 0; s <= 50; ++s) {
            const float x = s / 100.0F;
            CHECK(near(GranularEngine::envelope_value(S::Hann, x),
                       GranularEngine::envelope_value(S::Hann, 1.0F - x), 1.0e-6F));
            CHECK(near(GranularEngine::envelope_value(S::Triangle, x),
                       GranularEngine::envelope_value(S::Triangle, 1.0F - x), 1.0e-6F));
        }
    }

    // Test 3: Planck taper — flat top with a ~10% smooth taper on each end.
    {
        using S = GranularEnvelopeShape;
        CHECK(near(GranularEngine::envelope_value(S::PlanckTaper, 0.3F), 1.0F, 1.0e-6F));
        CHECK(near(GranularEngine::envelope_value(S::PlanckTaper, 0.5F), 1.0F, 1.0e-6F));
        CHECK(near(GranularEngine::envelope_value(S::PlanckTaper, 0.7F), 1.0F, 1.0e-6F));
        const float taper = GranularEngine::envelope_value(S::PlanckTaper, 0.05F);
        CHECK(taper > 0.0F && taper < 1.0F);  // inside the taper zone: partial
        // Monotonic rise through the taper zone, smooth (C-inf) — no kinks.
        float prev = 0.0F;
        for (int s = 1; s <= 10; ++s) {
            const float v = GranularEngine::envelope_value(S::PlanckTaper, s / 100.0F);
            CHECK(v >= prev);
            prev = v;
        }
        CHECK(near(GranularEngine::envelope_value(S::PlanckTaper, 0.10F), 1.0F, 1.0e-3F));
    }

    // Test 4: exponential decay — fast attack, smooth decaying tail.
    {
        using S = GranularEnvelopeShape;
        // Fast attack: monotonic rise from 0 to the peak at 10% of the grain.
        float prev = 0.0F;
        for (int s = 1; s <= 10; ++s) {
            const float v = GranularEngine::envelope_value(S::ExponentialDecay, s / 100.0F);
            CHECK(v >= prev);
            prev = v;
        }
        // The peak really is at 0.1: neighbors are lower.
        const float peak = GranularEngine::envelope_value(S::ExponentialDecay, 0.1F);
        CHECK(GranularEngine::envelope_value(S::ExponentialDecay, 0.08F) < peak);
        CHECK(GranularEngine::envelope_value(S::ExponentialDecay, 0.12F) < peak);
        // Smooth tail: monotonic fall after the peak, inaudible at the end.
        prev = peak;
        for (int s = 11; s <= 100; ++s) {
            const float v = GranularEngine::envelope_value(S::ExponentialDecay, s / 100.0F);
            CHECK(v <= prev + 1.0e-7F);
            prev = v;
        }
        CHECK(GranularEngine::envelope_value(S::ExponentialDecay, 1.0F) < 5.0e-3F);
        // Bounded in [0, 1] everywhere.
        for (int s = 0; s <= 1000; ++s) {
            const float v = GranularEngine::envelope_value(S::ExponentialDecay, s / 1000.0F);
            CHECK(v >= -1.0e-6F && v <= 1.0F + 1.0e-6F);
        }
    }

    // Test 5: the render path honors per-grain envelope shape selection.
    {
        std::vector<float> bank;
        install_sine_bank(bank, 220.0F);
        const GranularSource source{bank.data(), kBankFrames, kRate};
        for (auto shape : {GranularEnvelopeShape::Triangle, GranularEnvelopeShape::PlanckTaper}) {
            GranularEngine engine;
            engine.set_sample_rate(kRate);
            engine.set_seed(77U);
            GranularParameters params = neutral_params();
            params.densityHz = 4000.0F;
            params.durationMs = 10.0F;
            params.positionJitter01 = 0.0F;
            params.panScatter01 = 0.0F;
            params.reverseProbability01 = 0.0F;
            params.envelopeShape = shape;
            const Grain* spawned = nullptr;
            for (int s = 0; s < 64 && spawned == nullptr; ++s) {
                engine.render(source, params, 0.0F);
                for (const auto& grain : engine.grains()) {
                    if (grain.active) {
                        spawned = &grain;
                        break;
                    }
                }
            }
            CHECK(spawned != nullptr);
            CHECK(spawned->envelopeShape == shape);
        }
    }

    // Test 6: macro layer is a no-op where the spec says it is — the additive
    // macros (scatter/dust/freeze/smear) at 0, width at its 0.5 identity.
    // Cloud at 0 is a real (sparse) setting, not neutral: density follows the
    // documented x0.25 mapping while duration is unstretched there.
    {
        GranularParameters params = neutral_params();
        params.cloud01 = 0.0F;
        params.densityHz = 63.0F;
        params.durationMs = 97.0F;
        params.pitchSemitones = 4.0F;
        params.position01 = 0.35F;
        params.positionJitter01 = 0.22F;
        params.panScatter01 = 0.41F;
        params.gain = 0.9F;
        params.reverseProbability01 = 0.15F;
        params.envelopeShape = GranularEnvelopeShape::Triangle;
        const GranularEffectiveParams eff = apply_granular_macros(params, 0.12F);
        CHECK(near(eff.densityHz, params.densityHz * 0.25F));  // cloud=0 sparse point
        CHECK(near(eff.durationMs, params.durationMs));       // no stretch at cloud=0
        CHECK(near(eff.pitchSemitones, params.pitchSemitones));
        CHECK(near(eff.position01, 0.35F + 0.12F));  // positionMod folded in
        CHECK(near(eff.positionJitter01, params.positionJitter01));
        CHECK(near(eff.panScatter01, params.panScatter01));
        CHECK(near(eff.gain, params.gain));
        CHECK(near(eff.reverseProbability01, params.reverseProbability01));
        CHECK(eff.envelopeShape == params.envelopeShape);
        CHECK(eff.smearPitchSemitones == 0.0F);
        CHECK(eff.smearDurationSpread01 == 0.0F);
        CHECK(eff.dustTransientOverride01 == 0.0F);
    }

    // Test 7: cloud mapping — density x(0.25 + 3c), duration x(1 + 0.5c).
    {
        GranularParameters params = neutral_params();
        params.densityHz = 40.0F;
        params.durationMs = 100.0F;
        params.cloud01 = 0.0F;
        CHECK(near(apply_granular_macros(params, 0.0F).densityHz, 10.0F));  // x0.25
        params.cloud01 = 0.5F;
        CHECK(near(apply_granular_macros(params, 0.0F).densityHz, 70.0F));  // x1.75
        params.cloud01 = 1.0F;
        const auto eff = apply_granular_macros(params, 0.0F);
        CHECK(near(eff.densityHz, 130.0F));    // x3.25
        CHECK(near(eff.durationMs, 150.0F));  // x1.5 overlap stretch
        // Density clamp survives the macro scaling.
        params.densityHz = 4000.0F;
        CHECK(apply_granular_macros(params, 0.0F).densityHz <= 4000.0F);
        // Out-of-range macro knobs are clamped, never blow up the mapping.
        params.cloud01 = 5.0F;
        CHECK(near(apply_granular_macros(params, 0.0F).densityHz, 4000.0F));
    }

    // Test 8: cloud raises the admitted-grain count over a fixed render.
    {
        std::vector<float> bank;
        install_sine_bank(bank, 220.0F);
        const GranularSource source{bank.data(), kBankFrames, kRate};
        auto admitted = [&](float cloud) {
            GranularEngine engine;
            engine.set_sample_rate(kRate);
            engine.set_seed(1001U);
            GranularParameters params = neutral_params();
            params.densityHz = 40.0F;
            params.cloud01 = cloud;
            render_engine(engine, source, params, 0.0F, kRate / 10U);  // 0.1 s
            return engine.drain_counters().admittedGrains;
        };
        const auto sparse = admitted(0.0F);  // 10 Hz -> ~1 grain
        const auto dense = admitted(1.0F);   // 130 Hz -> ~13 grains
        CHECK(sparse <= 2U);
        CHECK(dense >= 10U);
        CHECK(dense > sparse);
    }

    // Test 9: scatter widens the position distribution.
    {
        GranularParameters params = neutral_params();
        params.densityHz = 4000.0F;
        params.cloud01 = 0.0F;  // isolate: eff density 1000 Hz
        params.durationMs = 400.0F;
        params.position01 = 0.5F;
        params.positionJitter01 = 0.0F;
        params.reverseProbability01 = 0.0F;
        params.scatter01 = 0.0F;
        const auto tight = collect_spawns(params, 555U, 3200U);
        params.scatter01 = 1.0F;
        const auto wide = collect_spawns(params, 555U, 3200U);
        CHECK(tight.grainCount > 32U && wide.grainCount > 32U);
        const double tightStd = stddev(tight.spawnPositionFrames);
        const double wideStd = stddev(wide.spawnPositionFrames);
        CHECK(tightStd < 1.0);  // no jitter at all: identical spawn positions
        const double lastFrame = static_cast<double>(kBankFrames - 1U);
        // scatter=1 adds jitter 0.5 -> uniform +/-0.5 x lastFrame: std ~= 0.29 x lastFrame
        CHECK(wideStd > 0.2 * lastFrame);
        CHECK(wideStd > 100.0 * tightStd + 1.0);
    }

    // Test 10: dust shortens mean duration, compensates gain, biases envelopes.
    {
        GranularParameters params = neutral_params();
        params.densityHz = 4000.0F;
        params.cloud01 = 0.0F;
        params.durationMs = 100.0F;
        params.reverseProbability01 = 0.0F;
        params.smear01 = 0.0F;
        params.dust01 = 0.0F;
        const auto clean = collect_spawns(params, 777U, 3200U);
        params.dust01 = 1.0F;
        const auto dusty = collect_spawns(params, 777U, 3200U);
        CHECK(clean.grainCount > 0U && dusty.grainCount > 0U);
        const double ratio = mean_of(dusty.durationFrames) / mean_of(clean.durationFrames);
        CHECK(ratio > 0.09 && ratio < 0.11);  // x(1 - 0.9) = x0.1
        // 5 ms floor: a 10 ms grain at full dust lands on the floor, not below.
        params.durationMs = 10.0F;
        params.dust01 = 1.0F;
        const auto floored = collect_spawns(params, 777U, 3200U);
        const float floorFrames = 5.0F * 0.001F * static_cast<float>(kRate);
        for (float d : floored.durationFrames) CHECK(d >= floorFrames - 1.0e-3F);
        // Pure-function checks: gain compensation and envelope-override probability.
        params.durationMs = 100.0F;
        const auto eff = apply_granular_macros(params, 0.0F);
        CHECK(near(eff.durationMs, 10.0F));
        CHECK(near(eff.gain, params.gain * 1.5F));
        CHECK(near(eff.dustTransientOverride01, 1.0F));
        // Engine behavior: full dust forces every grain to ExponentialDecay.
        params.envelopeShape = GranularEnvelopeShape::Hann;
        const auto dustyShapes = collect_spawns(params, 777U, 3200U);
        CHECK(!dustyShapes.shapes.empty());
        for (auto shape : dustyShapes.shapes)
            CHECK(shape == GranularEnvelopeShape::ExponentialDecay);
        // No dust keeps the preset's shape on every grain.
        params.dust01 = 0.0F;
        const auto cleanShapes = collect_spawns(params, 777U, 3200U);
        for (auto shape : cleanShapes.shapes) CHECK(shape == GranularEnvelopeShape::Hann);
    }

    // Test 11: freeze converges positions on the freeze point and kills jitter.
    {
        GranularParameters params = neutral_params();
        params.densityHz = 4000.0F;
        params.cloud01 = 0.0F;
        params.durationMs = 400.0F;
        params.position01 = 0.2F;
        params.positionJitter01 = 0.3F;
        params.scatter01 = 0.5F;  // would add 0.25 jitter without freeze
        params.reverseProbability01 = 0.0F;
        params.freezePosition01 = 0.8F;
        const double lastFrame = static_cast<double>(kBankFrames - 1U);
        params.freeze01 = 1.0F;
        const auto frozen = collect_spawns(params, 888U, 1200U);
        CHECK(frozen.grainCount > 0U);
        for (float pos : frozen.spawnPositionFrames)
            CHECK(std::fabs(pos - 0.8 * lastFrame) < 2.0);
        params.freeze01 = 0.0F;
        const auto free = collect_spawns(params, 888U, 1200U);
        CHECK(stddev(free.spawnPositionFrames) > 100.0);  // jitter alive
        // Half freeze lands halfway between base position and freeze point.
        params.freeze01 = 0.5F;
        params.positionJitter01 = 0.0F;
        params.scatter01 = 0.0F;
        const auto half = collect_spawns(params, 888U, 1200U);
        CHECK(half.grainCount > 0U);
        CHECK(near(static_cast<float>(mean_of(half.spawnPositionFrames)),
                   static_cast<float>(0.5 * lastFrame), 2.0F));
        // Pure-function: positionMod is folded in before the freeze lerp.
        const auto eff = apply_granular_macros(params, 0.1F);
        CHECK(near(eff.position01, (0.2F + 0.1F) * 0.5F + 0.8F * 0.5F));
        CHECK(near(eff.positionJitter01, 0.0F));
    }

    // Test 12: smear widens pitch and duration dispersion per grain.
    {
        GranularParameters params = neutral_params();
        params.densityHz = 4000.0F;
        params.cloud01 = 0.0F;
        params.durationMs = 400.0F;
        params.pitchSemitones = 0.0F;
        params.reverseProbability01 = 0.0F;
        params.smear01 = 0.0F;
        const auto clean = collect_spawns(params, 999U, 3200U);
        params.smear01 = 1.0F;
        const auto smeared = collect_spawns(params, 999U, 3200U);
        CHECK(clean.grainCount > 32U && smeared.grainCount > 32U);
        CHECK(stddev(clean.pitchIncrement) < 1.0e-6);  // all exactly 1.0
        CHECK(stddev(clean.durationFrames) < 1.0e-3F);
        // +/-12 semitones uniform: pitch ratios spread well beyond a semitone.
        CHECK(stddev(smeared.pitchIncrement) > 0.15);
        for (float inc : smeared.pitchIncrement) {
            CHECK(inc >= std::exp2(-12.0F) - 1.0e-4F && inc <= std::exp2(12.0F) + 1.0e-4F);
        }
        // Duration multiplier 1 +/- 0.75 around the 400 ms base.
        const double baseFrames = 400.0 * 0.001 * kRate;
        CHECK(stddev(smeared.durationFrames) > 0.1 * baseFrames);
        for (float d : smeared.durationFrames) {
            CHECK(d >= 0.25 * baseFrames - 1.0 && d <= 1.75 * baseFrames + 1.0);
        }
        // Pure-function ranges.
        const auto eff = apply_granular_macros(params, 0.0F);
        CHECK(near(eff.smearPitchSemitones, 12.0F));
        CHECK(near(eff.smearDurationSpread01, 0.75F));
    }

    // Test 13: width widens the per-grain pan distribution.
    {
        GranularParameters params = neutral_params();
        params.densityHz = 4000.0F;
        params.cloud01 = 0.0F;
        params.durationMs = 400.0F;
        params.panScatter01 = 0.3F;
        params.reverseProbability01 = 0.0F;
        params.width01 = 0.0F;  // x0.2 -> pan range +/-0.06
        const auto narrow = collect_spawns(params, 1111U, 3200U);
        params.width01 = 1.0F;  // x1.8 -> pan range +/-0.54
        const auto wide = collect_spawns(params, 1111U, 3200U);
        CHECK(narrow.grainCount > 32U && wide.grainCount > 32U);
        const double narrowStd = stddev(narrow.pan);
        const double wideStd = stddev(wide.pan);
        CHECK(narrowStd > 0.0 && wideStd > 3.0 * narrowStd);
        for (float p : wide.pan) CHECK(p >= -0.54F - 1.0e-4F && p <= 0.54F + 1.0e-4F);
        // Width at 0.5 is the identity on pan scatter.
        params.width01 = 0.5F;
        CHECK(near(apply_granular_macros(params, 0.0F).panScatter01, 0.3F));
    }

    // Test 14: determinism — fixed seed + active macros renders bit-identical output.
    {
        std::vector<float> bank;
        install_sine_bank(bank, 330.0F);
        const GranularSource source{bank.data(), kBankFrames, kRate};
        auto run = [&]() {
            GranularEngine engine;
            engine.set_sample_rate(kRate);
            engine.set_seed(424242U);
            GranularParameters params = neutral_params();
            params.densityHz = 90.0F;
            params.durationMs = 85.0F;
            params.pitchSemitones = 3.0F;
            params.positionJitter01 = 0.4F;
            params.panScatter01 = 0.7F;
            params.reverseProbability01 = 0.2F;
            params.envelopeShape = GranularEnvelopeShape::PlanckTaper;
            params.cloud01 = 0.8F;
            params.scatter01 = 0.6F;
            params.dust01 = 0.4F;
            params.freeze01 = 0.3F;
            params.freezePosition01 = 0.7F;
            params.smear01 = 0.5F;
            params.width01 = 0.9F;
            return render_engine(engine, source, params, 0.15F, 4096U);
        };
        const auto first = run();
        const auto second = run();
        CHECK(first == second);
    }

    // Test 15: the macro layer is actually wired in — macros change the render.
    {
        std::vector<float> bank;
        install_sine_bank(bank, 220.0F);
        const GranularSource source{bank.data(), kBankFrames, kRate};
        auto render_with = [&](std::function<void(GranularParameters&)> tweak) {
            GranularEngine engine;
            engine.set_sample_rate(kRate);
            engine.set_seed(2024U);
            GranularParameters params = neutral_params();
            params.densityHz = 60.0F;
            params.durationMs = 80.0F;
            tweak(params);
            return render_engine(engine, source, params, 0.0F, 4096U);
        };
        const auto base = render_with([](GranularParameters&) {});
        double diff = 0.0;
        for (float v : base) diff += std::fabs(v);
        CHECK(diff > 0.0);  // base render is non-silent
        auto differs = [&](std::function<void(GranularParameters&)> tweak) {
            const auto other = render_with(tweak);
            double d = 0.0;
            for (std::size_t i = 0; i < base.size(); ++i)
                d += std::fabs(static_cast<double>(base[i]) - other[i]);
            return d / static_cast<double>(base.size());
        };
        CHECK(differs([](GranularParameters& p) { p.cloud01 = 1.0F; }) > 1.0e-4);
        CHECK(differs([](GranularParameters& p) { p.dust01 = 1.0F; }) > 1.0e-4);
        CHECK(differs([](GranularParameters& p) { p.freeze01 = 1.0F; }) > 1.0e-4);
        CHECK(differs([](GranularParameters& p) { p.smear01 = 1.0F; }) > 1.0e-4);
    }

    if (g_failures == 0) std::printf("ALL GRANULAR MACRO TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
