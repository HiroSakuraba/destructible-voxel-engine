// Transform snapping and the scale gizmo (artist worklist ART-023, ART-024, ART-027, ART-114):
//   * move, rotate and scale snapping each have their own switch and step, saved in preferences,
//   * holding Ctrl inverts snapping for the drag,
//   * move snapping can quantize the drag (relative) or land on the world grid,
//   * the Scale tool scales voxel objects uniformly about the selection pivot by changing the
//     voxel size, as one undoable command, and Esc cancels the drag,
//   * transform tools with nothing selected say what they need.
#include "dve/editor_native.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
bool near(float a, float b, float tolerance = 1.0e-4F) { return std::abs(a - b) <= tolerance; }
bool on_step(float value, float step) { return near(value / step, std::round(value / step), 1.0e-3F); }

std::unique_ptr<NativeEditorController> make_controller() {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(make_native_editor_demo_document()));
    controller->resize(1280, 720);
    return controller;
}

EditorObjectId first_voxel_object(NativeEditorController& controller) {
    for (const auto& [id, object] : controller.workspace().document().objects())
        if (object.voxels && !object.text3d && !object.gaborVolume) return id;
    return 0;
}

// Drags gizmo axis `axis` (1..3) by `pixels` along its screen direction.
bool drag_axis(NativeEditorController& controller, int axis, float pixels, std::uint32_t modifiers = 0,
               bool release = true) {
    const auto axes = controller.gizmo_axes();
    if (axes.size() < static_cast<std::size_t>(axis)) return false;
    const GizmoScreenAxis& a = axes[static_cast<std::size_t>(axis - 1)];
    if (!a.start.visible || !a.end.visible) return false;
    const float dx = a.end.x - a.start.x;
    const float dy = a.end.y - a.start.y;
    const float length = std::max(1.0F, std::hypot(dx, dy));
    const int x0 = static_cast<int>(std::lround(a.start.x + dx * 0.7F));
    const int y0 = static_cast<int>(std::lround(a.start.y + dy * 0.7F));
    const int x1 = static_cast<int>(std::lround(static_cast<float>(x0) + dx / length * pixels));
    const int y1 = static_cast<int>(std::lround(static_cast<float>(y0) + dy / length * pixels));
    controller.pointer_down(PointerButton::Primary, x0, y0, modifiers);
    controller.pointer_move(x1, y1, modifiers);
    if (release) controller.pointer_up(PointerButton::Primary, x1, y1, modifiers);
    return true;
}

void test_preferences_round_trip() {
    EditorPreferences preferences;
    check(preferences.translateSnapEnabled && preferences.rotateSnapEnabled && preferences.scaleSnapEnabled,
          "snapping is on by default");
    check(!preferences.translateSnapToGrid, "relative move snapping by default");
    preferences.translateSnapEnabled = false;
    preferences.scaleSnapEnabled = false;
    preferences.translateSnapToGrid = true;
    preferences.scaleSnapStep = 0.25F;
    const auto parsed = EditorPreferences::parse(preferences.serialize());
    check(parsed.has_value(), "preferences parse back");
    if (parsed) {
        check(!parsed->translateSnapEnabled && parsed->rotateSnapEnabled && !parsed->scaleSnapEnabled,
              "snap switches survive a save");
        check(parsed->translateSnapToGrid && near(parsed->scaleSnapStep, 0.25F), "grid mode and scale step survive");
    }
    preferences.scaleSnapStep = 0.0F;
    check(!preferences.validate(), "a zero scale step is rejected");
    const auto old = EditorPreferences::parse("DVE_EDITOR_PREFERENCES=1\ntranslateSnapMeters=0.5\n");
    check(old && old->translateSnapEnabled && near(old->scaleSnapStep, 0.10F),
          "files written before the switches load with snapping on");
}

void test_toggles_and_menu_state() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    for (const char* action : {"view.toggle_move_snap", "view.toggle_angle_snap", "view.toggle_scale_snap",
                               "view.snap_to_grid"}) {
        const EditorPreferences before = controller.workspace().preferences();
        check(controller.dispatch_action(action), std::string(action) + " dispatches");
        const EditorPreferences& after = controller.workspace().preferences();
        check(before.translateSnapEnabled != after.translateSnapEnabled ||
                  before.rotateSnapEnabled != after.rotateSnapEnabled ||
                  before.scaleSnapEnabled != after.scaleSnapEnabled ||
                  before.translateSnapToGrid != after.translateSnapToGrid,
              std::string(action) + " flips exactly its switch");
        const MenuAction* item = std::as_const(controller.workspace().menus()).find(action);
        check(item && item->checkable, std::string(action) + " is a checkable menu item");
    }
    const MenuAction* move = std::as_const(controller.workspace().menus()).find("view.toggle_move_snap");
    check(move && !move->checked, "menu shows move snap off");
    (void)controller.dispatch_action("view.increase_scale_snap");
    check(near(controller.workspace().preferences().scaleSnapStep, 0.25F), "scale step goes 0.10 -> 0.25");
}

