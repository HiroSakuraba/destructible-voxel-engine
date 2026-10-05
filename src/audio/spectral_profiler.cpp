// Phase 5 (SYN-015) spectral CPU budgeting implementation.
// Worker D. See include/dve/audio/spectral_profiler.hpp for the documented
// cost model and degradation policy.
#include "dve/audio/spectral_profiler.hpp"

#include <algorithm>

namespace dve::audio {

// ---------------------------------------------------------------------------
// SpectralProfiler
// ---------------------------------------------------------------------------
void SpectralProfiler::record_block(std::uint32_t voice,
                                    const SpectralRenderStats& stats) noexcept {
    if (voice >= SynthSpectralProfiler::kMaxVoices)
        return;
    SpectralRenderStats& pv = data_.perVoice[voice];
    pv.ifftCount += stats.ifftCount;
    pv.frameUpdates += stats.frameUpdates;
    pv.overlapAddSamples += stats.overlapAddSamples;
    pv.ecoClamps += stats.ecoClamps;
    if (stats.activeBins > pv.activeBins)
        pv.activeBins = stats.activeBins;

    data_.total.ifftCount += stats.ifftCount;
    data_.total.frameUpdates += stats.frameUpdates;
    data_.total.overlapAddSamples += stats.overlapAddSamples;
    data_.total.ecoClamps += stats.ecoClamps;
    if (stats.activeBins > data_.total.activeBins)
        data_.total.activeBins = stats.activeBins;
    if (static_cast<std::uint32_t>(stats.activeBins) > data_.peakActiveBins)
        data_.peakActiveBins = static_cast<std::uint32_t>(stats.activeBins);
    ++data_.blocksRendered; // one record_block call == one voice-block
}

void SpectralProfiler::record_degradation() noexcept {
    ++data_.degradationEvents;
}

// ---------------------------------------------------------------------------
// SpectralBudgetPolicy
// ---------------------------------------------------------------------------
SpectralBudgetPolicy::SpectralBudgetPolicy(Config config) : config_(config) {
    recompute_target();
    current_.activeBins = targetBins_;
    current_.frameDivisor = targetDivisor_;
    current_.ecoForced = (level_ == kLevelEco);
}

void SpectralBudgetPolicy::set_quality(FilterQuality quality) noexcept {
    quality_ = quality;
    recompute_target();
}

void SpectralBudgetPolicy::set_active_voices(std::uint32_t voices) noexcept {
    voices_ = std::min<std::uint32_t>(voices, 16U);
    if (voices_ == 0U)
        voices_ = 1U;
    recompute_target();
}

void SpectralBudgetPolicy::set_request(std::uint32_t desiredBins, std::uint32_t harmonicBoostBins,
                                        bool freeze) noexcept {
    desiredBins_ = desiredBins;
    harmonicBoostBins_ = harmonicBoostBins;
    freeze_ = freeze;
    recompute_target();
}

void SpectralBudgetPolicy::recompute_target() noexcept {
    const SpectralTierCeiling ceiling = spectral_tier_ceiling(quality_);

    // Offline is for non-realtime renders: no budget gating, the ceiling
    // passes straight through (a bounce may take as long as it needs).
    if (quality_ == FilterQuality::Offline) {
        targetBins_ = spectral_snap_bins_down(std::min(desiredBins_ + harmonicBoostBins_,
                                                       ceiling.maxActiveBins));
        targetDivisor_ = ceiling.frameDivisor;
        level_ = kLevelNone;
        wasDegraded_ = false;
        return;
    }

    const bool eco = (quality_ == FilterQuality::Eco);

    // Eco tier always wins: the musical request is clamped to the Eco ceiling
    // before any budgeting, so musical params can never raise cost above it.
    std::uint32_t requested = desiredBins_ + harmonicBoostBins_;
    if (eco && requested > ceiling.maxActiveBins) {
        ++ecoClampEvents_;
        requested = ceiling.maxActiveBins;
    }
    const std::uint32_t unconstrainedBins =
        spectral_snap_bins_down(std::min(requested, ceiling.maxActiveBins));
    std::uint32_t unconstrainedDivisor = ceiling.frameDivisor;
    if (freeze_)
        unconstrainedDivisor = std::min(unconstrainedDivisor * 2U, 8U);

    std::uint32_t bins = unconstrainedBins;
    std::uint32_t divisor = unconstrainedDivisor;
    std::uint8_t level = kLevelNone;

    // Per-voice budget gate.
    while (bins > kSpectralBinLadder.front() &&
           estimate_voice_ms(bins, divisor) > static_cast<double>(config_.maxMsPerVoicePerBlock)) {
        bins = spectral_ladder_step_down(bins);
        level = kLevelBins;
    }

    // Global budget, in the mandated order: bins first...
    const auto global_ms = [&]() {
        return static_cast<double>(voices_) * estimate_voice_ms(bins, divisor);
    };
    while (bins > kSpectralBinLadder.front() &&
           global_ms() > static_cast<double>(config_.maxMsGlobalPerBlock)) {
        bins = spectral_ladder_step_down(bins);
        level = kLevelBins;
    }
    // ...then frame-update rate...
    while (divisor < 8U && global_ms() > static_cast<double>(config_.maxMsGlobalPerBlock)) {
        divisor *= 2U;
        if (level < kLevelRate)
            level = kLevelRate;
    }
    // ...then forced Eco behavior.
    if (global_ms() > static_cast<double>(config_.maxMsGlobalPerBlock)) {
        bins = kSpectralBinLadder.front();
        divisor = 8U;
        level = kLevelEco;
    }

    // Count a degradation event when budget gating bites the request and the
    // previous target was not already degraded (no double-counting).
    const bool degradedNow = (bins < unconstrainedBins) || (divisor > unconstrainedDivisor);
    if (degradedNow && !wasDegraded_)
        ++degradationEvents_;
    wasDegraded_ = degradedNow;

    targetBins_ = bins;
    targetDivisor_ = divisor;
    level_ = level;
}

SpectralVoiceBudget SpectralBudgetPolicy::advance_block() noexcept {
    // Slew: at most one ladder step / one divisor doubling per 128-sample
    // block toward the target. Deterministic, no discontinuities.
    if (current_.activeBins < targetBins_)
        current_.activeBins = spectral_ladder_step_up(current_.activeBins);
    else if (current_.activeBins > targetBins_)
        current_.activeBins = spectral_ladder_step_down(current_.activeBins);
    if (current_.frameDivisor < targetDivisor_)
        current_.frameDivisor *= 2U;
    else if (current_.frameDivisor > targetDivisor_)
        current_.frameDivisor /= 2U;
    current_.ecoForced = (level_ == kLevelEco);
    return current_;
}

double SpectralBudgetPolicy::estimated_block_ms() const noexcept {
    return static_cast<double>(voices_) *
           estimate_voice_ms(current_.activeBins, current_.frameDivisor);
}

// ---------------------------------------------------------------------------
// spectral_reference: procedural asset cooker + reference oscillator
// ---------------------------------------------------------------------------
namespace spectral_reference {

SpectralAsset cook_pad_asset(std::uint32_t binCount, std::uint32_t frameCount, float rootHz,
                             float sampleRate, float frameHopSeconds) {
    SpectralAsset asset;
    if (binCount == 0U || frameCount == 0U || rootHz <= 0.0F || sampleRate <= 0.0F)
        return asset;
    asset.binCount = binCount;
    asset.frameCount = frameCount;
    asset.frameHopSeconds = frameHopSeconds;
    asset.magnitudes.assign(static_cast<std::size_t>(frameCount) * binCount, 0.0F);
    asset.phases.assign(static_cast<std::size_t>(frameCount) * binCount, 0.0F);

    const float binHz = sampleRate / (2.0F * static_cast<float>(binCount));
    constexpr float kPi = 3.14159265358979323846F;

    // Deterministic LCG (bit-identical assets across runs/machines).
    std::uint32_t lcg = 0x12345678U;
    const auto rnd01 = [&]() {
        lcg = lcg * 1664525U + 1013904223U;
        return static_cast<float>(lcg >> 8U) * (1.0F / 16777216.0F);
    };

    float hottest = 0.0F;
    for (std::uint32_t f = 0U; f < frameCount; ++f) {
        const float fev = static_cast<float>(f) / static_cast<float>(frameCount); // 0..1
        float frameSum = 0.0F;
        for (std::uint32_t h = 1U; h <= 24U; ++h) {
            const float freq = rootHz * static_cast<float>(h);
            const std::uint32_t b = static_cast<std::uint32_t>(freq / binHz + 0.5F);
            if (b >= binCount)
                break;
            const float beat = 0.72F + 0.28F * std::sin(2.0F * kPi * fev * static_cast<float>(h) +
                                                       static_cast<float>(h));
            float mag = 1.0F / std::pow(static_cast<float>(h), 1.5F) * beat;
            // Two slow formant bumps (vowel-like pad movement).
            const float d1 = (freq - 800.0F) / 500.0F;
            const float d2 = (freq - 2400.0F) / 900.0F;
            const float form = std::exp(-d1 * d1) * (0.6F + 0.4F * std::sin(2.0F * kPi * fev * 2.0F)) +
                               std::exp(-d2 * d2) * (0.5F + 0.5F * std::cos(2.0F * kPi * fev * 3.0F));
            mag *= 0.55F + 0.45F * std::min(form, 1.5F);
            asset.magnitudes[static_cast<std::size_t>(f) * binCount + b] += mag;
            frameSum += mag;
        }
        hottest = std::max(hottest, frameSum);
        for (std::uint32_t b = 0U; b < binCount; ++b)
            asset.phases[static_cast<std::size_t>(f) * binCount + b] = rnd01() * 2.0F * kPi;
    }
    // Normalize so the hottest frame sums to ~1.5 (healthy resynthesis level).
    if (hottest > 0.0F) {
        const float scale = 1.5F / hottest;
        for (float& m : asset.magnitudes)
            m *= scale;
    }
    return asset;
}

ReferenceOscillator::ReferenceOscillator()
    : rotRe_(kMaxBins + 1U, 1.0F), rotIm_(kMaxBins + 1U, 0.0F),
      binRamp_(kMaxBins + 1U, 1.0F), spectrumReal_(kMaxFft, 0.0F),
      spectrumImag_(kMaxFft, 0.0F), grain_(kMaxFft, 0.0F), cachedGrain_(kMaxFft, 0.0F),
      accum_(kMaxFft, 0.0F) {}

bool ReferenceOscillator::attach_asset(const SpectralAsset* asset) noexcept {
    if (asset == nullptr || asset->binCount == 0U || asset->frameCount == 0U ||
        asset->magnitudes.size() < static_cast<std::size_t>(asset->frameCount) * asset->binCount ||
        asset->binCount > kMaxBins)
        return false;
    asset_ = asset;
    framePos_ = 0.0F;
    grainIndex_ = 0U;
    cachedFftSize_ = 0U;
    std::fill(rotRe_.begin(), rotRe_.end(), 1.0F);
    std::fill(rotIm_.begin(), rotIm_.end(), 0.0F);
    renormCountdown_ = 0U;
    std::fill(binRamp_.begin(), binRamp_.end(), 1.0F);
    std::fill(accum_.begin(), accum_.end(), 0.0F);
    return true;
}

void ReferenceOscillator::set_budget(const SpectralVoiceBudget& budget) noexcept {
    budgetBins_ = spectral_snap_bins_down(
        std::clamp(budget.activeBins, kSpectralBinLadder.front(), kMaxBins));
    budgetDivisor_ = std::clamp(budget.frameDivisor, 1U, 8U);
}

void ReferenceOscillator::note_on(float velocity) noexcept {
    noteActive_ = true;
    ampTarget_ = std::clamp(velocity, 0.0F, 1.0F) * params_.gain;
    ampStep_ = (ampTarget_ - amp_) / 240.0F; // 5 ms attack at 48 kHz
}

void ReferenceOscillator::note_off() noexcept {
    noteActive_ = false;
    ampTarget_ = 0.0F;
    ampStep_ = (amp_ > 0.0F) ? -amp_ / 3840.0F : 0.0F; // 80 ms release
}

void ReferenceOscillator::emit_grain(std::uint32_t sampleRate, std::uint32_t bins,
                                      SpectralRenderStats* stats) noexcept {
    constexpr float kPi = 3.14159265358979323846F;
    const std::uint32_t fftSize = 2U * bins;
    // Effective hop: fixed 256 for fftSize >= 512, else fftSize/2 (overlap 2x).
    const std::uint32_t hopEff = bins >= 256U ? kFixedHop : bins;
    const bool update =
        (grainIndex_ % budgetDivisor_ == 0U) || (cachedFftSize_ != fftSize);

    if (update && bins > 0U && asset_ != nullptr) {
        // Advance the spectral frame position (frozen when params_.freeze).
        const float hopSeconds = static_cast<float>(hopEff) *
                                 static_cast<float>(budgetDivisor_) / static_cast<float>(sampleRate);
        if (!params_.freeze && asset_->frameHopSeconds > 0.0F) {
            const float stretch = params_.timeStretch > 0.0F ? params_.timeStretch : 1.0F;
            framePos_ += hopSeconds / (asset_->frameHopSeconds * stretch);
            const float fc = static_cast<float>(asset_->frameCount);
            if (framePos_ >= fc)
                framePos_ = std::fmod(framePos_, fc);
        }
        const std::uint32_t frameCount = asset_->frameCount;
        const std::uint32_t f0 = static_cast<std::uint32_t>(framePos_) % frameCount;
        const std::uint32_t f1 = (f0 + 1U) % frameCount;
        const float frac = framePos_ - std::floor(framePos_);
        const float* mag0 = asset_->magnitudes.data() + static_cast<std::size_t>(f0) * asset_->binCount;
        const float* mag1 = asset_->magnitudes.data() + static_cast<std::size_t>(f1) * asset_->binCount;
        const float formantRatio = std::pow(2.0F, -params_.formantShiftSemitones / 12.0F);
        const float assetBins = static_cast<float>(asset_->binCount);

        // Click-free truncation ramps: ease toward 1 below the budget bin
        // count, 0 above. Budget changes never hard-cut partials.
        for (std::uint32_t b = 0U; b <= kMaxBins; ++b) {
            const float target = (b < bins) ? 1.0F : 0.0F;
            binRamp_[b] += (target - binRamp_[b]) * 0.5F;
        }

        // Phase advance per update covers divisor hops (phase-vocoder style).
        // The per-bin rotation is advanced with a coupled recurrence instead
        // of cos/sin per bin: step_b = step_1^b, rotation_b *= step_b.
        // Renormalized every 64 updates to bound float drift.
        const float phaseStepBase =
            kPi * static_cast<float>(hopEff) * static_cast<float>(budgetDivisor_) /
            static_cast<float>(bins) * params_.pitchScale;
        const float s1re = std::cos(phaseStepBase);
        const float s1im = std::sin(phaseStepBase);
        if (renormCountdown_ == 0U) {
            for (std::uint32_t b = 0U; b <= kMaxBins; ++b) {
                const float n = std::sqrt(rotRe_[b] * rotRe_[b] + rotIm_[b] * rotIm_[b]);
                if (n > 1.0e-12F) {
                    rotRe_[b] /= n;
                    rotIm_[b] /= n;
                } else {
                    rotRe_[b] = 1.0F;
                    rotIm_[b] = 0.0F;
                }
            }
            renormCountdown_ = 64U;
        }
        --renormCountdown_;
        float stRe = 1.0F;
        float stIm = 0.0F;
        for (std::uint32_t b = 0U; b < bins; ++b) {
            float src = static_cast<float>(b) * formantRatio;
            src = std::clamp(src, 0.0F, assetBins - 1.0F);
            const std::uint32_t s0 = static_cast<std::uint32_t>(src);
            const std::uint32_t s1 = std::min(s0 + 1U, asset_->binCount - 1U);
            const float sf = src - static_cast<float>(s0);
            const float mag = ((mag0[s0] * (1.0F - sf) + mag0[s1] * sf) * (1.0F - frac) +
                               (mag1[s0] * (1.0F - sf) + mag1[s1] * sf) * frac) *
                              binRamp_[b];
            const float rr = rotRe_[b];
            const float ri = rotIm_[b];
            const float nr = rr * stRe - ri * stIm;
            const float ni = rr * stIm + ri * stRe;
            rotRe_[b] = nr;
            rotIm_[b] = ni;
            spectrumReal_[b] = mag * nr;
            spectrumImag_[b] = mag * ni;
            const float nsr = stRe * s1re - stIm * s1im;
            stIm = stRe * s1im + stIm * s1re;
            stRe = nsr;
        }
        for (std::uint32_t b = 1U; b < bins; ++b) {
            spectrumReal_[fftSize - b] = spectrumReal_[b];
            spectrumImag_[fftSize - b] = -spectrumImag_[b];
        }
        for (std::uint32_t b = bins; b < fftSize; ++b) {
            spectrumReal_[b] = 0.0F;
            spectrumImag_[b] = 0.0F;
        }

        spectral_detail::reference_ifft(spectrumReal_.data(), spectrumImag_.data(),
                                        spectral_detail::fft_plan(fftSize));

        // Hann window + amplitude normalization: hopEff*0.5 makes a
        // magnitude-1 bin resynthesize to amplitude ~1 through overlap-add.
        const float norm = static_cast<float>(hopEff) * 0.5F;
        for (std::uint32_t i = 0U; i < fftSize; ++i) {
            const float w = 0.5F - 0.5F * std::cos(2.0F * kPi * static_cast<float>(i) /
                                                   static_cast<float>(fftSize));
            cachedGrain_[i] = spectrumReal_[i] * w * norm;
        }
        cachedFftSize_ = fftSize;
        if (stats != nullptr) {
            ++stats->ifftCount;
            ++stats->frameUpdates;
        }
    }
    ++grainIndex_;

    // Overlap-add the (possibly replayed) grain at the current read position.
    const std::uint32_t useFft = cachedFftSize_ > 0U ? cachedFftSize_ : 0U;
    std::uint32_t pos = readPos_;
    for (std::uint32_t i = 0U; i < useFft; ++i) {
        accum_[pos] += cachedGrain_[i];
        pos = (pos + 1U) % kMaxFft;
    }
}

void ReferenceOscillator::render_block(float* monoOut, std::uint32_t frames,
                                       std::uint32_t sampleRate,
                                       SpectralRenderStats* stats) noexcept {
    if (monoOut == nullptr || frames == 0U || sampleRate == 0U)
        return;
    SpectralRenderStats local{};
    const std::uint32_t bins =
        (asset_ != nullptr) ? std::min(budgetBins_, asset_->binCount) : 0U;
    local.activeBins = bins;
    // Effective hop matches the cost model: fixed 256 for fftSize >= 512,
    // else fftSize/2 (keeps overlap >= 2x so there are no gaps).
    const std::uint32_t hopEff = (bins >= 256U || bins == 0U) ? kFixedHop : bins;
    if (hopCount_ > hopEff)
        hopCount_ = hopEff; // budget shrank mid-stream: emit on time

    for (std::uint32_t n = 0U; n < frames; ++n) {
        if (hopCount_ == 0U) {
            emit_grain(sampleRate, bins, &local);
            hopCount_ = hopEff;
        }
        const float s = accum_[readPos_];
        accum_[readPos_] = 0.0F;
        readPos_ = (readPos_ + 1U) % kMaxFft;

        if (amp_ != ampTarget_) {
            amp_ += ampStep_;
            if ((ampStep_ > 0.0F && amp_ >= ampTarget_) ||
                (ampStep_ < 0.0F && amp_ <= ampTarget_)) {
                amp_ = ampTarget_;
                ampStep_ = 0.0F;
            }
        }
        monoOut[n] = s * amp_;
        --hopCount_;
        ++local.overlapAddSamples;
    }

    if (stats != nullptr) {
        stats->ifftCount += local.ifftCount;
        stats->frameUpdates += local.frameUpdates;
        stats->overlapAddSamples += local.overlapAddSamples;
        if (local.activeBins > stats->activeBins)
            stats->activeBins = local.activeBins;
    }
}

} // namespace spectral_reference
} // namespace dve::audio
