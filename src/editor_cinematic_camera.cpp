#include "dve/editor_cinematic_camera.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dve::editor {
namespace {

const camera::CameraPostProcessProfile kNeutralProfile{};

camera::CameraDollyKey make_key(float timeSeconds, const camera::CameraPose& pose,
                                const camera::CameraPostProcessProfile& profile) {
    camera::CameraDollyKey key;
    key.timeSeconds = timeSeconds;
    key.pose = pose;
    key.postProcess = profile;
    return key;
}

} // namespace

camera::CameraPostProcessProfile& EditorCinematicCameraPanel::profile_for_scope(
    std::optional<camera::CameraRigId> rigId) {
    return mutable_profile(rigId);
}

const camera::CameraPostProcessProfile& EditorCinematicCameraPanel::profile_for_scope(
    std::optional<camera::CameraRigId> rigId) const noexcept {
    switch (scope_) {
        case CinematicCameraScope::ProjectDefault: return projectProfile_;
        case CinematicCameraScope::ViewportPreview: return previewProfile_;
        case CinematicCameraScope::ShotOverride: {
            const auto& sequence = sequencer_.sequence();
            if (!sequence.dollies.empty() && !sequence.dollies.front().keys.empty()) {
                const float time = sequencer_.playhead_seconds();
                const auto& keys = sequence.dollies.front().keys;
                const auto found = std::min_element(keys.begin(), keys.end(), [&](const auto& a, const auto& b) {
                    return std::abs(a.timeSeconds - time) < std::abs(b.timeSeconds - time);
                });
                if (found != keys.end()) return found->postProcess;
            }
            [[fallthrough]];
        }
        case CinematicCameraScope::CameraInstance:
            if (rigId) {
                const auto found = rigProfiles_.find(*rigId);
                if (found != rigProfiles_.end()) return found->second;
            }
            return kNeutralProfile;
    }
    return kNeutralProfile;
}

camera::CameraPostProcessProfile& EditorCinematicCameraPanel::profile_for_rig(
    camera::CameraRigId rigId) {
    return rigProfiles_[rigId];
}

const camera::CameraPostProcessProfile& EditorCinematicCameraPanel::profile_for_rig(
    camera::CameraRigId rigId) const noexcept {
    const auto found = rigProfiles_.find(rigId);
    return found == rigProfiles_.end() ? kNeutralProfile : found->second;
}

camera::CameraPhysicalLens& EditorCinematicCameraPanel::lens_for_scope() noexcept {
    return scope_ == CinematicCameraScope::ViewportPreview ? previewLens_ : projectLens_;
}

const camera::CameraPhysicalLens& EditorCinematicCameraPanel::lens_for_scope() const noexcept {
    return scope_ == CinematicCameraScope::ViewportPreview ? previewLens_ : projectLens_;
}

camera::CameraPostProcessProfile& EditorCinematicCameraPanel::mutable_profile(
    std::optional<camera::CameraRigId> rigId) {
    switch (scope_) {
        case CinematicCameraScope::ProjectDefault: return projectProfile_;
        case CinematicCameraScope::ViewportPreview: return previewProfile_;
        case CinematicCameraScope::ShotOverride:
        case CinematicCameraScope::CameraInstance:
            return rigProfiles_[rigId.value_or(0U)];
    }
    return previewProfile_;
}

bool EditorCinematicCameraPanel::apply_preset(camera::CameraCinematicPreset preset,
                                               std::optional<camera::CameraRigId> rigId,
                                               std::string* error) {
    const camera::CameraPostProcessProfile profile = camera::camera_cinematic_preset(preset);
    if (!profile.validate(error)) return false;
    push_undo();
    mutable_profile(rigId) = profile;
    status_ = "Applied " + std::string(preset_name(preset));
    return true;
}

bool EditorCinematicCameraPanel::apply_filmback(camera::CameraFilmbackPreset preset,
                                                 EditorCamera& viewportCamera,
                                                 camera::CameraRig* rig,
                                                 std::string* error) {
    const float focalLength = viewportCamera.physicalLens.focalLengthMillimeters;
    camera::CameraPhysicalLens lens = camera::camera_physical_lens_preset(preset, focalLength);
    if (!lens.validate(error)) return false;
    push_undo();
    if (scope_ == CinematicCameraScope::ProjectDefault) projectLens_ = lens;
    else if (scope_ == CinematicCameraScope::ViewportPreview) previewLens_ = lens;
    else {
        viewportCamera.physicalLens = lens;
        viewportCamera.physicalLens.enabled = true;
        if (rig) {
            rig->authoredPose.lens.physical = lens;
            rig->authoredPose.lens.physical.enabled = true;
            rig->lens.physical = lens;
            rig->lens.physical.enabled = true;
        }
    }
    status_ = "Applied " + std::string(filmback_name(preset));
    return true;
}

