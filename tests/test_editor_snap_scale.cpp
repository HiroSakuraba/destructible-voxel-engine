// Editor snapping summary, transform-tool hints and Scale Voxel Size (artist worklist ART-023,
// ART-027, ART-114). Snapping switches, the Scale tool (resampling) and grid snapping come from
// the editor interaction core (#91); this suite covers what the snap/scale change adds on top:
//   * the status bar always summarizes the move, rotate and scale snap settings on the right and
//     elides the status message before it, at the supported window sizes,
//   * Move/Rotate/Scale with nothing selected say what they need,
//   * Scale Voxel Size x2 / x0.5 (Edit menu and command palette) scales the selected voxel
//     objects uniformly about the pivot by changing their voxel size, keeps the voxel count, is
//     one undo step, skips non-voxel objects, explains itself when unavailable and stays inside
//     the supported voxel-size range,
//   * there is exactly one menu item per snap action and one preference key per setting.
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_ui_zoom.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
bool near(float a, float b, float tolerance = 1.0e-4F) { return std::abs(a - b) <= tolerance; }
bool near3(Float3 a, Float3 b, float tolerance = 1.0e-4F) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}
std::string str(Float3 v) {
    return "(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")";
}

struct DrawnText { int x; int y; std::string value; int width; };
// 7 px per code point, like the chrome-fit suite: a little wider than the real UI font.
class MeasuringCanvas final : public IEditorCanvas {
public:
    void fill(UiRect, EditorColor) const override {}
    void outline(UiRect, EditorColor) const override {}
    void line(int, int, int, int, EditorColor, int) const override {}
    void text(int x, int y, std::string_view value, EditorColor) const override {
        texts.push_back({x, y, std::string(value), text_width(value)});
    }
    [[nodiscard]] int text_width(std::string_view value) const override {
        int count = 0;
        for (const char c : value) if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) ++count;
        return count * 7;
    }
    mutable std::vector<DrawnText> texts;
};

std::unique_ptr<NativeEditorController> make_controller(int width = 1280, int height = 720) {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(make_native_editor_demo_document()));
    controller->resize(width, height);
    return controller;
}

std::vector<EditorObjectId> voxel_objects(NativeEditorController& controller) {
    std::vector<EditorObjectId> ids;
    for (const auto& [id, object] : controller.workspace().document().objects())
        if (object.voxels && object.voxels->brick_count() != 0U && !object.text3d && !object.gaborVolume)
            ids.push_back(id);
    return ids;
}

EditorObjectId add_empty_object(NativeEditorController& controller, Float3 position) {
    EditorDocument& document = controller.workspace().document();
    EditorObject object(document.allocate_object_id(), "Empty marker");  // no occupied voxels
    object.transform.position = position;
    return document.add_object(std::move(object)).id;
}

const MenuAction* menu_action(NativeEditorController& controller, std::string_view id) {
    return std::as_const(controller.workspace().menus()).find(id);
}

// Produces a long, real status message: Scale Voxel Size on two voxel objects plus a skipped
// non-voxel object reports all of that in one line.
void set_long_status(NativeEditorController& controller) {
    const auto ids = voxel_objects(controller);
    if (ids.size() < 2U) return;
    const EditorObjectId empty = add_empty_object(controller, {0.0F, 0.0F, 0.0F});
    controller.workspace().select_object(ids[0]);
    controller.workspace().toggle_selection(ids[1]);
    controller.workspace().toggle_selection(empty);
    (void)controller.scale_selection_voxel_size(2.0F);
}

// ---------------------------------------------------------------------------------------------

std::vector<const DrawnText*> status_bar_texts(const MeasuringCanvas& canvas, const UiRect& bar) {
    std::vector<const DrawnText*> result;
    for (const DrawnText& t : canvas.texts)
        if (t.y == bar.y + bar.height - 6 && t.y - 4 >= bar.y) result.push_back(&t);
    return result;
}

