#include "dve/editor_audio_timeline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dve::editor {
namespace {

const audio::AudioWaveformPeakCache* find_cache(
    const std::vector<AudioTimelinePeakCacheEntry>& caches,
    audio::AudioEditSourceId source) noexcept {
    const auto it = std::find_if(caches.begin(), caches.end(),
        [source](const AudioTimelinePeakCacheEntry& entry) { return entry.source == source; });
    return it == caches.end() ? nullptr : &it->cache;
}

const audio::AudioSpectrogram* find_spectrogram(
    const std::vector<AudioTimelineSpectrogramCacheEntry>& caches,
    audio::AudioEditSourceId source) noexcept {
    const auto it = std::find_if(caches.begin(), caches.end(),
        [source](const AudioTimelineSpectrogramCacheEntry& entry) { return entry.source == source; });
    return it == caches.end() ? nullptr : &it->spectrogram;
}

std::optional<std::uint64_t> nearest_candidate(std::uint64_t frame,
                                               std::uint64_t candidate,
                                               std::uint64_t tolerance,
                                               std::optional<std::uint64_t> current) noexcept {
    const std::uint64_t distance = frame > candidate ? frame - candidate : candidate - frame;
    if (distance > tolerance) return current;
    if (!current) return candidate;
    const std::uint64_t currentDistance = frame > *current ? frame - *current : *current - frame;
    return distance < currentDistance ? std::optional<std::uint64_t>(candidate) : current;
}

} // namespace

int audio_timeline_frame_to_x(const AudioTimelineView& view, UiRect content,
                              std::uint64_t frame) noexcept {
    if (view.framesPerPixel <= 0.0 || frame <= view.firstFrame) return content.x;
    const double offset = static_cast<double>(frame - view.firstFrame) / view.framesPerPixel;
    if (offset >= static_cast<double>(std::numeric_limits<int>::max() - content.x)) return std::numeric_limits<int>::max();
    return content.x + static_cast<int>(std::llround(offset));
}

std::uint64_t audio_timeline_x_to_frame(const AudioTimelineView& view, UiRect content,
                                        int x) noexcept {
    if (x <= content.x || view.framesPerPixel <= 0.0) return view.firstFrame;
    const double frame = static_cast<double>(view.firstFrame) +
                         static_cast<double>(x - content.x) * view.framesPerPixel;
    if (frame >= static_cast<double>(std::numeric_limits<std::uint64_t>::max()))
        return std::numeric_limits<std::uint64_t>::max();
    return static_cast<std::uint64_t>(std::llround(frame));
}

