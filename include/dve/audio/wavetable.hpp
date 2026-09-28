// High-quality wavetable oscillator for Phase 1.
// 64 frames x 512 samples, band-limited mip levels, cubic interpolation,
// phase distortion, and position modulation.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dve::audio {

inline constexpr std::size_t kHQWavetableFrames = 64;
inline constexpr std::size_t kHQWavetableSamples = 512;
inline constexpr std::size_t kHQWavetableMips = 6;

// A cooked wavetable: frames x samples, with band-limited mip levels.
// Mip 0 is full bandwidth; each higher mip halves the harmonic content.
struct CookedWavetable {
    std::string name{"Init"};
    // samples[mip][frame][sample]
    std::vector<float> samples;
    std::uint64_t contentHash{0};

    CookedWavetable();
    [[nodiscard]] bool valid() const noexcept { return !samples.empty(); }
    [[nodiscard]] const float* frame_data(std::size_t mip, std::size_t frame) const noexcept;
};

// Builds a CookedWavetable from raw frames (each frame = kHQWavetableSamples).
// Computes mip levels by progressive lowpass filtering.
[[nodiscard]] CookedWavetable cook_wavetable(const std::string& name,
                                            const std::vector<std::vector<float>>& frames);

// Factory wavetables.
[[nodiscard]] CookedWavetable make_basic_morph_table();
[[nodiscard]] CookedWavetable make_harmonic_series_table();
[[nodiscard]] CookedWavetable make_formant_table();
[[nodiscard]] CookedWavetable make_digital_table();

// Oscillator state (per voice per wavetable osc).
struct WavetableOscState {
    float phase{0.0F};        // 0..1
    float position{0.0F};     // 0..1 through table
    float phaseDistortion{0.0F}; // -1..1
};

// Sample the wavetable with cubic interpolation between frames and samples.
// mip selected by caller based on frequency.
[[nodiscard]] float sample_wavetable(const CookedWavetable& table,
                                     float phase, float position,
                                     std::size_t mip) noexcept;

// Select mip level for a given frequency and sample rate.
[[nodiscard]] std::size_t wavetable_mip_for_frequency(float frequencyHertz,
                                                      float sampleRate) noexcept;

} // namespace dve::audio