bool EditorCinematicCameraPanel::clear_effects(std::optional<camera::CameraRigId> rigId,
                                                std::string* error) {
    camera::CameraPostProcessProfile neutral;
    if (!neutral.validate(error)) return false;
    push_undo();
    mutable_profile(rigId) = neutral;
    status_ = "Cleared cinematic effects";
    return true;
}

void EditorCinematicCameraPanel::copy_profile(std::optional<camera::CameraRigId> rigId) {
    clipboard_ = profile_for_scope(rigId);
    status_ = "Copied cinematic profile";
}

bool EditorCinematicCameraPanel::paste_profile(std::optional<camera::CameraRigId> rigId,
                                                std::string* error) {
    if (!clipboard_) {
        if (error) *error = "no copied cinematic profile";
        return false;
    }
    if (!clipboard_->validate(error)) return false;
    push_undo();
    mutable_profile(rigId) = *clipboard_;
    status_ = "Pasted cinematic profile";
    return true;
}

bool EditorCinematicCameraPanel::keyframe_current(const camera::CameraPose& pose,
                                                   std::optional<camera::CameraRigId> rigId,
                                                   std::string* error) {
    camera::CameraSequence candidate = sequencer_.sequence();
    if (candidate.durationSeconds <= 0.0F) {
        candidate.name = "Cinematic Camera Sequence";
        candidate.durationSeconds = 5.0F;
    }
    if (candidate.dollies.empty()) {
        camera::CameraDollySpline dolly;
        dolly.id = 1U;
        dolly.name = "Cinematic Camera";
        candidate.dollies.push_back(std::move(dolly));
    }
    camera::CameraDollySpline& dolly = candidate.dollies.front();
    const float time = std::clamp(sequencer_.playhead_seconds(), 0.0F, candidate.durationSeconds);
    const camera::CameraPostProcessProfile profile = mutable_profile(rigId);
    const float tolerance = std::max(1.0e-4F, sequencer_.snap_seconds() * 0.25F);
    const auto existing = std::find_if(dolly.keys.begin(), dolly.keys.end(), [&](const auto& key) {
        return std::abs(key.timeSeconds - time) <= tolerance;
    });
    if (existing == dolly.keys.end()) dolly.keys.push_back(make_key(time, pose, profile));
    else *existing = make_key(time, pose, profile);
    if (dolly.keys.size() == 1U) {
        const float companionTime = time <= candidate.durationSeconds * 0.5F
            ? candidate.durationSeconds
            : 0.0F;
        dolly.keys.push_back(make_key(companionTime, pose, profile));
    }
    std::stable_sort(dolly.keys.begin(), dolly.keys.end(), [](const auto& a, const auto& b) {
        return a.timeSeconds < b.timeSeconds;
    });
    if (candidate.shots.empty()) {
        camera::CameraShot shot;
        shot.id = 1U;
        shot.name = "Cinematic Shot";
        shot.startSeconds = 0.0F;
        shot.durationSeconds = candidate.durationSeconds;
        shot.rigId.reset();
        shot.dollyId = dolly.id;
        shot.blendIn = {camera::CameraBlendCurve::Cut, 0.0F};
        candidate.shots.push_back(std::move(shot));
    }
    if (!candidate.validate(error)) return false;
    if (!sequencer_.set_sequence(std::move(candidate), error)) return false;
    sequencer_.set_playhead(time);
    sequencerOpen_ = true;
    status_ = "Keyframed complete cinematic profile";
    return true;
}

CinematicCameraPanelSnapshot EditorCinematicCameraPanel::snapshot() const {
    return {scope_, section_, projectProfile_, previewProfile_, projectLens_, previewLens_,
            rigProfiles_, overlays_};
}

void EditorCinematicCameraPanel::restore(CinematicCameraPanelSnapshot value) {
    scope_ = value.scope;
    section_ = value.section;
    projectProfile_ = std::move(value.projectProfile);
    previewProfile_ = std::move(value.previewProfile);
    projectLens_ = value.projectLens;
    previewLens_ = value.previewLens;
    rigProfiles_ = std::move(value.rigProfiles);
    overlays_ = value.overlays;
}

void EditorCinematicCameraPanel::push_undo() {
    undo_.push_back(snapshot());
    if (undo_.size() > 128U) undo_.erase(undo_.begin());
    redo_.clear();
}

bool EditorCinematicCameraPanel::undo() noexcept {
    if (undo_.empty()) return false;
    redo_.push_back(snapshot());
    restore(std::move(undo_.back()));
    undo_.pop_back();
    status_ = "Undid cinematic camera edit";
    return true;
}

bool EditorCinematicCameraPanel::redo() noexcept {
    if (redo_.empty()) return false;
    undo_.push_back(snapshot());
    restore(std::move(redo_.back()));
    redo_.pop_back();
    status_ = "Redid cinematic camera edit";
    return true;
}

