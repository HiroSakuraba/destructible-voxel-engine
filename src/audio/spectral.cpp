// Phase 5 spectral resynthesis oscillator (SYN-015).
// See include/dve/audio/spectral.hpp for the design contract.
#include "dve/audio/spectral.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace dve::audio {
namespace {

constexpr float kTwoPi = std::numbers::pi_v<float> * 2.0F;

[[nodiscard]] float clampf(float v, float lo, float hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

[[nodiscard]] float clamp01(float v) noexcept { return clampf(v, 0.0F, 1.0F); }

[[nodiscard]] bool is_power_of_two(std::uint32_t n) noexcept {
    return n != 0U && (n & (n - 1U)) == 0U;
}

}  // namespace

float SpectralOscillator::principal_angle(float radians) noexcept {
    // Wrap into (-pi, pi] via a rounded multiple of 2pi.
    const float turns = std::round(radians / kTwoPi);
    float wrapped = radians - turns * kTwoPi;
    // std::round can leave exactly -pi from below; canonicalize to (-pi, pi].
    if (wrapped <= -std::numbers::pi_v<float>) wrapped += kTwoPi;
    return wrapped;
}

void SpectralOscillator::fft_inplace(float* real, float* imag, std::uint32_t n, bool inverse) noexcept {
    if (real == nullptr || imag == nullptr || n < 2U || !is_power_of_two(n)) return;
    // Bit-reversal permutation.
    std::uint32_t j = 0U;
    for (std::uint32_t i = 1U; i < n; ++i) {
        std::uint32_t bit = n >> 1U;
        while ((j & bit) != 0U) {
            j ^= bit;
            bit >>= 1U;
        }
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }
    // Iterative Cooley-Tukey: twiddle factors via per-stage recurrence so the
    // transform uses no tables and is bit-deterministic for a given input.
    const float sign = inverse ? 1.0F : -1.0F;
    for (std::uint32_t len = 2U; len <= n; len <<= 1U) {
        const float angle = sign * kTwoPi / static_cast<float>(len);
        const float wpr = std::cos(angle);
        const float wpi = std::sin(angle);
        const std::uint32_t half = len >> 1U;
        for (std::uint32_t i = 0U; i < n; i += len) {
            float wr = 1.0F;
            float wi = 0.0F;
            for (std::uint32_t k = 0U; k < half; ++k) {
                const float ar = real[i + k + half];
                const float ai = imag[i + k + half];
                const float tr = wr * ar - wi * ai;
                const float ti = wr * ai + wi * ar;
                real[i + k + half] = real[i + k] - tr;
                imag[i + k + half] = imag[i + k] - ti;
                real[i + k] += tr;
                imag[i + k] += ti;
                const float nextWr = wr * wpr - wi * wpi;
                wi = wr * wpi + wi * wpr;
                wr = nextWr;
            }
        }
    }
    if (inverse) {
        const float inv = 1.0F / static_cast<float>(n);
        for (std::uint32_t i = 0U; i < n; ++i) {
            real[i] *= inv;
            imag[i] *= inv;
        }
    }
}

void SpectralOscillator::set_sample_rate(std::uint32_t sampleRate) noexcept {
    if (sampleRate != 0U) sampleRate_ = sampleRate;
}

// The window and OLA normalization must be valid from birth: the first asset
// may already match the default FFT size, in which case synthesize_frame
// never reconfigures.
SpectralOscillator::SpectralOscillator() noexcept { configure_fft(1024U); }

void SpectralOscillator::set_seed(std::uint32_t seed) noexcept {
    rngState_ = seed != 0U ? seed : 1U;
    // Per-bin random phase in (-pi, pi] and L/R decorrelation offset in
    // [-pi, pi]: deterministic per seed, drawn once here (never on the
    // per-frame path).
    for (std::uint32_t k = 0U; k < kMaxBins; ++k) {
        randomPhase_[k] = random_u32() / 4294967296.0F * kTwoPi - std::numbers::pi_v<float>;
        decor_[k] = (random_u32() / 4294967296.0F * 2.0F - 1.0F) * std::numbers::pi_v<float>;
    }
}

void SpectralOscillator::reset() noexcept {
    configure_fft(fftSize_);
    counters_ = SpectralCounters{};
}

void SpectralOscillator::kill() noexcept {
    olaLeft_.fill(0.0F);
    olaRight_.fill(0.0F);
    outPhase_.fill(0.0F);
    phaseSeeded_ = false;
    timeFrames_ = 0.0F;
    totalFramesAdvanced_ = 0.0;
    samplesUntilFrame_ = 0U;
    readPos_ = 0U;
    fadeRemaining_ = fftSize_;
}

bool SpectralOscillator::asset_valid(const SpectralAssetView& asset) const noexcept {
    if (asset.frames == nullptr || asset.frameCount < 2U) return false;
    // The realtime IFFT provider (Worker A's SpectralFft) is the single
    // source of truth for supported sizes.
    if (!SpectralFft::is_supported_size(asset.fftSize)) return false;
    if (asset.hopSize == 0U || asset.hopSize > asset.fftSize) return false;
    if (asset.sampleRate == 0U) return false;
    if (asset.frames[0].magnitudes == nullptr || asset.frames[0].phases == nullptr) return false;
    return true;
}

void SpectralOscillator::configure_fft(std::uint32_t fftSize) noexcept {
    fftSize_ = fftSize;
    hop_ = fftSize_ / kOverlapFactor;
    binCount_ = fftSize_ / 2U + 1U;
    // Periodic Hann window (denominator N): overlap-add at 4x is exactly
    // constant, and olaNorm_ divides it out numerically regardless.
    const float n = static_cast<float>(fftSize_);
    for (std::uint32_t i = 0U; i < fftSize_; ++i)
        hann_[i] = 0.5F - 0.5F * std::cos(kTwoPi * static_cast<float>(i) / n);
    for (std::uint32_t i = 0U; i < hop_; ++i) {
        float sum = 0.0F;
        for (std::uint32_t m = 0U; m < kOverlapFactor; ++m) sum += hann_[i + m * hop_];
        olaNorm_[i] = sum;
    }
    kill();
}

std::uint32_t SpectralOscillator::random_u32() noexcept {
    // xorshift32 (same family as the granular engine's RNG).
    std::uint32_t x = rngState_;
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    rngState_ = x != 0U ? x : 1U;
    return rngState_;
}

float SpectralOscillator::frame_magnitude_at(const SpectralFrame& frame, std::uint32_t binCount,
                                             float binPos, bool cubic) noexcept {
    if (frame.magnitudes == nullptr || binCount < 2U) return 0.0F;
    const float maxBin = static_cast<float>(binCount - 1U);
    const float p = clampf(binPos, 0.0F, maxBin);
    const float* m = frame.magnitudes;
    const std::uint32_t i = static_cast<std::uint32_t>(p);
    const float f = p - static_cast<float>(i);
    if (!cubic) {
        const std::uint32_t i1 = i + 1U < binCount ? i + 1U : binCount - 1U;
        return m[i] + (m[i1] - m[i]) * f;
    }
    // Catmull-Rom cubic interpolation over clamped neighbours.
    const std::uint32_t i0 = i > 0U ? i - 1U : 0U;
    const std::uint32_t i2 = i + 1U < binCount ? i + 1U : binCount - 1U;
    const std::uint32_t i3 = i + 2U < binCount ? i + 2U : binCount - 1U;
    const float m0 = m[i0], m1 = m[i], m2 = m[i2], m3 = m[i3];
    return m1 + 0.5F * f * (m2 - m0 +
                            f * (2.0F * m0 - 5.0F * m1 + 4.0F * m2 - m3 +
                                 f * (3.0F * (m1 - m2) + m3 - m0)));
}

float SpectralOscillator::frame_phase_at(const SpectralFrame& frame, std::uint32_t binCount,
                                         float binPos) noexcept {
    if (frame.phases == nullptr || binCount < 2U) return 0.0F;
    const float maxBin = static_cast<float>(binCount - 1U);
    const float p = clampf(binPos, 0.0F, maxBin);
    const float* ph = frame.phases;
    const std::uint32_t i = static_cast<std::uint32_t>(p);
    const float f = p - static_cast<float>(i);
    const std::uint32_t i1 = i + 1U < binCount ? i + 1U : binCount - 1U;
    // Circular interpolation so the 2pi branch cut never smears the phase.
    return ph[i] + principal_angle(ph[i1] - ph[i]) * f;
}

float SpectralOscillator::frame_bin_magnitude(const SpectralAssetView& asset, float framePos,
                                              float binPos, float blur01, bool cubicBins) const noexcept {
    // Triangular frame window of half-width (1 + 2*blur) around the
    // fractional position. At blur == 0 this reduces exactly to the linear
    // crossfade between the two adjacent frames (needed for smooth
    // time-stretch); blur > 0 widens the window for spectral smearing.
    const float halfWidth = 1.0F + 2.0F * blur01;
    const std::uint32_t frameCount = asset.frameCount;
    const int jFirst = static_cast<int>(std::floor(framePos - halfWidth));
    const int jLast = static_cast<int>(std::ceil(framePos + halfWidth));
    float sum = 0.0F;
    float weightSum = 0.0F;
    for (int j = jFirst; j <= jLast; ++j) {
        const float d = std::fabs(framePos - static_cast<float>(j));
        if (d >= halfWidth) continue;
        const float w = 1.0F - d / halfWidth;
        const int wrapped = ((j % static_cast<int>(frameCount)) + static_cast<int>(frameCount)) %
                            static_cast<int>(frameCount);
        sum += w * frame_magnitude_at(asset.frames[static_cast<std::uint32_t>(wrapped)], binCount_,
                                      binPos, cubicBins);
        weightSum += w;
    }
    return weightSum > 0.0F ? sum / weightSum : 0.0F;
}

void SpectralOscillator::synthesize_frame(const SpectralAssetView& asset,
                                          const SpectralParameters& params,
                                          float pitchFrequencyHz) noexcept {
    if (!params.enabled || !asset_valid(asset)) {
        ++counters_.silentFrames;
        return;  // overlap-add ring decays naturally; no clicks, no crash
    }
    if (asset.fftSize != fftSize_) configure_fft(asset.fftSize);

    // Seed the output phases from the asset's first analysis frame. The
    // phase-vocoder tracks each bin's instantaneous frequency from here, so
    // seeding only fixes the constant per-bin offset — but that offset is
    // the bins' true relative phase (e.g. the Hann sidelobes sit at the
    // opposite sign from the main lobe). Without it every bin starts at 0
    // and steady spectra partially cancel, costing ~25% of the amplitude.
    // (phaseSeeded_ is false only right after kill(), which also resets the
    // frame clock, so frame 0 is the correct seed here.)
    if (!phaseSeeded_) {
        const float* seedPhases = asset.frames[0].phases;
        if (seedPhases != nullptr) {
            for (std::uint32_t k = 0U; k < binCount_; ++k) outPhase_[k] = seedPhases[k];
        }
        phaseSeeded_ = true;
    }

    // Quality tier: bin ceiling + interpolation path. Eco always wins over
    // musical settings (forced below).
    const bool eco = (params.spectralQuality == FilterQuality::Eco);
    std::uint32_t activeBins = binCount_;
    bool cubicBins = false;
    switch (params.spectralQuality) {
        case FilterQuality::Eco: activeBins = std::min(activeBins, kEcoBins); break;
        case FilterQuality::Standard: activeBins = std::min(activeBins, kStandardBins); break;
        case FilterQuality::High: activeBins = std::min(activeBins, kHighBins); cubicBins = true; break;
        case FilterQuality::Offline: cubicBins = true; break;
    }

    const float base =
        asset.baseFrequencyHz > 0.0F ? asset.baseFrequencyHz : 440.0F;
    float pitchRatio = pitchFrequencyHz > 0.0F ? pitchFrequencyHz / base : 1.0F;
    pitchRatio = clampf(pitchRatio, 1.0F / 64.0F, 64.0F);
    const float harmStretch = eco ? 1.0F : clampf(params.harmonicStretch, 0.25F, 4.0F);
    // R scales partial frequencies (pitch input x harmonic stretch); the
    // time-stretch clock below is independent of R (stretch != pitch shift).
    const float ratio = clampf(pitchRatio * harmStretch, 1.0F / 64.0F, 64.0F);
    const float formantRatio =
        (!eco && params.formantShiftSemitones != 0.0F)
            ? clampf(std::pow(2.0F, params.formantShiftSemitones / 12.0F), 1.0F / 8.0F, 8.0F)
            : 1.0F;
    const float blur01 = eco ? 0.0F : clamp01(params.spectralBlur01);
    const float tiltDb = eco ? 0.0F : clampf(params.spectralTiltDbPerOct, -24.0F, 24.0F);
    const float quant = eco ? 0.0F : clamp01(params.frequencyQuantize01);
    const float inharmon = eco ? 0.0F : clamp01(params.inharmonicity01);
    const float phaseRnd = eco ? 0.0F : clamp01(params.phaseRandom01);
    const float spread = eco ? 0.0F : clamp01(params.stereoSpread01);
    const float inharmonB = inharmon * kMaxInharmonicityB;

    // Time-stretch clock: advance through analysis frames. Independent of the
    // pitch ratio, so 2x stretch renders the asset at half speed with no
    // pitch change. The asset loops (oscillator behaviour); freeze holds the
    // current frame indefinitely while phases keep advancing (infinite
    // sustain of the current spectrum).
    const float stretch = clampf(params.timeStretch, 0.0625F, 16.0F);
    const bool frozen = params.freeze01 >= 0.5F;
    if (!frozen) {
        const float advance =
            (static_cast<float>(hop_) / static_cast<float>(asset.hopSize)) / stretch;
        timeFrames_ += advance;
        totalFramesAdvanced_ += static_cast<double>(advance);
        const float loopLen = static_cast<float>(asset.frameCount - 1U);
        while (timeFrames_ >= loopLen) timeFrames_ -= loopLen;
    }

    const float maxBin = static_cast<float>(binCount_ - 1U);
    const float binHz = static_cast<float>(sampleRate_) / static_cast<float>(fftSize_);
    const float analysisHop = static_cast<float>(asset.hopSize);
    const float twoPiOverN = kTwoPi / static_cast<float>(fftSize_);
    const float hopFloat = static_cast<float>(hop_);

    // Amplitude calibration: the asset stores single-sided magnitudes
    // (2|X|/N interior, |X|/N at DC/Nyquist). Restoring the full DFT scale
    // for the 1/N-normalized inverse FFT needs xN/2 on interior bins — the
    // conjugate mirror below doubles them — and xN on the unmirrored
    // DC/Nyquist bins. The 4/3 compensates the Hann WOLA gain
    // (sum(w^2)/sum(w) = 0.75 at 4x overlap). Together a steady sinusoid
    // resynthesizes at its analyzed amplitude. Applied here so both the L
    // pass and the stereo R pass below inherit it via magnitudes_[k].
    const float nFloat = static_cast<float>(fftSize_);
    const float interiorScale = (2.0F / 3.0F) * nFloat;  // 4/3 * N/2
    const float edgeScale = (4.0F / 3.0F) * nFloat;      // 4/3 * N

    // Pass 1: magnitudes (varispeed-style envelope read at rM = k/(R*F), so
    // formant shift moves the envelope while partials keep their frequencies).
    float peak = 0.0F;
    for (std::uint32_t k = 0U; k < activeBins; ++k) {
        const float kf = static_cast<float>(k);
        float rM = (kf / ratio) / formantRatio;
        rM = clampf(rM, 0.0F, maxBin);
        const float mag = frame_bin_magnitude(asset, timeFrames_, rM, blur01, cubicBins);
        const float scaled =
            mag * ((k == 0U || k + 1U == binCount_) ? edgeScale : interiorScale);
        magnitudes_[k] = scaled;
        if (scaled > peak) peak = scaled;
    }
    const float threshold = eco ? 0.0F : clamp01(params.partialThreshold01) * peak;

    // Pass 2: phase-vocoder phase accumulation + complex spectrum.
    const std::uint32_t frameCount = asset.frameCount;
    const std::uint32_t f0 = static_cast<std::uint32_t>(timeFrames_) % frameCount;
    const std::uint32_t f1 = (f0 + 1U) % frameCount;
    const SpectralFrame& frame0 = asset.frames[f0];
    const SpectralFrame& frame1 = asset.frames[f1];
    const bool stereo = spread > 0.0F;
    for (std::uint32_t k = 0U; k < activeBins; ++k) {
        const float kf = static_cast<float>(k);
        float mag = magnitudes_[k];
        if (mag < threshold) mag = 0.0F;
        if (tiltDb != 0.0F && k > 0U) {
            // dB/oct tilt around the asset's base frequency.
            const float octaves = std::log2((kf * binHz) / base);
            mag *= std::pow(2.0F, (tiltDb / 6.0206F) * clampf(octaves, -8.0F, 8.0F));
        }
        // Partial (pitch) read position: content at analysis bin rP sounds at
        // output bin k with its frequency scaled by `ratio`.
        const float rP = clampf(kf / ratio, 0.0F, maxBin);
        // Instantaneous frequency from the unwrapped analysis phase advance.
        const float ph0 = frame_phase_at(frame0, binCount_, rP);
        const float ph1 = frame_phase_at(frame1, binCount_, rP);
        const float delta =
            principal_angle(ph1 - ph0 - twoPiOverN * rP * analysisHop);
        float freqOut = (twoPiOverN * rP + delta / analysisHop) * ratio;
        if (inharmonB > 0.0F) freqOut *= std::sqrt(1.0F + inharmonB * kf * kf);
        if (quant > 0.0F && freqOut > 0.0F) {
            const float fHz = freqOut * static_cast<float>(sampleRate_) / kTwoPi;
            if (fHz > 1.0F) {
                const float semis = 12.0F * std::log2(fHz / base);
                const float snapped = base * std::pow(2.0F, std::round(semis) / 12.0F);
                const float fq = fHz + (snapped - fHz) * quant;
                freqOut = fq * kTwoPi / static_cast<float>(sampleRate_);
            }
        }
        float phase = outPhase_[k] + freqOut * hopFloat;
        if (phaseRnd > 0.0F) phase += (randomPhase_[k] - phase) * phaseRnd;
        outPhase_[k] = phase;
        // magnitudes_[k] keeps the shaped magnitude for the R pass below.
        fftReal_[k] = mag * std::cos(phase);
        fftImag_[k] = mag * std::sin(phase);
    }
    // Bins above the quality ceiling stay silent.
    for (std::uint32_t k = activeBins; k < binCount_; ++k) {
        fftReal_[k] = 0.0F;
        fftImag_[k] = 0.0F;
    }

    // Inverse FFT per channel with the engine's allocation-free radix-2
    // transform. The single-sided bins are mirrored into a conjugate-
    // symmetric full-length spectrum in the scratch arrays, then transformed
    // in place. The windowed result accumulates into the rings at the current
    // read position (this frame's first sample is the current output sample).
    const std::uint32_t n = fftSize_;
    const std::uint32_t halfN = n / 2U;
    for (std::uint32_t k = 0U; k < binCount_; ++k) {
        if (k > 0U && k < halfN) {
            fftReal_[n - k] = fftReal_[k];
            fftImag_[n - k] = -fftImag_[k];
        }
    }
    fft_inplace(fftReal_.data(), fftImag_.data(), n, true);
    for (std::uint32_t i = 0U; i < n; ++i) {
        const std::uint32_t pos = (readPos_ + i) % fftSize_;
        const float w = hann_[i] * fftReal_[i];
        olaLeft_[pos] += w;
        if (!stereo) olaRight_[pos] += w;
    }
    if (stereo) {
        // True-stereo spectral voice: per-bin L/R phase decorrelation.
        // At spread == 0 the R pass is skipped and both channels share the
        // phase exactly, so the mono sum is bit-identical to either channel
        // (mono-compatible). magnitudes_[k] still holds the shaped magnitude.
        for (std::uint32_t k = 0U; k < activeBins; ++k) {
            const float phaseR = outPhase_[k] + decor_[k] * spread;
            fftReal_[k] = magnitudes_[k] * std::cos(phaseR);
            fftImag_[k] = magnitudes_[k] * std::sin(phaseR);
        }
        for (std::uint32_t k = activeBins; k < binCount_; ++k) {
            fftReal_[k] = 0.0F;
            fftImag_[k] = 0.0F;
        }
        for (std::uint32_t k = 0U; k < binCount_; ++k) {
            if (k > 0U && k < halfN) {
                fftReal_[n - k] = fftReal_[k];
                fftImag_[n - k] = -fftImag_[k];
            }
        }
        fft_inplace(fftReal_.data(), fftImag_.data(), n, true);
        for (std::uint32_t i = 0U; i < n; ++i) {
            const std::uint32_t pos = (readPos_ + i) % fftSize_;
            olaRight_[pos] += hann_[i] * fftReal_[i];
        }
    }

    ++counters_.framesRendered;
    counters_.binsRendered += activeBins;
}

std::pair<float, float> SpectralOscillator::render(const SpectralAssetView& asset,
                                                  const SpectralParameters& params,
                                                  float pitchFrequencyHz) noexcept {
    // One synthesis frame per hop (control rate); per-sample output from the
    // overlap-add rings.
    if (samplesUntilFrame_ == 0U) {
        synthesize_frame(asset, params, pitchFrequencyHz);
        samplesUntilFrame_ = hop_;
    }
    --samplesUntilFrame_;

    const float norm = olaNorm_[readPos_ % hop_];
    float left = olaLeft_[readPos_];
    float right = olaRight_[readPos_];
    olaLeft_[readPos_] = 0.0F;
    olaRight_[readPos_] = 0.0F;
    readPos_ += 1U;
    if (readPos_ >= fftSize_) readPos_ = 0U;
    if (norm > 1.0e-6F) {
        left /= norm;
        right /= norm;
    } else {
        left = 0.0F;
        right = 0.0F;
    }
    // Fade-in over the first FFT length masks the overlap-add priming
    // (partial window sums before the ring is full).
    if (fadeRemaining_ > 0U) {
        const float fade =
            1.0F - static_cast<float>(fadeRemaining_) / static_cast<float>(fftSize_);
        left *= fade;
        right *= fade;
        --fadeRemaining_;
    }
    return {left * params.gain, right * params.gain};
}

SpectralCounters SpectralOscillator::drain_counters() noexcept {
    const SpectralCounters drained = counters_;
    counters_ = SpectralCounters{};
    return drained;
}

bool SpectralAssetViewData::convert(const SpectralAsset& asset) {
    magnitudes_.clear();
    phases_.clear();
    frames_.clear();
    view_ = SpectralAssetView{};
    if (asset.empty() || asset.numBins == 0U) return false;
    if (!SpectralFft::is_supported_size(asset.fftSize)) return false;
    if (asset.numBins != asset.fftSize / 2U + 1U) return false;
    const std::size_t need =
        static_cast<std::size_t>(asset.numFrames) * asset.numBins * 2U;
    if (asset.frames.size() < need) return false;
    const std::uint32_t nFrames = asset.numFrames;
    const std::uint32_t nBins = asset.numBins;
    const float n = static_cast<float>(asset.fftSize);
    magnitudes_.resize(static_cast<std::size_t>(nFrames) * nBins);
    phases_.resize(static_cast<std::size_t>(nFrames) * nBins);
    frames_.resize(nFrames);
    for (std::uint32_t f = 0U; f < nFrames; ++f) {
        const float* src = asset.frame_data(f);
        float* mag = magnitudes_.data() + static_cast<std::size_t>(f) * nBins;
        float* ph = phases_.data() + static_cast<std::size_t>(f) * nBins;
        for (std::uint32_t k = 0U; k < nBins; ++k) {
            const float re = src[2U * k];
            const float im = src[2U * k + 1U];
            // Engine magnitude convention (see SpectralFrame docs):
            // 2*|X[k]|/N interior, |X|/N at DC/Nyquist — inverts exactly, so
            // a steady sinusoid round-trips at its original amplitude.
            const float scale = (k == 0U || k == nBins - 1U) ? 1.0F / n : 2.0F / n;
            mag[k] = std::sqrt(re * re + im * im) * scale;
            ph[k] = std::atan2(im, re);
        }
        frames_[f] = SpectralFrame{mag, ph};
    }
    view_ = SpectralAssetView{frames_.data(), nFrames, asset.fftSize, asset.hopSize,
                              asset.sampleRate, asset.estimatedPitchHz};
    return true;
}

}  // namespace dve::audio
