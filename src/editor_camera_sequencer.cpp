#include "dve/editor_camera_sequencer.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace dve::editor {

bool EditorCameraSequencer::set_sequence(camera::CameraSequence sequence, std::string* error) {
    if (!sequence.validate(error)) return false;
    sequence_ = std::move(sequence);
    playheadSeconds_ = std::clamp(playheadSeconds_, 0.0F, sequence_.durationSeconds);
    selectedShot_.reset();
    selectedShots_.clear();
    undo_.clear();
    redo_.clear();
    return true;
}

void EditorCameraSequencer::set_playhead(float seconds) noexcept {
    playheadSeconds_ = std::clamp(snapped(seconds), 0.0F,
                                 std::max(0.0F, sequence_.durationSeconds));
}

void EditorCameraSequencer::set_snap_seconds(float seconds) noexcept {
    if (std::isfinite(seconds) && seconds >= 0.0F) snapSeconds_ = seconds;
}

float EditorCameraSequencer::snapped(float seconds) const noexcept {
    if (!(snapSeconds_ > 0.0F) || !std::isfinite(seconds))
        return std::isfinite(seconds) ? seconds : 0.0F;
    return std::round(seconds / snapSeconds_) * snapSeconds_;
}

void EditorCameraSequencer::push_undo() {
    undo_.push_back(sequence_);
    if (undo_.size() > 128U) undo_.erase(undo_.begin());
    redo_.clear();
}

bool EditorCameraSequencer::select_shot(camera::CameraShotId id, bool additive) noexcept {
    if (std::none_of(sequence_.shots.begin(), sequence_.shots.end(),
                     [&](const auto& shot) { return shot.id == id; })) return false;
    if (!additive) selectedShots_.clear();
    if (additive && selectedShots_.contains(id)) selectedShots_.erase(id);
    else selectedShots_.insert(id);
    selectedShot_ = selectedShots_.contains(id) ? std::optional{id}
                                                 : (selectedShots_.empty()
                                                        ? std::nullopt
                                                        : std::optional{*selectedShots_.rbegin()});
    return true;
}

bool EditorCameraSequencer::move_selected_shot(float start, std::string* error) {
    if (!selectedShot_) {
        if (error) *error = "no selected camera shot";
        return false;
    }
    camera::CameraSequence candidate = sequence_;
    auto it = std::find_if(candidate.shots.begin(), candidate.shots.end(),
                           [&](const auto& shot) { return shot.id == *selectedShot_; });
    if (it == candidate.shots.end()) {
        if (error) *error = "selected camera shot no longer exists";
        return false;
    }
    it->startSeconds = std::clamp(snapped(start), 0.0F,
                                  std::max(0.0F, candidate.durationSeconds - it->durationSeconds));
    std::stable_sort(candidate.shots.begin(), candidate.shots.end(),
                     [](const auto& a, const auto& b) { return a.startSeconds < b.startSeconds; });
    if (!candidate.validate(error)) return false;
    push_undo();
    sequence_ = std::move(candidate);
    return true;
}

bool EditorCameraSequencer::trim_selected_shot(float duration, std::string* error) {
    if (!selectedShot_) {
        if (error) *error = "no selected camera shot";
        return false;
    }
    camera::CameraSequence candidate = sequence_;
    auto it = std::find_if(candidate.shots.begin(), candidate.shots.end(),
                           [&](const auto& shot) { return shot.id == *selectedShot_; });
    if (it == candidate.shots.end()) {
        if (error) *error = "selected camera shot no longer exists";
        return false;
    }
    it->durationSeconds = std::max(snapSeconds_ > 0.0F ? snapSeconds_ : 0.001F,
                                   snapped(duration));
    if (!candidate.validate(error)) return false;
    push_undo();
    sequence_ = std::move(candidate);
    return true;
}

