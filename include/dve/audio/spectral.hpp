// Phase 5 spectral resynthesis oscillator (SYN-015, plan section 11).
//
// A per-voice phase-vocoder oscillator: it plays back a cooked spectral asset
// (Worker A: SpectralAsset, an STFT magnitude/phase frame sequence) with
// independent time stretch (phase-vocoder per-bin phase accumulation —
// stretch never changes pitch), freeze, formant shift, harmonic
// stretch/compress, spectral tilt, partial gating, spectral blur, frequency
// quantization, inharmonicity, and seeded randomized partial phases, plus a
// true-stereo path with per-bin phase decorrelation that stays
// mono-compatible at zero spread.
//
// Design rules (mirrors the Phase 4 granular engine):
//   - No allocation on the audio thread: every buffer is a fixed-size
//     std::array member sized for the maximum 4096-point FFT. One
//     SpectralOscillator instance per synth voice.
//   - The engine is decoupled from the synthesizer and from Worker A's asset
//     type: it renders from a plain SpectralAssetView (non-owning pointer
//     view, same philosophy as GranularSource). A null/degenerate asset
//     renders silence, never a crash.
//   - Frame advance happens at control rate (one synthesis frame per hop,
//     hop = FFT/4); per-sample output comes from the overlap-add ring. The
//     realtime IFFT is the engine's own allocation-free radix-2 transform
//     (fft_inplace); Worker A's SpectralFft is the offline/analyzer
//     transform — its current implementation allocates scratch per call, so
//     it stays off the audio thread (cross-validated in tests).
//   - Quality tiers mirror granular's FilterQuality. Eco always wins over
//     musical settings: it truncates to kEcoBins bins, forces nearest-
//     neighbour magnitude lookup, and zeroes blur/tilt/threshold/formant
//     shift/inharmonicity/quantization/phase-random/stereo-spread.
//   - Determinism: same asset + same seed + same params + same pitch input =
//     bit-identical output across instances (seeded xorshift RNG, no
//     wall-clock, no allocation).
//   - Profiler counters are accumulated locally and drained by the owner,
//     mirroring GranularCounters.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "dve/audio/granular.hpp"  // FilterQuality (shared generator CPU tiers)
#include "dve/audio/spectral_asset.hpp"  // Worker A: SpectralAsset (SpectralFft used offline/tests)

namespace dve::audio {

// One STFT analysis frame: magnitude/phase per bin.
// binCount is always fftSize/2 + 1 for the owning asset.
// Magnitude convention: single-sided amplitude, 2*|X[k]|/N for interior bins
// (|X[0]|/N and |X[N/2]|/N at DC/Nyquist), where X is the unnormalized forward
// DFT of the Hann-windowed analysis frame. For a steady sinusoid this reads
// the true peak amplitude times the window's coherent gain (1/2 for Hann),
// so a bin-centered tone of peak amplitude A stores A/2 at its peak bin.
// The oscillator's resynthesis inverts this convention exactly (including the
// window gain and the overlap-add gain), so a steady sinusoid round-trips at
// its original amplitude.
struct SpectralFrame {
    const float* magnitudes{};  // linear amplitude per bin; nullptr = no data
    const float* phases{};      // radians per bin, (-pi, pi]
};

// Non-owning view of a cooked spectral asset (Worker A's SpectralAsset).
// The caller owns the asset storage; the pointer must stay valid for as long
// as the voice renders from it. A null frames pointer, frameCount < 2, a
// zero hopSize, a zero sampleRate, or an fftSize outside Worker A's
// SpectralFft supported range (256..4096) renders silence.
struct SpectralAssetView {
    const SpectralFrame* frames{};
    std::uint32_t frameCount{};
    std::uint32_t fftSize{};       // analysis/synthesis FFT size (power of two)
    std::uint32_t hopSize{};       // analysis hop in samples
    std::uint32_t sampleRate{};
    float baseFrequencyHz{440.0F};  // pitch the asset "means": pitch ratio is 1
                                    // when the oscillator is asked for this Hz
};

// Preset-level spectral oscillator parameters. Field names are the contract
// used by wave-2 workers (serialization, presets), the preset layer, and the
// editor UI. Trivially copyable so it can live in RealtimePreset.
struct SpectralParameters {
    bool enabled{true};
    float gain{0.8F};                 // overall generator gain
    float timeStretch{1.0F};          // > 1 = slower/longer; 2x consumes asset
                                      // frames at half speed (pitch unchanged)
    float freeze01{0.0F};             // >= 0.5 holds the current analysis frame
                                      // indefinitely (infinite sustain)
    float formantShiftSemitones{0.0F};  // bin-domain spectral-envelope shift
                                        // (partials keep their frequencies)
    float harmonicStretch{1.0F};      // > 1 spreads partials apart, < 1
                                      // compresses; 1 = neutral
    float spectralTiltDbPerOct{0.0F};  // spectral tilt, dB per octave, 0 = flat
    float partialThreshold01{0.0F};   // bins below threshold * frame peak are
                                      // gated to zero, 0..1
    float spectralBlur01{0.0F};       // crossfade width across adjacent
                                      // analysis frames, 0..1
    float frequencyQuantize01{0.0F};  // snap partial frequencies to the
                                      // semitone grid, 0..1
    float inharmonicity01{0.0F};      // progressive partial stretch
                                      // (piano-string style), 0..1
    float phaseRandom01{0.0F};        // blend per-bin phase toward a seeded
                                      // random phase, 0..1 (deterministic)
    float stereoSpread01{0.0F};       // per-bin L/R phase decorrelation;
                                      // 0 = bit-identical channels (mono-safe)
    // NOTE: the RNG seed is not a preset field: the voice seeds the engine
    // per note via SpectralOscillator::set_seed() (note/age-derived, like the
    // granular engine), so identical notes render identical output.
    // CPU quality tier. Eco truncates to kEcoBins bins, forces nearest-
    // neighbour magnitude lookup, and zeroes every musical spectral-shaping
    // control (blur/tilt/threshold/formant/inharmonicity/quantize/
    // phase-random/spread). Standard/High/Offline raise the bin ceiling and
    // switch High/Offline to cubic magnitude interpolation.
    FilterQuality spectralQuality{FilterQuality::Standard};
};

// Counters accumulated on the audio thread. The owner drains them (draining
// zeroes) and may forward them into a synth-level profiler.
struct SpectralCounters {
    std::uint64_t framesRendered{};  // synthesis (IFFT) frames computed
    std::uint64_t silentFrames{};    // frames skipped: disabled or bad asset
    std::uint64_t binsRendered{};    // total output bins synthesized
};

// Fixed-buffer phase-vocoder oscillator: one instance per synth voice.
class SpectralOscillator {
public:
    static constexpr std::uint32_t kMaxFftSize = 4096U;
    static constexpr std::uint32_t kMaxBins = kMaxFftSize / 2U + 1U;  // 2049
    static constexpr std::uint32_t kMaxHop = kMaxFftSize / 4U;       // 1024
    static constexpr std::uint32_t kOverlapFactor = 4U;
    // Quality-tier active-bin ceilings (Eco = fewest bins + cheapest path).
    static constexpr std::uint32_t kEcoBins = 256U;
    static constexpr std::uint32_t kStandardBins = 512U;
    static constexpr std::uint32_t kHighBins = 1024U;
    // Maximum inharmonicity coefficient B in f_k *= sqrt(1 + B*k^2) at
    // inharmonicity01 == 1 (piano-like strings sit around B = 1e-4..1e-3).
    static constexpr float kMaxInharmonicityB = 0.002F;

