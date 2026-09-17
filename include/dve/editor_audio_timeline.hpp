#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "dve/audio/audio_analysis.hpp"
#include "dve/audio/audio_edit.hpp"
#include "dve/editor_viewport.hpp"

namespace dve::editor {

enum class AudioTimelineRuler : std::uint8_t { Time, Frames, Beats };
enum class AudioTimelineSnap : std::uint8_t { Off, Frames, Beats, Markers, ClipEdges };
enum class AudioTimelineDisplay : std::uint8_t { Waveform, Spectrogram, Combined };

struct AudioTimelineView {
    std::uint64_t firstFrame{};
    double framesPerPixel{256.0};
    int verticalScroll{};
    int trackHeight{84};
    AudioTimelineRuler ruler{AudioTimelineRuler::Time};
    AudioTimelineSnap snap{AudioTimelineSnap::Beats};
    std::uint32_t beatSubdivisions{4};
    AudioTimelineDisplay display{AudioTimelineDisplay::Waveform};
};

struct AudioTimelinePeakCacheEntry {
    audio::AudioEditSourceId source{};
    audio::AudioWaveformPeakCache cache;
};

struct AudioTimelineSpectrogramCacheEntry {
    audio::AudioEditSourceId source{};
    audio::AudioSpectrogram spectrogram;
};

struct AudioTimelineWaveColumn {
    int x{};
    float minimumLeft{};
    float maximumLeft{};
    float minimumRight{};
    float maximumRight{};
};

struct AudioTimelineSpectrogramCell {
    UiRect rect{};
    float normalizedEnergy{};
};

struct AudioTimelineClipVisual {
    audio::AudioEditClipId clip{};
    audio::AudioEditTrackId track{};
    UiRect rect{};
    bool activeTake{};
    bool selected{};
    std::vector<AudioTimelineWaveColumn> waveform;
    std::vector<AudioTimelineSpectrogramCell> spectrogram;
};

struct AudioTimelineGridLine {
    int x{};
    std::uint64_t frame{};
    bool major{};
};

struct AudioTimelineMarkerVisual {
    int x{};
    std::uint64_t frame{};
    std::string name;
};

struct AudioTimelineDrawModel {
    UiRect content{};
    std::vector<AudioTimelineClipVisual> clips;
    std::vector<AudioTimelineGridLine> grid;
    std::vector<AudioTimelineMarkerVisual> markers;
    int playheadX{};
    std::optional<UiRect> loopRect;
    std::optional<UiRect> punchRect;
};

[[nodiscard]] AudioTimelineDrawModel build_audio_timeline_draw_model(
    const audio::AudioEditSession& session, const AudioTimelineView& view,
    UiRect content, std::uint64_t playheadFrame,
    const std::vector<AudioTimelinePeakCacheEntry>& peakCaches,
    std::optional<audio::AudioEditClipId> selectedClip = std::nullopt,
    const std::vector<AudioTimelineSpectrogramCacheEntry>& spectrogramCaches = {});
[[nodiscard]] std::optional<audio::AudioEditClipId> hit_test_audio_timeline_clip(
    const AudioTimelineDrawModel& model, int x, int y) noexcept;
[[nodiscard]] std::uint64_t audio_timeline_x_to_frame(const AudioTimelineView& view,
                                                       UiRect content, int x) noexcept;
[[nodiscard]] int audio_timeline_frame_to_x(const AudioTimelineView& view,
                                             UiRect content, std::uint64_t frame) noexcept;
[[nodiscard]] std::uint64_t snap_audio_timeline_frame(
    const audio::AudioEditSession& session, const AudioTimelineView& view,
    std::uint64_t frame, std::uint64_t toleranceFrames = 0U) noexcept;

} // namespace dve::editor