bool EditorCameraSequencer::move_dolly_key(camera::CameraDollyId id, std::size_t keyIndex,
                                           float newTime, std::string* error) {
    camera::CameraSequence candidate = sequence_;
    auto dolly = std::find_if(candidate.dollies.begin(), candidate.dollies.end(),
                              [&](const auto& value) { return value.id == id; });
    if (dolly == candidate.dollies.end() || keyIndex >= dolly->keys.size()) {
        if (error) *error = "unknown dolly key";
        return false;
    }
    dolly->keys[keyIndex].timeSeconds = std::max(0.0F, snapped(newTime));
    std::stable_sort(dolly->keys.begin(), dolly->keys.end(),
                     [](const auto& a, const auto& b) { return a.timeSeconds < b.timeSeconds; });
    if (!candidate.validate(error)) return false;
    push_undo();
    sequence_ = std::move(candidate);
    return true;
}

bool EditorCameraSequencer::set_dolly_key_post_process(
    camera::CameraDollyId dollyId,std::size_t keyIndex,
    camera::CameraPostProcessProfile profile,std::string* error) {
    if(!profile.validate(error))return false;
    auto candidate=sequence_;
    const auto dolly=std::find_if(candidate.dollies.begin(),candidate.dollies.end(),
        [&](const auto& value){return value.id==dollyId;});
    if(dolly==candidate.dollies.end()||keyIndex>=dolly->keys.size()){
        if(error) *error="unknown camera dolly key";
        return false;
    }
    dolly->keys[keyIndex].postProcess=std::move(profile);
    if(!candidate.validate(error))return false;
    push_undo();sequence_=std::move(candidate);return true;
}

bool EditorCameraSequencer::apply_cinematic_preset_to_dolly_key(
    camera::CameraDollyId dollyId,std::size_t keyIndex,
    camera::CameraCinematicPreset preset,std::string* error) {
    return set_dolly_key_post_process(dollyId,keyIndex,camera::camera_cinematic_preset(preset),error);
}

bool EditorCameraSequencer::apply_filmback_preset_to_dolly_key(
    camera::CameraDollyId dollyId,std::size_t keyIndex,camera::CameraFilmbackPreset preset,
    float focalLengthMillimeters,std::string* error) {
    auto candidate=sequence_;
    const auto dolly=std::find_if(candidate.dollies.begin(),candidate.dollies.end(),
        [&](const auto& value){return value.id==dollyId;});
    if(dolly==candidate.dollies.end()||keyIndex>=dolly->keys.size()){
        if(error) *error="unknown camera dolly key";
        return false;
    }
    dolly->keys[keyIndex].pose.lens.physical=
        camera::camera_physical_lens_preset(preset,focalLengthMillimeters);
    dolly->keys[keyIndex].postProcess.focusDistanceMeters=
        dolly->keys[keyIndex].pose.lens.physical.focusDistanceMeters;
    dolly->keys[keyIndex].postProcess.apertureFStop=
        dolly->keys[keyIndex].pose.lens.physical.apertureFStop;
    if(!candidate.validate(error))return false;
    push_undo();sequence_=std::move(candidate);return true;
}

