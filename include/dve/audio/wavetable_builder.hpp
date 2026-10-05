// Audio-to-wavetable builder: converts audio recordings into wavetables.
// Pipeline: DC removal -> normalize -> pitch estimate -> cycle extraction ->
// resample -> phase align -> outlier rejection -> cook.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dve/audio/wavetable.hpp"

namespace dve::audio {

// Options for the builder.
struct WavetableBuildOptions {
    float sampleRate{44100.0F};
    std::size_t targetFrames{kHQWavetableFrames}; // frames to extract
    float minFrequencyHertz{20.0F};
    float maxFrequencyHertz{2000.0F};
    float outlierThreshold{2.0F}; // stddevs for rejection
};

// Result of building.
struct WavetableBuildResult {
    CookedWavetable table;
    float estimatedFrequencyHertz{0.0F};
    std::size_t cyclesFound{0};
    std::size_t cyclesKept{0};
    bool success{false};
    std::string error;
};

// Build a wavetable from mono audio samples.
[[nodiscard]] WavetableBuildResult build_wavetable_from_audio(
    const std::string& name,
    const float* samples, std::size_t sampleCount,
    const WavetableBuildOptions& options = {});

// Estimate fundamental frequency via autocorrelation.
[[nodiscard]] float estimate_fundamental_frequency(
    const float* samples, std::size_t sampleCount, float sampleRate,
    float minHz = 20.0F, float maxHz = 2000.0F) noexcept;

} // namespace dve::audio
