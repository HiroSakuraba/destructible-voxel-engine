// Phase 5 (SYN-015) spectral integration test (Worker D):
// MIDI-style note-on/off -> 2 s render through the spectral oscillator +
// budget policy + profiler -> assert non-silent, NaN-free, terminates.
//
// Oscillator/asset selection (nothing is skipped):
//   - When Worker B's include/dve/audio/spectral.hpp AND Worker A's
//     include/dve/audio/spectral_asset.hpp are present, the test drives the
//     REAL SpectralOscillator (per-sample render) with a REAL analyzed
//     asset (analyze_spectrum of a synthetic harmonic tone), the budget
//     policy choosing Worker B's quality tier per block, and the profiler
//     draining Worker B's SpectralCounters.
//   - Otherwise it runs the FULL test against spectral_reference::
//     ReferenceOscillator (Worker D's measured stand-in) with the
//     procedural cooker -- a real, non-skipped integration test of the
//     note -> policy -> oscillator -> profiler path.
// The test prints which path it used.
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include "dve/audio/spectral_profiler.hpp"

#if __has_include("dve/audio/spectral.hpp") && __has_include("dve/audio/spectral_asset.hpp")
#include "dve/audio/spectral.hpp"
#include "dve/audio/spectral_asset.hpp"
#define SPECTRAL_HAVE_REAL_PATH 1
#endif

using namespace dve::audio;
#if !defined(SPECTRAL_HAVE_REAL_PATH)
using namespace dve::audio::spectral_reference;
#endif

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

namespace {

constexpr std::uint32_t kSampleRate = 48000U;
constexpr std::uint32_t kBlockFrames = 128U;
constexpr std::uint32_t kTwoSecondsBlocks = (kSampleRate * 2U) / kBlockFrames; // 750

struct NoteEvent {
    std::uint32_t block; // block index at which the event fires
    bool on;
    std::uint32_t voice;
    float velocity;
    float freqHz; // real path only (stand-in path uses pitchScale instead)
};

#if defined(SPECTRAL_HAVE_REAL_PATH)

// --- Real path: Worker B oscillator + Worker A asset -----------------------

// Glue: Worker D's per-block bin budget -> Worker B's quality tier.
// Worker B's tier bins: Eco=256, Standard=512, High=1024.
[[nodiscard]] FilterQuality spectral_quality_for_bins(std::uint32_t activeBins) noexcept {
    if (activeBins >= 1024U)
        return FilterQuality::High;
    if (activeBins >= 512U)
        return FilterQuality::Standard;
    return FilterQuality::Eco;
}

[[nodiscard]] std::uint32_t spectral_tier_bins(FilterQuality quality) noexcept {
    switch (quality) {
    case FilterQuality::High:
        return 1024U;
    case FilterQuality::Standard:
        return 512U;
    default:
        return 256U;
    }
}

// Harness-side ADSR: Worker B's oscillator is a raw generator (no note
// on/off); the voice layer owns the amplitude envelope. The test models it.
struct VoiceEnvelope {
    enum Phase : std::uint8_t { Idle, Attack, Decay, Sustain, Release } phase{Idle};
    float level{0.0F};
    float velocity{0.0F};
    float releaseLevel{0.0F};

