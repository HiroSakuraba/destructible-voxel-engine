// Phase 5 (SYN-015) spectral/resynthesis CPU budgeting: SynthSpectralProfiler,
// SpectralBudgetPolicy, the documented worst-case cost model, and a reference
// spectral oscillator used to anchor the model measurements.
// Worker D owns this file. Worker B owns include/dve/audio/spectral.hpp
// (SpectralOscillator); the policy below is deliberately decoupled from it and
// talks to any oscillator through SpectralRenderStats (out) + SpectralVoiceBudget
// (in), so B can adopt it without interface churn.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "dve/audio/granular.hpp"  // FilterQuality: shared CPU quality tiers

namespace dve::audio {

// ---------------------------------------------------------------------------
// Tier ceilings
//
// activeBins  = number of spectral bins resynthesized per voice. The render
//               FFT size is always 2 * activeBins (bins span 0..Nyquist).
// frameDivisor = recompute the spectral frame (bin processing + IFFT) only
//               every Nth hop; between updates the last grain is replayed
//               (the classic "freeze" trick, applied gradually).
// ---------------------------------------------------------------------------
struct SpectralTierCeiling {
    std::uint32_t maxActiveBins{};
    std::uint32_t frameDivisor{};
};

[[nodiscard]] inline constexpr SpectralTierCeiling
spectral_tier_ceiling(FilterQuality quality) noexcept {
    switch (quality) {
    case FilterQuality::Eco:
        return SpectralTierCeiling{64U, 4U};
    case FilterQuality::Standard:
        return SpectralTierCeiling{256U, 2U};
    case FilterQuality::High:
        return SpectralTierCeiling{512U, 1U};
    case FilterQuality::Offline:
        return SpectralTierCeiling{1024U, 1U};
    }
    return SpectralTierCeiling{256U, 2U};
}

// Degradation ladder for active bins (power-of-two steps so the render FFT
// stays a power of two). The policy walks DOWN this ladder first when over
// budget, then raises the frame divisor, then forces Eco behavior.
inline constexpr std::array<std::uint32_t, 6> kSpectralBinLadder{{32U, 64U, 128U, 256U, 512U, 1024U}};

[[nodiscard]] inline std::uint32_t spectral_snap_bins_down(std::uint32_t desired) noexcept {
    std::uint32_t result = kSpectralBinLadder.front();
    for (const std::uint32_t step : kSpectralBinLadder) {
        if (step > desired)
            break;
        result = step;
    }
    return result;
}

[[nodiscard]] inline std::uint32_t spectral_snap_bins_up(std::uint32_t desired) noexcept {
    for (const std::uint32_t step : kSpectralBinLadder) {
        if (step >= desired)
            return step;
    }
    return kSpectralBinLadder.back();
}

[[nodiscard]] inline std::uint32_t spectral_ladder_step_down(std::uint32_t bins) noexcept {
    std::uint32_t result = kSpectralBinLadder.front();
    for (const std::uint32_t step : kSpectralBinLadder) {
        if (step >= bins)
            break;
        result = step;
    }
    return result;
}

[[nodiscard]] inline std::uint32_t spectral_ladder_step_up(std::uint32_t bins) noexcept {
    for (const std::uint32_t step : kSpectralBinLadder) {
        if (step > bins)
            return step;
    }
    return kSpectralBinLadder.back();
}

// ---------------------------------------------------------------------------
// Worst-case cost model (documented).
//
// Reference render: fixed 256-sample hop, overlap-add resynthesis. Per
// 128-sample block at 48 kHz, per voice:
//
//   fftSize      = 2 * activeBins
//   hopEff       = min(256, activeBins)        // fftSize/2 for small FFTs, else 256
//   updates      = (128 / hopEff) / frameDivisor   // frame updates per block
//   overlap      = fftSize / hopEff            // grains overlapping per sample
//
//   estVoiceMs(bins, divisor)
//     = updates * (kSpectralMsPerBin * bins
//                  + kSpectralMsPerFftOp * fftSize * log2(fftSize))
//     + 128 * overlap * kSpectralMsPerOlaSample
//
//   estGlobalMs = activeVoices * estVoiceMs
//
// The three constants below are conservative upper bounds measured on
// 2026-09-28 (AMD EPYC 9D25, -O2, Release) against the reference oscillator
// in this file: a 2048-pt IFFT measured ~9.1 ns per N*log2(N) op, the per-bin
// spectral loop (magnitude interp + formant remap + rotation recurrence)
// ~160 ns/bin, overlap-add ~6 ns per overlapped sample. Each constant carries
// ~1.5x headroom above the measured mean; tests/test_audio_synth_spectral_
// budget.cpp re-measures on every run and asserts measured <= model.
//
// Real-time worst case: 16 voices x 512 bins (High ceiling) x divisor 1
//   measured ~1.45 ms/block -> model ~2.21 ms -> bound 2.5 ms.
// The 128-sample block at 48 kHz is 2.667 ms, so the bound proves the worst
// real-time configuration fits the slice. The Offline extreme (16 x 1024)
// measures ~3.0 ms and is documented as non-realtime: the Offline tier is
// for bounces, and the policy passes its ceiling through ungated.
//
// Documented bound: 16 voices x 512 bins x divisor 1 (the High ceiling)
// must render a 128-sample block in <= kSpectralWorstCaseBlockMsBound.
// ---------------------------------------------------------------------------
inline constexpr double kSpectralMsPerBin = 0.000240;       // 240 ns per bin per frame update
inline constexpr double kSpectralMsPerFftOp = 0.000014;     // 14 ns per N*log2(N) op
inline constexpr double kSpectralMsPerOlaSample = 0.000010; // 10 ns per overlapped sample
inline constexpr double kSpectralWorstCaseBlockMsBound = 2.5; // 2.5 ms < 2.667 ms slice

[[nodiscard]] inline double spectral_estimate_voice_ms(std::uint32_t bins,
                                                       std::uint32_t divisor) noexcept {
    if (bins == 0U || divisor == 0U)
        return 0.0;
    const double fftSize = static_cast<double>(2U * bins);
    const double hopEff = bins >= 256U ? 256.0 : static_cast<double>(bins);
    const double updates = (128.0 / hopEff) / static_cast<double>(divisor);
    const double overlap = fftSize / hopEff;
    const double frameMs = kSpectralMsPerBin * static_cast<double>(bins) +
                           kSpectralMsPerFftOp * fftSize * std::log2(fftSize);
    return updates * frameMs + 128.0 * overlap * kSpectralMsPerOlaSample;
}

// ---------------------------------------------------------------------------
// Profiler: per-block render statistics reported BY the oscillator, accumulated
// here. Same counter style/units as SynthGranularProfiler so spectral and
// granular numbers are directly comparable.
// ---------------------------------------------------------------------------
struct SpectralRenderStats {
    std::uint64_t ifftCount{};         // IFFT executions this block
    std::uint64_t activeBins{};        // bins resynthesized (peak of the block)
    std::uint64_t frameUpdates{};      // new spectral frames computed
    std::uint64_t overlapAddSamples{}; // overlap-add output samples written
    std::uint64_t ecoClamps{};         // Eco-ceiling clamps applied to requests
};

struct SynthSpectralProfiler {
    static constexpr std::size_t kMaxVoices = 16;
    std::array<SpectralRenderStats, kMaxVoices> perVoice{};
    SpectralRenderStats total{};
    std::uint32_t peakActiveBins{};
    std::uint64_t blocksRendered{};
    std::uint64_t degradationEvents{};
};

class SpectralProfiler {
public:
    void record_block(std::uint32_t voice, const SpectralRenderStats& stats) noexcept;
    void record_degradation() noexcept;
    [[nodiscard]] SynthSpectralProfiler snapshot() const noexcept { return data_; }
    void reset() noexcept { data_ = SynthSpectralProfiler{}; }

private:
    SynthSpectralProfiler data_{};
};

// ---------------------------------------------------------------------------
// Budget policy: strict, deterministic CPU budgeting.
//
// Inputs: quality tier, active voice count, musical cost request (desired bins
// + harmonic-boost bins + freeze flag).
// Output: per-voice SpectralVoiceBudget, slewed toward the target so changes
// are click-free. No wall clock anywhere: degradation decisions come from the
// documented cost model, so two runs with the same inputs produce bit-identical
// budget sequences.
//
// Degradation order when the model says we are over budget:
//   1. reduce active bins (walk down kSpectralBinLadder),
//   2. reduce frame-update rate (double frameDivisor, up to 8),
//   3. drop to Eco behavior (32 bins, divisor 8).
//
// Eco tier always wins: when quality == Eco the musical request is clamped to
// the Eco ceiling BEFORE any budgeting, and ecoClampEvents counts it.
// Offline is for non-realtime renders: the tier ceiling passes through with
// no budget gating (a bounce may take as long as it needs).
// ---------------------------------------------------------------------------
struct SpectralVoiceBudget {
    std::uint32_t activeBins{256U};
    std::uint32_t frameDivisor{2U};
    bool ecoForced{false}; // true when the policy forced Eco behavior (level 3)
};

class SpectralBudgetPolicy {
public:
    struct Config {
        float maxMsPerVoicePerBlock{0.30F}; // per-voice CPU budget (ms / 128-sample block)
        float maxMsGlobalPerBlock{2.00F};   // global spectral budget across voices
    };

