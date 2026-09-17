#include <iostream>
#include <stdexcept>
#include <string>

#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_settings.hpp"
#include "dve/editor_workspace.hpp"

namespace {
using namespace dve;
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
    [[nodiscard]] bool contains(std::string_view needle) const {
        return std::any_of(texts.begin(), texts.end(), [&](const std::string& text) {
            return text.find(needle) != std::string::npos;
        });
    }
    mutable int fills{};
    mutable int outlines{};
    mutable int lines{};
    mutable std::vector<std::string> texts;
};

void test_registry_precedence_and_transactional_parse() {
    EditorSettingsRegistry registry = EditorSettingsRegistry::make_default();
    std::string error;
    require(registry.validate(&error), error.c_str());
    require(std::get<double>(registry.value("camera.fly_speed")) == 5.0, "camera default missing");
    require(std::get<std::string>(registry.value("render.gi_mode")) == "voxel_one_bounce",
            "global illumination is not enabled by default");
    require(std::get<std::string>(registry.value("render.shadow_mode")) == "soft",
            "soft shadows are not the default shadow type");
    require(std::get<double>(registry.value("render.shadow_strength")) == 0.85,
            "default shadow strength drifted");
    require(registry.set(SettingScope::User, "camera.fly_speed", 7.5, &error), error.c_str());
    require(registry.set(SettingScope::Project, "camera.fly_speed", 11.0, &error), error.c_str());
    require(registry.set(SettingScope::Session, "camera.fly_speed", 18.0, &error), error.c_str());
    SettingScope source{};
    bool inherited{};
    require(std::get<double>(registry.value("camera.fly_speed", &source, &inherited)) == 18.0,
            "session setting did not win");
    require(source == SettingScope::Session && !inherited, "setting source scope incorrect");
    require(registry.clear(SettingScope::Session, "camera.fly_speed"), "session clear failed");
    require(std::get<double>(registry.value("camera.fly_speed")) == 11.0, "project fallback failed");

    const std::string serialized = registry.serialize_scope(SettingScope::Project);
    EditorSettingsRegistry loaded = EditorSettingsRegistry::make_default();
    require(loaded.parse_scope(SettingScope::Project, serialized, &error), error.c_str());
    require(std::get<double>(loaded.value("camera.fly_speed")) == 11.0, "scope round trip failed");
    const std::string before = loaded.serialize_scope(SettingScope::Project);
    require(!loaded.parse_scope(SettingScope::Project,
        "DVE_SETTINGS 1 Project\ncamera.fly_speed=not-a-number\n", &error),
        "corrupt settings were accepted");
    require(loaded.serialize_scope(SettingScope::Project) == before,
            "failed settings parse was not transactional");
}

void test_capability_search_and_panel_staging() {
    EditorSettingsRegistry registry = EditorSettingsRegistry::make_default();
    const auto voxelOnly = registry.categories(SettingCapabilityEditor | SettingCapabilityVoxel);
    require(std::find(voxelOnly.begin(), voxelOnly.end(), "Voxel") != voxelOnly.end(), "voxel category missing");
    require(std::find(voxelOnly.begin(), voxelOnly.end(), "Polygon") == voxelOnly.end(),
            "polygon category ignored capabilities");
    const auto cameraResults = registry.search("physical lens", false, SettingCapabilityAll, 8);
    require(!cameraResults.empty() && cameraResults.front().definition->id == "camera.physical_lens",
            "settings search did not prioritize physical lens");

    EditorSettingsPanelState panel;
    panel.open_for(SettingScope::Project, "Camera");
    std::string error;
    require(panel.cycle(registry, "camera.projection", 1, &error), error.c_str());
    require(std::get<std::string>(panel.displayed_value(registry, "camera.projection")) == "orthographic",
            "enum cycling failed");
    require(panel.stage(registry, "camera.field_of_view", 75.0, &error), error.c_str());
    require(panel.dirty, "staging did not mark panel dirty");
    require(panel.apply(registry, &error), error.c_str());
    require(std::get<double>(registry.value("camera.field_of_view")) == 75.0, "staged float not applied");
    require(std::get<std::string>(registry.value("camera.projection")) == "orthographic",
            "staged enum not applied");

    panel.open_for(SettingScope::Project, "Camera");
    require(panel.begin_value_edit(registry, "camera.field_of_view", &error), error.c_str());
    panel.append_value_text("82.5");
    require(panel.commit_value_edit(registry, &error), error.c_str());
    require(panel.apply(registry, &error), error.c_str());
    require(std::get<double>(registry.value("camera.field_of_view")) == 82.5,
            "direct numeric setting entry failed");
    require(panel.reset(registry, "camera.field_of_view", &error), error.c_str());
    require(panel.apply(registry, &error), error.c_str());
    require(std::get<double>(registry.value("camera.field_of_view")) == 60.0,
            "reset option did not restore inherited/default value");
}