void test_status_bar_summary() {
    for (auto [w, h] : std::vector<std::pair<int, int>>{{1280, 720}, {1536, 960}}) {
        for (float zoom : {1.0F, 1.5F, 2.0F}) {
            const float effective = effective_ui_zoom(zoom, w, h);
            const int lw = ui_zoom_logical_extent(w, effective);
            const int lh = ui_zoom_logical_extent(h, effective);
            const std::string tag = std::to_string(w) + "x" + std::to_string(h) + "@" +
                                    std::to_string(static_cast<int>(zoom * 100)) + "%: ";
            auto owner = make_controller(lw, lh);
            NativeEditorController& controller = *owner;
            const UiRect bar = controller.layout().statusBar;
            for (const bool longMessage : {false, true}) {
                if (longMessage) set_long_status(controller);
                MeasuringCanvas canvas;
                render_native_editor(canvas, controller, lw, lh);
                const auto texts = status_bar_texts(canvas, bar);
                const DrawnText* summary = nullptr;
                for (const DrawnText* t : texts) if (t->value.starts_with("UI ")) summary = t;
                check(summary != nullptr, tag + "status bar shows the snap summary");
                if (!summary) continue;
                check(summary->x + summary->width <= lw, tag + "summary stays inside the window");
                if (zoom == 1.0F) {
                    check(summary->value.find("Move 0.10m") != std::string::npos &&
                              summary->value.find("Rotate 15\u00B0") != std::string::npos &&
                              summary->value.find("Scale 0.10x") != std::string::npos,
                          tag + "summary names all three snap settings, got '" + summary->value + "'");
                }
                for (const DrawnText* t : texts) {
                    if (t == summary) continue;
                    check(t->x + t->width <= summary->x - 12,
                          tag + "status message '" + t->value + "' runs under the snap summary");
                }
            }
        }
    }
    // The summary follows MAIN's preferences: switches, absolute grid alignment and steps.
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    (void)controller.dispatch_action("view.toggle_angle_snap");
    (void)controller.dispatch_action("view.toggle_scale_snap");
    (void)controller.dispatch_action("view.toggle_absolute_grid");
    MeasuringCanvas canvas;
    render_native_editor(canvas, controller, 1536, 960);
    bool found = false;
    for (const DrawnText& t : canvas.texts) {
        if (!t.value.starts_with("UI ")) continue;
        found = true;
        check(t.value.find("Move 0.10m grid") != std::string::npos, "summary shows world-grid move snapping: " + t.value);
        check(t.value.find("Rotate off") != std::string::npos, "summary shows angle snap off: " + t.value);
        check(t.value.find("Scale off") != std::string::npos, "summary shows scale snap off: " + t.value);
    }
    check(found, "summary rendered after toggling");
}

void test_status_message_elided_before_summary() {
    auto owner = make_controller(800, 600);
    NativeEditorController& controller = *owner;
    set_long_status(controller);
    const std::string full = controller.status().text;
    MeasuringCanvas canvas;
    render_native_editor(canvas, controller, 800, 600);
    const auto texts = status_bar_texts(canvas, controller.layout().statusBar);
    const DrawnText* summary = nullptr;
    const DrawnText* message = nullptr;
    for (const DrawnText* t : texts) {
        if (t->value.starts_with("UI ")) summary = t;
        else if (t->x <= 12) message = t;
    }
    check(summary && message, "both the message and the summary are drawn at 800x600");
    if (summary && message) {
        check(message->x + message->width <= summary->x - 12, "message ends before the summary at 800x600");
        check(message->value != full && message->value.size() < full.size(),
              "a long message is elided, got '" + message->value + "'");
        check(full.starts_with(message->value.substr(0, 20)), "the elided message is a prefix of the status");
    }
}

// ---------------------------------------------------------------------------------------------

