// Selection basics (artist worklist ART-010..014):
//   - one modifier rule everywhere: Shift adds, Ctrl removes, Ctrl+Shift intersects, and Shift+click
//     on something already selected removes it;
//   - arrow-key pick walking: objects (parent / child / siblings), mesh components (grow, shrink,
//     step to a neighbour) and voxels (grow / shrink);
//   - the Alt+click pick list names each stacked object with its kind and parent path.
#include "dve/editor_native.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

constexpr std::uint32_t kShift = 1U, kCtrl = 2U, kAlt = 4U;

void fill_box(EditorObject& object, Int3 min, Int3 max) {
    object.voxelSizeMeters = 1.0F;
    for (int x = min.x; x <= max.x; ++x)
        for (int y = min.y; y <= max.y; ++y)
            for (int z = min.z; z <= max.z; ++z) (void)object.voxels->set_voxel({x, y, z}, 1);
}

// Root "Rig" with children "Arm" and "Leg" (voxel boxes) and "Hand" under Arm; a second
// root "Crate" (voxel box) sits behind Arm along the camera ray.
std::unique_ptr<NativeEditorController> make_controller() {
    EditorDocument document("Selection basics");
    EditorObject rig(1, "Rig");  // one cell of its own, far away: a plain parent, not a closed group
    fill_box(rig, {-20, 0, 0}, {-20, 0, 0});
    document.add_object(std::move(rig));
    EditorObject arm(2, "Arm");
    fill_box(arm, {0, 0, 0}, {3, 3, 3});
    arm.parent = 1;
    document.add_object(std::move(arm));
    EditorObject leg(3, "Leg");
    fill_box(leg, {0, 0, 0}, {3, 3, 3});
    leg.parent = 1;
    leg.transform.position = {4.0F, 0.0F, 0.0F};
    document.add_object(std::move(leg));
    EditorObject hand(4, "Hand");
    fill_box(hand, {0, 0, 0}, {1, 1, 1});
    hand.parent = 2;
    hand.transform.position = {0.0F, 3.0F, 0.0F};
    document.add_object(std::move(hand));
    EditorObject crate(5, "Crate");
    fill_box(crate, {0, 0, 0}, {3, 3, 3});
    crate.transform.position = {0.0F, 0.0F, -8.0F};
    document.add_object(std::move(crate));
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    controller->resize(1280, 720);
    controller->camera().position = {0.5F, 0.5F, 16.0F};
    controller->camera().target = {0.5F, 0.5F, 0.0F};
    return controller;
}

void click(NativeEditorController& controller, Float3 point, std::uint32_t modifiers = 0) {
    const ScreenPoint p = project_world_to_screen(controller.camera(), controller.layout().viewport, point);
    check(p.visible, "test point visible");
    const int x = static_cast<int>(std::lround(p.x)), y = static_cast<int>(std::lround(p.y));
    controller.pointer_down(PointerButton::Primary, x, y, modifiers);
    controller.pointer_up(PointerButton::Primary, x, y, modifiers);
}

void test_modifier_rule() {
    check(selection_operation_for_modifiers(0) == SelectionOperation::Replace, "no modifier replaces");
    check(selection_operation_for_modifiers(kShift) == SelectionOperation::Add, "Shift adds");
    check(selection_operation_for_modifiers(kCtrl) == SelectionOperation::Subtract, "Ctrl removes");
    check(selection_operation_for_modifiers(kCtrl | kShift) == SelectionOperation::Intersect, "Ctrl+Shift intersects");
    check(selection_operation_for_modifiers(kAlt) == SelectionOperation::Subtract, "Alt drag still removes");
}

void test_object_clicks() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    const Float3 onArm{0.5F, 0.5F, 4.0F};
    const Float3 onLeg{4.5F, 0.5F, 4.0F};
    click(controller, onArm);
    check(controller.workspace().is_selected(2) && controller.workspace().selected_objects().size() == 1,
          "click selects Arm: " + controller.status().text + " n=" + std::to_string(controller.workspace().selected_objects().size()) +
              (controller.workspace().selected_objects().empty() ? "" : " first=" + std::to_string(*controller.workspace().selected_objects().begin())));
    click(controller, onLeg, kShift);
    check(controller.workspace().selected_objects().size() == 2, "Shift+click adds Leg");
    click(controller, onLeg, kShift);
    check(!controller.workspace().is_selected(3) && controller.workspace().is_selected(2),
          "Shift+click on Leg again removes it");
    click(controller, onLeg, kShift);
    click(controller, onArm, kCtrl);
    check(!controller.workspace().is_selected(2) && controller.workspace().is_selected(3), "Ctrl+click removes Arm");
    click(controller, onArm, kCtrl);
    check(!controller.workspace().is_selected(2), "Ctrl+click never adds");
}

