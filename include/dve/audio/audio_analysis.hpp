#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dve/audio/audio_asset.hpp"

namespace dve::audio {

struct AudioWaveformPeak {
    float minimumLeft{};
    float maximumLeft{};
    float minimumRight{};
    float maximumRight{};
};

struct AudioWaveformPeakLevel {
    std::uint32_t framesPerPeak{};
    std::vector<AudioWaveformPeak> peaks;
};

struct AudioWaveformPeakCache {
    std::uint64_t contentHash{};
    std::uint32_t sampleRate{};
    std::uint8_t channels{};
    std::uint64_t frameCount{};
    std::vector<AudioWaveformPeakLevel> levels;
};

[[nodiscard]] AudioWaveformPeakCache build_audio_waveform_peak_cache(
    const DecodedAudioAsset& asset, std::uint32_t baseFramesPerPeak = 64U,
    std::size_t maximumLevels = 16U);
[[nodiscard]] const AudioWaveformPeakLevel* choose_audio_waveform_peak_level(
    const AudioWaveformPeakCache& cache, double framesPerPixel) noexcept;

struct AudioSpectrogram {
    std::uint32_t sampleRate{};
    std::uint32_t windowFrames{};
    std::uint32_t hopFrames{};
    std::uint32_t frequencyBins{};
    std::uint32_t timeBins{};
    float floorDecibels{-100.0F};
    std::vector<float> decibels;
};

[[nodiscard]] AudioSpectrogram build_audio_spectrogram(
    const DecodedAudioAsset& asset, std::uint32_t windowFrames = 512U,
    std::uint32_t hopFrames = 128U, std::uint32_t maximumFrequencyBins = 256U,
    std::uint32_t maximumTimeBins = 4096U, float floorDecibels = -100.0F);

struct AudioAnalysisReport {
    float samplePeak{};
    float approximateTruePeak{};
    float rms{};
    float dcOffset{};
    float crestFactor{};
    float approximateIntegratedLufs{-120.0F};
    std::uint64_t clippedSampleCount{};
    std::uint64_t silentFrameCount{};
    std::vector<std::uint64_t> transientFrames;
    double estimatedTempoBpm{};
    float tempoConfidence{};
};

[[nodiscard]] AudioAnalysisReport analyze_audio_asset(
    const DecodedAudioAsset& asset, float silenceThreshold = 1.0e-4F,
    float transientSensitivity = 1.5F, double minimumTempoBpm = 60.0,
    double maximumTempoBpm = 200.0);

} // namespace dve::audio
