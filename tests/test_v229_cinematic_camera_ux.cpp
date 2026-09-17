#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::camera;
using namespace dve::editor;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class RecordingCanvas final : public IEditorCanvas {
public:
    void fill(UiRect, EditorColor) const override { ++fills; }
    void outline(UiRect, EditorColor) const override { ++outlines; }
    void line(int, int, int, int, EditorColor, int) const override { ++lines; }
    void text(int, int, std::string_view value, EditorColor) const override {
        texts.emplace_back(value);
    }
    [[nodiscard]] int text_width(std::string_view value) const override {
        return static_cast<int>(value.size()) * 7;
    }
    mutable std::size_t fills{};
    mutable std::size_t outlines{};
    mutable std::size_t lines{};
    mutable std::vector<std::string> texts;
};

void test_registry_and_settings() {
    EditorMenuRegistry menus = EditorMenuRegistry::make_default();
    require(menus.find("camera.inspector") != nullptr, "cinematic camera inspector command missing");
    require(menus.find("camera.preset_imax143") != nullptr, "IMAX 1.43 command missing");
    require(menus.find("camera.preset_split_diopter") != nullptr, "split-diopter command missing");
    const auto bokeh = menus.search("bokeh blades", 32);
    require(std::any_of(bokeh.begin(), bokeh.end(), [](const MenuAction& action) {
        return action.id == "camera.inspector";
    }), "Command Center could not discover the cinematic camera inspector from bokeh terms");

    EditorSettingsRegistry settings = EditorSettingsRegistry::make_default();
    require(settings.find("camera.default_cinematic_preset") != nullptr,
            "default cinematic preset setting missing");
    require(settings.find("camera.lut_resolution") != nullptr, "LUT resolution setting missing");
    require(settings.find("camera.accessibility_limit_fisheye") != nullptr,
            "fisheye accessibility setting missing");
    const auto imax = settings.search("IMAX filmback", true);
    require(!imax.empty(), "camera settings search did not find filmback controls");
}

