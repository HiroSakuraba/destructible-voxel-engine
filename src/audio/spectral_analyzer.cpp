// Phase 5 offline spectral analyzer (SYN-015), Worker A.
// Implements SpectralFft (radix-2), analyze_spectrum (STFT), and
// resynthesize_spectral_asset (overlap-add ISTFT) declared in
// include/dve/audio/spectral_asset.hpp. Offline only: allocates freely.
#include "dve/audio/spectral_asset.hpp"

#include <cmath>
#include <complex>
#include <stdexcept>
#include <vector>

namespace dve::audio {
namespace {

constexpr double kPiD = 3.14159265358979323846;

bool is_pow2(std::uint32_t v) noexcept { return v != 0 && (v & (v - 1U)) == 0U; }

using ComplexD = std::complex<double>;

// In-place iterative radix-2 DIT forward FFT on `size` complex samples,
// bit-reversed from `src` into `dst`. Double precision internally; the
// public float interface converts at the boundary.
void dit_forward(const ComplexD* src, ComplexD* dst, const std::uint32_t* bitReverse,
                 std::uint32_t size, double sign) {
    for (std::uint32_t i = 0; i < size; ++i) dst[bitReverse[i]] = src[i];
    for (std::uint32_t len = 2U; len <= size; len <<= 1U) {
        const double ang = sign * 2.0 * kPiD / static_cast<double>(len);
        const ComplexD wlen(std::cos(ang), std::sin(ang));
        for (std::uint32_t i = 0; i < size; i += len) {
            ComplexD w(1.0, 0.0);
            for (std::uint32_t j = 0; j < len / 2U; ++j) {
                const ComplexD u = dst[i + j];
                const ComplexD v = dst[i + j + len / 2U] * w;
                dst[i + j] = u + v;
                dst[i + j + len / 2U] = u - v;
                w *= wlen;
            }
        }
    }
}

}  // namespace

bool SpectralFft::is_supported_size(std::uint32_t size) noexcept {
    return size >= 256U && size <= 4096U && is_pow2(size);
}

SpectralFft::SpectralFft(std::uint32_t size) {
    if (!is_supported_size(size))
        throw std::invalid_argument("SpectralFft: size must be a power of 2 in [256, 4096]");
    size_ = size;
    bitReverse_.resize(size_);
    std::uint32_t bits = 0;
    for (std::uint32_t t = size_; t > 1U; t >>= 1U) ++bits;
    for (std::uint32_t i = 0; i < size_; ++i) {
        std::uint32_t r = 0;
        for (std::uint32_t b = 0; b < bits; ++b)
            if (i & (1U << b)) r |= 1U << (bits - 1U - b);
        bitReverse_[i] = r;
    }
}

void SpectralFft::forward(const float* time, float* outInterleaved) const {
    // Iterative radix-2 DIT on complex pairs (real input -> imag 0),
    // computed in double precision for tight round-trip error.
    std::vector<ComplexD> src(size_), dst(size_);
    for (std::uint32_t i = 0; i < size_; ++i) src[i] = ComplexD(time[i], 0.0);
    dit_forward(src.data(), dst.data(), bitReverse_.data(), size_, -1.0);
    const std::uint32_t bins = size_ / 2U + 1U;
    for (std::uint32_t b = 0; b < bins; ++b) {
        outInterleaved[2U * b] = static_cast<float>(dst[b].real());
        outInterleaved[2U * b + 1U] = static_cast<float>(dst[b].imag());
    }
}

void SpectralFft::inverse(const float* inInterleaved, float* time) const {
    // Rebuild the full conjugate-symmetric spectrum, then the conjugate
    // trick: ifft(X) = conj(fft(conj(X))) / N.
    std::vector<ComplexD> src(size_), dst(size_);
    const std::uint32_t bins = size_ / 2U + 1U;
    for (std::uint32_t b = 0; b < bins; ++b)
        src[b] = ComplexD(inInterleaved[2U * b], inInterleaved[2U * b + 1U]);
    for (std::uint32_t b = bins; b < size_; ++b)
        src[b] = std::conj(src[size_ - b]);
    for (auto& v : src) v = std::conj(v);
    dit_forward(src.data(), dst.data(), bitReverse_.data(), size_, -1.0);
    const double invN = 1.0 / static_cast<double>(size_);
    for (std::uint32_t i = 0; i < size_; ++i)
        time[i] = static_cast<float>(std::conj(dst[i]).real() * invN);
}

SpectralAsset analyze_spectrum(const float* samples,
                               std::uint32_t frameCount,
                               std::uint32_t sampleRate,
                               SpectralAnalyzerConfig config) {
    if (frameCount > 0 && samples == nullptr)
        throw std::invalid_argument("analyze_spectrum: null samples with nonzero frameCount");
    if (sampleRate == 0)
        throw std::invalid_argument("analyze_spectrum: sampleRate must be nonzero");
    if (!SpectralFft::is_supported_size(config.fftSize))
        throw std::invalid_argument("analyze_spectrum: fftSize must be a power of 2 in [256, 4096]");
    if (config.hopSize == 0 || config.hopSize > config.fftSize)
        throw std::invalid_argument("analyze_spectrum: hopSize must be in [1, fftSize]");

    SpectralAsset asset;
    asset.sampleRate = sampleRate;
    asset.fftSize = config.fftSize;
    asset.hopSize = config.hopSize;
    asset.numBins = config.fftSize / 2U + 1U;
    asset.window = SpectralWindowKind::Hann;
    asset.sourceFrameCount = frameCount;

    if (frameCount == 0) return asset;  // zero frames, no crash

    const std::uint32_t n = config.fftSize;
    const std::uint32_t hop = config.hopSize;
    // Ceiling division: frames cover [0, frameCount), zero-padded past the end.
    const std::uint32_t numFrames = (frameCount + hop - 1U) / hop;
    asset.numFrames = numFrames;
    asset.frames.assign(static_cast<std::size_t>(numFrames) * asset.numBins * 2U, 0.0F);
    asset.frameEnergy.assign(numFrames, 0.0F);

    SpectralFft fft(n);
    std::vector<float> windowed(n);
    std::vector<float> spectrum(static_cast<std::size_t>(asset.numBins) * 2U);
    std::vector<double> avgMag(asset.numBins, 0.0);

    for (std::uint32_t f = 0; f < numFrames; ++f) {
        const std::uint32_t start = f * hop;
        double energy = 0.0;
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t src = start + i;
            const float s = (src < frameCount) ? samples[src] : 0.0F;
            const float w = spectral_hann_window(i, n);
            const float ws = s * w;
            windowed[i] = ws;
            energy += static_cast<double>(ws) * ws;
        }
        asset.frameEnergy[f] = static_cast<float>(energy / n);
        fft.forward(windowed.data(), spectrum.data());
        float* dst = asset.frame_data(f);
        for (std::size_t k = 0; k < spectrum.size(); ++k) dst[k] = spectrum[k];
        for (std::uint32_t b = 0; b < asset.numBins; ++b) {
            const float re = spectrum[2U * b];
            const float im = spectrum[2U * b + 1U];
            avgMag[b] += std::sqrt(static_cast<double>(re) * re + im * im);
        }
    }