    SpectralOscillator() noexcept;
    SpectralOscillator(const SpectralOscillator&) = default;
    SpectralOscillator& operator=(const SpectralOscillator&) = default;

    // Output sample rate in Hz.
    void set_sample_rate(std::uint32_t sampleRate) noexcept;
    // Seeds the deterministic xorshift stream (per-bin random phases and L/R
    // decorrelation offsets). Same seed + same asset + same params = the
    // per-bin random draws are identical; call after reset() for a fully
    // deterministic note start (mirrors GranularEngine::set_seed).
    void set_seed(std::uint32_t seed) noexcept;
    // Full reset: zeroes phases, overlap-add rings, time pointer, and
    // counters. Does not reseed the RNG (call set_seed explicitly).
    void reset() noexcept;
    // Fast silence: zeroes phases and overlap-add rings without touching the
    // RNG stream or counters (voice kill / all-sound-off path).
    void kill() noexcept;

    // Renders one stereo sample. pitchFrequencyHz is the oscillator pitch
    // input; the pitch ratio is pitchFrequencyHz / asset.baseFrequencyHz and
    // scales partial frequencies without touching the time-stretch clock
    // (stretch != pitch shift: stretch only changes how fast analysis frames
    // are consumed).
    //
    // A null/degenerate asset or disabled params produce silence; the
    // overlap-add ring decays naturally (no clicks, no counters lost).
    std::pair<float, float> render(const SpectralAssetView& asset,
                                   const SpectralParameters& params,
                                   float pitchFrequencyHz) noexcept;

    // Fractional analysis-frame position (wrapped to the asset loop, for
    // telemetry).
    [[nodiscard]] float asset_position_frames() const noexcept { return timeFrames_; }
    // Total analysis frames consumed since reset (never wraps): the honest
    // measure of time-stretch (2x stretch advances this at half speed).
    [[nodiscard]] double total_frames_advanced() const noexcept { return totalFramesAdvanced_; }
    [[nodiscard]] const SpectralCounters& counters() const noexcept { return counters_; }
    // Returns the accumulated counters and zeroes them.
    SpectralCounters drain_counters() noexcept;