void test_tools_say_what_they_need() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.workspace().clear_selection();
    for (const char* action : {"transform.translate", "transform.rotate", "transform.scale"}) {
        (void)controller.dispatch_action(action);
        const auto gestures = controller.viewport_tool_gestures();
        check(gestures.size() == 1U && gestures.front() == "Select an object first (Q select tool)",
              std::string(action) + " with nothing selected asks for a selection, got '" +
                  (gestures.empty() ? std::string() : gestures.front()) + "'");
    }
    // With a selection, the tools show MAIN's gestures again (Shift snap toggle, world/local).
    const auto ids = voxel_objects(controller);
    check(!ids.empty(), "demo has voxel objects");
    if (ids.empty()) return;
    controller.workspace().select_object(ids.front());
    (void)controller.dispatch_action("transform.scale");
    auto gestures = controller.viewport_tool_gestures();
    check(!gestures.empty() && gestures.front() == "Drag handle to scale", "Scale keeps its drag hint");
    check(std::find(gestures.begin(), gestures.end(), "Shift toggles snap") != gestures.end(),
          "Scale keeps the Shift snap toggle hint");
    (void)controller.dispatch_action("transform.translate");
    gestures = controller.viewport_tool_gestures();
    check(!gestures.empty() && gestures.front() == "Drag axis to move", "Move keeps its drag hint");
    bool worldLocal = false;
    for (const std::string& g : gestures) worldLocal = worldLocal || g.find("world/local") != std::string::npos;
    check(worldLocal, "Move keeps the world/local hint");
    for (const std::string& g : gestures) check(g.find("Ctrl") == std::string::npos, "no Ctrl snap hint: " + g);
    check(controller.layout().toolbarButtons.size() == kEditorToolCount &&
              std::find(kToolbarTools.begin(), kToolbarTools.end(), EditorToolId::Scale) == kToolbarTools.end(),
          "Scale still has no toolbar button");
}

// ---------------------------------------------------------------------------------------------

void test_scale_voxel_size() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    const auto ids = voxel_objects(controller);
    check(ids.size() >= 2U, "demo has two voxel objects");
    if (ids.size() < 2U) return;
    const EditorObjectId a = ids[0];
    const EditorObjectId b = ids[1];
    const EditorObjectId empty = add_empty_object(controller, {3.0F, 1.0F, -2.0F});
    controller.workspace().select_object(a);
    controller.workspace().toggle_selection(b);
    controller.workspace().toggle_selection(empty);
    check(controller.workspace().selected_objects().size() == 3U, "three objects selected");

    struct Snapshot { Float3 position; Quaternion rotation; float size; std::uint64_t voxels; };
    std::map<EditorObjectId, Snapshot> before;
    for (EditorObjectId id : {a, b, empty}) {
        const EditorObject* object = controller.workspace().document().find_object(id);
        before[id] = {object->transform.position, object->transform.rotation, object->voxelSizeMeters,
                      object->voxels ? object->voxels->occupied_voxel_count() : 0U};
    }
    const Float3 pivot = controller.current_pivot();
    controller.refresh_menu_state();
    const MenuAction* up = menu_action(controller, "transform.scale_voxel_size_up");
    check(up && up->enabled && up->disabledReason.empty(), "Scale Voxel Size x2 is available with voxel objects selected");

    const std::size_t undoDepthBefore = controller.workspace().commands().size();
    check(controller.dispatch_action("transform.scale_voxel_size_up"), "Scale Voxel Size x2 runs");
    check(controller.workspace().commands().size() == undoDepthBefore + 1U, "exactly one undo step");
    check(controller.workspace().commands().undo_label() == "Scale voxel size", "undo label is Scale voxel size");
    check(controller.status().text.find("skipped 1 non-voxel object") != std::string::npos,
          "status notes the skipped non-voxel object: " + controller.status().text);
    for (EditorObjectId id : {a, b}) {
        const EditorObject* object = controller.workspace().document().find_object(id);
        const Snapshot& s = before[id];
        const Float3 expected{pivot.x + (s.position.x - pivot.x) * 2.0F, pivot.y + (s.position.y - pivot.y) * 2.0F,
                              pivot.z + (s.position.z - pivot.z) * 2.0F};
        check(near(object->voxelSizeMeters, s.size * 2.0F), "voxel size doubled");
        check(near3(object->transform.position, expected, 1.0e-4F),
              "position scaled about the pivot: got " + str(object->transform.position) + " want " + str(expected));
        check(object->transform.rotation.x == s.rotation.x && object->transform.rotation.w == s.rotation.w,
              "rotation untouched");
        check(object->voxels->occupied_voxel_count() == s.voxels, "voxel count unchanged (no resampling)");
    }
    const EditorObject* marker = controller.workspace().document().find_object(empty);
    check(near3(marker->transform.position, before[empty].position, 0.0F), "non-voxel object is not moved");
    check(near3(controller.current_pivot(), pivot, 1.0e-3F), "selection bounds stay centred on the pivot");

    check(controller.dispatch_action("edit.undo"), "undo Scale Voxel Size");
    for (EditorObjectId id : {a, b}) {
        const EditorObject* object = controller.workspace().document().find_object(id);
        check(object->voxelSizeMeters == before[id].size, "undo restores the exact voxel size");
        check(object->transform.position.x == before[id].position.x &&
                  object->transform.position.y == before[id].position.y &&
                  object->transform.position.z == before[id].position.z,
              "undo restores the exact position");
    }
    check(controller.dispatch_action("edit.redo"), "redo Scale Voxel Size");
    for (EditorObjectId id : {a, b})
        check(near(controller.workspace().document().find_object(id)->voxelSizeMeters, before[id].size * 2.0F),
              "redo reapplies the voxel size");

    check(controller.dispatch_action("transform.scale_voxel_size_down"), "Scale Voxel Size x0.5 runs");
    for (EditorObjectId id : {a, b}) {
        const EditorObject* object = controller.workspace().document().find_object(id);
        check(near(object->voxelSizeMeters, before[id].size), "x0.5 after x2 returns to the original size");
        check(near3(object->transform.position, before[id].position, 1.0e-4F), "and the original position");
    }
}