bool EditorCameraSequencer::move_selected_shots(float deltaSeconds, bool rippleFollowing,
                                                std::string* error) {
    if (selectedShots_.empty()) {
        if (error) *error = "no selected camera shots";
        return false;
    }
    deltaSeconds = snapped(deltaSeconds);
    camera::CameraSequence candidate = sequence_;
    float selectedEnd = 0.0F;
    for (const auto& shot : candidate.shots)
        if (selectedShots_.contains(shot.id))
            selectedEnd = std::max(selectedEnd, shot.startSeconds + shot.durationSeconds);
    for (auto& shot : candidate.shots) {
        if (selectedShots_.contains(shot.id)) shot.startSeconds += deltaSeconds;
        else if (rippleFollowing && shot.startSeconds >= selectedEnd - 1.0e-5F)
            shot.startSeconds += deltaSeconds;
        if (shot.startSeconds < -1.0e-5F) {
            if (error) *error = "camera shot move would cross sequence start";
            return false;
        }
        shot.startSeconds = std::max(0.0F, shot.startSeconds);
    }
    if (rippleFollowing) {
        for (auto& event : candidate.events)
            if (event.timeSeconds >= selectedEnd - 1.0e-5F)
                event.timeSeconds = std::max(0.0F, event.timeSeconds + deltaSeconds);
        float maximumEnd = 0.0F;
        for (const auto& shot : candidate.shots)
            maximumEnd = std::max(maximumEnd, shot.startSeconds + shot.durationSeconds);
        candidate.durationSeconds = std::max(candidate.durationSeconds + deltaSeconds, maximumEnd);
    }
    std::stable_sort(candidate.shots.begin(), candidate.shots.end(),
                     [](const auto& a, const auto& b) { return a.startSeconds < b.startSeconds; });
    std::stable_sort(candidate.events.begin(), candidate.events.end(),
                     [](const auto& a, const auto& b) { return a.timeSeconds < b.timeSeconds; });
    if (!candidate.validate(error)) return false;
    push_undo();
    sequence_ = std::move(candidate);
    return true;
}

bool EditorCameraSequencer::roll_boundary(camera::CameraShotId leftShot,
                                          camera::CameraShotId rightShot,
                                          float newBoundarySeconds,
                                          std::string* error) {
    camera::CameraSequence candidate = sequence_;
    auto left = std::find_if(candidate.shots.begin(), candidate.shots.end(),
                             [&](const auto& shot) { return shot.id == leftShot; });
    auto right = std::find_if(candidate.shots.begin(), candidate.shots.end(),
                              [&](const auto& shot) { return shot.id == rightShot; });
    if (left == candidate.shots.end() || right == candidate.shots.end()) {
        if (error) *error = "roll edit references an unknown shot";
        return false;
    }
    const float oldBoundary = left->startSeconds + left->durationSeconds;
    if (std::abs(oldBoundary - right->startSeconds) > 1.0e-4F) {
        if (error) *error = "roll edit requires adjacent shots";
        return false;
    }
    const float totalEnd = right->startSeconds + right->durationSeconds;
    const float boundary = snapped(newBoundarySeconds);
    const float minimumDuration = snapSeconds_ > 0.0F ? snapSeconds_ : 0.001F;
    if (boundary <= left->startSeconds + minimumDuration ||
        boundary >= totalEnd - minimumDuration) {
        if (error) *error = "roll boundary would collapse a shot";
        return false;
    }
    left->durationSeconds = boundary - left->startSeconds;
    right->startSeconds = boundary;
    right->durationSeconds = totalEnd - boundary;
    if (!candidate.validate(error)) return false;
    push_undo();
    sequence_ = std::move(candidate);
    return true;
}

bool EditorCameraSequencer::ripple_delete_selected(std::string* error) {
    if (selectedShots_.empty()) {
        if (error) *error = "no selected camera shots";
        return false;
    }
    camera::CameraSequence candidate = sequence_;
    float begin = candidate.durationSeconds;
    float end = 0.0F;
    for (const auto& shot : candidate.shots) if (selectedShots_.contains(shot.id)) {
        begin = std::min(begin, shot.startSeconds);
        end = std::max(end, shot.startSeconds + shot.durationSeconds);
    }
    const float removed = std::max(0.0F, end - begin);
    candidate.shots.erase(std::remove_if(candidate.shots.begin(), candidate.shots.end(),
        [&](const auto& shot) { return selectedShots_.contains(shot.id); }), candidate.shots.end());
    for (auto& shot : candidate.shots) if (shot.startSeconds >= end - 1.0e-5F)
        shot.startSeconds -= removed;
    candidate.events.erase(std::remove_if(candidate.events.begin(), candidate.events.end(),
        [&](const auto& event) { return event.timeSeconds >= begin && event.timeSeconds < end; }),
        candidate.events.end());
    for (auto& event : candidate.events) if (event.timeSeconds >= end) event.timeSeconds -= removed;
    candidate.durationSeconds = std::max(0.001F, candidate.durationSeconds - removed);
    if (!candidate.validate(error)) return false;
    push_undo();
    sequence_ = std::move(candidate);
    clear_selection();
    playheadSeconds_ = std::min(playheadSeconds_, sequence_.durationSeconds);
    return true;
}

