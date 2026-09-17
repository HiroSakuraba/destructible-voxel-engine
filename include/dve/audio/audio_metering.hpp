#pragma once

#include <cstdint>
#include <vector>

#include "dve/audio/audio_asset.hpp"

namespace dve::audio {

struct AudioLoudnessWindow {
    std::uint64_t startFrame{};
    float loudnessLufs{-120.0F};
};

struct AudioLoudnessReport {
    float integratedLufs{-120.0F};
    float loudnessRangeLu{};
    float maximumMomentaryLufs{-120.0F};
    float maximumShortTermLufs{-120.0F};
    float samplePeak{};
    float truePeak{};
    float truePeakDbtp{-120.0F};
    std::uint64_t gatedBlockCount{};
    bool ebuR128Compatible{};
    std::vector<AudioLoudnessWindow> momentary;
    std::vector<AudioLoudnessWindow> shortTerm;
};

// EBU R128 / ITU-R BS.1770-style metering for mono and stereo PCM. The implementation uses the
// specified 48 kHz K-weighting filters, 400 ms/100 ms integration blocks, absolute and relative
// gating, 3-second short-term windows, and four-times oversampled true-peak estimation. Inputs at
// other sample rates are internally converted to 48 kHz for measurement.
[[nodiscard]] AudioLoudnessReport measure_audio_loudness_ebu_r128(
    const DecodedAudioAsset& asset, bool retainWindows = false);

} // namespace dve::audio