AudioTimelineDrawModel build_audio_timeline_draw_model(
    const audio::AudioEditSession& session, const AudioTimelineView& view,
    UiRect content, std::uint64_t playheadFrame,
    const std::vector<AudioTimelinePeakCacheEntry>& peakCaches,
    std::optional<audio::AudioEditClipId> selectedClip,
    const std::vector<AudioTimelineSpectrogramCacheEntry>& spectrogramCaches) {
    AudioTimelineDrawModel model;
    model.content = content;
    model.playheadX = audio_timeline_frame_to_x(view, content, playheadFrame);
    const std::uint64_t visibleEnd = audio_timeline_x_to_frame(view, content, content.x + content.width);
    const int trackHeight = std::max(32, view.trackHeight);

    for (std::size_t trackIndex = 0; trackIndex < session.tracks.size(); ++trackIndex) {
        const auto& track = session.tracks[trackIndex];
        const int y = content.y + static_cast<int>(trackIndex) * trackHeight - view.verticalScroll;
        if (y + trackHeight < content.y || y > content.y + content.height) continue;
        for (const auto& clip : track.clips) {
            const std::uint64_t clipEnd = clip.timelineStartFrame + clip.timelineFrameCount;
            if (clipEnd <= view.firstFrame || clip.timelineStartFrame >= visibleEnd) continue;
            const int left = audio_timeline_frame_to_x(view, content, clip.timelineStartFrame);
            const int right = audio_timeline_frame_to_x(view, content, clipEnd);
            AudioTimelineClipVisual visual;
            visual.clip = clip.id;
            visual.track = track.id;
            visual.rect = {left, y + 4, std::max(2, right - left), trackHeight - 8};
            visual.activeTake = clip.takeLane == track.activeTakeLane;
            visual.selected = selectedClip && *selectedClip == clip.id;
            const auto* cache = find_cache(peakCaches, clip.source);
            if (cache != nullptr && view.display != AudioTimelineDisplay::Spectrogram) {
                const auto* level = audio::choose_audio_waveform_peak_level(*cache, view.framesPerPixel);
                if (level != nullptr && !level->peaks.empty()) {
                    const int firstX = std::max(content.x, visual.rect.x);
                    const int lastX = std::min(content.x + content.width, visual.rect.x + visual.rect.width);
                    visual.waveform.reserve(static_cast<std::size_t>(std::max(0, lastX - firstX)));
                    for (int x = firstX; x < lastX; ++x) {
                        const std::uint64_t timelineFrame = audio_timeline_x_to_frame(view, content, x);
                        if (timelineFrame < clip.timelineStartFrame) continue;
                        const std::uint64_t local = timelineFrame - clip.timelineStartFrame;
                        if (!clip.loopSource && local >= clip.sourceFrameCount) continue;
                        const std::uint64_t sourceLocal = clip.loopSource ? local % clip.sourceFrameCount : local;
                        const std::uint64_t sourceFrame = clip.sourceStartFrame + sourceLocal;
                        const std::size_t peakIndex = static_cast<std::size_t>(sourceFrame / level->framesPerPeak);
                        if (peakIndex >= level->peaks.size()) continue;
                        const auto& peak = level->peaks[peakIndex];
                        visual.waveform.push_back({x, peak.minimumLeft, peak.maximumLeft,
                                                   peak.minimumRight, peak.maximumRight});
                    }
                }
            }
            const auto* spectrogram = find_spectrogram(spectrogramCaches, clip.source);
            if (spectrogram != nullptr && !spectrogram->decibels.empty() &&
                spectrogram->timeBins > 0U && spectrogram->frequencyBins > 0U &&
                view.display != AudioTimelineDisplay::Waveform) {
                const int firstX = std::max(content.x, visual.rect.x);
                const int lastX = std::min(content.x + content.width, visual.rect.x + visual.rect.width);
                const std::uint32_t displayedBins = std::min<std::uint32_t>(32U, spectrogram->frequencyBins);
                const int cellHeight = std::max(1, visual.rect.height / static_cast<int>(displayedBins));
                const int xStride = std::max(1, (lastX - firstX) / 320);
                visual.spectrogram.reserve(static_cast<std::size_t>(std::max(0, (lastX - firstX) / xStride)) * displayedBins);
                for (int x = firstX; x < lastX; x += xStride) {
                    const std::uint64_t timelineFrame = audio_timeline_x_to_frame(view, content, x);
                    if (timelineFrame < clip.timelineStartFrame) continue;
                    const std::uint64_t local = timelineFrame - clip.timelineStartFrame;
                    if (!clip.loopSource && local >= clip.sourceFrameCount) continue;
                    const std::uint64_t sourceLocal = clip.loopSource ? local % clip.sourceFrameCount : local;
                    const std::uint64_t sourceFrame = clip.sourceStartFrame + sourceLocal;
                    const std::uint32_t timeBin = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                        spectrogram->timeBins - 1U, sourceFrame / std::max(1U, spectrogram->hopFrames)));
                    for (std::uint32_t displayBin = 0U; displayBin < displayedBins; ++displayBin) {
                        const std::uint32_t sourceBin = std::min<std::uint32_t>(spectrogram->frequencyBins - 1U,
                            displayBin * spectrogram->frequencyBins / displayedBins);
                        const float db = spectrogram->decibels[static_cast<std::size_t>(timeBin) *
                                                               spectrogram->frequencyBins + sourceBin];
                        const float normalized = std::clamp((db - spectrogram->floorDecibels) /
                                                            -spectrogram->floorDecibels, 0.0F, 1.0F);
                        const int row = static_cast<int>(displayedBins - 1U - displayBin);
                        visual.spectrogram.push_back({{x, visual.rect.y + row * cellHeight,
                            std::max(1, xStride), cellHeight}, normalized});
                    }
                }
            }
            model.clips.push_back(std::move(visual));
        }
    }

    const std::uint32_t subdivisions = std::clamp<std::uint32_t>(view.beatSubdivisions, 1U, 64U);
    const double firstBeat = audio::audio_edit_frame_to_beats(session, view.firstFrame);
    const double lastBeat = audio::audio_edit_frame_to_beats(session, visibleEnd);
    const double step = 1.0 / static_cast<double>(subdivisions);
    const double startBeat = std::floor(firstBeat / step) * step;
    const std::size_t maximumLines = 4096U;
    for (double beat = startBeat; beat <= lastBeat + step && model.grid.size() < maximumLines; beat += step) {
        const std::uint64_t frame = audio::audio_edit_beats_to_frame(session, std::max(0.0, beat));
        const int x = audio_timeline_frame_to_x(view, content, frame);
        const bool major = std::abs(beat - std::round(beat)) < 1.0e-6;
        model.grid.push_back({x, frame, major});
    }
    for (const auto& marker : session.markers) {
        if (marker.frame < view.firstFrame || marker.frame > visibleEnd) continue;
        model.markers.push_back({audio_timeline_frame_to_x(view, content, marker.frame), marker.frame, marker.name});
    }
    if (session.loop.enabled) {
        const int x1 = audio_timeline_frame_to_x(view, content, session.loop.beginFrame);
        const int x2 = audio_timeline_frame_to_x(view, content, session.loop.endFrame);
        model.loopRect = UiRect{x1, content.y, std::max(1, x2 - x1), content.height};
    }
    if (session.punch.enabled) {
        const int x1 = audio_timeline_frame_to_x(view, content, session.punch.beginFrame);
        const int x2 = audio_timeline_frame_to_x(view, content, session.punch.endFrame);
        model.punchRect = UiRect{x1, content.y, std::max(1, x2 - x1), content.height};
    }
    return model;
}

