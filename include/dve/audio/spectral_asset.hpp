// Phase 5 spectral/resynthesis (SYN-015, plan section 11 "Generator 5").
//
// CONTRACT HEADER owned by Worker A (offline analyzer). Workers B (spectral
// oscillator), C (freeze/stretch/formant/harmonic controls) and D (CPU
// budgeting/preset/editor) build against this file; do not reshape the
// public structs without coordinating with all of them.
//
// The pipeline: an offline analyzer (src/audio/spectral_analyzer.cpp) turns a
// mono float sample buffer into a SpectralAsset — a cooked, trivially
// serializable STFT frame store. The realtime spectral oscillator (Worker B)
// then resynthesizes from the asset under a strict CPU budget (Worker D).
//
// Layout contract (the part other workers depend on):
//   - SpectralFft: self-contained radix-2 FFT, float, power-of-2 sizes
//     256..4096, forward and inverse. Correctness over speed; no external
//     deps (none exist in this repo). The inverse scales by 1/N so
//     inverse(forward(x)) == x.
//   - SpectralAsset: flat POD-ish struct with std::vector<float> payloads.
//     frames holds numFrames * numBins * 2 float32s: for each frame, for each
//     bin 0..numBins-1, an interleaved (real, imag) complex pair. Bins cover
//     DC..Nyquist (numBins == fftSize/2 + 1). Forward frames are Hann-windowed
//     (periodic Hann, w[n] = 0.5*(1 - cos(2*pi*n/fftSize))); the matching
//     inverse path (resynthesize_spectral_asset) overlap-adds with the same
//     window and normalizes by the accumulated window sum, so any Worker B/C
//     frame edit followed by resynthesize stays consistent.
//   - frameEnergy[f] is the mean square of frame f's windowed time samples
//     (0 for silence; never NaN/inf for finite input).
//   - estimatedPitchHz is a rough monophonic pitch estimate in Hz from the
//     average magnitude spectrum (parabolic refinement around the peak bin),
//     or 0.0 when no confident pitched content is found (silence, noise,
//     empty input). Treat it as a hint, not a measurement.
//   - Empty input analyzes to zero frames (numFrames == 0, empty vectors) and
//     never crashes; short input (< fftSize) is zero-padded to one frame.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dve::audio {

// ---------------------------------------------------------------------------
// SpectralFft
// ---------------------------------------------------------------------------

// Self-contained iterative radix-2 Cooley-Tukey FFT, single precision.
// Only power-of-2 sizes in [256, 4096] are accepted; anything else throws
// std::invalid_argument. May allocate in the constructor only.
class SpectralFft {
public:
    explicit SpectralFft(std::uint32_t size);

    std::uint32_t size() const noexcept { return size_; }

    // Forward DFT of `size` real time samples. Writes size/2+1 complex bins
    // as interleaved (real, imag) float pairs: out[2*b], out[2*b+1].
    // Unnormalized (matches the textbook DFT sum).
    void forward(const float* time, float* outInterleaved) const;

    // Inverse DFT of size/2+1 interleaved bins -> `size` real samples.
    // Scales by 1/size, so inverse(forward(x)) reproduces x.
    void inverse(const float* inInterleaved, float* time) const;

    static bool is_supported_size(std::uint32_t size) noexcept;

private:
    std::uint32_t size_{0};
    std::vector<std::uint32_t> bitReverse_;  // size_ entries
};

inline float spectral_hann_window(std::uint32_t n, std::uint32_t size) noexcept {
    // Periodic Hann (no duplicate endpoint), COLA-clean at 75% overlap.
    constexpr float kTwoPi = 6.28318530717958647692F;
    const float x = static_cast<float>(n) / static_cast<float>(size);
    return 0.5F * (1.0F - std::cos(kTwoPi * x));
}

// ---------------------------------------------------------------------------
// SpectralAsset
// ---------------------------------------------------------------------------

enum class SpectralWindowKind : std::uint8_t { Hann };

struct SpectralAnalyzerConfig {
    std::uint32_t fftSize{2048U};  // power of 2, 256..4096
    std::uint32_t hopSize{512U};   // typically fftSize/4 for Hann COLA
};

struct SpectralAsset {
    std::uint32_t sampleRate{0};
    std::uint32_t fftSize{0};
    std::uint32_t hopSize{0};
    std::uint32_t numFrames{0};
    std::uint32_t numBins{0};  // fftSize/2 + 1
    SpectralWindowKind window{SpectralWindowKind::Hann};
    std::uint32_t sourceFrameCount{0};
    float estimatedPitchHz{0.0F};  // 0 = unknown / unpitched

    // numFrames * numBins * 2 floats: per frame, per bin, (real, imag).
    std::vector<float> frames;
    // numFrames floats: mean-square energy of each windowed frame.
    std::vector<float> frameEnergy;

    bool empty() const noexcept { return numFrames == 0; }

    // Pointer to frame f's first bin pair, or nullptr if out of range.
    const float* frame_data(std::uint32_t f) const noexcept {
        if (f >= numFrames || numBins == 0) return nullptr;
        return frames.data() + static_cast<std::size_t>(f) * numBins * 2U;
    }
    float* frame_data(std::uint32_t f) noexcept {
        if (f >= numFrames || numBins == 0) return nullptr;
        return frames.data() + static_cast<std::size_t>(f) * numBins * 2U;
    }

    static float bin_magnitude(const float* interleavedBin) noexcept {
        const float re = interleavedBin[0];
        const float im = interleavedBin[1];
        return std::sqrt(re * re + im * im);
    }
};

// ---------------------------------------------------------------------------
// Offline analysis / resynthesis (defined in src/audio/spectral_analyzer.cpp)
// ---------------------------------------------------------------------------

// Mono float input -> cooked SpectralAsset. Offline only: allocates freely.
// Empty input yields a zero-frame asset; short input is zero-padded to one
// frame. Throws std::invalid_argument on null samples with frameCount > 0,
// sampleRate == 0, or an unsupported fftSize/hopSize.
SpectralAsset analyze_spectrum(const float* samples,
                               std::uint32_t frameCount,
                               std::uint32_t sampleRate,
                               SpectralAnalyzerConfig config = SpectralAnalyzerConfig{});

// Inverse of the analysis path: overlap-adds the asset's Hann-windowed
// frames and normalizes by the accumulated window sum. Output length is
// (numFrames-1)*hopSize + fftSize (0 for an empty asset). Offline only:
// allocates freely.
void resynthesize_spectral_asset(const SpectralAsset& asset, std::vector<float>& outMono);

}  // namespace dve::audio
