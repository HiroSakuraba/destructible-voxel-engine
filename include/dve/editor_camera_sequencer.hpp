#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dve/camera_sequence.hpp"

namespace dve::editor {

struct CameraSequencerShotRect {
    camera::CameraShotId shotId{};
    float x{};
    float width{};
    bool selected{};
};

struct CameraSequencerKeyMarker {
    camera::CameraDollyId dollyId{};
    std::size_t keyIndex{};
    float x{};
};

struct CameraSequencerLayout {
    float width{};
    float playheadX{};
    std::vector<CameraSequencerShotRect> shots;
    std::vector<CameraSequencerKeyMarker> keys;
};

class EditorCameraSequencer {
public:
    [[nodiscard]] bool set_sequence(camera::CameraSequence sequence, std::string* error = nullptr);
    [[nodiscard]] const camera::CameraSequence& sequence() const noexcept { return sequence_; }
    [[nodiscard]] camera::CameraSequence& sequence() noexcept { return sequence_; }

    void set_playhead(float seconds) noexcept;
    void set_snap_seconds(float seconds) noexcept;
    [[nodiscard]] float playhead_seconds() const noexcept { return playheadSeconds_; }
    [[nodiscard]] float snap_seconds() const noexcept { return snapSeconds_; }

    [[nodiscard]] bool select_shot(camera::CameraShotId id, bool additive = false) noexcept;
    void clear_selection() noexcept { selectedShots_.clear(); selectedShot_.reset(); }
    [[nodiscard]] std::optional<camera::CameraShotId> selected_shot() const noexcept { return selectedShot_; }
    [[nodiscard]] const std::set<camera::CameraShotId>& selected_shots() const noexcept { return selectedShots_; }
    [[nodiscard]] bool move_selected_shot(float newStartSeconds, std::string* error = nullptr);
    [[nodiscard]] bool trim_selected_shot(float newDurationSeconds, std::string* error = nullptr);
    [[nodiscard]] bool move_dolly_key(camera::CameraDollyId dollyId, std::size_t keyIndex,
                                      float newTimeSeconds, std::string* error = nullptr);
    [[nodiscard]] bool set_dolly_key_post_process(
        camera::CameraDollyId dollyId,
        std::size_t keyIndex,
        camera::CameraPostProcessProfile profile,
        std::string* error = nullptr);
    [[nodiscard]] bool apply_cinematic_preset_to_dolly_key(
        camera::CameraDollyId dollyId,
        std::size_t keyIndex,
        camera::CameraCinematicPreset preset,
        std::string* error = nullptr);
    [[nodiscard]] bool apply_filmback_preset_to_dolly_key(
        camera::CameraDollyId dollyId,
        std::size_t keyIndex,
        camera::CameraFilmbackPreset preset,
        float focalLengthMillimeters,
        std::string* error = nullptr);
    [[nodiscard]] bool move_selected_shots(float deltaSeconds, bool rippleFollowing,
                                           std::string* error = nullptr);
    [[nodiscard]] bool roll_boundary(camera::CameraShotId leftShot,
                                     camera::CameraShotId rightShot,
                                     float newBoundarySeconds,
                                     std::string* error = nullptr);
    [[nodiscard]] bool ripple_delete_selected(std::string* error = nullptr);
    [[nodiscard]] bool undo() noexcept;
    [[nodiscard]] bool redo() noexcept;
    [[nodiscard]] CameraSequencerLayout layout(float widthPixels) const;
    [[nodiscard]] std::string export_otio_json() const;

private:
    [[nodiscard]] float snapped(float seconds) const noexcept;
    void push_undo();
    camera::CameraSequence sequence_{};
    float playheadSeconds_{};
    float snapSeconds_{1.0F / 30.0F};
    std::optional<camera::CameraShotId> selectedShot_;
    std::set<camera::CameraShotId> selectedShots_;
    std::vector<camera::CameraSequence> undo_;
    std::vector<camera::CameraSequence> redo_;
};

} // namespace dve::editor
