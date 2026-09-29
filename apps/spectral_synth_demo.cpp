// Phase 5 (SYN-015) spectral showcase demo (Worker D):
// renders ~8 seconds of a spectral pad/freeze preset through the budget
// policy + profiler and writes a stereo WAV for listening.
//
// Uses Worker B's SpectralOscillator when include/dve/audio/spectral.hpp is
// present, otherwise spectral_reference::ReferenceOscillator (stand-in).
// Usage: dve_spectral_synth_demo [wav_path]
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

#include "dve/audio/spectral_profiler.hpp"
#include "dve/audio/synthesizer.hpp" // write_float_wav

#if __has_include("dve/audio/spectral.hpp")
#include "dve/audio/spectral.hpp"
#define SPECTRAL_HAVE_WORKER_B_OSC 1
#endif

using namespace dve::audio;
using namespace dve::audio::spectral_reference;

namespace {

constexpr std::uint32_t kSampleRate = 48000U;
constexpr std::uint32_t kBlockFrames = 128U;
constexpr double kDurationSeconds = 8.0;
constexpr std::uint32_t kTotalBlocks =
    static_cast<std::uint32_t>((kSampleRate * kDurationSeconds) / kBlockFrames); // 3000

struct DemoVoice {
    ReferenceOscillator osc;
    float pan{};
    float pitchScale{};
};

} // namespace

