// Tests for the Phase 4 granular generator core (SYN-014):
// include/dve/audio/granular.hpp + src/audio/granular.cpp.
// Synthetic sample data is generated in-memory; no audio files on disk.
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

GranularParameters default_params() {
    GranularParameters params;
    params.densityHz = 40.0F;
    params.durationMs = 120.0F;
    params.gain = 0.8F;
    return params;
}

// Renders `frames` samples through an engine; optionally records the peak.
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

const Grain* find_active_grain(const GranularEngine& engine) {
    for (const auto& grain : engine.grains()) {
        if (grain.active) return &grain;
    }
    return nullptr;
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

}  // namespace

int main() {
    setbuf(stdout, nullptr);
    std::vector<float> bank;

    // Test 1: envelope shapes — endpoints, midpoint, boundedness.
    {
        CHECK(near(GranularEngine::envelope_value(GranularEnvelopeShape::Hann, 0.0F), 0.0F));
        CHECK(near(GranularEngine::envelope_value(GranularEnvelopeShape::Hann, 0.5F), 1.0F));
        CHECK(near(GranularEngine::envelope_value(GranularEnvelopeShape::Hann, 1.0F), 0.0F));
        CHECK(near(GranularEngine::envelope_value(GranularEnvelopeShape::Triangle, 0.0F), 0.0F));
        CHECK(near(GranularEngine::envelope_value(GranularEnvelopeShape::Triangle, 0.5F), 1.0F));
        CHECK(near(GranularEngine::envelope_value(GranularEnvelopeShape::Triangle, 1.0F), 0.0F));
        CHECK(near(GranularEngine::envelope_value(GranularEnvelopeShape::ExponentialDecay, 0.0F), 0.0F));
        // Tuned gamma window: peak 1 at 10% of the grain, smooth tail to ~1.2e-3.
        CHECK(near(GranularEngine::envelope_value(GranularEnvelopeShape::ExponentialDecay, 0.1F), 1.0F, 1.0e-5F));
        CHECK(GranularEngine::envelope_value(GranularEnvelopeShape::ExponentialDecay, 1.0F) < 5.0e-3F);
        CHECK(near(GranularEngine::envelope_value(GranularEnvelopeShape::PlanckTaper, 0.0F), 0.0F, 1.0e-3F));
        CHECK(near(GranularEngine::envelope_value(GranularEnvelopeShape::PlanckTaper, 0.5F), 1.0F));
        CHECK(near(GranularEngine::envelope_value(GranularEnvelopeShape::PlanckTaper, 1.0F), 0.0F, 1.0e-3F));
        for (int shape = 0; shape < 4; ++shape) {
            for (int i = 0; i <= 100; ++i) {
                const float v = GranularEngine::envelope_value(
                    static_cast<GranularEnvelopeShape>(shape), static_cast<float>(i) / 100.0F);
                CHECK(v >= -1.0e-6F && v <= 1.0F + 1.0e-6F);
            }
        }
    }

    // Test 2: cubic interpolation sanity — constant and linear sources.
    {
        install_sine_bank(bank, 220.0F);
        std::vector<float> constant(kBankFrames, 0.75F);
        CHECK(near(GranularEngine::cubic_sample(constant.data(), kBankFrames, 12.34F), 0.75F, 1.0e-6F));
        CHECK(near(GranularEngine::cubic_sample(constant.data(), kBankFrames, 0.0F), 0.75F, 1.0e-6F));
        std::vector<float> ramp;
        install_ramp_bank(ramp);
        // Catmull-Rom reproduces a linear ramp exactly (away from the clamped edges).
        CHECK(near(GranularEngine::cubic_sample(ramp.data(), kBankFrames, 1234.5F),
                   1234.5F / static_cast<float>(kBankFrames - 1U), 1.0e-5F));
        // Out-of-range positions clamp to the edge frames, never read OOB.
        CHECK(near(GranularEngine::cubic_sample(ramp.data(), kBankFrames, -100.0F), ramp.front(), 1.0e-6F));
        CHECK(near(GranularEngine::cubic_sample(ramp.data(), kBankFrames, 1.0e6F), ramp.back(), 1.0e-6F));
        CHECK(GranularEngine::cubic_sample(nullptr, 0U, 3.0F) == 0.0F);
    }

    // Test 3: semitone -> ratio math.
    {
        CHECK(near(GranularEngine::semitones_to_ratio(0.0F), 1.0F, 1.0e-6F));
        CHECK(near(GranularEngine::semitones_to_ratio(12.0F), 2.0F, 1.0e-6F));
        CHECK(near(GranularEngine::semitones_to_ratio(-12.0F), 0.5F, 1.0e-6F));
        CHECK(near(GranularEngine::semitones_to_ratio(7.0F), std::exp2(7.0F / 12.0F), 1.0e-6F));
        // The spawned grain's pitch increment follows ratio * srcRate / dstRate.
        install_sine_bank(bank, 220.0F);
        GranularEngine engine;
        engine.set_sample_rate(kRate);
        engine.set_seed(3U);
        GranularParameters params = default_params();
        params.pitchSemitones = 12.0F;
        params.densityHz = 4000.0F;  // engine clamps density; spin until spawned
        params.positionJitter01 = 0.0F;
        const GranularSource source{bank.data(), kBankFrames, kRate};
        const Grain* grain = nullptr;
        for (int s = 0; s < 64 && grain == nullptr; ++s) {
            engine.render(source, params, 0.0F);
            grain = find_active_grain(engine);
        }
        CHECK(grain != nullptr);
        CHECK(near(grain->pitchIncrement, 2.0F, 1.0e-5F));
        const GranularSource halfRate{bank.data(), kBankFrames, kRate / 2U};
        GranularEngine engine2;
        engine2.set_sample_rate(kRate);
        engine2.set_seed(3U);
        const Grain* grain2 = nullptr;
        for (int s = 0; s < 64 && grain2 == nullptr; ++s) {
            engine2.render(halfRate, params, 0.0F);
            grain2 = find_active_grain(engine2);
        }
        CHECK(grain2 != nullptr);
        CHECK(near(grain2->pitchIncrement, 1.0F, 1.0e-5F));  // 2.0 * 24000/48000
    }

    // Test 4: single-grain end-to-end — constant source renders the Hann window exactly.
    {
        std::vector<float> constant(kBankFrames, 1.0F);
        GranularEngine engine;
        engine.set_sample_rate(kRate);
        engine.set_seed(42U);
        GranularParameters params;
        params.densityHz = 4000.0F;  // engine clamps density; spin until spawned
        params.durationMs = 10.0F;                     // 480 frames
        params.cloud01 = 0.0F;                         // no cloud duration stretch
        params.position01 = 0.0F;
        params.positionJitter01 = 0.0F;
        params.panScatter01 = 0.0F;
        params.gain = 1.0F;
        params.reverseProbability01 = 0.0F;
        params.envelopeShape = GranularEnvelopeShape::Hann;
        const GranularSource source{constant.data(), kBankFrames, kRate};
        const Grain* spawned = nullptr;
        for (int s = 0; s < 64 && spawned == nullptr; ++s) {
            engine.render(source, params, 0.0F);
            spawned = find_active_grain(engine);
        }
        CHECK(spawned != nullptr);
        const float ageAtCapture = spawned != nullptr ? spawned->ageFrames : -1.0F;
        params.densityHz = 0.0F;              // no further spawns: isolate the grain
        constexpr std::uint32_t kGrainFrames = 480U;
        const float panLaw = std::sqrt(0.5F);  // pan == 0 -> equal power
        bool matched = true;
        for (std::uint32_t i = 0; i < kGrainFrames; ++i) {
            const auto [left, right] = engine.render(source, params, 0.0F);
            // The grain was already aged once in its spawn sample.
            const float expected =
                GranularEngine::envelope_value(GranularEnvelopeShape::Hann,
                                               (ageAtCapture + static_cast<float>(i) + 1.0F) /
                                                   static_cast<float>(kGrainFrames)) *
                panLaw;
            if (!near(left, expected, 2.0e-5F) || !near(right, expected, 2.0e-5F)) matched = false;
        }
        CHECK(matched);
        CHECK(engine.active_grain_count() == 0U);  // grain expired on schedule
    }

    // Test 5: silence on empty/missing bank — grainMiss counted, never a crash.
    {
        install_sine_bank(bank, 220.0F);
        GranularEngine engine;
        engine.set_sample_rate(kRate);
        engine.set_seed(7U);
        GranularParameters params = default_params();
        params.densityHz = 100.0F;
        const GranularSource empty{nullptr, 0U, kRate};
        float peak = 0.0F;
        render_engine(engine, empty, params, 0.0F, 4800U, &peak);
        CHECK(peak == 0.0F);
        CHECK(engine.counters().grainMisses > 0U);
        CHECK(engine.counters().requestedGrains > 0U);
        CHECK(engine.counters().admittedGrains == 0U);
        // A single-frame bank is equally unusable.
        const GranularSource one{bank.data(), 1U, kRate};
        render_engine(engine, one, params, 0.0F, 480U, &peak);
        CHECK(peak == 0.0F);
        // Disabled generator: silence with no counter activity at all.
        GranularEngine off;
        off.set_sample_rate(kRate);
        GranularParameters offParams = default_params();
        offParams.enabled = false;
        const GranularSource source{bank.data(), kBankFrames, kRate};
        render_engine(off, source, offParams, 0.0F, 480U, &peak);
        CHECK(peak == 0.0F);
        CHECK(off.counters().requestedGrains == 0U);
    }

    // Test 6: pool exhaustion steals — no crash, steal counted, pool bounded.
    {
        install_sine_bank(bank, 220.0F);
        GranularEngine engine;
        engine.set_sample_rate(kRate);
        engine.set_seed(1234U);
        GranularParameters params = default_params();
        params.densityHz = 2000.0F;  // ~1000 concurrent wanted vs 64-slot pool
        params.durationMs = 500.0F;
        const GranularSource source{bank.data(), kBankFrames, kRate};
        float peak = 0.0F;
        const auto audio = render_engine(engine, source, params, 0.0F, kRate, &peak);
        for (float sample : audio) CHECK(std::isfinite(sample));
        const GranularCounters counters = engine.counters();
        CHECK(counters.requestedGrains > 1000U);
        CHECK(counters.admittedGrains == counters.requestedGrains);
        CHECK(counters.grainSteals > 0U);
        CHECK(counters.grainMisses == 0U);
        CHECK(engine.active_grain_count() <= GranularEngine::kMaxGrains);
        CHECK(peak > 0.01F);
    }

    // Test 7: profiler counters increment on a normal render.
    {
        install_sine_bank(bank, 220.0F);
        GranularEngine engine;
        engine.set_sample_rate(kRate);
        engine.set_seed(11U);
        const GranularParameters params = default_params();
        const GranularSource source{bank.data(), kBankFrames, kRate};
        render_engine(engine, source, params, 0.0F, 4800U);
        const GranularCounters counters = engine.counters();
        CHECK(counters.requestedGrains > 0U);
        CHECK(counters.admittedGrains == counters.requestedGrains);
        CHECK(counters.grainSteals == 0U);  // light load: no steals
        CHECK(counters.grainMisses == 0U);
        CHECK(engine.active_grain_count() > 0U);
        const GranularCounters drained = engine.drain_counters();
        CHECK(drained.requestedGrains == counters.requestedGrains);
        CHECK(engine.counters().requestedGrains == 0U);
        CHECK(engine.counters().admittedGrains == 0U);
    }

    // Test 8: reverse playback — flag/increment on the grain, audible difference.
    {
        install_ramp_bank(bank);
        GranularParameters params = default_params();
        params.densityHz = 4000.0F;  // engine clamps density; spin until spawned
        params.positionJitter01 = 0.0F;
        params.reverseProbability01 = 1.0F;
        const GranularSource source{bank.data(), kBankFrames, kRate};
        GranularEngine engine;
        engine.set_sample_rate(kRate);
        engine.set_seed(5U);
        const Grain* grain = nullptr;
        for (int s = 0; s < 64 && grain == nullptr; ++s) {
            engine.render(source, params, 0.0F);
            grain = find_active_grain(engine);
        }
        CHECK(grain != nullptr);
        CHECK(grain->reverse);
        CHECK(grain->pitchIncrement < 0.0F);
        // Same seed, forward vs reverse: the audio must differ on a ramp source.
        auto render_stream = [&](float reverseProb) {
            GranularEngine e;
            e.set_sample_rate(kRate);
            e.set_seed(21U);
            GranularParameters p = default_params();
            p.densityHz = 200.0F;
            p.durationMs = 60.0F;
            p.position01 = 0.3F;
            p.positionJitter01 = 0.0F;
            p.panScatter01 = 0.0F;
            p.reverseProbability01 = reverseProb;
            return render_engine(e, source, p, 0.0F, 2400U);
        };
        const auto forward = render_stream(0.0F);
        const auto reversed = render_stream(1.0F);
        CHECK(mean_abs_difference(forward, reversed) > 1.0e-4);
    }

    // Test 9: GranularPosition modulation moves the grain source position.
    {
        install_ramp_bank(bank);
        const GranularSource source{bank.data(), kBankFrames, kRate};
        auto spawn_position = [&](float positionMod) {
            GranularEngine engine;
            engine.set_sample_rate(kRate);
            engine.set_seed(9U);
            GranularParameters params = default_params();
            params.densityHz = 4000.0F;  // engine clamps density; spin until spawned
            params.position01 = 0.2F;
            params.positionJitter01 = 0.0F;
            params.reverseProbability01 = 0.0F;
            const Grain* found = nullptr;
            for (int s = 0; s < 64 && found == nullptr; ++s) {
                engine.render(source, params, positionMod);
                found = find_active_grain(engine);
            }
            CHECK(found != nullptr);
            if (found == nullptr) return -1.0F;
            // The spawn sample already advanced the read position once.
            return found->sourcePositionFrames - found->pitchIncrement;
        };
        const float lastFrame = static_cast<float>(kBankFrames - 1U);
        CHECK(near(spawn_position(0.0F), 0.2F * lastFrame, 1.0e-3F));
        CHECK(near(spawn_position(0.3F), 0.5F * lastFrame, 1.0e-3F));
        CHECK(near(spawn_position(5.0F), lastFrame, 1.0e-3F));   // clamped high
        CHECK(near(spawn_position(-5.0F), 0.0F, 1.0e-3F));       // clamped low
    }

    // Test 10: determinism — fixed seed renders bit-identical output.
    {
        install_sine_bank(bank, 330.0F);
        const GranularSource source{bank.data(), kBankFrames, kRate};
        auto run = [&]() {
            GranularEngine engine;
            engine.set_sample_rate(kRate);
            engine.set_seed(424242U);
            GranularParameters params = default_params();
            params.densityHz = 90.0F;
            params.durationMs = 85.0F;
            params.pitchSemitones = 3.0F;
            params.positionJitter01 = 0.4F;
            params.panScatter01 = 0.7F;
            params.reverseProbability01 = 0.2F;
            return render_engine(engine, source, params, 0.15F, 4096U);
        };
        const auto first = run();
        const auto second = run();
        CHECK(first == second);
    }

    // Test 11: synth integration — Granular oscillator through the voice path.
    {
        CHECK(modulation_destination_name(ModulationDestination::GranularPosition) == "granular_pos");
        SynthPreset preset = SynthPreset::make_default();
        preset.name = "Granular Core Test";
        for (auto& oscillator : preset.oscillators) oscillator.enabled = false;
        auto& oscillator = preset.oscillators[0];
        oscillator.enabled = true;
        oscillator.waveform = OscillatorWaveform::Granular;
        oscillator.gain = 1.0F;
        oscillator.semitones = 0.0F;
        oscillator.cents = 0.0F;
        preset.ampEnvelope.attackSeconds = 0.001F;
        preset.ampEnvelope.decaySeconds = 0.001F;
        preset.ampEnvelope.sustainLevel = 1.0F;
        preset.ampEnvelope.releaseSeconds = 0.05F;
        preset.tuning.analogDriftCents = 0.0F;
        preset.filter.enabled = false;
        preset.distortion.enabled = false;
        preset.eq.enabled = false;
        preset.chorus.enabled = false;
        preset.phaser.enabled = false;
        preset.delay.enabled = false;
        preset.reverb.enabled = false;
        preset.compressor.enabled = false;
        preset.limiter.enabled = false;
        preset.masterGain = 0.8F;
        preset.sampleBank.enabled = true;
        preset.sampleBank.sampleRate = kRate;
        preset.sampleBank.rootNote = 60;
        preset.sampleBank.frameCount = kBankFrames;
        for (std::uint32_t i = 0; i < kBankFrames; ++i)
            preset.sampleBank.samples[i] =
                0.9F * std::sin(2.0F * 3.14159265358979F * 220.0F * static_cast<float>(i) /
                                static_cast<float>(kRate));
        preset.granular.enabled = true;
        preset.granular.densityHz = 40.0F;
        preset.granular.durationMs = 120.0F;
        preset.granular.gain = 0.8F;

        auto render_preset = [&](const SynthPreset& p) {
            Synthesizer synth(kRate);
            synth.set_preset(p);
            if (!synth.note_on(60, 0.9F)) return std::vector<float>();
            std::vector<float> audio(8192U * 2U);
            synth.render(audio);
            return audio;
        };
        const auto audio = render_preset(preset);
        CHECK(!audio.empty());
        for (float sample : audio) CHECK(std::isfinite(sample));
        CHECK(rms(audio) > 0.005);

        // Determinism through the full synth: identical notes, identical audio.
        const auto again = render_preset(preset);
        CHECK(audio == again);

        // Profiler path: requested/admitted counted, activity visible.
        Synthesizer metered(kRate);
        metered.set_preset(preset);
        metered.reset_granular_profiler();
        CHECK(metered.note_on(60, 0.9F));
        std::vector<float> meteredAudio(4096U * 2U);
        metered.render(meteredAudio);
        const SynthGranularProfiler profiler = metered.granular_profiler();
        CHECK(profiler.requestedGrains > 0U);
        CHECK(profiler.admittedGrains == profiler.requestedGrains);
        CHECK(profiler.grainMisses == 0U);
        CHECK(profiler.activeGrains > 0U);
        CHECK(profiler.maximumActiveGrains >= profiler.activeGrains);

        // GranularPosition modulation audibly moves the source position: route
        // velocity (constant for the note) to GranularPosition and compare
        // against the unmodulated render on a position-sensitive ramp bank.
        SynthPreset rampPreset = preset;
        for (std::uint32_t i = 0; i < kBankFrames; ++i)
            rampPreset.sampleBank.samples[i] =
                static_cast<float>(i) / static_cast<float>(kBankFrames - 1U) * 2.0F - 1.0F;
        rampPreset.granular.position01 = 0.1F;
        rampPreset.granular.positionJitter01 = 0.0F;
        rampPreset.granular.densityHz = 60.0F;
        const auto plainAudio = render_preset(rampPreset);
        rampPreset.modulation[0] = {true,
                                    ModulationSource::Velocity,
                                    ModulationDestination::GranularPosition,
                                    0.7F,
                                    0.0F,
                                    ModulationCurve::Linear,
                                    ModulationPolarity::Unipolar,
                                    0.0F};
        const auto modAudio = render_preset(rampPreset);
        CHECK(mean_abs_difference(plainAudio, modAudio) > 1.0e-4);
    }

    if (g_failures == 0) std::printf("ALL GRANULAR TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