    // In-place radix-2 complex FFT/IFFT over exactly n (power of two) points.
    // This is the realtime transform: no allocation, twiddles via a per-stage
    // recurrence, bit-deterministic for a given input on this binary.
    // (Worker A's SpectralFft is the offline/analyzer transform; its current
    // implementation allocates scratch vectors per forward/inverse call, so
    // it cannot run on the audio thread. The test suite cross-validates the
    // two transforms against each other.)
    static void fft_inplace(float* real, float* imag, std::uint32_t n, bool inverse) noexcept;
    // Wraps radians into (-pi, pi].
    static float principal_angle(float radians) noexcept;

private:
    // Active FFT size and hop (hop = fftSize / kOverlapFactor); the buffers
    // are always sized for kMaxFftSize, so switching assets never allocates.
    [[nodiscard]] bool asset_valid(const SpectralAssetView& asset) const noexcept;
    // (Re)configures the synthesis FFT size for a new asset; zeroes all
    // time-domain state and rebuilds the Hann window + OLA normalization.
    void configure_fft(std::uint32_t fftSize) noexcept;
    void synthesize_frame(const SpectralAssetView& asset,
                          const SpectralParameters& params,
                          float pitchFrequencyHz) noexcept;
    // Blur-weighted magnitude lookup: triangular frame window of half-width
    // (1 + 2*blur) around the fractional frame position, then bin
    // interpolation (linear, or Catmull-Rom cubic when cubicBins is true).
    [[nodiscard]] float frame_bin_magnitude(const SpectralAssetView& asset,
                                            float framePos, float binPos,
                                            float blur01, bool cubicBins) const noexcept;
    // Linearly interpolated magnitude of one analysis frame at a fractional
    // bin (helper for frame_bin_magnitude).
    [[nodiscard]] static float frame_magnitude_at(const SpectralFrame& frame,
                                                 std::uint32_t binCount,
                                                 float binPos, bool cubic) noexcept;
    // Circularly interpolated analysis phase of one frame at a fractional bin.
    [[nodiscard]] static float frame_phase_at(const SpectralFrame& frame,
                                              std::uint32_t binCount,
                                              float binPos) noexcept;

    std::uint32_t random_u32() noexcept;

    std::uint32_t sampleRate_{48000U};
    std::uint32_t rngState_{1U};

    // Active synthesis geometry (reconfigured from the asset; always <= max).
    std::uint32_t fftSize_{1024U};
    std::uint32_t hop_{256U};
    std::uint32_t binCount_{513U};

    float timeFrames_{0.0F};  // fractional analysis-frame playback position
    double totalFramesAdvanced_{0.0};  // unwrapped frame consumption (telemetry)

    // Per-bin state (indexed 0 .. kMaxBins; only binCount_ active).
    std::array<float, kMaxBins> outPhase_{};     // accumulated output phase
    std::array<float, kMaxBins> randomPhase_{};  // seeded per-bin random phase
    std::array<float, kMaxBins> decor_{};        // seeded L/R phase offset
    std::array<float, kMaxBins> magnitudes_{};   // scratch: current frame mags
    // False until the output phases are seeded from the asset's analysis
    // phases on the first synthesis frame after a (re)start. Seeding keeps
    // each bin's true relative phase (e.g. the Hann sidelobes' sign), so a
    // steady asset resynthesizes at its analyzed amplitude instead of
    // starting every bin at phase 0 (which partially cancels sidelobes).
    bool phaseSeeded_{false};

    // Overlap-add rings and IFFT scratch (indexed 0 .. kMaxFftSize).
    std::array<float, kMaxFftSize> olaLeft_{};
    std::array<float, kMaxFftSize> olaRight_{};
    std::array<float, kMaxFftSize> fftReal_{};
    std::array<float, kMaxFftSize> fftImag_{};
    std::array<float, kMaxFftSize> hann_{};       // synthesis window
    std::array<float, kMaxHop> olaNorm_{};        // hop-periodic window sum

    std::uint32_t readPos_{0U};           // current output position in the rings
    std::uint32_t samplesUntilFrame_{0U};  // samples left before next synthesis
    std::uint32_t fadeRemaining_{0U};     // fade-in samples left (masks OLA priming)

    SpectralCounters counters_{};
};

// Owning adaptor: converts Worker A's SpectralAsset (interleaved complex
// frames, unnormalized forward DFT) into the magnitude/phase SpectralAssetView
// this engine renders. Convert once, off the audio thread (allocates freely);
// the resulting view() is then safe to hand to render() on the audio thread.
// baseFrequencyHz comes from the asset's estimatedPitchHz (0/unknown falls
// back to the engine's 440 Hz default).
struct SpectralAssetViewData {
    // Returns false when the asset is degenerate (empty, bad geometry, or an
    // unsupported FFT size); the view is null in that case.
    bool convert(const SpectralAsset& asset);
    [[nodiscard]] const SpectralAssetView& view() const noexcept { return view_; }
    [[nodiscard]] bool valid() const noexcept { return view_.frames != nullptr; }

private:
    std::vector<float> magnitudes_;
    std::vector<float> phases_;
    std::vector<SpectralFrame> frames_;
    SpectralAssetView view_{};
};

}  // namespace dve::audio
