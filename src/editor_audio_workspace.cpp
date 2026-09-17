#include "dve/editor_audio_workspace.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>

namespace dve::editor {
namespace {

constexpr std::array<AudioEditorCommandDescriptor, 20> kCommands{{
    {"tool.select", "Select tool", "1"}, {"tool.move", "Move tool", "2"},
    {"tool.trim", "Trim tool", "3"}, {"tool.split", "Split at pointer", "4"},
    {"tool.slip", "Slip tool", "5"}, {"tool.ripple", "Ripple edit tool", "6"},
    {"tool.roll", "Roll edit tool", "7"}, {"tool.crossfade", "Crossfade tool", "8"},
    {"tool.scrub", "Scrub tool", "9"}, {"tool.shuttle", "Shuttle tool", "0"},
    {"tool.zoom", "Zoom tool", "Z"}, {"transport.play_pause", "Play or pause", "Space"},
    {"transport.stop", "Stop", "Shift+Space"}, {"transport.rewind", "Rewind", "Home"},
    {"edit.delete", "Delete selected clip", "Delete"}, {"edit.ripple_delete", "Ripple delete selected clip", "Shift+Delete"},
    {"edit.split", "Split selected clip at playhead", "S"}, {"edit.undo", "Undo", "Ctrl+Z"},
    {"edit.redo", "Redo", "Ctrl+Shift+Z"}, {"view.zoom_selection", "Zoom to selection", "F"},
}};

audio::AudioEditClip* find_clip(audio::AudioEditSession& session, audio::AudioEditClipId id,
                                audio::AudioEditTrack** trackOut = nullptr) noexcept {
    for (auto& track : session.tracks) {
        const auto it = std::find_if(track.clips.begin(), track.clips.end(),
                                     [id](const audio::AudioEditClip& clip) { return clip.id == id; });
        if (it != track.clips.end()) {
            if (trackOut != nullptr) *trackOut = &track;
            return &*it;
        }
    }
    return nullptr;
}


std::uint64_t signed_add_clamped(std::uint64_t value, std::int64_t delta) noexcept {
    if (delta < 0) {
        const auto amount = static_cast<std::uint64_t>(-delta);
        return amount > value ? 0U : value - amount;
    }
    const auto amount = static_cast<std::uint64_t>(delta);
    return amount > std::numeric_limits<std::uint64_t>::max() - value
        ? std::numeric_limits<std::uint64_t>::max() : value + amount;
}

int lane_y(const AudioTimelineDrawModel& model, const audio::AudioEditSession& session,
           audio::AudioEditTrackId track, std::uint32_t lane, int trackHeight) {
    const auto it = std::find_if(session.tracks.begin(), session.tracks.end(),
        [track](const audio::AudioEditTrack& item) { return item.id == track; });
    if (it == session.tracks.end()) return model.content.y;
    const std::size_t index = static_cast<std::size_t>(std::distance(session.tracks.begin(), it));
    const int laneHeight = std::max(18, trackHeight / static_cast<int>(std::max(1U, it->takeLaneCount)));
    return model.content.y + static_cast<int>(index) * trackHeight + static_cast<int>(lane) * laneHeight;
}

} // namespace

std::span<const AudioEditorCommandDescriptor> audio_editor_commands() noexcept { return kCommands; }

std::string_view audio_editor_tool_name(AudioEditorTool tool) noexcept {
    static constexpr std::array<std::string_view, 11> names{
        "Select", "Move", "Trim", "Split", "Slip", "Ripple", "Roll", "Crossfade", "Scrub", "Shuttle", "Zoom"
    };
    return names[static_cast<std::size_t>(tool)];
}

AudioTimelineEditorController::AudioTimelineEditorController(audio::AudioEditSession& session,
                                                             audio::AudioEditHistory& history,
                                                             audio::AudioEditTransport& transport,
                                                             AudioTimelineView view)
    : session_(session), history_(history), transport_(transport), view_(view) {
    history_.reset(session_);
}

AudioTimelineDrawModel AudioTimelineEditorController::draw_model() const {
    static const std::vector<AudioTimelinePeakCacheEntry> empty;
    static const std::vector<AudioTimelineSpectrogramCacheEntry> emptySpectrograms;
    return build_audio_timeline_draw_model(session_, view_, content_, transport_.cursor(),
                                           caches_ != nullptr ? *caches_ : empty, selectedClip_,
                                           spectrograms_ != nullptr ? *spectrograms_ : emptySpectrograms);
}

bool AudioTimelineEditorController::pointer_down(int x, int y, bool extendSelection) {
    const auto model = draw_model();
    const auto hit = hit_test_audio_timeline_clip(model, x, y);
    dragStartX_ = x;
    dragStartFrame_ = snap_audio_timeline_frame(session_, view_,
        audio_timeline_x_to_frame(view_, content_, x));
    transactionChanged_ = false;
    dragOtherClip_.reset();

    if (tool_ == AudioEditorTool::Scrub || tool_ == AudioEditorTool::Shuttle) {
        transport_.seek(dragStartFrame_);
        dragMode_ = DragMode::Scrub;
        return true;
    }
    if (tool_ == AudioEditorTool::Split) {
        if (!hit) return false;
        std::string error;
        selectedClip_ = audio::split_audio_clip(session_, *hit, dragStartFrame_, &error);
        if (selectedClip_) history_.commit(session_);
        return selectedClip_.has_value();
    }
    if (tool_ == AudioEditorTool::Crossfade && hit) {
        if (selectedClip_ && *selectedClip_ != *hit) {
            const auto* left = find_clip(session_, *selectedClip_);
            const auto* right = find_clip(session_, *hit);
            if (left != nullptr && right != nullptr) {
                const std::uint64_t maximum = std::min(left->timelineFrameCount, right->timelineFrameCount) / 2U;
                const std::uint64_t requested = std::min<std::uint64_t>(maximum,
                    std::max<std::uint64_t>(64U, session_.sampleRate / 20U));
                std::string error;
                if (requested > 0U && audio::create_audio_crossfade(session_, *selectedClip_, *hit,
                        requested, audio::AudioEditFadeCurve::EqualPower, &error)) {
                    history_.commit(session_);
                    selectedClip_ = *hit;
                    return true;
                }
            }
        }
        selectedClip_ = *hit;
        return true;
    }
    if (!hit) {
        if (!extendSelection) selectedClip_.reset();
        selection_ = {dragStartFrame_, dragStartFrame_, true};
        dragMode_ = DragMode::SelectRange;
        return true;
    }
    selectedClip_ = *hit;
    auto* clip = find_clip(session_, *hit);
    if (clip == nullptr) return false;
    originalTimelineStart_ = clip->timelineStartFrame;
    originalSourceStart_ = clip->sourceStartFrame;
    originalSourceCount_ = clip->sourceFrameCount;
    const auto visual = std::find_if(model.clips.begin(), model.clips.end(),
                                     [hit](const AudioTimelineClipVisual& item) { return item.clip == *hit; });
    const int edge = 7;
    if (tool_ == AudioEditorTool::Trim && visual != model.clips.end()) {
        if (std::abs(x - visual->rect.x) <= edge) dragMode_ = DragMode::TrimLeft;
        else if (std::abs(x - (visual->rect.x + visual->rect.width)) <= edge) dragMode_ = DragMode::TrimRight;
        else dragMode_ = DragMode::Move;
    } else if (tool_ == AudioEditorTool::Slip) dragMode_ = DragMode::Slip;
    else if (tool_ == AudioEditorTool::Move || tool_ == AudioEditorTool::Select) dragMode_ = DragMode::Move;
    else if (tool_ == AudioEditorTool::Roll) {
        audio::AudioEditTrack* track{};
        (void)find_clip(session_, *hit, &track);
        if (track == nullptr) return false;
        const std::uint64_t boundary = clip->timelineStartFrame + clip->timelineFrameCount;
        const auto other = std::find_if(track->clips.begin(), track->clips.end(),
            [boundary, hit](const audio::AudioEditClip& candidate) {
                return candidate.id != *hit && candidate.timelineStartFrame == boundary;
            });
        if (other == track->clips.end()) return false;
        dragOtherClip_ = other->id;
        dragMode_ = DragMode::Move;
    } else dragMode_ = DragMode::Inactive;
    return true;
}

bool AudioTimelineEditorController::pointer_move(int x, int) {
    const std::uint64_t frame = snap_audio_timeline_frame(session_, view_,
        audio_timeline_x_to_frame(view_, content_, x));
    if (dragMode_ == DragMode::SelectRange) {
        selection_.beginFrame = std::min(dragStartFrame_, frame);
        selection_.endFrame = std::max(dragStartFrame_, frame);
        selection_.active = true;
        return true;
    }
    if (dragMode_ == DragMode::Scrub) {
        transport_.seek(frame);
        return true;
    }
    if (!selectedClip_) return false;
    auto* clip = find_clip(session_, *selectedClip_);
    if (clip == nullptr) return false;
    const std::int64_t delta = frame >= dragStartFrame_
        ? static_cast<std::int64_t>(frame - dragStartFrame_)
        : -static_cast<std::int64_t>(dragStartFrame_ - frame);
    if (tool_ == AudioEditorTool::Roll && dragOtherClip_) {
        std::string error;
        const std::uint64_t boundary = signed_add_clamped(originalTimelineStart_ + originalSourceCount_, delta);
        transactionChanged_ = audio::roll_audio_clip_boundary(session_, *selectedClip_, *dragOtherClip_, boundary, &error) || transactionChanged_;
        return transactionChanged_;
    }
    switch (dragMode_) {
        case DragMode::Move:
            transactionChanged_ = audio::move_audio_clip(session_, *selectedClip_,
                signed_add_clamped(originalTimelineStart_, delta)) || transactionChanged_;
            return true;
        case DragMode::Slip: {
            clip->sourceStartFrame = originalSourceStart_;
            clip->sourceFrameCount = originalSourceCount_;
            transactionChanged_ = audio::slip_audio_clip(session_, *selectedClip_, delta) || transactionChanged_;
            return true;
        }
        case DragMode::TrimLeft: {
            const std::uint64_t originalEnd = originalTimelineStart_ + originalSourceCount_;
            const std::uint64_t newStart = std::min(signed_add_clamped(originalTimelineStart_, delta), originalEnd - 1U);
            const std::uint64_t amount = newStart - originalTimelineStart_;
            clip->timelineStartFrame = newStart;
            clip->sourceStartFrame = originalSourceStart_ + amount;
            clip->sourceFrameCount = originalSourceCount_ - amount;
            clip->timelineFrameCount = clip->sourceFrameCount;
            transactionChanged_ = true;
            return true;
        }
        case DragMode::TrimRight: {
            const std::uint64_t newEnd = std::max<std::uint64_t>(originalTimelineStart_ + 1U,
                signed_add_clamped(originalTimelineStart_ + originalSourceCount_, delta));
            clip->sourceStartFrame = originalSourceStart_;
            clip->sourceFrameCount = newEnd - originalTimelineStart_;
            clip->timelineFrameCount = clip->sourceFrameCount;
            transactionChanged_ = true;
            return true;
        }
        default: break;
    }
    return false;
}

bool AudioTimelineEditorController::pointer_up(int x, int y) {
    (void)pointer_move(x, y);
    const bool changed = transactionChanged_;
    if (changed) history_.commit(session_);
    dragMode_ = DragMode::Inactive;
    dragOtherClip_.reset();
    transactionChanged_ = false;
    return changed;
}

bool AudioTimelineEditorController::wheel(float delta, int x, bool controlModifier) {
    if (delta == 0.0F) return false;
    if (controlModifier || tool_ == AudioEditorTool::Zoom) {
        const std::uint64_t anchor = audio_timeline_x_to_frame(view_, content_, x);
        const double scale = std::pow(1.2, -static_cast<double>(delta));
        view_.framesPerPixel = std::clamp(view_.framesPerPixel * scale, 0.125, 1.0e8);
        const std::uint64_t anchorAfter = audio_timeline_x_to_frame(view_, content_, x);
        if (anchorAfter > anchor) view_.firstFrame -= std::min(view_.firstFrame, anchorAfter - anchor);
        else view_.firstFrame = signed_add_clamped(view_.firstFrame,
            static_cast<std::int64_t>(anchor - anchorAfter));
    } else {
        const auto scroll = static_cast<std::int64_t>(std::llround(-delta * view_.framesPerPixel * 80.0));
        view_.firstFrame = signed_add_clamped(view_.firstFrame, scroll);
    }
    return true;
}

bool AudioTimelineEditorController::dispatch_command(std::string_view command) {
    if (command == "tool.select") tool_ = AudioEditorTool::Select;
    else if (command == "tool.move") tool_ = AudioEditorTool::Move;
    else if (command == "tool.trim") tool_ = AudioEditorTool::Trim;
    else if (command == "tool.split") tool_ = AudioEditorTool::Split;
    else if (command == "tool.slip") tool_ = AudioEditorTool::Slip;
    else if (command == "tool.ripple") tool_ = AudioEditorTool::Ripple;
    else if (command == "tool.roll") tool_ = AudioEditorTool::Roll;
    else if (command == "tool.crossfade") tool_ = AudioEditorTool::Crossfade;
    else if (command == "tool.scrub") tool_ = AudioEditorTool::Scrub;
    else if (command == "tool.shuttle") tool_ = AudioEditorTool::Shuttle;
    else if (command == "tool.zoom") tool_ = AudioEditorTool::Zoom;
    else if (command == "transport.play_pause") {
        if (transport_.state() == audio::AudioTransportState::Playing) transport_.pause(); else transport_.play();
    } else if (command == "transport.stop") transport_.stop();
    else if (command == "transport.rewind") transport_.rewind();
    else if (command == "edit.delete" && selectedClip_) {
        if (audio::delete_audio_clip(session_, *selectedClip_)) { selectedClip_.reset(); history_.commit(session_); }
    } else if (command == "edit.ripple_delete" && selectedClip_) {
        if (audio::ripple_delete_audio_clip(session_, *selectedClip_)) { selectedClip_.reset(); history_.commit(session_); }
    } else if (command == "edit.split" && selectedClip_) {
        std::string error;
        auto right = audio::split_audio_clip(session_, *selectedClip_, transport_.cursor(), &error);
        if (right) { selectedClip_ = *right; history_.commit(session_); }
    } else if (command == "edit.undo") return history_.undo(session_);
    else if (command == "edit.redo") return history_.redo(session_);
    else if (command == "view.zoom_selection" && selection_.active && selection_.endFrame > selection_.beginFrame && content_.width > 0) {
        view_.firstFrame = selection_.beginFrame;
        view_.framesPerPixel = static_cast<double>(selection_.endFrame - selection_.beginFrame) /
                               static_cast<double>(content_.width);
    } else return false;
    return true;
}

void render_audio_timeline_workspace(const IEditorCanvas& canvas,
                                     const audio::AudioEditSession& session,
                                     const AudioTimelineDrawModel& model,
                                     const AudioTimelineView& view,
                                     AudioEditorTool tool,
                                     AudioTimelineSelection selection,
                                     const audio::AudioAnalysisReport* analysis,
                                     const AudioTimelineRenderStyle& style) {
    canvas.fill(model.content, style.background);
    const UiRect toolbar{model.content.x, model.content.y, model.content.width, 30};
    canvas.fill(toolbar, style.panel);
    canvas.text(toolbar.x + 8, toolbar.y + 20, "AUDIO WORKSTATION", style.text);
    canvas.text(toolbar.x + 160, toolbar.y + 20,
                std::string("Tool: ") + std::string(audio_editor_tool_name(tool)), style.marker);
    canvas.text(toolbar.x + 290, toolbar.y + 20,
                "Session: " + session.name + "  " + std::to_string(session.sampleRate) + " Hz", style.mutedText);

    for (const auto& line : model.grid)
        canvas.line(line.x, model.content.y + toolbar.height, line.x,
                    model.content.y + model.content.height,
                    line.major ? style.gridMajor : style.gridMinor, line.major ? 2 : 1);
    if (model.loopRect) { canvas.fill(*model.loopRect, style.loop); canvas.outline(*model.loopRect, style.comp); }
    if (model.punchRect) { canvas.fill(*model.punchRect, style.punch); canvas.outline(*model.punchRect, style.marker); }
    if (selection.active && selection.endFrame > selection.beginFrame) {
        const int x1 = audio_timeline_frame_to_x(view, model.content, selection.beginFrame);
        const int x2 = audio_timeline_frame_to_x(view, model.content, selection.endFrame);
        canvas.fill({x1, model.content.y + toolbar.height, std::max(1, x2 - x1),
                     model.content.height - toolbar.height}, 0x1D3550U);
    }

    for (std::size_t trackIndex = 0U; trackIndex < session.tracks.size(); ++trackIndex) {
        const int y = model.content.y + toolbar.height + static_cast<int>(trackIndex) * view.trackHeight;
        canvas.line(model.content.x, y, model.content.x + model.content.width, y, style.gridMajor);
        canvas.text(model.content.x + 6, y + 16, session.tracks[trackIndex].name, style.text);
        const auto& track = session.tracks[trackIndex];
        for (std::uint32_t lane = 1U; lane < track.takeLaneCount; ++lane) {
            const int laneY = y + static_cast<int>(lane) * std::max(18, view.trackHeight / static_cast<int>(track.takeLaneCount));
            canvas.line(model.content.x, laneY, model.content.x + model.content.width, laneY, style.gridMinor);
        }
        for (const auto& segment : track.compSegments) {
            const int x1 = audio_timeline_frame_to_x(view, model.content, segment.timelineStartFrame);
            const int x2 = audio_timeline_frame_to_x(view, model.content,
                segment.timelineStartFrame + segment.timelineFrameCount);
            const int sy = lane_y(model, session, track.id, segment.takeLane, view.trackHeight) + toolbar.height;
            canvas.outline({x1, sy, std::max(1, x2 - x1),
                            std::max(16, view.trackHeight / static_cast<int>(track.takeLaneCount) - 2)}, style.comp);
        }
    }

    for (const auto& clip : model.clips) {
        UiRect rect = clip.rect;
        rect.y += toolbar.height;
        canvas.fill(rect, clip.selected ? style.clipSelected : style.clip);
        for (const auto& cell : clip.spectrogram) {
            UiRect shifted = cell.rect;
            shifted.y += toolbar.height;
            const auto level = static_cast<std::uint32_t>(std::clamp(cell.normalizedEnergy, 0.0F, 1.0F) * 255.0F);
            const EditorColor spectral = ((20U + level / 3U) << 16U) | ((38U + level / 2U) << 8U) | (70U + level * 3U / 4U);
            canvas.fill(shifted, spectral);
        }
        canvas.outline(rect, clip.activeTake ? style.comp : style.gridMajor);
        const int mid = rect.y + rect.height / 2;
        for (const auto& column : clip.waveform) {
            const int x = column.x;
            const int y1 = mid - static_cast<int>(column.maximumLeft * static_cast<float>(rect.height) * 0.42F);
            const int y2 = mid - static_cast<int>(column.minimumLeft * static_cast<float>(rect.height) * 0.42F);
            canvas.line(x, y1, x, y2, style.waveform);
        }
        canvas.text(rect.x + 5, rect.y + 15, "Clip " + std::to_string(clip.clip.value), style.text);
    }
    for (const auto& marker : model.markers) {
        canvas.line(marker.x, model.content.y + toolbar.height, marker.x,
                    model.content.y + model.content.height, style.marker, 2);
        canvas.text(marker.x + 3, model.content.y + toolbar.height + 15, marker.name, style.marker);
    }
    canvas.line(model.playheadX, model.content.y + toolbar.height, model.playheadX,
                model.content.y + model.content.height, style.playhead, 2);
    if (analysis != nullptr) {
        const int right = model.content.x + model.content.width - 260;
        canvas.text(right, toolbar.y + 20,
            "Peak " + std::to_string(analysis->samplePeak).substr(0, 5) +
            "  LUFS~ " + std::to_string(analysis->approximateIntegratedLufs).substr(0, 6), style.text);
    }
}

} // namespace dve::editor
