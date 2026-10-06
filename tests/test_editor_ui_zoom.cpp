// UI zoom (Phase A): zoom math, window cap, hotkeys, settings wiring, persistence round-trip,
// and pointer mapping through the platform bridge.
#include "dve/editor_native.hpp"
#include "dve/editor_platform_bridge.hpp"
#include "dve/editor_settings.hpp"
#include "dve/editor_ui_zoom.hpp"
#include "dve/editor_workspace.hpp"

#include <utility>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>

using namespace dve;
using namespace dve::editor;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(float a, float b) { return std::fabs(a - b) < 1.0e-5F; }

std::filesystem::path temp_root(const char* name) {
    const auto root = std::filesystem::temp_directory_path() / name;
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    return root;
}

double stored_zoom(const EditorSettingsRegistry& settings, SettingScope* source = nullptr) {
    SettingScope scope = SettingScope::User;
    const SettingValue value = settings.value(kUiZoomSettingId, &scope);
    if (source) *source = scope;
    const auto* number = std::get_if<double>(&value);
    require(number != nullptr, "editor.ui_scale is not a float setting");
    return *number;
}

void test_snap_and_clamp() {
    require(near(snap_ui_zoom(1.0F), 1.0F) && near(snap_ui_zoom(2.0F), 2.0F), "endpoints must be preserved");
    require(near(snap_ui_zoom(0.5F), 1.0F) && near(snap_ui_zoom(0.75F), 1.0F), "values below 100% clamp to 100%");
    require(near(snap_ui_zoom(3.0F), 2.0F) && near(snap_ui_zoom(2.6F), 2.0F), "values above 200% clamp to 200%");
    require(near(snap_ui_zoom(1.1F), 1.0F) && near(snap_ui_zoom(1.13F), 1.25F), "values snap to the nearest 25% step");
    require(near(snap_ui_zoom(1.6F), 1.5F) && near(snap_ui_zoom(1.9F), 2.0F), "mid-range snapping failed");
    require(near(snap_ui_zoom(std::numeric_limits<float>::quiet_NaN()), 1.0F), "NaN must fall back to 100%");
    require(near(snap_ui_zoom(std::numeric_limits<float>::infinity()), 1.0F), "infinity must fall back to 100%");
    for (float zoom = kUiZoomMin; zoom <= kUiZoomMax + 1.0e-4F; zoom += kUiZoomStep)
        require(near(snap_ui_zoom(zoom), zoom), "every 25% step is a fixed point");

    require(near(step_ui_zoom(1.0F, 1), 1.25F) && near(step_ui_zoom(1.75F, 1), 2.0F), "zoom in steps by 25%");
    require(near(step_ui_zoom(2.0F, 1), 2.0F), "zoom in saturates at 200%");
    require(near(step_ui_zoom(1.5F, -1), 1.25F) && near(step_ui_zoom(1.0F, -1), 1.0F), "zoom out saturates at 100%");
    require(near(step_ui_zoom(1.75F, 0), 1.0F), "reset returns to 100%");
    require(near(step_ui_zoom(1.1F, 1), 1.25F), "stepping from an off-grid value snaps first");
    require(format_ui_zoom_percent(1.5F) == "150%" && format_ui_zoom_percent(1.25F) == "125%", "percent formatting");
}

void test_window_cap() {
    // The logical canvas must stay >= 640x480.
    require(near(max_ui_zoom_for_window(640, 480), 1.0F), "minimum window allows only 100%");
    require(near(max_ui_zoom_for_window(1280, 800), 1.5F), "1280x800 fits 150% (853x533 logical) but not 175%");
    require(near(max_ui_zoom_for_window(1280, 960), 2.0F), "1280x960 fits exactly 200%");
    require(near(max_ui_zoom_for_window(1920, 1080), 2.0F), "1080p fits 200% (limited by 1080/480=2.25)");
    require(near(max_ui_zoom_for_window(1600, 900), 1.75F), "1600x900 fits 175%");
    require(near(max_ui_zoom_for_window(4000, 4000), 2.0F), "huge windows are still capped at 200%");
    require(near(max_ui_zoom_for_window(320, 200), 1.0F) && near(max_ui_zoom_for_window(0, 0), 1.0F),
            "tiny/invalid windows never go below 100%");
    require(near(effective_ui_zoom(2.0F, 1280, 800), 1.5F), "requested 200% is capped to 150% at 1280x800");
    require(near(effective_ui_zoom(1.25F, 1280, 800), 1.25F), "requests under the cap are honoured");
    require(near(effective_ui_zoom(3.0F, 2560, 1600), 2.0F), "requests are clamped before capping");
    for (int w = 640; w <= 2600; w += 37)
        for (int h = 480; h <= 1700; h += 53) {
            const float zoom = max_ui_zoom_for_window(w, h);
            require(ui_zoom_logical_extent(w, zoom) >= kUiZoomMinLogicalWidth &&
                    ui_zoom_logical_extent(h, zoom) >= kUiZoomMinLogicalHeight,
                    "window cap must keep the logical canvas >= 640x480 at " + std::to_string(w) + "x" + std::to_string(h));
        }
}