void test_scale_voxel_size_disabled_and_clamped() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.workspace().clear_selection();
    controller.refresh_menu_state();
    for (const char* id : {"transform.scale_voxel_size_up", "transform.scale_voxel_size_down"}) {
        const MenuAction* action = menu_action(controller, id);
        check(action && !action->enabled && action->disabledReason == "Select one or more voxel objects first.",
              std::string(id) + " explains itself with nothing selected");
    }
    const std::size_t depth = controller.workspace().commands().size();
    check(!controller.scale_selection_voxel_size(2.0F).success, "nothing selected: no-op");
    check(controller.workspace().commands().size() == depth, "no undo step without a selection");

    const EditorObjectId empty = add_empty_object(controller, {});
    controller.workspace().select_object(empty);
    controller.refresh_menu_state();
    const MenuAction* up = menu_action(controller, "transform.scale_voxel_size_up");
    check(up && !up->enabled && up->disabledReason.find("voxel objects") != std::string::npos,
          "only non-voxel objects selected: disabled with a reason");

    const auto ids = voxel_objects(controller);
    if (ids.empty()) return;
    EditorObject* object = controller.workspace().document().find_object(ids.front());
    controller.workspace().select_object(ids.front());
    object->flags.locked = true;
    controller.refresh_menu_state();
    up = menu_action(controller, "transform.scale_voxel_size_up");
    check(up && !up->enabled && up->disabledReason.find("Unlock") != std::string::npos, "locked object: disabled");
    object->flags.locked = false;

    // Clamping: near the 10 m maximum the factor is limited for the whole selection.
    object->voxelSizeMeters = 8.0F;
    controller.refresh_menu_state();
    check(controller.scale_selection_voxel_size(2.0F).success, "x2 at 8 m is limited, not refused");
    object = controller.workspace().document().find_object(ids.front());
    check(near(object->voxelSizeMeters, kMaxVoxelSizeMeters), "voxel size clamps to 10 m");
    check(controller.status().text.find("limited") != std::string::npos, "status says the factor was limited");
    const std::size_t atMax = controller.workspace().commands().size();
    check(!controller.scale_selection_voxel_size(2.0F).success, "x2 at the maximum is refused");
    check(controller.workspace().commands().size() == atMax, "no undo step at the maximum");
    object->voxelSizeMeters = 0.0015F;
    check(controller.scale_selection_voxel_size(0.5F).success, "x0.5 near the 1 mm minimum is limited");
    check(near(controller.workspace().document().find_object(ids.front())->voxelSizeMeters, kMinVoxelSizeMeters, 1.0e-7F),
          "voxel size clamps to 1 mm");
    check(!controller.scale_selection_voxel_size(0.5F).success, "x0.5 at the minimum is refused");
    check(!controller.scale_selection_voxel_size(-1.0F).success, "a negative factor is refused");
}