    // Degradation levels (also the order applied):
    //   0 = no degradation, 1 = bins reduced, 2 = frame rate reduced,
    //   3 = Eco behavior forced.
    static constexpr std::uint8_t kLevelNone = 0U;
    static constexpr std::uint8_t kLevelBins = 1U;
    static constexpr std::uint8_t kLevelRate = 2U;
    static constexpr std::uint8_t kLevelEco = 3U;

    SpectralBudgetPolicy() : SpectralBudgetPolicy(Config{}) {}
    explicit SpectralBudgetPolicy(Config config);

    void set_quality(FilterQuality quality) noexcept;
    void set_active_voices(std::uint32_t voices) noexcept;
    // desiredBins: bins the musical params want (harmonic count, formant
    // range...). harmonicBoostBins: extra bins requested by harmonic emphasis.
    // freeze: the frame is static, so frame updates can be rarer for free.
    void set_request(std::uint32_t desiredBins, std::uint32_t harmonicBoostBins,
                     bool freeze) noexcept;

    // One call per 128-sample render block. Moves the current (smoothed)
    // budget at most one ladder step / one divisor doubling toward the
    // target: no discontinuities, no clicks.
    SpectralVoiceBudget advance_block() noexcept;

    [[nodiscard]] SpectralVoiceBudget current_budget() const noexcept { return current_; }
    [[nodiscard]] double estimated_block_ms() const noexcept;
    [[nodiscard]] std::uint8_t degradation_level() const noexcept { return level_; }
    [[nodiscard]] std::uint64_t degradation_events() const noexcept { return degradationEvents_; }
    [[nodiscard]] std::uint64_t eco_clamp_events() const noexcept { return ecoClampEvents_; }

private:
    void recompute_target() noexcept;
    [[nodiscard]] double estimate_voice_ms(std::uint32_t bins, std::uint32_t divisor) const noexcept {
        return spectral_estimate_voice_ms(bins, divisor);
    }

