#pragma once

#include <cstdint>
#include <optional>

#include "dve/audio/audio_asset.hpp"

namespace dve::audio {

struct AudioSilenceTrimOptions {
    float thresholdLinear{1.0e-4F};
    std::uint32_t minimumSilenceFrames{256U};
    std::uint32_t preserveFrames{64U};
};

[[nodiscard]] DecodedAudioAsset audio_remove_dc(const DecodedAudioAsset& asset);
[[nodiscard]] DecodedAudioAsset audio_normalize_peak(const DecodedAudioAsset& asset,
                                                      float targetPeak = 0.9885531F);
[[nodiscard]] DecodedAudioAsset audio_normalize_loudness(const DecodedAudioAsset& asset,
                                                          float targetLufs = -23.0F,
                                                          float maximumTruePeak = 0.9885531F);
[[nodiscard]] DecodedAudioAsset audio_trim_silence(const DecodedAudioAsset& asset,
                                                    const AudioSilenceTrimOptions& options = {});
[[nodiscard]] DecodedAudioAsset audio_apply_fades(const DecodedAudioAsset& asset,
                                                   std::uint64_t fadeInFrames,
                                                   std::uint64_t fadeOutFrames,
                                                   bool equalPower = false);
[[nodiscard]] DecodedAudioAsset audio_reverse(const DecodedAudioAsset& asset);
[[nodiscard]] std::optional<DecodedAudioAsset> audio_convert_channels(const DecodedAudioAsset& asset,
                                                                      std::uint8_t channels);

} // namespace dve::audio