void test_move_snapping() {
    for (int mode = 0; mode < 4; ++mode) {
        // 0 relative, 1 world grid, 2 snapping off, 3 relative but Ctrl held (inverts to off).
        auto owner = make_controller();
        NativeEditorController& controller = *owner;
        const EditorObjectId id = first_voxel_object(controller);
        check(id != 0, "demo has a voxel object");
        EditorObject* object = controller.workspace().document().find_object(id);
        object->transform.position = {0.03F, 0.0F, 0.0F};
        controller.workspace().select_object(id);
        (void)controller.dispatch_action("transform.translate");
        if (mode == 1) (void)controller.dispatch_action("view.snap_to_grid");
        if (mode == 2) (void)controller.dispatch_action("view.toggle_move_snap");
        check(drag_axis(controller, 1, 37.0F, mode == 3 ? 2U : 0U), "drag the X axis");
        const float x = controller.workspace().document().find_object(id)->transform.position.x;
        const float delta = x - 0.03F;
        const std::string tag = "move mode " + std::to_string(mode) + ": x=" + std::to_string(x) + " ";
        check(!near(delta, 0.0F), tag + "object moved");
        if (mode == 0) check(on_step(delta, 0.1F) && !on_step(x, 0.1F), tag + "relative: delta on the step, stays off-grid");
        if (mode == 1) check(on_step(x, 0.1F), tag + "grid: lands on the world grid");
        if (mode >= 2) check(!on_step(delta, 0.1F), tag + "unsnapped: free delta");
    }
}

void test_rotate_snapping() {
    for (const bool snapOn : {true, false}) {
        auto owner = make_controller();
        NativeEditorController& controller = *owner;
        const EditorObjectId id = first_voxel_object(controller);
        controller.workspace().select_object(id);
        (void)controller.dispatch_action("transform.rotate");
        if (!snapOn) (void)controller.dispatch_action("view.toggle_angle_snap");
        check(drag_axis(controller, 2, 23.0F), "drag the Y ring");
        const Quaternion q = controller.workspace().document().find_object(id)->transform.rotation;
        const float degrees = 2.0F * std::acos(std::clamp(std::abs(q.w), 0.0F, 1.0F)) * 57.29578F;
        check(degrees > 0.5F, "object rotated");
        check(on_step(degrees, 15.0F) == snapOn,
              std::string(snapOn ? "snapped" : "free") + " rotation, got " + std::to_string(degrees) + " deg");
    }
}

void test_scale_tool() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    (void)controller.dispatch_action("transform.scale");
    check(controller.active_tool() == EditorToolId::Scale, "transform.scale selects the Scale tool");
    const EditorObjectId id = first_voxel_object(controller);
    EditorObject* object = controller.workspace().document().find_object(id);
    object->transform.position = {2.0F, 0.0F, 0.0F};
    controller.workspace().select_object(id);
    const float size0 = object->voxelSizeMeters;
    const Float3 position0 = object->transform.position;
    const std::uint64_t voxels0 = object->voxels->occupied_voxel_count();
    const Float3 pivot = controller.selection_pivot();

    // Esc mid-drag restores everything.
    check(drag_axis(controller, 1, 60.0F, 0, false), "start a scale drag");
    check(!near(controller.workspace().document().find_object(id)->voxelSizeMeters, size0), "drag previews a new size");
    controller.key_down("escape", false, false, false);
    object = controller.workspace().document().find_object(id);
    check(near(object->voxelSizeMeters, size0) && near(object->transform.position.x, position0.x),
          "Esc cancels the scale drag");
    if (controller.gizmo_axes().empty()) return;
    controller.pointer_up(PointerButton::Primary, 0, 0);

    check(drag_axis(controller, 1, 60.0F), "scale drag");
    object = controller.workspace().document().find_object(id);
    const float factor = object->voxelSizeMeters / size0;
    check(factor > 1.05F, "dragging outward grows the object, factor " + std::to_string(factor));
    check(on_step(factor, 0.1F), "scale factor snaps to 0.1 steps, got " + std::to_string(factor));
    check(object->voxels->occupied_voxel_count() == voxels0, "scaling keeps the voxel count (no resampling)");
    const Float3 expected{pivot.x + (position0.x - pivot.x) * factor, pivot.y + (position0.y - pivot.y) * factor,
                          pivot.z + (position0.z - pivot.z) * factor};
    check(near(object->transform.position.x, expected.x, 1.0e-3F) && near(object->transform.position.y, expected.y, 1.0e-3F) &&
              near(object->transform.position.z, expected.z, 1.0e-3F),
          "scaling is about the selection pivot");
    const float scaledSize = object->voxelSizeMeters;
    check(controller.dispatch_action("edit.undo"), "undo the scale");
    object = controller.workspace().document().find_object(id);
    check(near(object->voxelSizeMeters, size0) && near(object->transform.position.x, position0.x),
          "one undo restores size and position");
    check(controller.dispatch_action("edit.redo"), "redo the scale");
    object = controller.workspace().document().find_object(id);
    check(near(object->voxelSizeMeters, scaledSize), "redo reapplies the scale");

    // Ctrl inverts scale snapping.
    check(drag_axis(controller, 1, 23.0F, 2U), "unsnapped scale drag");
    const float freeFactor = controller.workspace().document().find_object(id)->voxelSizeMeters / scaledSize;
    check(!on_step(freeFactor, 0.1F), "Ctrl scales freely, got " + std::to_string(freeFactor));
}

void test_tools_say_what_they_need() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.workspace().clear_selection();
    for (const char* action : {"transform.translate", "transform.rotate", "transform.scale"}) {
        (void)controller.dispatch_action(action);
        const auto gestures = controller.viewport_tool_gestures();
        check(!gestures.empty() && gestures.front().starts_with("Select an object first"),
              std::string(action) + " with nothing selected asks for a selection");
    }
}

} // namespace

int main() {
    test_preferences_round_trip();
    test_toggles_and_menu_state();
    test_move_snapping();
    test_rotate_snapping();
    test_scale_tool();
    test_tools_say_what_they_need();
    if (g_failures != 0) {
        std::printf("dve_editor_snap_scale_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("dve_editor_snap_scale_tests: PASS\n");
    return 0;
}