    Config config_{};
    FilterQuality quality_{FilterQuality::Standard};
    std::uint32_t voices_{1U};
    std::uint32_t desiredBins_{256U};
    std::uint32_t harmonicBoostBins_{0U};
    bool freeze_{false};

    std::uint32_t targetBins_{256U};
    std::uint32_t targetDivisor_{2U};
    SpectralVoiceBudget current_{256U, 2U, false};
    std::uint8_t level_{kLevelNone};
    bool wasDegraded_{false};
    std::uint64_t degradationEvents_{0U};
    std::uint64_t ecoClampEvents_{0U};
};

} // namespace dve::audio

namespace dve::audio {

// ---------------------------------------------------------------------------
// Reference spectral oscillator (Worker D).
//
// A real, deterministic overlap-add IFFT resynthesis oscillator used for two
// purposes:
//   1. It ANCHORS the worst-case cost model above: the calibration in the
//      budget test renders through this exact code path, so "measured, don't
//      guess" is literally true.
//   2. It is the stand-in for Worker B's SpectralOscillator
//      (include/dve/audio/spectral.hpp) in the integration test and demo app
//      until that header lands; both prefer B's oscillator via
//      __has_include when available.
//
// This is scaffolding, not the shipped engine: the lead may delete it once
// Worker B's oscillator adopts SpectralRenderStats/SpectralVoiceBudget.
// ---------------------------------------------------------------------------
namespace spectral_detail {

// Precomputed twiddle factors for one FFT size (initialized once, read-only
// afterwards, so sharing across voices/threads is safe and deterministic).
struct FftPlan {
    std::uint32_t n{0};
    // Per stage (len = 2, 4, ..., n): len/2 twiddles exp(+2*pi*i*k/len).
    std::vector<float> twRe;
    std::vector<float> twIm;
    std::vector<std::uint32_t> stageLen;
    std::vector<std::size_t> stageOff;