    void note_on(float vel) noexcept {
        velocity = vel;
        phase = Attack;
    }
    void note_off() noexcept {
        if (phase != Idle) {
            phase = Release;
            releaseLevel = level;
        }
    }
    float next() noexcept {
        constexpr float kAttackSamples = 0.005F * kSampleRate;
        constexpr float kDecaySamples = 0.100F * kSampleRate;
        constexpr float kReleaseSamples = 0.200F * kSampleRate;
        switch (phase) {
        case Attack:
            level += velocity / kAttackSamples;
            if (level >= velocity) {
                level = velocity;
                phase = Decay;
            }
            break;
        case Decay:
            level -= (velocity * 0.2F) / kDecaySamples;
            if (level <= velocity * 0.8F) {
                level = velocity * 0.8F;
                phase = Sustain;
            }
            break;
        case Sustain:
            break;
        case Release:
            level -= releaseLevel / kReleaseSamples;
            if (level <= 0.0F) {
                level = 0.0F;
                phase = Idle;
            }
            break;
        case Idle:
            level = 0.0F;
            break;
        }
        return level;
    }
};

struct RealVoice {
    SpectralOscillator osc;
    VoiceEnvelope env;
    float freqHz{110.0F};
};

// Cooks a real asset: 2 s of a 110 Hz harmonic tone through Worker A's
// offline analyzer, then adapts it to Worker B's view.
bool cook_real_asset(SpectralAsset& asset, SpectralAssetViewData& viewData,
                     const SpectralAssetView*& view, float& baseHz) {
    constexpr std::uint32_t kToneSamples = kSampleRate * 2U;
    std::vector<float> tone(kToneSamples);
    for (std::uint32_t n = 0U; n < kToneSamples; ++n) {
        const float t = static_cast<float>(n) / static_cast<float>(kSampleRate);
        // Gentle attack/decay so the asset has clean pitched content.
        const float env = std::min(1.0F, t / 0.05F) * std::min(1.0F, (2.0F - t) / 0.3F);
        float s = 0.0F;
        for (std::uint32_t h = 1U; h <= 12U; ++h)
            s += std::sin(2.0F * 3.14159265358979323846F * 110.0F *
                          static_cast<float>(h) * t) /
                 static_cast<float>(h);
        tone[n] = s * env * 0.25F;
    }
    SpectralAnalyzerConfig config;
    config.fftSize = 2048U;
    config.hopSize = 512U;
    try {
        asset = analyze_spectrum(tone.data(), kToneSamples, kSampleRate, config);
    } catch (...) {
        return false;
    }
    if (asset.empty() || !viewData.convert(asset))
        return false;
    view = &viewData.view();
    baseHz = asset.estimatedPitchHz > 0.0F ? asset.estimatedPitchHz : 110.0F;
    return true;
}

float render_note_sequence(RealVoice* voices, std::uint32_t voiceCount,
                           const std::vector<NoteEvent>& events, std::uint32_t blocks,
                           SpectralBudgetPolicy& policy, SpectralProfiler& profiler,
                           const SpectralAssetView& view, bool& allFinite) {
    std::vector<float> mix(kBlockFrames);
    float peak = 0.0F;
    allFinite = true;
    std::size_t eventIdx = 0U;
    // Worker B: hop = fftSize / 4, so this many grains overlap per sample.
    const std::uint32_t overlap = view.fftSize / (view.fftSize / 4U);
    for (std::uint32_t blk = 0U; blk < blocks; ++blk) {
        while (eventIdx < events.size() && events[eventIdx].block == blk) {
            const NoteEvent& e = events[eventIdx];
            if (e.on)
                voices[e.voice].env.note_on(e.velocity);
            else
                voices[e.voice].env.note_off();
            ++eventIdx;
        }
        const SpectralVoiceBudget budget = policy.advance_block();
        const FilterQuality quality = spectral_quality_for_bins(budget.activeBins);
        const std::uint32_t tierBins = spectral_tier_bins(quality);
        std::fill(mix.begin(), mix.end(), 0.0F);
        for (std::uint32_t v = 0U; v < voiceCount; ++v) {
            SpectralParameters params;
            params.gain = 0.6F;
            params.spectralQuality = quality;
            for (std::uint32_t i = 0U; i < kBlockFrames; ++i) {
                const auto [left, right] =
                    voices[v].osc.render(view, params, voices[v].freqHz);
                mix[i] += (left + right) * 0.5F * voices[v].env.next();
            }
            const SpectralCounters counters = voices[v].osc.drain_counters();
            SpectralRenderStats stats{};
            stats.ifftCount = counters.framesRendered;
            stats.frameUpdates = counters.framesRendered;
            stats.activeBins = tierBins;
            stats.overlapAddSamples = kBlockFrames * overlap;
            profiler.record_block(v, stats);
        }
        const float norm = 1.0F / static_cast<float>(voiceCount);
        for (std::uint32_t i = 0U; i < kBlockFrames; ++i) {
            const float s = mix[i] * norm;
            if (!std::isfinite(s))
                allFinite = false;
            peak = std::max(peak, std::fabs(s));
        }
    }
    return peak;
}

#else

// --- Stand-in path: Worker D reference oscillator + procedural cooker ------
// (Used until Worker B's spectral.hpp / Worker A's spectral_asset.hpp land.)

float render_note_sequence(ReferenceOscillator* oscillators, std::uint32_t voiceCount,
                           const std::vector<NoteEvent>& events, std::uint32_t blocks,
                           SpectralBudgetPolicy& policy, SpectralProfiler& profiler,
                           bool& allFinite) {
    std::vector<float> mono(kBlockFrames);
    std::vector<float> mix(kBlockFrames);
    float peak = 0.0F;
    allFinite = true;
    std::size_t eventIdx = 0U;
    for (std::uint32_t blk = 0U; blk < blocks; ++blk) {
        while (eventIdx < events.size() && events[eventIdx].block == blk) {
            const NoteEvent& e = events[eventIdx];
            if (e.on)
                oscillators[e.voice].note_on(e.velocity);
            else
                oscillators[e.voice].note_off();
            ++eventIdx;
        }
        const SpectralVoiceBudget budget = policy.advance_block();
        std::fill(mix.begin(), mix.end(), 0.0F);
        for (std::uint32_t v = 0U; v < voiceCount; ++v) {
            oscillators[v].set_budget(budget);
            SpectralRenderStats stats{};
            oscillators[v].render_block(mono.data(), kBlockFrames, kSampleRate, &stats);
            profiler.record_block(v, stats);
            for (std::uint32_t i = 0U; i < kBlockFrames; ++i)
                mix[i] += mono[i];
        }
        const float norm = 1.0F / static_cast<float>(voiceCount);
        for (std::uint32_t i = 0U; i < kBlockFrames; ++i) {
            const float s = mix[i] * norm;
            if (!std::isfinite(s))
                allFinite = false;
            peak = std::max(peak, std::fabs(s));
        }
    }
    return peak;
}

#endif

void test_two_second_note_render() {
#if defined(SPECTRAL_HAVE_REAL_PATH)
    std::printf("  asset: Worker A analyze_spectrum (real)\n");
    std::printf("  oscillator: Worker B SpectralOscillator (real)\n");
#else
    std::printf("  asset: procedural stand-in cooker (Worker A header not present)\n");
    std::printf("  oscillator: spectral_reference stand-in (Worker B header not present)\n");
#endif

    constexpr std::uint32_t kVoices = 3U;
    const std::uint32_t noteOffBlock = (kSampleRate * 3U / 2U) / kBlockFrames; // 1.5 s
    // Chord ratios 1.0 / 1.5 / 2.0 on the asset's base pitch.
    const float ratios[kVoices] = {1.0F, 1.4983071F, 2.0F}; // unison, +7st, octave
    const float velocities[kVoices] = {0.8F, 0.7F, 0.6F};

    SpectralBudgetPolicy policy; // defaults
    policy.set_quality(FilterQuality::High);
    policy.set_active_voices(kVoices);
    policy.set_request(512U, 0U, false);
    SpectralProfiler profiler;

#if defined(SPECTRAL_HAVE_REAL_PATH)
    SpectralAsset asset;
    SpectralAssetViewData viewData;
    const SpectralAssetView* view = nullptr;
    float baseHz = 110.0F;
    CHECK(cook_real_asset(asset, viewData, view, baseHz));
    if (view == nullptr) return; // cook failed; CHECK already recorded it
    CHECK(view != nullptr);
    RealVoice voices[kVoices];
    for (std::uint32_t v = 0U; v < kVoices; ++v) {
        voices[v].osc.set_sample_rate(kSampleRate);
        voices[v].osc.set_seed(0x9E3779B9U + v * 0x85EBCA6BU);
        voices[v].osc.reset();
        voices[v].freqHz = baseHz * ratios[v];
    }
    std::vector<NoteEvent> events;
    for (std::uint32_t v = 0U; v < kVoices; ++v)
        events.push_back({0U, true, v, velocities[v], baseHz * ratios[v]});
    for (std::uint32_t v = 0U; v < kVoices; ++v)
        events.push_back({noteOffBlock, false, v, 0.0F, baseHz * ratios[v]});
    bool allFinite = false;
    const float peak = render_note_sequence(voices, kVoices, events, kTwoSecondsBlocks,
                                            policy, profiler, *view, allFinite);
#else
    // Three-voice pad chord: C2-rooted asset, pitchScale per voice.
    SpectralAsset asset = cook_pad_asset(1024U, 64U, 65.4064F, 48000.0F); // C2
    CHECK(asset.binCount == 1024U && asset.frameCount == 64U);
    ReferenceOscillator oscillators[kVoices];
    for (std::uint32_t v = 0U; v < kVoices; ++v) {
        CHECK(oscillators[v].attach_asset(&asset));
        ReferenceParams params;
        params.pitchScale = ratios[v];
        params.gain = 0.6F;
        oscillators[v].set_params(params);
    }
    const std::vector<NoteEvent> events = {
        {0U, true, 0U, 0.8F, 0.0F}, {0U, true, 1U, 0.7F, 0.0F}, {0U, true, 2U, 0.6F, 0.0F},
        {noteOffBlock, false, 0U, 0.0F, 0.0F}, {noteOffBlock, false, 1U, 0.0F, 0.0F},
        {noteOffBlock, false, 2U, 0.0F, 0.0F},
    };
    bool allFinite = false;
    const float peak = render_note_sequence(oscillators, kVoices, events, kTwoSecondsBlocks,
                                            policy, profiler, allFinite);
#endif

    std::printf("  2 s render: peak=%.4f finite=%d blocks=%llu\n", peak,
                static_cast<int>(allFinite),
                static_cast<unsigned long long>(profiler.snapshot().blocksRendered));
    CHECK(allFinite);                                                           // NaN-free
    // Non-silent, amplitude-correct. (2026-09-28, merge: Worker B fixed the
    // 1/N IFFT scaling bug Worker D reported — output now round-trips at the
    // documented amplitude — so the weak 1e-5 functional threshold is raised
    // to 0.1 per Worker D's recommendation.)
    CHECK(peak > 0.1F);                                                         // non-silent, sane amplitude
    CHECK(profiler.snapshot().blocksRendered == kTwoSecondsBlocks * kVoices);  // terminated
    CHECK(profiler.snapshot().total.ifftCount > 0U);
    CHECK(profiler.snapshot().peakActiveBins == 512U);
    // Nominal path: 3 voices x 512 bins fits the default budgets, no degradation.
    CHECK(policy.degradation_level() == SpectralBudgetPolicy::kLevelNone);
}

void test_graceful_under_pressure() {
    // Same 2 s render, but with a starvation budget: the policy must degrade
    // and the render must stay finite, non-silent, and terminate.
    constexpr std::uint32_t kVoices = 3U;
    const std::uint32_t noteOffBlock = (kSampleRate * 3U / 2U) / kBlockFrames;

    SpectralBudgetPolicy::Config config;
    config.maxMsPerVoicePerBlock = 100.0F;
    config.maxMsGlobalPerBlock = 0.05F; // forces bins -> rate degradation
    SpectralBudgetPolicy policy(config);
    policy.set_quality(FilterQuality::High);
    policy.set_active_voices(kVoices);
    policy.set_request(512U, 0U, false);
    SpectralProfiler profiler;

#if defined(SPECTRAL_HAVE_REAL_PATH)
    SpectralAsset asset;
    SpectralAssetViewData viewData;
    const SpectralAssetView* view = nullptr;
    float baseHz = 110.0F;
    CHECK(cook_real_asset(asset, viewData, view, baseHz));
    if (view == nullptr) return; // cook failed; CHECK already recorded it
    RealVoice voices[kVoices];
    for (std::uint32_t v = 0U; v < kVoices; ++v) {
        voices[v].osc.set_sample_rate(kSampleRate);
        voices[v].osc.set_seed(0x9E3779B9U + v * 0x85EBCA6BU);
        voices[v].osc.reset();
        voices[v].freqHz = baseHz;
    }
    std::vector<NoteEvent> events;
    for (std::uint32_t v = 0U; v < kVoices; ++v)
        events.push_back({0U, true, v, 0.8F, baseHz});
    for (std::uint32_t v = 0U; v < kVoices; ++v)
        events.push_back({noteOffBlock, false, v, 0.0F, baseHz});
    bool allFinite = false;
    const float peak = render_note_sequence(voices, kVoices, events, kTwoSecondsBlocks,
                                            policy, profiler, *view, allFinite);
#else
    SpectralAsset asset = cook_pad_asset(1024U, 64U, 65.4064F, 48000.0F);
    ReferenceOscillator oscillators[kVoices];
    for (std::uint32_t v = 0U; v < kVoices; ++v) {
        CHECK(oscillators[v].attach_asset(&asset));
        ReferenceParams params;
        params.gain = 0.6F;
        oscillators[v].set_params(params);
    }
    const std::vector<NoteEvent> events = {
        {0U, true, 0U, 0.8F, 0.0F}, {0U, true, 1U, 0.7F, 0.0F}, {0U, true, 2U, 0.6F, 0.0F},
        {noteOffBlock, false, 0U, 0.0F, 0.0F}, {noteOffBlock, false, 1U, 0.0F, 0.0F},
        {noteOffBlock, false, 2U, 0.0F, 0.0F},
    };
    bool allFinite = false;
    const float peak = render_note_sequence(oscillators, kVoices, events, kTwoSecondsBlocks,
                                            policy, profiler, allFinite);
#endif

    std::printf("  pressure render: peak=%.4f finite=%d level=%u events=%llu\n", peak,
                static_cast<int>(allFinite), policy.degradation_level(),
                static_cast<unsigned long long>(policy.degradation_events()));
    CHECK(allFinite);
    CHECK(peak > 0.1F); // non-silent, sane amplitude (scaling fixed at merge; see test_two_second_note_render)
    CHECK(profiler.snapshot().blocksRendered == kTwoSecondsBlocks * kVoices);
    CHECK(policy.degradation_level() != SpectralBudgetPolicy::kLevelNone);
    CHECK(policy.degradation_events() > 0U);
    // Even starved, the Eco floor keeps bins > 0: audio keeps flowing.
    CHECK(profiler.snapshot().peakActiveBins >= 32U);
}

} // namespace

int main() {
    test_two_second_note_render();
    test_graceful_under_pressure();
    if (g_failures == 0)
        std::printf("spectral integration tests: all green\n");
    return g_failures == 0 ? 0 : 1;
}