void test_object_walk() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.workspace().select_object(2);  // Arm
    const auto cameraBefore = controller.camera();
    check(controller.dispatch_action("select.walk_down") && controller.workspace().is_selected(4), "Down walks to the child Hand");
    check(controller.dispatch_action("select.walk_up") && controller.workspace().is_selected(2), "Up walks back to Arm");
    check(controller.dispatch_action("select.walk_right") && controller.workspace().is_selected(3), "Right walks to sibling Leg");
    check(controller.dispatch_action("select.walk_right") && controller.workspace().is_selected(2), "Right wraps to Arm");
    check(controller.dispatch_action("select.walk_left") && controller.workspace().is_selected(3), "Left wraps back to Leg");
    check(controller.dispatch_action("select.walk_up") && controller.workspace().is_selected(1), "Up from Leg selects Rig");
    check(!controller.dispatch_action("select.walk_up") && controller.workspace().is_selected(1), "Up from a root stays put");
    check(controller.dispatch_action("select.walk_right") && controller.workspace().is_selected(5), "Right among roots reaches Crate");
    check(controller.workspace().selected_objects().size() == 1, "a walk leaves one object selected");
    check(controller.camera().position.x == cameraBefore.position.x && controller.camera().position.z == cameraBefore.position.z,
          "walking never moves the camera");

    controller.workspace().select_object(2);
    controller.key_down("down", false, false, false);
    check(controller.workspace().is_selected(4), "the Down arrow key walks in the viewport");
    controller.workspace().select_object(2);
    controller.key_down("right", false, false, true);
    check(controller.workspace().is_selected(3), "Alt+arrow walks too");
}

void test_voxel_walk() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.workspace().select_object(2);
    (void)controller.dispatch_action("select.domain_voxels");
    controller.select_voxels(2, {Int3{1, 1, 1}}, SelectionOperation::Replace);
    check(controller.dispatch_action("select.walk_up") && controller.voxel_selection().size() > 1, "Up grows the voxel selection");
    const std::size_t grown = controller.voxel_selection().size();
    check(controller.dispatch_action("select.walk_down") && controller.voxel_selection().size() < grown,
          "Down shrinks it: " + std::to_string(grown) + " -> " + std::to_string(controller.voxel_selection().size()));
    check(!controller.dispatch_action("select.walk_left"), "Left/Right do nothing on voxels");
}

void test_polygon_walk() {
    EditorDocument document("Walk mesh");
    EditorObject mesh(1, "Mesh");
    mesh.polygon = make_editor_polygon_cube(1);
    document.add_object(std::move(mesh));
    auto owner = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    NativeEditorController& controller = *owner;
    controller.resize(1600, 900);
    controller.workspace().select_object(1);
    controller.camera().position = {0.5F, 0.5F, 5.0F};
    controller.camera().target = {0.5F, 0.5F, 0.5F};
    check(controller.dispatch_action("polygon.vertex_mode"), "vertex mode");
    (void)controller.dispatch_action("edit.select_all");
    const std::size_t all = controller.polygon_selection().components.size();
    controller.select_polygon_components({*controller.polygon_selection().components.begin()}, SelectionOperation::Replace);
    const std::uint64_t start = *controller.polygon_selection().components.begin();
    check(controller.dispatch_action("select.walk_up"), "Up grows a mesh selection");
    check(controller.polygon_selection().components.size() > 1 && controller.polygon_selection().components.size() <= all,
          "grown by one ring");
    check(controller.dispatch_action("select.walk_down") && controller.polygon_selection().components.size() == 1,
          "Down shrinks it back");
    const bool right = controller.dispatch_action("select.walk_right");
    const bool left = !right && controller.dispatch_action("select.walk_left");
    check(right || left, "Left or Right steps to a neighbour: " + controller.status().text);
    check(controller.polygon_selection().components.size() == 1 &&
              *controller.polygon_selection().components.begin() != start,
          "one different element is selected");
    if (right) check(controller.dispatch_action("select.walk_left") && *controller.polygon_selection().components.begin() == start,
                     "Left walks back");
}

void test_pick_list() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    click(controller, {0.5F, 0.5F, 4.0F}, kAlt);
    const ContextMenuState& menu = controller.context_menu();
    check(menu.open && menu.items.size() >= 2, "Alt+click over stacked objects opens the pick list");
    bool arm = false, crate = false;
    for (const ContextMenuItem& item : menu.items) {
        arm = arm || item.label == "Arm  (voxels, 64)  Rig / Arm";
        crate = crate || item.label == "Crate  (voxels, 64)";
    }
    check(arm, "a child shows its kind and parent path");
    check(crate, "a root shows its kind only");
    if (!menu.items.empty()) check(menu.items.front().label.rfind("Arm", 0) == 0, "nearest first: " + menu.items.front().label);

    // Alt+drag still orbits and opens nothing.
    auto other = make_controller();
    NativeEditorController& orbit = *other;
    const auto before = orbit.camera().position;
    const ScreenPoint p = project_world_to_screen(orbit.camera(), orbit.layout().viewport, {0.5F, 0.5F, 4.0F});
    const int x = static_cast<int>(p.x), y = static_cast<int>(p.y);
    orbit.pointer_down(PointerButton::Primary, x, y, kAlt);
    orbit.pointer_move(x + 60, y + 10);
    orbit.pointer_up(PointerButton::Primary, x + 60, y + 10, kAlt);
    check(!orbit.context_menu().open, "Alt+drag opens no pick list");
    check(orbit.camera().position.x != before.x || orbit.camera().position.z != before.z, "Alt+drag still orbits");
}
}  // namespace

int main() {
    test_modifier_rule();
    test_object_clicks();
    test_object_walk();
    test_voxel_walk();
    test_polygon_walk();
    test_pick_list();
    if (g_failures == 0) std::printf("selection basics tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