CinematicCameraPanelLayout EditorCinematicCameraPanel::layout(int windowWidth,
                                                               int windowHeight) const noexcept {
    CinematicCameraPanelLayout result;
    const int width = std::clamp(windowWidth - 120, 760, 1180);
    const int height = std::clamp(windowHeight - 100, 560, 860);
    result.panel = {(windowWidth - width) / 2, (windowHeight - height) / 2, width, height};
    result.closeButton = {result.panel.x + result.panel.width - 92, result.panel.y + 14, 74, 28};
    const int tabWidth = (result.panel.width - 36) / 4;
    for (std::size_t i = 0U; i < result.scopeTabs.size(); ++i)
        result.scopeTabs[i] = {result.panel.x + 18 + static_cast<int>(i) * tabWidth,
                               result.panel.y + 54, tabWidth - 4, 30};
    const int leftWidth = 190;
    for (std::size_t i = 0U; i < result.sectionRows.size(); ++i)
        result.sectionRows[i] = {result.panel.x + 18, result.panel.y + 100 + static_cast<int>(i) * 34,
                                 leftWidth - 28, 30};
    const int contentX = result.panel.x + leftWidth;
    const int rowWidth = (result.panel.width - leftWidth - 36) / 2;
    for (std::size_t i = 0U; i < result.presetRows.size(); ++i)
        result.presetRows[i] = {contentX + static_cast<int>(i % 2U) * rowWidth,
                                result.panel.y + 112 + static_cast<int>(i / 2U) * 34,
                                rowWidth - 6, 30};
    for (std::size_t i = 0U; i < result.filmbackRows.size(); ++i)
        result.filmbackRows[i] = {contentX + static_cast<int>(i % 2U) * rowWidth,
                                  result.panel.y + 304 + static_cast<int>(i / 2U) * 34,
                                  rowWidth - 6, 30};
    const int bottomY = result.panel.y + result.panel.height - 48;
    result.copyButton = {contentX, bottomY, 94, 30};
    result.pasteButton = {contentX + 100, bottomY, 94, 30};
    result.resetButton = {contentX + 200, bottomY, 110, 30};
    result.keyframeButton = {contentX + 316, bottomY, 132, 30};
    result.sequencerButton = {contentX + 454, bottomY, 138, 30};
    return result;
}

std::string_view EditorCinematicCameraPanel::scope_name(CinematicCameraScope scope) noexcept {
    switch (scope) {
        case CinematicCameraScope::CameraInstance: return "Camera";
        case CinematicCameraScope::ShotOverride: return "Shot";
        case CinematicCameraScope::ProjectDefault: return "Project";
        case CinematicCameraScope::ViewportPreview: return "Preview";
    }
    return "Camera";
}

std::string_view EditorCinematicCameraPanel::section_name(CinematicCameraSection section) noexcept {
    switch (section) {
        case CinematicCameraSection::Composition: return "Composition";
        case CinematicCameraSection::Lens: return "Lens";
        case CinematicCameraSection::FocusAndBokeh: return "Focus and Bokeh";
        case CinematicCameraSection::SplitDiopter: return "Split Diopter";
        case CinematicCameraSection::ColorGrade: return "Color Grade";
        case CinematicCameraSection::Film: return "Film";
        case CinematicCameraSection::Framing: return "Framing";
        case CinematicCameraSection::Accessibility: return "Accessibility";
    }
    return "Lens";
}

std::string_view EditorCinematicCameraPanel::preset_name(camera::CameraCinematicPreset preset) noexcept {
    switch (preset) {
        case camera::CameraCinematicPreset::Neutral: return "Neutral";
        case camera::CameraCinematicPreset::AcademyClassic: return "Academy Classic";
        case camera::CameraCinematicPreset::Imax143: return "IMAX 1.43";
        case camera::CameraCinematicPreset::Imax190: return "IMAX 1.90";
        case camera::CameraCinematicPreset::Scope239: return "Scope 2.39";
        case camera::CameraCinematicPreset::VintageAnamorphic: return "Vintage Anamorphic";
        case camera::CameraCinematicPreset::FisheyeAction: return "Fisheye Action";
        case camera::CameraCinematicPreset::SplitDiopter: return "Split Diopter";
        case camera::CameraCinematicPreset::BleachBypass: return "Bleach Bypass";
        case camera::CameraCinematicPreset::SeventiesFilm: return "Seventies Film";
    }
    return "Neutral";
}

std::string_view EditorCinematicCameraPanel::filmback_name(camera::CameraFilmbackPreset preset) noexcept {
    switch (preset) {
        case camera::CameraFilmbackPreset::Custom: return "Custom Filmback";
        case camera::CameraFilmbackPreset::Super16: return "Super 16";
        case camera::CameraFilmbackPreset::Super35: return "Super 35";
        case camera::CameraFilmbackPreset::FullFrame35: return "Full Frame 35";
        case camera::CameraFilmbackPreset::Anamorphic35: return "Anamorphic 35";
        case camera::CameraFilmbackPreset::Imax15Perf: return "IMAX 15-perf";
        case camera::CameraFilmbackPreset::ImaxDigital: return "IMAX Digital";
    }
    return "Custom Filmback";
}

} // namespace dve::editor