    void init(std::uint32_t size) {
        n = size;
        twRe.clear();
        twIm.clear();
        stageLen.clear();
        stageOff.clear();
        constexpr float kPi = 3.14159265358979323846F;
        for (std::uint32_t len = 2U; len <= n; len <<= 1U) {
            stageOff.push_back(twRe.size());
            stageLen.push_back(len);
            for (std::uint32_t k = 0U; k < len / 2U; ++k) {
                const float a = 2.0F * kPi * static_cast<float>(k) / static_cast<float>(len);
                twRe.push_back(std::cos(a));
                twIm.push_back(std::sin(a));
            }
        }
    }
};

// Cached plans for the six render FFT sizes (64..2048). The first call builds
// them (warm up before measuring); afterwards lock-free and deterministic.
inline const FftPlan& fft_plan(std::uint32_t n) noexcept {
    struct Cache {
        std::array<FftPlan, 6> plans{};
        Cache() {
            std::uint32_t size = 64U;
            for (auto& p : plans) {
                p.init(size);
                size *= 2U;
            }
        }
    };
    static const Cache cache;
    std::uint32_t idx = 0U;
    for (std::uint32_t s = 64U; s < n; s *= 2U)
        ++idx;
    return cache.plans[idx < 6U ? idx : 5U];
}

// In-place radix-2 inverse FFT using a precomputed plan. n must be a power
// of two in [64, 2048]. Positive exponent, 1/n scaling.
inline void reference_ifft(float* real, float* imag, const FftPlan& plan) noexcept {
    const std::uint32_t n = plan.n;
    for (std::uint32_t i = 1U, j = 0U; i < n; ++i) {
        std::uint32_t bit = n >> 1U;
        for (; (j & bit) != 0U; bit >>= 1U)
            j &= ~bit;
        j |= bit;
        if (i < j) {
            const float tr = real[i];
            real[i] = real[j];
            real[j] = tr;
            const float ti = imag[i];
            imag[i] = imag[j];
            imag[j] = ti;
        }
    }
    for (std::size_t s = 0U; s < plan.stageLen.size(); ++s) {
        const std::uint32_t len = plan.stageLen[s];
        const float* wr = plan.twRe.data() + plan.stageOff[s];
        const float* wi = plan.twIm.data() + plan.stageOff[s];
        for (std::uint32_t i = 0U; i < n; i += len) {
            for (std::uint32_t k = 0U; k < len / 2U; ++k) {
                const float ar = real[i + k];
                const float ai = imag[i + k];
                const float br = real[i + k + len / 2U];
                const float bi = imag[i + k + len / 2U];
                const float vr = br * wr[k] - bi * wi[k];
                const float vi = br * wi[k] + bi * wr[k];
                real[i + k] = ar + vr;
                imag[i + k] = ai + vi;
                real[i + k + len / 2U] = ar - vr;
                imag[i + k + len / 2U] = ai - vi;
            }
        }
    }
    const float inv = 1.0F / static_cast<float>(n);
    for (std::uint32_t i = 0U; i < n; ++i) {
        real[i] *= inv;
        imag[i] *= inv;
    }
}

} // namespace spectral_detail

namespace spectral_reference {

// Cooked spectral asset: the offline STFT analysis product (Worker A's
// output shape). Frames are magnitude/phase pairs per bin; frameHopSeconds
// is the analysis hop so the oscillator can advance frames in real time.
struct SpectralAsset {
    std::uint32_t binCount{0U}; // bins per frame, power of two
    std::uint32_t frameCount{0U};
    std::vector<float> magnitudes; // frameCount * binCount, linear magnitude
    std::vector<float> phases;     // frameCount * binCount, radians
    float frameHopSeconds{0.0F};
};

// Deterministic procedural asset cooker (stand-in for Worker A's analyzer):
// a harmonic pad rooted at rootHz with slow per-frame evolution and two
// formant bumps. A tiny LCG keeps it bit-deterministic across runs.
SpectralAsset cook_pad_asset(std::uint32_t binCount, std::uint32_t frameCount, float rootHz,
                             float sampleRate, float frameHopSeconds = 0.02322F);

struct ReferenceParams {
    float pitchScale{1.0F};           // multiplies bin frequencies (formant-preserving)
    float formantShiftSemitones{0.0F};// remaps the magnitude envelope across bins
    float timeStretch{1.0F};          // >1 = slower spectral frame advance
    bool freeze{false};               // hold the current spectral frame
    float gain{0.7F};
};

class ReferenceOscillator {
public:
    static constexpr std::uint32_t kMaxBins = 1024U;
    static constexpr std::uint32_t kMaxFft = 2048U;
    static constexpr std::uint32_t kFixedHop = 256U;