int main(int argc, char** argv) {
#if defined(SPECTRAL_HAVE_WORKER_B_OSC)
    std::printf("spectral demo: Worker B SpectralOscillator\n");
#else
    std::printf("spectral demo: spectral_reference stand-in (Worker B header not present)\n");
#endif

    const std::filesystem::path wav =
        argc > 1 ? argv[1] : "/home/hatch/workspace/your_files/spectral_phase5_demo.wav";

    // Cooked spectral asset: C2-rooted harmonic pad, 64 analysis frames.
    // (Qualified: Worker B's real dve::audio::SpectralAsset now coexists in
    // the merged tree; the demo intentionally uses the reference stand-in.)
    spectral_reference::SpectralAsset asset = cook_pad_asset(1024U, 64U, 65.4064F, 48000.0F);
    if (asset.binCount == 0U) {
        std::fprintf(stderr, "failed to cook spectral asset\n");
        return 1;
    }

    // Four-voice pad: C2 / G2 / C3 / E3 (+0, +7, +12, +16 semitones).
    constexpr std::uint32_t kVoices = 4U;
    DemoVoice voices[kVoices];
    const float pitchScales[kVoices] = {1.0F, 1.4983071F, 2.0F, 2.5198421F};
    const float pans[kVoices] = {-0.6F, -0.2F, 0.2F, 0.6F};
    const float vels[kVoices] = {0.85F, 0.7F, 0.65F, 0.55F};
    for (std::uint32_t v = 0U; v < kVoices; ++v) {
        if (!voices[v].osc.attach_asset(&asset)) {
            std::fprintf(stderr, "voice %u: attach failed\n", v);
            return 1;
        }
        voices[v].pan = pans[v];
        voices[v].pitchScale = pitchScales[v];
        ReferenceParams params;
        params.pitchScale = pitchScales[v];
        params.gain = 0.55F;
        voices[v].osc.set_params(params);
        voices[v].osc.note_on(vels[v]);
    }

    SpectralBudgetPolicy policy; // defaults: 0.30 ms/voice, 2.0 ms global
    policy.set_quality(FilterQuality::High);
    policy.set_active_voices(kVoices);
    policy.set_request(512U, 0U, false);
    SpectralProfiler profiler;

    std::vector<float> stereo(static_cast<std::size_t>(kTotalBlocks) * kBlockFrames * 2U, 0.0F);
    std::vector<float> mono(kBlockFrames);

    const std::uint32_t freezeBlock = static_cast<std::uint32_t>(kTotalBlocks * 0.5);   // 4.0 s
    const std::uint32_t releaseBlock = static_cast<std::uint32_t>(kTotalBlocks * 0.8125); // 6.5 s

    float peak = 0.0F;
    double energy = 0.0;
    bool allFinite = true;
    for (std::uint32_t blk = 0U; blk < kTotalBlocks; ++blk) {
        const double t = static_cast<double>(blk) / kTotalBlocks; // 0..1
        if (blk == freezeBlock) {
            // Freeze the inner voices + slow the formant sweep: the classic
            // spectral "hold" moment.
            for (std::uint32_t v = 1U; v < 3U; ++v) {
                ReferenceParams params;
                params.pitchScale = voices[v].pitchScale;
                params.gain = 0.55F;
                params.freeze = true;
                params.timeStretch = 4.0F;
                voices[v].osc.set_params(params);
            }
            policy.set_request(512U, 0U, true);
            std::printf("  freeze engaged at %.1f s\n", blk * kBlockFrames / 48000.0);
        }
        if (blk == releaseBlock) {
            for (auto& voice : voices)
                voice.osc.note_off();
        }
        // Formant sweep 0 -> +5 semitones across the render (all voices).
        const float formant = static_cast<float>(t * 5.0);
        const SpectralVoiceBudget budget = policy.advance_block();
        float* out = stereo.data() + static_cast<std::size_t>(blk) * kBlockFrames * 2U;
        for (std::uint32_t v = 0U; v < kVoices; ++v) {
            ReferenceParams params;
            params.pitchScale = voices[v].pitchScale;
            params.gain = 0.55F;
            params.formantShiftSemitones = formant;
            if (blk >= freezeBlock && (v == 1U || v == 2U)) {
                params.freeze = true;
                params.timeStretch = 4.0F;
            }
            voices[v].osc.set_params(params);
            voices[v].osc.set_budget(budget);
            SpectralRenderStats stats{};
            voices[v].osc.render_block(mono.data(), kBlockFrames, kSampleRate, &stats);
            profiler.record_block(v, stats);
            const float angle = (voices[v].pan + 1.0F) * 0.25F *
                                static_cast<float>(std::numbers::pi);
            const float left = std::cos(angle);
            const float right = std::sin(angle);
            for (std::uint32_t i = 0U; i < kBlockFrames; ++i) {
                out[i * 2U] += mono[i] * left * 0.5F;
                out[i * 2U + 1U] += mono[i] * right * 0.5F;
            }
        }
        for (std::uint32_t i = 0U; i < kBlockFrames * 2U; ++i) {
            const float s = out[i];
            if (!std::isfinite(s))
                allFinite = false;
            peak = std::max(peak, std::fabs(s));
            energy += static_cast<double>(s) * s;
        }
    }

    std::string error;
    if (!write_float_wav(wav, stereo, kSampleRate, &error)) {
        std::fprintf(stderr, "wav write failed: %s\n", error.c_str());
        return 1;
    }

    const SynthSpectralProfiler prof = profiler.snapshot();
    const double rms = std::sqrt(energy / static_cast<double>(stereo.size()));
    std::printf("wrote %s (%.1f s, peak %.3f, rms %.4f, finite %d)\n", wav.c_str(),
                kDurationSeconds, peak, rms, static_cast<int>(allFinite));
    std::printf("profiler: voice-blocks %llu, iffts %llu, frame updates %llu, peak bins %u\n",
                static_cast<unsigned long long>(prof.blocksRendered),
                static_cast<unsigned long long>(prof.total.ifftCount),
                static_cast<unsigned long long>(prof.total.frameUpdates), prof.peakActiveBins);
    std::printf("policy: degradation level %u, events %llu, eco clamps %llu, est %.3f ms/block\n",
                policy.degradation_level(),
                static_cast<unsigned long long>(policy.degradation_events()),
                static_cast<unsigned long long>(policy.eco_clamp_events()),
                policy.estimated_block_ms());
    return (allFinite && peak > 1.0e-4F) ? 0 : 1;
}