void test_controller_commands_profiles_overlays_and_sequencer() {
    NativeEditorController controller{EditorWorkspace{make_native_editor_demo_document()}};
    require(controller.selected_camera_rig().has_value(), "default camera rig was not created");
    const CameraRigId rigId = *controller.selected_camera_rig();

    require(controller.dispatch_action("window.cinematic_camera"), "camera inspector did not open");
    require(controller.cinematic_camera_panel().is_open(), "camera inspector open state missing");
    require(controller.dispatch_action("camera.scope_shot"), "shot scope command failed");
    require(controller.cinematic_camera_panel().scope() == CinematicCameraScope::ShotOverride,
            "camera inspector did not enter shot scope");
    if (!controller.dispatch_action("camera.preset_split_diopter")) throw std::runtime_error("split-diopter preset failed: " + controller.status().text);
    const auto& profile = controller.cinematic_camera_panel().profile_for_rig(rigId);
    require(profile.cinematic.splitDiopter.enabled,
            "split-diopter preset did not propagate to selected camera profile");
    require(controller.cinematic_camera_panel().sequencer_open(),
            "shot-scope preset did not open/create sequencer state");
    require(!controller.cinematic_camera_panel().sequencer().sequence().dollies.empty(),
            "shot-scope preset did not create a dolly profile key");

    require(controller.dispatch_action("camera.filmback_imax15"), "IMAX filmback command failed");
    const CameraRig* rig = controller.camera_director().find_rig(rigId);
    require(rig != nullptr, "selected camera rig vanished");
    require(rig->authoredPose.lens.physical.enabled &&
            rig->authoredPose.lens.physical.sensorWidthMillimeters > 70.0F,
            "IMAX filmback did not reach selected camera rig");

    require(controller.dispatch_action("camera.overlay_focus"), "focus overlay command failed");
    require(controller.dispatch_action("camera.overlay_split_diopter"), "split overlay command failed");
    require(controller.dispatch_action("camera.overlay_aspect_mattes"), "matte overlay command failed");
    const auto& overlays = controller.cinematic_camera_panel().overlays();
    require(overlays.focusPlanes && overlays.splitDiopter && overlays.aspectMattes,
            "camera overlay state did not update");

    require(controller.dispatch_action("camera.copy_profile"), "profile copy failed");
    require(controller.dispatch_action("camera.clear_effects"), "profile clear failed");
    require(!controller.cinematic_camera_panel().profile_for_rig(rigId).cinematic.splitDiopter.enabled,
            "profile clear did not remove split diopter");
    require(controller.dispatch_action("camera.paste_profile"), "profile paste failed");
    require(controller.cinematic_camera_panel().profile_for_rig(rigId).cinematic.splitDiopter.enabled,
            "profile paste did not restore split diopter");

    require(controller.dispatch_action("camera.scope_project"), "project scope command failed");
    require(controller.dispatch_action("camera.preset_academy"), "project preset command failed");
    const SettingValue projectPreset =
        controller.workspace().settings().value("camera.default_cinematic_preset");
    require(std::get_if<std::string>(&projectPreset) && std::get<std::string>(projectPreset) == "academy",
            "project-scope preset did not persist to project settings");
    require(controller.dispatch_action("camera.filmback_super35"), "project filmback command failed");
    const SettingValue projectFilmback = controller.workspace().settings().value("camera.sensor_preset");
    require(std::get_if<std::string>(&projectFilmback) && std::get<std::string>(projectFilmback) == "super35",
            "project-scope filmback did not persist to project settings");

    require(controller.dispatch_action("camera.scope_instance"), "camera instance scope failed");
    require(controller.dispatch_action("camera.preset_imax190"), "IMAX 1.90 preset failed");
    require(controller.cinematic_camera_panel().profile_for_rig(rigId).cinematic.film.framing ==
                CameraFramingPreset::Imax190,
            "IMAX 1.90 framing did not propagate to the selected camera");
    require(controller.dispatch_action("camera.keyframe_profile"), "complete-profile keyframe failed");
    require(!controller.cinematic_camera_panel().sequencer().sequence().shots.empty(),
            "complete-profile keyframe did not retain a shot");

    require(controller.dispatch_action("camera.lock_viewport"), "viewport lock command failed");
    const SettingValue lock = controller.workspace().settings().value("camera.lock_viewport_to_selected");
    require(std::get_if<bool>(&lock) && std::get<bool>(lock), "viewport lock setting did not update");

    controller.resize(1280, 800);
    const CinematicCameraPanelLayout panelLayout = controller.cinematic_camera_panel().layout(1280, 800);
    controller.pointer_down(PointerButton::Primary,
                            panelLayout.presetRows[2].x + 4,
                            panelLayout.presetRows[2].y + 4, 0U);
    require(controller.cinematic_camera_panel().profile_for_rig(rigId).cinematic.film.framing ==
                CameraFramingPreset::Imax143,
            "native camera panel preset row did not dispatch IMAX 1.43");

    RecordingCanvas canvas;
    render_native_editor(canvas, controller, 1280, 800);
    require(canvas.fills > 0U && canvas.outlines > 0U && canvas.lines > 0U,
            "native editor renderer produced no camera UI or overlays");
    require(std::find(canvas.texts.begin(), canvas.texts.end(), "CINEMATIC CAMERA") != canvas.texts.end(),
            "native cinematic camera panel title was not rendered");
    require(std::find(canvas.texts.begin(), canvas.texts.end(), "CAMERA / FILM PRESETS") != canvas.texts.end(),
            "cinematic preset browser was not rendered");
}

} // namespace

int main() {
    try {
        test_registry_and_settings();
        test_controller_commands_profiles_overlays_and_sequencer();
        std::cout << "dve_v229_cinematic_camera_ux_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v229_cinematic_camera_ux_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