void test_menu_and_native_controller_integration() {
    EditorMenuRegistry menus = EditorMenuRegistry::make_default();
    require(kMenuBarNames.size() == 8U, "top-level menu bar was not simplified");
    require(menus.find("camera.mode_orbit") != nullptr, "camera menu action missing");
    require(menus.find("camera.mode_orbit")->menu == "View", "camera commands were not grouped under View");
    require(menus.find("physics.settings")->menu == "Tools", "physics commands were not grouped under Tools");
    require(!menus.search("quick actions", 8).empty(), "menu descriptions/keywords are not searchable");
    require(menus.set_checked("camera.mode_orbit", true), "camera radio check failed");
    require(menus.set_checked("camera.mode_free", true), "camera radio switch failed");
    require(menus.find("camera.mode_free")->checked, "new camera radio item not checked");
    require(!menus.find("camera.mode_orbit")->checked, "old camera radio item remained checked");

    NativeEditorController controller{EditorWorkspace{make_native_editor_demo_document()}};
    controller.resize(1280, 800);
    require(controller.dispatch_action("camera.mode_free"), "free camera action failed");
    require(controller.camera_mode() == camera::CameraRigMode::FreeFly, "camera mode did not change");
    require(controller.workspace().menus().find("camera.mode_free")->checked, "menu check did not refresh");
    require(controller.dispatch_action("camera.physical_lens"), "physical lens action failed");
    require(controller.camera().physicalLens.enabled, "physical lens did not reach editor camera");
    controller.save_camera_bookmark(0);
    const Float3 saved = controller.camera().position;
    controller.camera().position = {99.0F, 99.0F, 99.0F};
    require(controller.load_camera_bookmark(0), "camera bookmark load failed");
    require(length(subtract(controller.camera().position, saved)) < 1.0e-6F, "camera bookmark did not restore pose");

    controller.open_settings(SettingScope::Project, "Camera");
    require(controller.settings_panel().open, "settings panel did not open");
    const NativeSettingsModalLayout layout = controller.settings_modal_layout();
    require(layout.panel.width > 600 && !layout.settingRows.empty(), "settings modal layout invalid");
    controller.text_input("field of view");
    require(!controller.settings_rows().empty(), "settings modal search returned nothing");
    controller.key_down("escape", false, false, false);
    require(!controller.settings_panel().open, "settings panel did not close on escape");

    require(controller.dispatch_action("help.command_palette"), "command palette action failed");
    RecordingCanvas paletteCanvas;
    render_native_editor(paletteCanvas, controller, 1280, 800);
    require(paletteCanvas.contains("Search commands and settings"),
            "native renderer did not draw the command palette");
    controller.text_input("field of view");
    const auto paletteResults = controller.command_palette_results(12);
    require(!paletteResults.empty() &&
            paletteResults.front().kind == CommandPaletteResultKind::Setting &&
            paletteResults.front().id == "camera.field_of_view",
            "command palette did not search settings");
    const NativeCommandPaletteLayout paletteLayout = controller.command_palette_layout();
    require(paletteLayout.panel.width > 500 && !paletteLayout.rows.empty(),
            "command palette has no native layout");
    controller.key_down("enter", false, false, false);
    require(controller.settings_panel().open, "activating a setting result did not open settings");
    require(!controller.settings_rows().empty() &&
            controller.settings_rows().front()->id == "camera.field_of_view",
            "setting result did not focus the requested option");
    controller.key_down("escape", false, false, false);
}

void test_dependencies_reset_changed_and_profiles() {
    EditorSettingsRegistry registry=EditorSettingsRegistry::make_default();std::string error;
    const auto unavailable=registry.availability("camera.focal_length_mm");require(!unavailable.available,"physical-lens dependency was ignored");
    require(registry.set(SettingScope::Project,"camera.physical_lens",true,&error),error.c_str());require(registry.availability("camera.focal_length_mm").available,"physical-lens dependency did not unlock");
    require(registry.set(SettingScope::Project,"camera.fly_speed",22.0,&error),error.c_str());require(!registry.changed().empty(),"changed-only view is empty");
    const std::string profile=registry.serialize_profile("Cinematic",SettingScope::Project);EditorSettingsRegistry loaded=EditorSettingsRegistry::make_default();require(loaded.parse_profile(SettingScope::Project,profile,&error),error.c_str());require(std::get<double>(loaded.value("camera.fly_speed"))==22.0,"settings profile lost value");
    const std::string orphanProfile="DVE_SETTINGS_PROFILE 1 \"Old\" Project\ncount 1\n\"retired.camera.option\" bool 1\n";require(loaded.parse_profile(SettingScope::Project,orphanProfile,&error),error.c_str());require(loaded.orphaned_settings().size()==1,"orphaned setting was not diagnosed");
    require(registry.reset_category(SettingScope::Project,"Camera")>=2U,"category reset removed too few values");require(std::get<double>(registry.value("camera.fly_speed"))==5.0,"category reset did not restore inheritance");
}

} // namespace

int main() {
    try {
        test_registry_precedence_and_transactional_parse();
        test_capability_search_and_panel_staging();
        test_menu_and_native_controller_integration();
        test_dependencies_reset_changed_and_profiles();
        std::cout << "dve_editor_settings_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_editor_settings_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