// ---------------------------------------------------------------------------------------------

void test_menu_and_palette() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    const auto& actions = controller.workspace().menus().actions();
    std::set<std::string> ids;
    std::map<std::string, int> labels;
    for (const MenuAction& action : actions) {
        check(ids.insert(action.id).second, "duplicate menu action id " + action.id);
        ++labels[action.menu + "/" + action.section + "/" + action.label];
    }
    for (const auto& [label, count] : labels) check(count == 1, "duplicate menu item " + label);
    for (const char* removed : {"view.snap_to_grid"})
        check(menu_action(controller, removed) == nullptr, std::string(removed) + " is not a second grid-snap item");
    int snapItems = 0;
    for (const MenuAction& action : actions)
        if (action.menu == "View" && action.section == "Snapping") ++snapItems;
    check(snapItems == 4, "View > Snapping has exactly four switches, got " + std::to_string(snapItems));
    for (const char* id : {"transform.scale_voxel_size_up", "transform.scale_voxel_size_down"}) {
        const MenuAction* action = menu_action(controller, id);
        check(action && action->menu == "Edit" && action->visibility == MenuVisibility::Primary,
              std::string(id) + " is a visible Edit menu item");
        check(action && action->shortcut.empty(), std::string(id) + " has no default shortcut");
    }
    const auto results = controller.workspace().menus().search("scale voxel size", 12);
    std::set<std::string> found;
    for (const MenuAction& action : results) found.insert(action.id);
    check(found.count("transform.scale_voxel_size_up") && found.count("transform.scale_voxel_size_down"),
          "the command palette finds Scale Voxel Size");
    const auto keepVoxels = controller.workspace().menus().search("keep voxels", 12);
    check(!keepVoxels.empty(), "palette keyword 'keep voxels' matches");
}

void test_preference_keys() {
    EditorPreferences preferences;
    const std::string text = preferences.serialize();
    std::istringstream lines(text);
    std::set<std::string> keys;
    std::string line;
    while (std::getline(lines, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        check(keys.insert(line.substr(0, eq)).second, "duplicate preference key " + line.substr(0, eq));
    }
    check(keys.count("absoluteGridSnap") == 1U && keys.count("translateSnapToGrid") == 0U,
          "one grid-snap preference (absoluteGridSnap)");
    for (const char* key : {"translateSnapEnabled", "rotateSnapEnabled", "scaleSnapEnabled", "scaleSnapStep"})
        check(keys.count(key) == 1U, std::string("preference key ") + key);
    const auto parsed = EditorPreferences::parse(text);
    check(parsed.has_value(), "default preferences parse back");
}

} // namespace

int main() {
    test_status_bar_summary();
    test_status_message_elided_before_summary();
    test_tools_say_what_they_need();
    test_scale_voxel_size();
    test_scale_voxel_size_disabled_and_clamped();
    test_menu_and_palette();
    test_preference_keys();
    if (g_failures != 0) {
        std::printf("dve_editor_snap_scale_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("dve_editor_snap_scale_tests: PASS\n");
    return 0;
}