    // Rough monophonic pitch hint: peak of the average magnitude spectrum
    // (skip DC), parabolic refinement. Require the peak to stand clearly
    // above the spectral floor, else report 0 (unpitched/unknown).
    double total = 0.0;
    for (double m : avgMag) total += m;
    std::uint32_t peakBin = 0;
    double peakMag = 0.0;
    for (std::uint32_t b = 1; b + 1 < asset.numBins; ++b) {
        if (avgMag[b] > peakMag) { peakMag = avgMag[b]; peakBin = b; }
    }
    const double mean = total / asset.numBins;
    if (peakBin > 0 && peakMag > 8.0 * mean && peakMag > 1e-12) {
        // Parabolic interpolation around the peak for sub-bin accuracy.
        const double a = avgMag[peakBin - 1], b = avgMag[peakBin], c = avgMag[peakBin + 1];
        const double denom = (a - 2.0 * b + c);
        double delta = 0.0;
        if (std::fabs(denom) > 1e-12) delta = 0.5 * (a - c) / denom;
        if (delta < -1.0) delta = -1.0;
        if (delta > 1.0) delta = 1.0;
        const double refinedBin = static_cast<double>(peakBin) + delta;
        asset.estimatedPitchHz =
            static_cast<float>(refinedBin * sampleRate / static_cast<double>(n));
    }
    return asset;
}

void resynthesize_spectral_asset(const SpectralAsset& asset, std::vector<float>& outMono) {
    outMono.clear();
    if (asset.empty() || asset.fftSize == 0 || asset.hopSize == 0 || asset.numBins == 0)
        return;
    const std::uint32_t n = asset.fftSize;
    const std::uint32_t hop = asset.hopSize;
    const std::uint32_t outLen = (asset.numFrames - 1U) * hop + n;
    std::vector<double> acc(outLen, 0.0);
    std::vector<double> winSum(outLen, 0.0);

    SpectralFft fft(n);
    std::vector<float> frame(n);
    for (std::uint32_t f = 0; f < asset.numFrames; ++f) {
        // inverse() returns the analysis-windowed frame (the window was
        // applied before the forward transform), so overlap-add it directly
        // and normalize by the accumulated window sum.
        fft.inverse(asset.frame_data(f), frame.data());
        const std::uint32_t start = f * hop;
        for (std::uint32_t i = 0; i < n; ++i) {
            const float w = spectral_hann_window(i, n);
            acc[start + i] += static_cast<double>(frame[i]);
            winSum[start + i] += w;
        }
    }
    outMono.resize(outLen);
    for (std::uint32_t i = 0; i < outLen; ++i) {
        const double ws = winSum[i];
        outMono[i] = (ws > 1e-9) ? static_cast<float>(acc[i] / ws) : 0.0F;
    }
}

}  // namespace dve::audio