bool EditorCameraSequencer::undo() noexcept {
    if (undo_.empty()) return false;
    redo_.push_back(sequence_);
    sequence_ = std::move(undo_.back());
    undo_.pop_back();
    for (auto it = selectedShots_.begin(); it != selectedShots_.end();) {
        if (std::none_of(sequence_.shots.begin(), sequence_.shots.end(),
                         [&](const auto& shot) { return shot.id == *it; })) it = selectedShots_.erase(it);
        else ++it;
    }
    if (selectedShot_ && !selectedShots_.contains(*selectedShot_)) selectedShot_.reset();
    return true;
}

bool EditorCameraSequencer::redo() noexcept {
    if (redo_.empty()) return false;
    undo_.push_back(sequence_);
    sequence_ = std::move(redo_.back());
    redo_.pop_back();
    return true;
}

CameraSequencerLayout EditorCameraSequencer::layout(float width) const {
    CameraSequencerLayout result;
    result.width = std::max(1.0F, width);
    const float duration = std::max(0.001F, sequence_.durationSeconds);
    result.playheadX = playheadSeconds_ / duration * result.width;
    for (const auto& shot : sequence_.shots)
        result.shots.push_back({shot.id, shot.startSeconds / duration * result.width,
                                shot.durationSeconds / duration * result.width,
                                selectedShots_.contains(shot.id)});
    for (const auto& dolly : sequence_.dollies) {
        const float sourceDuration = dolly.keys.empty() ? 1.0F
                                                        : std::max(0.001F, dolly.keys.back().timeSeconds);
        for (std::size_t index = 0; index < dolly.keys.size(); ++index)
            result.keys.push_back({dolly.id, index,
                dolly.keys[index].timeSeconds / sourceDuration * result.width});
    }
    return result;
}

std::string EditorCameraSequencer::export_otio_json() const {
    std::ostringstream out;
    out << "{\n  \"OTIO_SCHEMA\": \"Timeline.1\",\n  \"name\": " << std::quoted(sequence_.name)
        << ",\n  \"metadata\": {\"dve_duration_seconds\": " << sequence_.durationSeconds;
    for (const auto& [key, value] : sequence_.metadata)
        out << ", " << std::quoted(key) << ": " << std::quoted(value);
    out << "},\n  \"tracks\": {\"OTIO_SCHEMA\": \"Stack.1\", \"children\": "
           "[{\"OTIO_SCHEMA\": \"Track.1\", \"kind\": \"Video\", \"children\": [\n";
    for (std::size_t index = 0; index < sequence_.shots.size(); ++index) {
        const auto& shot = sequence_.shots[index];
        out << "    {\"OTIO_SCHEMA\": \"Clip.2\", \"name\": " << std::quoted(shot.name)
            << ", \"source_range\": {\"start_time\": {\"value\": " << shot.startSeconds
            << ", \"rate\": 1}, \"duration\": {\"value\": " << shot.durationSeconds
            << ", \"rate\": 1}}, \"metadata\": {\"dve_shot_id\": " << shot.id
            << ", \"dve_rig_id\": " << shot.rigId.value_or(0)
            << ", \"dve_dolly_id\": " << shot.dollyId.value_or(0)
            << ", \"dve_constant_speed\": " << (shot.constantSpeedDolly ? "true" : "false")
            << "}}" << (index + 1U < sequence_.shots.size() ? "," : "") << "\n";
    }
    out << "  ]}]}}\n";
    return out.str();
}

} // namespace dve::editor
