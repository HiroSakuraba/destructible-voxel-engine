// Phase 6 audio-to-synth search (SYN-016, plan section 12 "Generator 6"),
// Worker A: feature extractor.
//
// Implements include/dve/audio/audio_features.hpp exactly. Offline analysis
// only: framing + Hann-windowed mean magnitude spectra come from the Phase 5
// SpectralFft (include/dve/audio/spectral_asset.hpp); pitch comes from a
// normalized autocorrelation (YIN-lite style) on the mono mix; the amplitude
// envelope comes from a block-peak follower.
//
// Finite-safety: every public entry point returns finite values for finite
// input (0.0 for undefined quantities). Degenerate input (null, empty,
// silence, bad channel count/rate) yields an all-zero vector.

#include "dve/audio/audio_features.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "dve/audio/spectral_asset.hpp"

namespace dve::audio {
namespace {

constexpr std::uint32_t kFftSize = 2048U;
constexpr std::uint32_t kHopSize = 512U;
constexpr std::uint32_t kNumMelBands = 16U;
constexpr std::size_t kEnvBlock = 256U;

// Smallest positive magnitude treated as "has signal" for log/mean math.
constexpr double kEps = 1e-12;

inline double finite_or(double v, double fallback = 0.0) noexcept {
    return std::isfinite(v) ? v : fallback;
}

inline double clamp01(double v) noexcept {
    if (!std::isfinite(v)) return 0.0;
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

// log10(1 + f/700) mel scale, double precision.
inline double hz_to_mel(double hz) noexcept {
    return 2595.0 * std::log10(1.0 + hz / 700.0);
}

}  // namespace

// ---------------------------------------------------------------------------
// feature_distance
// ---------------------------------------------------------------------------

double feature_distance(const AudioFeatureVector& a, const AudioFeatureVector& b,
                        const FeatureWeights& weights) noexcept {
    // Weighted RMS over the 14 scalar fields, with the 16 mel bands folded
    // into one averaged term (mean of squared band differences).
    const double da[14] = {
        a.rms - b.rms,
        a.peak - b.peak,
        a.spectralCentroidHz - b.spectralCentroidHz,
        a.spectralRolloffHz - b.spectralRolloffHz,
        a.spectralFlatness - b.spectralFlatness,
        a.zeroCrossingRate - b.zeroCrossingRate,
        a.estimatedPitchHz - b.estimatedPitchHz,
        a.pitchConfidence - b.pitchConfidence,
        a.harmonicEnergyRatio - b.harmonicEnergyRatio,
        a.attackSeconds - b.attackSeconds,
        a.decaySeconds - b.decaySeconds,
        a.sustainLevel - b.sustainLevel,
        a.releaseSeconds - b.releaseSeconds,
        a.stereoWidth - b.stereoWidth,
    };
    const double w[14] = {
        weights.rms, weights.peak,
        weights.spectralCentroidHz, weights.spectralRolloffHz,
        weights.spectralFlatness, weights.zeroCrossingRate,
        weights.estimatedPitchHz, weights.pitchConfidence,
        weights.harmonicEnergyRatio, weights.attackSeconds,
        weights.decaySeconds, weights.sustainLevel,
        weights.releaseSeconds, weights.stereoWidth,
    };
    double total = 0.0;
    double wsum = 0.0;
    for (int i = 0; i < 14; ++i) {
        const double wi = finite_or(w[i]);
        if (wi == 0.0) continue;
        const double d = finite_or(da[i]);
        total += wi * d * d;
        wsum += wi;
    }
    const double wmel = finite_or(weights.melBands);
    if (wmel != 0.0) {
        double melSq = 0.0;
        for (std::uint32_t i = 0; i < kNumMelBands; ++i) {
            const double d = finite_or(a.melBands[i] - b.melBands[i]);
            melSq += d * d;
        }
        melSq /= static_cast<double>(kNumMelBands);
        total += wmel * melSq;
        wsum += wmel;
    }
    if (!(wsum > 0.0) || !std::isfinite(total)) return 0.0;
    const double ms = total / wsum;
    return (ms > 0.0 && std::isfinite(ms)) ? std::sqrt(ms) : 0.0;
}

// ---------------------------------------------------------------------------
// Internal analysis helpers
// ---------------------------------------------------------------------------

namespace {

// Build the mono analysis mix and (for stereo) mid/side energies.
// Non-finite input samples are treated as 0. Returns false when the input
// is unusable (caller then returns a zero vector).
bool build_mix(const float* samples, std::size_t numFrames, int numChannels,
               std::vector<double>& mid, double& midEnergy, double& sideEnergy) noexcept {
    if (samples == nullptr || numFrames == 0) return false;
    if (numChannels != 1 && numChannels != 2) return false;
    mid.assign(numFrames, 0.0);
    midEnergy = 0.0;
    sideEnergy = 0.0;
    for (std::size_t i = 0; i < numFrames; ++i) {
        double l = static_cast<double>(samples[i * static_cast<std::size_t>(numChannels)]);
        double r = (numChannels == 2)
                       ? static_cast<double>(samples[i * 2U + 1U])
                       : l;
        if (!std::isfinite(l)) l = 0.0;
        if (!std::isfinite(r)) r = 0.0;
        const double m = 0.5 * (l + r);
        const double s = 0.5 * (l - r);
        mid[i] = m;
        midEnergy += m * m;
        sideEnergy += s * s;
    }
    return true;
}

// Mean magnitude spectrum (bins 0..fftSize/2) over Hann-windowed frames.
// Short input is zero-padded to a single frame.
void mean_magnitude_spectrum(const std::vector<double>& mid, std::vector<double>& meanMag,
                             std::uint32_t& numSpecFrames) {
    const std::size_t n = mid.size();
    const std::uint32_t bins = kFftSize / 2U + 1U;
    meanMag.assign(bins, 0.0);
    numSpecFrames = 0;
    if (n == 0) return;
    SpectralFft fft(kFftSize);  // 2048 is a supported size; cannot throw here
    std::vector<float> windowed(kFftSize);
    std::vector<float> spec(kFftSize + 2U);
    const std::size_t count = (n >= kFftSize) ? (1U + (n - kFftSize) / kHopSize) : 1U;
    for (std::size_t f = 0; f < count; ++f) {
        const std::size_t start = f * kHopSize;
        for (std::uint32_t i = 0; i < kFftSize; ++i) {
            const double x = (start + i < n) ? mid[start + i] : 0.0;
            windowed[i] = static_cast<float>(x * spectral_hann_window(i, kFftSize));
        }
        fft.forward(windowed.data(), spec.data());
        for (std::uint32_t b = 0; b < bins; ++b) {
            const double re = spec[2U * b];
            const double im = spec[2U * b + 1U];
            meanMag[b] += std::sqrt(re * re + im * im);
        }
    }
    const double inv = 1.0 / static_cast<double>(count);
    for (std::uint32_t b = 0; b < bins; ++b) meanMag[b] *= inv;
    numSpecFrames = static_cast<std::uint32_t>(count);
}

struct PitchResult {
    double hz = 0.0;
    double confidence = 0.0;
};

// Normalized autocorrelation pitch (YIN-lite): parabolic refinement around
// the strongest non-zero-lag peak. Returns 0/0 when silent or unpitched.
PitchResult estimate_pitch(const std::vector<double>& mid, double sampleRate) noexcept {
    PitchResult out;
    const std::size_t n = mid.size();
    if (n < 64 || !(sampleRate > 0.0) || !std::isfinite(sampleRate)) return out;
    // Remove DC so it cannot bias the correlation.
    double mean = 0.0;
    for (double x : mid) mean += x;
    mean /= static_cast<double>(n);
    double r0 = 0.0;
    for (double x : mid) {
        const double c = x - mean;
        r0 += c * c;
    }
    if (!(r0 > 0.0) || !std::isfinite(r0)) return out;  // silent
    std::size_t lagMin = static_cast<std::size_t>(sampleRate / 2000.0);
    std::size_t lagMax = static_cast<std::size_t>(sampleRate / 40.0);
    if (lagMin < 2) lagMin = 2;
    if (lagMax > n / 2) lagMax = n / 2;
    if (lagMax <= lagMin) return out;
    const double invR0 = 1.0 / r0;
    double best = -2.0;
    std::size_t bestLag = lagMin;
    // First pass: find the strongest peak lag.
    for (std::size_t lag = lagMin; lag <= lagMax; ++lag) {
        double r = 0.0;
        for (std::size_t i = 0; i + lag < n; ++i) r += (mid[i] - mean) * (mid[i + lag] - mean);
        const double nr = r * invR0;
        if (nr > best) {
            best = nr;
            bestLag = lag;
        }
    }
    if (!(best > 0.35) || !std::isfinite(best)) return out;  // unpitched
    // Parabolic interpolation around the peak for sub-sample accuracy.
    double lagF = static_cast<double>(bestLag);
    if (bestLag > lagMin && bestLag < lagMax) {
        double y[3] = {0.0, 0.0, 0.0};
        for (int k = -1; k <= 1; ++k) {
            const std::size_t lag = bestLag + static_cast<std::size_t>(k);
            double r = 0.0;
            for (std::size_t i = 0; i + lag < n; ++i) r += (mid[i] - mean) * (mid[i + lag] - mean);
            y[k + 1] = r * invR0;
        }
        const double denom = y[0] - 2.0 * y[1] + y[2];
        if (std::fabs(denom) > 1e-9) {
            double shift = 0.5 * (y[0] - y[2]) / denom;
            if (shift > 1.0) shift = 1.0;
            if (shift < -1.0) shift = -1.0;
            lagF += shift;
        }
    }
    if (lagF < 1.0 || !std::isfinite(lagF)) return out;
    out.hz = finite_or(sampleRate / lagF);
    out.confidence = clamp01(best);
    if (!(out.hz > 0.0) || !std::isfinite(out.hz)) {
        out.hz = 0.0;
        out.confidence = 0.0;
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// extract_audio_features
// ---------------------------------------------------------------------------

AudioFeatureVector extract_audio_features(const float* samples, std::size_t numFrames,
                                         int numChannels, double sampleRate) noexcept {
    AudioFeatureVector v;  // all zeros by default
    if (!(sampleRate > 0.0) || !std::isfinite(sampleRate)) return v;
    if (sampleRate > 1000000.0) return v;  // absurd rate; stay defensive

    std::vector<double> mid;
    double midEnergy = 0.0, sideEnergy = 0.0;
    if (!build_mix(samples, numFrames, numChannels, mid, midEnergy, sideEnergy)) return v;
    const std::size_t n = mid.size();
    const double duration = static_cast<double>(n) / sampleRate;
    if (!(duration > 0.0)) return v;

    // ---- Level -----------------------------------------------------------
    double peak = 0.0;
    for (double x : mid) {
        const double ax = std::fabs(x);
        if (ax > peak) peak = ax;
    }
    v.rms = finite_or(std::sqrt(midEnergy / static_cast<double>(n)));
    v.peak = finite_or(peak);

    // ---- Stereo width ----------------------------------------------------
    {
        const double denom = midEnergy + sideEnergy;
        v.stereoWidth = (denom > 0.0 && std::isfinite(denom))
                            ? clamp01(sideEnergy / denom)
                            : 0.0;
    }

    // ---- Zero-crossing rate ----------------------------------------------
    {
        std::size_t crossings = 0;
        bool prevPos = mid[0] >= 0.0;
        for (std::size_t i = 1; i < n; ++i) {
            const bool pos = mid[i] >= 0.0;
            if (pos != prevPos) ++crossings;
            prevPos = pos;
        }
        v.zeroCrossingRate = finite_or(static_cast<double>(crossings) / duration);
    }

    // ---- Spectral features from the mean magnitude spectrum --------------
    std::vector<double> meanMag;
    std::uint32_t numSpecFrames = 0;
    mean_magnitude_spectrum(mid, meanMag, numSpecFrames);
    const std::uint32_t bins = static_cast<std::uint32_t>(meanMag.size());
    const double binHz = sampleRate / static_cast<double>(kFftSize);

    double magSum = 0.0;      // sum of magnitudes, bins 1..N (skip DC)
    double magSqSum = 0.0;    // sum of squared magnitudes (energy)
    double centroidNum = 0.0;
    for (std::uint32_t b = 1; b < bins; ++b) {
        const double m = meanMag[b];
        magSum += m;
        magSqSum += m * m;
        centroidNum += m * (static_cast<double>(b) * binHz);
    }
    if (magSum > 0.0 && std::isfinite(magSum) && std::isfinite(centroidNum)) {
        v.spectralCentroidHz = centroidNum / magSum;
    }
    // 85% rolloff on the magnitude spectrum, linearly interpolated.
    if (magSum > 0.0 && std::isfinite(magSum)) {
        const double target = 0.85 * magSum;
        double cum = 0.0;
        double rolloff = 0.0;
        for (std::uint32_t b = 1; b < bins; ++b) {
            const double prev = cum;
            cum += meanMag[b];
            if (cum >= target) {
                const double frac = (meanMag[b] > 0.0) ? (target - prev) / meanMag[b] : 0.0;
                rolloff = (static_cast<double>(b - 1) + std::clamp(frac, 0.0, 1.0)) * binHz;
                break;
            }
        }
        v.spectralRolloffHz = finite_or(rolloff);
    }
    // Spectral flatness: geometric / arithmetic mean of the mean magnitudes.
    if (magSum > 0.0 && std::isfinite(magSum)) {
        double logSum = 0.0;
        std::uint32_t count = 0;
        for (std::uint32_t b = 1; b < bins; ++b) {
            const double m = meanMag[b];
            if (m > 0.0) {
                logSum += std::log(m);
                ++count;
            }
        }
        if (count > 0) {
            const double geo = std::exp(logSum / static_cast<double>(count));
            const double arith = magSum / static_cast<double>(count);
            v.spectralFlatness = (arith > 0.0 && std::isfinite(geo) && std::isfinite(arith))
                                     ? clamp01(geo / arith)
                                     : 0.0;
        }
    }

    // ---- Pitch + harmonic energy ratio -----------------------------------
    const PitchResult pitch = estimate_pitch(mid, sampleRate);
    v.estimatedPitchHz = pitch.hz;
    v.pitchConfidence = pitch.confidence;
    if (pitch.hz > 0.0 && magSqSum > 0.0 && std::isfinite(magSqSum)) {
        const double nyquist = 0.5 * sampleRate;
        std::vector<char> counted(bins, 0);
        double harmE = 0.0;
        const std::uint32_t maxH = static_cast<std::uint32_t>(nyquist / pitch.hz);
        for (std::uint32_t h = 1; h <= maxH; ++h) {
            const double f = static_cast<double>(h) * pitch.hz;
            const std::uint32_t lo =
                static_cast<std::uint32_t>(std::max(1.0, std::floor((f * 0.97) / binHz)));
            std::uint32_t hi = static_cast<std::uint32_t>(std::ceil((f * 1.03) / binHz));
            if (hi >= bins) hi = bins - 1U;
            for (std::uint32_t b = lo; b <= hi; ++b) {
                if (!counted[b]) {
                    counted[b] = 1;
                    harmE += meanMag[b] * meanMag[b];
                }
            }
        }
        v.harmonicEnergyRatio = (harmE >= 0.0 && std::isfinite(harmE))
                                    ? clamp01(harmE / magSqSum)
                                    : 0.0;
    }

    // ---- Amplitude envelope (block peak follower) ------------------------
    {
        const std::size_t nBlocks = (n + kEnvBlock - 1U) / kEnvBlock;
        double envPeak = 0.0;
        std::vector<double> env(nBlocks, 0.0);
        for (std::size_t b = 0; b < nBlocks; ++b) {
            double m = 0.0;
            const std::size_t start = b * kEnvBlock;
            const std::size_t end = std::min(start + kEnvBlock, n);
            for (std::size_t i = start; i < end; ++i) {
                const double ax = std::fabs(mid[i]);
                if (ax > m) m = ax;
            }
            env[b] = m;
            if (m > envPeak) envPeak = m;
        }
        if (envPeak > 0.0 && std::isfinite(envPeak)) {
            const auto t = [&](std::size_t b) {
                return static_cast<double>(b * kEnvBlock) / sampleRate;
            };
            // Attack: first 10% crossing -> first 90% crossing.
            std::size_t i10 = nBlocks, i90 = nBlocks;
            for (std::size_t b = 0; b < nBlocks; ++b) {
                if (i10 == nBlocks && env[b] >= 0.1 * envPeak) i10 = b;
                if (env[b] >= 0.9 * envPeak) {
                    i90 = b;
                    break;
                }
            }
            if (i10 != nBlocks && i90 != nBlocks && i90 >= i10)
                v.attackSeconds = finite_or(t(i90) - t(i10));
            // Peak time: first block at ~full peak.
            std::size_t iPeak = 0;
            for (std::size_t b = 0; b < nBlocks; ++b) {
                if (env[b] >= 0.99 * envPeak) {
                    iPeak = b;
                    break;
                }
            }
            // Sustain: mean envelope over the last 15% of blocks.
            std::size_t tailStart = nBlocks > 0 ? (nBlocks * 85U) / 100U : 0;
            if (tailStart >= nBlocks) tailStart = nBlocks > 0 ? nBlocks - 1 : 0;
            double tailSum = 0.0;
            std::size_t tailCount = 0;
            for (std::size_t b = tailStart; b < nBlocks; ++b) {
                tailSum += env[b];
                ++tailCount;
            }
            const double sustainAbs = tailCount > 0 ? tailSum / static_cast<double>(tailCount) : 0.0;
            v.sustainLevel = clamp01(sustainAbs / envPeak);
            // Decay: peak -> envelope settles near sustain.
            const double decayTarget = sustainAbs + 0.05 * envPeak;
            std::size_t iDecay = nBlocks;
            for (std::size_t b = iPeak; b < nBlocks; ++b) {
                if (env[b] <= decayTarget) {
                    iDecay = b;
                    break;
                }
            }
            if (iDecay != nBlocks && iDecay >= iPeak)
                v.decaySeconds = finite_or(t(iDecay) - t(iPeak));
            // Release: last meaningfully-above-sustain block -> -60 dB or buffer end.
            const double sustainFloor = 0.5 * sustainAbs + 1e-4 * envPeak;
            std::size_t iSusEnd = 0;
            for (std::size_t b = 0; b < nBlocks; ++b) {
                if (env[b] >= sustainFloor) iSusEnd = b;
            }
            std::size_t iEnd = nBlocks;  // buffer end
            for (std::size_t b = iSusEnd; b < nBlocks; ++b) {
                if (env[b] <= 0.001 * envPeak) {  // -60 dB
                    iEnd = b;
                    break;
                }
            }
            const double tEnd = (iEnd == nBlocks) ? duration : t(iEnd);
            v.releaseSeconds = finite_or(std::max(0.0, tEnd - t(iSusEnd)));
        }
    }

    // ---- 16 log mel bands, centered to ~[-1, 1] ---------------------------
    {
        const double nyquist = 0.5 * sampleRate;
        const double melLo = hz_to_mel(0.0);
        const double melHi = hz_to_mel(nyquist);
        double edges[kNumMelBands + 2];
        for (std::uint32_t i = 0; i <= kNumMelBands + 1U; ++i)
            edges[i] = melLo + (melHi - melLo) * static_cast<double>(i) /
                                    static_cast<double>(kNumMelBands + 1U);
        double bandE[kNumMelBands] = {};
        for (std::uint32_t b = 1; b < bins; ++b) {
            const double m = meanMag[b];
            if (!(m > 0.0)) continue;
            const double mel = hz_to_mel(static_cast<double>(b) * binHz);
            for (std::uint32_t band = 0; band < kNumMelBands; ++band) {
                const double lo = edges[band], pk = edges[band + 1], hi = edges[band + 2];
                double wgt = 0.0;
                if (mel >= lo && mel <= pk && pk > lo)
                    wgt = (mel - lo) / (pk - lo);
                else if (mel > pk && mel <= hi && hi > pk)
                    wgt = (hi - mel) / (hi - pk);
                if (wgt > 0.0) bandE[band] += wgt * m;
            }
        }
        double logE[kNumMelBands];
        double logMean = 0.0;
        for (std::uint32_t i = 0; i < kNumMelBands; ++i) {
            logE[i] = std::log(bandE[i] + kEps);
            logMean += logE[i];
        }
        logMean /= static_cast<double>(kNumMelBands);
        for (std::uint32_t i = 0; i < kNumMelBands; ++i) {
            const double centered = logE[i] - logMean;
            v.melBands[i] = finite_or(std::tanh(0.5 * centered));
        }
    }

    // Final sweep: guarantee finiteness no matter what.
    v.rms = finite_or(v.rms);
    v.peak = finite_or(v.peak);
    v.spectralCentroidHz = finite_or(v.spectralCentroidHz);
    v.spectralRolloffHz = finite_or(v.spectralRolloffHz);
    v.spectralFlatness = finite_or(v.spectralFlatness);
    v.zeroCrossingRate = finite_or(v.zeroCrossingRate);
    v.estimatedPitchHz = finite_or(v.estimatedPitchHz);
    v.pitchConfidence = finite_or(v.pitchConfidence);
    v.harmonicEnergyRatio = finite_or(v.harmonicEnergyRatio);
    v.attackSeconds = finite_or(v.attackSeconds);
    v.decaySeconds = finite_or(v.decaySeconds);
    v.sustainLevel = finite_or(v.sustainLevel);
    v.releaseSeconds = finite_or(v.releaseSeconds);
    v.stereoWidth = finite_or(v.stereoWidth);
    return v;
}

}  // namespace dve::audio