void test_logical_physical_mapping() {
    require(ui_zoom_to_physical(100, 1.5F) == 150 && ui_zoom_to_physical(7, 1.25F) == 9, "logical -> physical");
    require(ui_zoom_to_logical(150, 1.5F) == 100 && ui_zoom_to_logical(151, 1.5F) == 100 &&
            ui_zoom_to_logical(149, 1.5F) == 99, "physical -> logical floors");
    require(ui_zoom_logical_extent(2560, 2.0F) == 1280 && ui_zoom_logical_extent(1920, 1.5F) == 1280,
            "logical window extent");
    // Every physical pixel inside a mapped rect maps back into the logical rect (hit-testing is consistent
    // with what is drawn), and adjacent rects tile without gaps or overlaps at every zoom step.
    for (float zoom = kUiZoomMin; zoom <= kUiZoomMax + 1.0e-4F; zoom += kUiZoomStep) {
        int previousRight = 0;
        for (int x = 0; x < 200; x += 7) {
            const UiRect logical{x, 3, 7, 5};
            const UiRect physical = ui_zoom_to_physical(logical, zoom);
            require(physical.x == previousRight, "adjacent rects must tile seamlessly");
            previousRight = physical.x + physical.width;
            for (int px = physical.x; px < physical.x + physical.width; ++px)
                for (int py = physical.y; py < physical.y + physical.height; ++py)
                    require(logical.contains(ui_zoom_to_logical(px, zoom), ui_zoom_to_logical(py, zoom)),
                            "drawn physical pixel hit-tests outside its logical rect");
        }
    }
    require(ui_zoom_stroke(1.0F, 1.0F) == 1 && ui_zoom_stroke(1.0F, 2.0F) == 2 && ui_zoom_stroke(2.0F, 1.5F) == 3,
            "stroke widths scale");
    require(ui_zoom_text_pixel_size(1.0F) == 11 && ui_zoom_text_pixel_size(1.5F) == 17 &&
            ui_zoom_text_pixel_size(2.0F) == 22 && ui_zoom_text_pixel_size(0.5F) == 11, "text raster size");
}

void test_hotkey_names() {
    for (const char* key : {"=", "equal", "+", "plus", "KP_Add", "Keypad +"})
        require(ui_zoom_hotkey_direction(key, true, false) == 1, std::string("zoom-in hotkey not recognized: ") + key);
    for (const char* key : {"-", "minus", "KP_Subtract", "Keypad -"})
        require(ui_zoom_hotkey_direction(key, true, false) == -1, std::string("zoom-out hotkey not recognized: ") + key);
    for (const char* key : {"0", "KP_0", "Keypad 0"})
        require(ui_zoom_hotkey_direction(key, true, false) == 0, std::string("reset hotkey not recognized: ") + key);
    require(!ui_zoom_hotkey_direction("=", false, false), "plain '=' must not zoom");
    require(!ui_zoom_hotkey_direction("=", true, true), "Ctrl+Alt+= must not zoom");
    require(!ui_zoom_hotkey_direction("1", true, false), "Ctrl+1 is a panel toggle, not zoom");
}

void test_setting_definition_and_legacy_preferences() {
    const EditorSettingsRegistry registry = EditorSettingsRegistry::make_default();
    const SettingDefinition* definition = registry.find(kUiZoomSettingId);
    require(definition != nullptr && definition->label == "UI Zoom", "settings row must be labelled UI Zoom");
    require(definition->minimum && near(static_cast<float>(*definition->minimum), kUiZoomMin) &&
            definition->maximum && near(static_cast<float>(*definition->maximum), kUiZoomMax) &&
            definition->step && near(static_cast<float>(*definition->step), kUiZoomStep),
            "settings row must use the shared 100-200% / 25% range");

    std::string error;
    auto legacy = EditorPreferences::parse("DVE_EDITOR_PREFERENCES=1\nuiScale=3\n", &error);
    require(legacy && near(legacy->uiScale, 2.0F), "legacy 3.0 UI scale must load and snap to 200%: " + error);
    legacy = EditorPreferences::parse("DVE_EDITOR_PREFERENCES=1\nuiScale=0.75\n", &error);
    require(legacy && near(legacy->uiScale, 1.0F), "legacy 0.75 UI scale must load and snap to 100%");
    legacy = EditorPreferences::parse("DVE_EDITOR_PREFERENCES=1\nuiScale=1.3\n", &error);
    require(legacy && near(legacy->uiScale, 1.25F), "off-step UI scale must snap");
    EditorPreferences preferences;
    preferences.uiScale = 2.5F;
    require(!preferences.validate(&error), "validate must enforce the shared 1.0-2.0 clamp");
    preferences.uiScale = 1.75F;
    const auto roundTrip = EditorPreferences::parse(preferences.serialize(), &error);
    require(roundTrip && near(roundTrip->uiScale, 1.75F), "preferences UI zoom round trip");
}

