#pragma once

#include <cstdint>
#include <span>

#include "dve/audio/audio_asset.hpp"

namespace dve::audio {

struct AudioWarpMarker {
    std::uint64_t sourceFrame{};
    std::uint64_t timelineFrame{};
};

// Deterministic offline WSOLA-style duration change. durationRatio is output duration divided by
// source duration. The algorithm aligns overlapping grains by normalized waveform correlation,
// preserving local pitch substantially better than ordinary resampling. It is authoring/offline
// work and is never called from the audio callback.
[[nodiscard]] DecodedAudioAsset audio_time_stretch_wsola(const DecodedAudioAsset& asset,
                                                          double durationRatio);

// Independent pitch shift built from high-quality linear resampling followed by WSOLA duration
// restoration. Positive semitones raise pitch while retaining the original frame count.
[[nodiscard]] DecodedAudioAsset audio_pitch_shift_wsola(const DecodedAudioAsset& asset,
                                                         double semitones);

// Piecewise source-to-timeline mapping. Markers must be strictly increasing in both coordinates;
// implicit endpoints are inserted at source frame zero and at the end when absent.
[[nodiscard]] DecodedAudioAsset audio_render_warp_markers(const DecodedAudioAsset& asset,
                                                           std::span<const AudioWarpMarker> markers);

} // namespace dve::audio
