#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/camera_runtime.hpp"
#include "dve/editor_camera_sequencer.hpp"
#include "dve/editor_viewport.hpp"

namespace dve::editor {

enum class CinematicCameraScope : std::uint8_t {
    CameraInstance,
    ShotOverride,
    ProjectDefault,
    ViewportPreview,
};

enum class CinematicCameraSection : std::uint8_t {
    Composition,
    Lens,
    FocusAndBokeh,
    SplitDiopter,
    ColorGrade,
    Film,
    Framing,
    Accessibility,
};

struct CinematicCameraOverlayState {
    bool focusPlanes{};
    bool splitDiopter{};
    bool safeFrames{};
    bool aspectMattes{};
    bool motionVectors{};
    bool exposurePreview{};
    bool compareUngraded{};
};

struct CinematicCameraPanelSnapshot {
    CinematicCameraScope scope{CinematicCameraScope::CameraInstance};
    CinematicCameraSection section{CinematicCameraSection::Lens};
    camera::CameraPostProcessProfile projectProfile{};
    camera::CameraPostProcessProfile previewProfile{};
    camera::CameraPhysicalLens projectLens{};
    camera::CameraPhysicalLens previewLens{};
    std::map<camera::CameraRigId, camera::CameraPostProcessProfile> rigProfiles;
    CinematicCameraOverlayState overlays{};
};

struct CinematicCameraPanelLayout {
    UiRect panel{};
    UiRect closeButton{};
    std::array<UiRect, 4> scopeTabs{};
    std::array<UiRect, 8> sectionRows{};
    std::array<UiRect, 10> presetRows{};
    std::array<UiRect, 7> filmbackRows{};
    UiRect copyButton{};
    UiRect pasteButton{};
    UiRect resetButton{};
    UiRect keyframeButton{};
    UiRect sequencerButton{};
};

class EditorCinematicCameraPanel {
public:
    void open() noexcept { open_ = true; }
    void close() noexcept { open_ = false; }
    void toggle() noexcept { open_ = !open_; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    void set_scope(CinematicCameraScope scope) noexcept { scope_ = scope; }
    [[nodiscard]] CinematicCameraScope scope() const noexcept { return scope_; }
    void set_section(CinematicCameraSection section) noexcept { section_ = section; }
    [[nodiscard]] CinematicCameraSection section() const noexcept { return section_; }

    [[nodiscard]] CinematicCameraOverlayState& overlays() noexcept { return overlays_; }
    [[nodiscard]] const CinematicCameraOverlayState& overlays() const noexcept { return overlays_; }

    [[nodiscard]] camera::CameraPostProcessProfile& profile_for_scope(
        std::optional<camera::CameraRigId> rigId);
    [[nodiscard]] const camera::CameraPostProcessProfile& profile_for_scope(
        std::optional<camera::CameraRigId> rigId) const noexcept;
    [[nodiscard]] camera::CameraPostProcessProfile& profile_for_rig(camera::CameraRigId rigId);
    [[nodiscard]] const camera::CameraPostProcessProfile& profile_for_rig(
        camera::CameraRigId rigId) const noexcept;

    [[nodiscard]] camera::CameraPhysicalLens& lens_for_scope() noexcept;
    [[nodiscard]] const camera::CameraPhysicalLens& lens_for_scope() const noexcept;

    [[nodiscard]] bool apply_preset(camera::CameraCinematicPreset preset,
                                    std::optional<camera::CameraRigId> rigId,
                                    std::string* error = nullptr);
    [[nodiscard]] bool apply_filmback(camera::CameraFilmbackPreset preset,
                                      EditorCamera& viewportCamera,
                                      camera::CameraRig* rig,
                                      std::string* error = nullptr);
    [[nodiscard]] bool clear_effects(std::optional<camera::CameraRigId> rigId,
                                     std::string* error = nullptr);
    void copy_profile(std::optional<camera::CameraRigId> rigId);
    [[nodiscard]] bool paste_profile(std::optional<camera::CameraRigId> rigId,
                                     std::string* error = nullptr);
    [[nodiscard]] bool can_paste() const noexcept { return clipboard_.has_value(); }

    [[nodiscard]] bool keyframe_current(const camera::CameraPose& pose,
                                        std::optional<camera::CameraRigId> rigId,
                                        std::string* error = nullptr);
    [[nodiscard]] EditorCameraSequencer& sequencer() noexcept { return sequencer_; }
    [[nodiscard]] const EditorCameraSequencer& sequencer() const noexcept { return sequencer_; }
    void toggle_sequencer() noexcept { sequencerOpen_ = !sequencerOpen_; }
    void open_sequencer() noexcept { sequencerOpen_ = true; }
    [[nodiscard]] bool sequencer_open() const noexcept { return sequencerOpen_; }

    [[nodiscard]] bool undo() noexcept;
    [[nodiscard]] bool redo() noexcept;
    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }

    [[nodiscard]] CinematicCameraPanelLayout layout(int windowWidth, int windowHeight) const noexcept;
    [[nodiscard]] std::string_view status() const noexcept { return status_; }
    void set_status(std::string status) { status_ = std::move(status); }

    [[nodiscard]] static std::string_view scope_name(CinematicCameraScope scope) noexcept;
    [[nodiscard]] static std::string_view section_name(CinematicCameraSection section) noexcept;
    [[nodiscard]] static std::string_view preset_name(camera::CameraCinematicPreset preset) noexcept;
    [[nodiscard]] static std::string_view filmback_name(camera::CameraFilmbackPreset preset) noexcept;

private:
    [[nodiscard]] CinematicCameraPanelSnapshot snapshot() const;
    void restore(CinematicCameraPanelSnapshot snapshot);
    void push_undo();
    [[nodiscard]] camera::CameraPostProcessProfile& mutable_profile(
        std::optional<camera::CameraRigId> rigId);

    bool open_{};
    bool sequencerOpen_{};
    CinematicCameraScope scope_{CinematicCameraScope::CameraInstance};
    CinematicCameraSection section_{CinematicCameraSection::Lens};
    camera::CameraPostProcessProfile projectProfile_{};
    camera::CameraPostProcessProfile previewProfile_{};
    camera::CameraPhysicalLens projectLens_{};
    camera::CameraPhysicalLens previewLens_{};
    std::map<camera::CameraRigId, camera::CameraPostProcessProfile> rigProfiles_;
    CinematicCameraOverlayState overlays_{};
    std::optional<camera::CameraPostProcessProfile> clipboard_;
    EditorCameraSequencer sequencer_{};
    std::vector<CinematicCameraPanelSnapshot> undo_;
    std::vector<CinematicCameraPanelSnapshot> redo_;
    std::string status_{"Camera-instance edits"};
};

} // namespace dve::editor