void test_controller_hotkeys_and_cap() {
    NativeEditorController controller{EditorWorkspace(make_new_project_document())};
    require(near(controller.ui_zoom(), 1.0F), "default UI zoom is 100%");
    controller.key_down("equal", true, false, false);
    require(near(controller.ui_zoom(), 1.25F), "Ctrl+= zooms in");
    controller.key_down("+", true, true, false);
    require(near(controller.ui_zoom(), 1.5F), "Ctrl+Shift+= (plus) zooms in");
    require(near(static_cast<float>(stored_zoom(controller.workspace().settings())), 1.5F),
            "hotkeys must write the editor.ui_scale setting");
    controller.key_down("minus", true, false, false);
    require(near(controller.ui_zoom(), 1.25F), "Ctrl+- zooms out");
    controller.key_down("0", true, false, false);
    require(near(controller.ui_zoom(), 1.0F), "Ctrl+0 resets");
    for (int i = 0; i < 10; ++i) controller.key_down("=", true, false, false);
    require(near(controller.ui_zoom(), 2.0F), "zoom in saturates at 200%");

    controller.set_ui_zoom_window_limit(max_ui_zoom_for_window(1280, 800));
    require(near(controller.effective_ui_zoom(), 1.5F), "effective zoom honours the window cap");
    require(near(controller.ui_zoom(), 2.0F), "the cap does not overwrite the user's request");
    (void)controller.step_ui_zoom(1);
    require(controller.status().text.find("window too small") != std::string::npos,
            "status must explain when the window caps the zoom");
    controller.set_ui_zoom_window_limit(2.0F);
    require(near(controller.effective_ui_zoom(), 2.0F), "growing the window restores the requested zoom");

    // Hotkeys work while the Settings modal is open and agree with its row.
    (void)controller.dispatch_action("window.settings");
    require(controller.settings_panel().open, "settings did not open");
    controller.key_down("minus", true, false, false);
    require(near(controller.ui_zoom(), 1.75F), "Ctrl+- must work with Settings open");
    const SettingValue displayed = controller.settings_panel().displayed_value(controller.workspace().settings(), kUiZoomSettingId);
    require(std::get_if<double>(&displayed) && near(static_cast<float>(std::get<double>(displayed)), 1.75F),
            "settings row must show the hotkey-updated zoom");

    // Menu / command-palette actions route to the same state.
    controller.close_settings(false);
    require(controller.dispatch_action("view.ui_zoom_reset") && near(controller.ui_zoom(), 1.0F), "menu reset action");
    require(controller.dispatch_action("view.ui_zoom_in") && near(controller.ui_zoom(), 1.25F), "menu zoom-in action");
    require(std::as_const(controller.workspace().menus()).find("view.ui_zoom_in") != nullptr, "View menu exposes UI zoom");
}

void test_settings_row_apply_writes_user_scope() {
    NativeEditorController controller{EditorWorkspace(make_new_project_document())};
    controller.open_settings(SettingScope::Project, "General");
    std::string error;
    require(controller.settings_panel().stage(controller.workspace().settings(), kUiZoomSettingId, 1.5, &error),
            "could not stage UI zoom: " + error);
    controller.close_settings(true);
    SettingScope source = SettingScope::Session;
    require(near(static_cast<float>(stored_zoom(controller.workspace().settings(), &source)), 1.5F) &&
            source == SettingScope::User, "UI zoom edited on any scope tab must land in the User layer");
    require(!controller.workspace().settings().has_override(SettingScope::Project, kUiZoomSettingId),
            "UI zoom must not create a Project override");
    require(near(controller.ui_zoom(), 1.5F), "applying the row must update the live zoom");

    // A Session override (e.g. --ui-zoom) is replaced by an explicit choice.
    (void)controller.workspace().settings().set(SettingScope::Session, kUiZoomSettingId, 2.0);
    controller.workspace().synchronize_preferences_from_settings();
    require(near(controller.ui_zoom(), 2.0F), "session override applies");
    controller.key_down("minus", true, false, false);
    require(near(controller.ui_zoom(), 1.75F) &&
            !controller.workspace().settings().has_override(SettingScope::Session, kUiZoomSettingId),
            "hotkey replaces the session override and writes User");
}