std::optional<audio::AudioEditClipId> hit_test_audio_timeline_clip(
    const AudioTimelineDrawModel& model, int x, int y) noexcept {
    for (auto it = model.clips.rbegin(); it != model.clips.rend(); ++it) {
        if (it->rect.contains(x, y)) return it->clip;
    }
    return std::nullopt;
}

std::uint64_t snap_audio_timeline_frame(const audio::AudioEditSession& session,
                                        const AudioTimelineView& view,
                                        std::uint64_t frame,
                                        std::uint64_t toleranceFrames) noexcept {
    if (view.snap == AudioTimelineSnap::Off) return frame;
    if (toleranceFrames == 0U) {
        toleranceFrames = static_cast<std::uint64_t>(std::max(1.0, view.framesPerPixel * 8.0));
    }
    if (view.snap == AudioTimelineSnap::Frames) return frame;
    if (view.snap == AudioTimelineSnap::Beats) {
        const std::uint64_t candidate = audio::audio_edit_snap_frame_to_beat(session, frame, view.beatSubdivisions);
        const std::uint64_t distance = frame > candidate ? frame - candidate : candidate - frame;
        return distance <= toleranceFrames ? candidate : frame;
    }
    std::optional<std::uint64_t> best;
    if (view.snap == AudioTimelineSnap::Markers) {
        for (const auto& marker : session.markers)
            best = nearest_candidate(frame, marker.frame, toleranceFrames, best);
    } else if (view.snap == AudioTimelineSnap::ClipEdges) {
        for (const auto& track : session.tracks) {
            for (const auto& clip : track.clips) {
                best = nearest_candidate(frame, clip.timelineStartFrame, toleranceFrames, best);
                best = nearest_candidate(frame, clip.timelineStartFrame + clip.timelineFrameCount,
                                         toleranceFrames, best);
            }
        }
    }
    return best.value_or(frame);
}

} // namespace dve::editor