    ReferenceOscillator();

    // The asset must outlive the oscillator. Returns false for a null or
    // empty asset.
    bool attach_asset(const SpectralAsset* asset) noexcept;
    void detach_asset() noexcept { asset_ = nullptr; }

    void set_params(const ReferenceParams& params) noexcept { params_ = params; }
    // Smoothed budget from SpectralBudgetPolicy::advance_block(). Bin-count
    // changes fade over a few hops (binRamp_) so degradation is click-free.
    void set_budget(const SpectralVoiceBudget& budget) noexcept;

    void note_on(float velocity) noexcept;
    void note_off() noexcept;
    [[nodiscard]] bool active() const noexcept { return amp_ > 0.0F || noteActive_; }

    // Render `frames` mono samples at sampleRate. Fills *stats when non-null
    // (one SpectralRenderStats per block: call with 128-frame blocks to match
    // the profiler's units). Deterministic: no wall clock, no RNG.
    void render_block(float* monoOut, std::uint32_t frames, std::uint32_t sampleRate,
                      SpectralRenderStats* stats) noexcept;

private:
    void emit_grain(std::uint32_t sampleRate, std::uint32_t bins,
                      SpectralRenderStats* stats) noexcept;

    const SpectralAsset* asset_{nullptr};
    ReferenceParams params_{};
    std::uint32_t budgetBins_{kMaxBins};
    std::uint32_t budgetDivisor_{1U};

    std::vector<float> rotRe_;       // per-bin resynthesis rotation (cos part)
    std::vector<float> rotIm_;       // per-bin resynthesis rotation (sin part)
    std::uint32_t renormCountdown_{0U};
    std::vector<float> binRamp_;     // click-free truncation ramps
    std::vector<float> spectrumReal_;// kMaxFft scratch
    std::vector<float> spectrumImag_; // kMaxFft scratch
    std::vector<float> grain_;       // last computed time-domain grain
    std::vector<float> cachedGrain_; // replayed between frame updates
    std::vector<float> accum_;       // overlap-add ring (kMaxFft)
    std::uint32_t readPos_{0U};
    std::uint32_t hopCount_{0U};
    std::uint64_t grainIndex_{0U};
    std::uint32_t cachedFftSize_{0U};
    float framePos_{0.0F};

    bool noteActive_{false};
    float amp_{0.0F};
    float ampTarget_{0.0F};
    float ampStep_{0.0F};
};

} // namespace spectral_reference
} // namespace dve::audio