void test_persistence_round_trip() {
    const auto root = temp_root("dve_ui_zoom_tests");
    const auto path = root / ".dve" / "user" / "editor_settings.txt";
    {
        NativeEditorController controller{EditorWorkspace(make_new_project_document())};
        controller.configure_user_settings(path);
        require(!std::filesystem::exists(path), "configuring must not create the file eagerly");
        controller.key_down("=", true, false, false);
        controller.key_down("=", true, false, false);
        controller.key_down("=", true, false, false);
        require(std::filesystem::exists(path), "hotkey must save User settings");
    }
    {
        NativeEditorController reloaded{EditorWorkspace(make_new_project_document())};
        reloaded.configure_user_settings(path);
        require(near(reloaded.ui_zoom(), 1.75F), "UI zoom must survive a restart");
        reloaded.open_settings(SettingScope::User, "General");
        std::string error;
        require(reloaded.settings_panel().stage(reloaded.workspace().settings(), kUiZoomSettingId, 1.25, &error), error);
        reloaded.close_settings(true);
    }
    {
        NativeEditorController reloaded{EditorWorkspace(make_new_project_document())};
        reloaded.configure_user_settings(path);
        require(near(reloaded.ui_zoom(), 1.25F), "Settings > Apply must persist UI zoom");
    }
    // Registry-level round trip of the User layer.
    EditorSettingsRegistry registry = EditorSettingsRegistry::make_default();
    std::string error;
    require(registry.set(SettingScope::User, kUiZoomSettingId, 2.0, &error), error);
    const auto file = root / "registry.txt";
    require(registry.save_scope_file(SettingScope::User, file, &error), error);
    EditorSettingsRegistry loaded = EditorSettingsRegistry::make_default();
    require(loaded.load_scope_file(SettingScope::User, file, &error), error);
    require(near(static_cast<float>(stored_zoom(loaded)), 2.0F), "registry User-scope file round trip");
    // Corrupt files are reported, not fatal, and leave defaults in place.
    std::ofstream(path, std::ios::trunc) << "garbage\n";
    NativeEditorController recovered{EditorWorkspace(make_new_project_document())};
    recovered.configure_user_settings(path);
    require(near(recovered.ui_zoom(), 1.0F), "corrupt settings file must fall back to defaults");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void test_bridge_maps_pointer_to_logical_pixels() {
    NativeEditorController controller{EditorWorkspace(make_new_project_document())};
    EditorPlatformBridge bridge(controller);
    // Physical 1280x960 window at 200% -> 640x480 logical.
    controller.set_ui_zoom(2.0F);
    bridge.set_ui_zoom(2.0F);
    platform::PlatformEvent resize;
    resize.type = platform::EventType::WindowResized;
    resize.width = 1280;
    resize.height = 960;
    bridge.handle_event(resize);
    require(near(controller.effective_ui_zoom(), 2.0F), "bridge resize must report the window cap");
    require(controller.layout().menuBar.width == 640, "controller must run at the logical width");

    controller.key_down("h", false, false, true); // Alt+H opens Help
    require(controller.open_menu().has_value(), "menu did not open");
    // Same rule as NativeEditorController::menu_name_at: 78 px labels, narrowed to fit the bar.
    const int menuItemWidth = std::min(78, 640 / static_cast<int>(kMenuBarNames.size()));
    platform::PlatformEvent move;
    move.type = platform::EventType::PointerMove;
    move.x = (menuItemWidth + 4) * 2 + 1; // physical pixel inside the logical "Edit" label
    move.y = 4 * 2 + 1;
    bridge.handle_event(move);
    require(controller.open_menu() && *controller.open_menu() == "Edit",
            "zoomed pointer did not hit-test the Edit menu label");
    move.x = (menuItemWidth - 1) * 2 + 1; // last physical pixel column of the "File" label
    bridge.handle_event(move);
    require(controller.open_menu() && *controller.open_menu() == "File",
            "zoomed pointer at a label edge hit the wrong menu");
}

} // namespace

int main() {
    try {
        test_snap_and_clamp();
        test_window_cap();
        test_logical_physical_mapping();
        test_hotkey_names();
        test_setting_definition_and_legacy_preferences();
        test_controller_hotkeys_and_cap();
        test_settings_row_apply_writes_user_scope();
        test_persistence_round_trip();
        test_bridge_maps_pointer_to_logical_pixels();
    } catch (const std::exception& exception) {
        std::cerr << "dve_editor_ui_zoom_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "dve_editor_ui_zoom_tests: PASS\n";
    return 0;
}
