// Selection masks, front-only object boxes and domain-wide grow / shrink / connected / invert
// (artist worklist ART-015, ART-016, ART-017).
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
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

void fill_box(EditorObject& object, Int3 min, Int3 max) {
    object.voxelSizeMeters = 1.0F;
    for (int x = min.x; x <= max.x; ++x)
        for (int y = min.y; y <= max.y; ++y)
            for (int z = min.z; z <= max.z; ++z) (void)object.voxels->set_voxel({x, y, z}, 1);
}

// Camera on +Z looking down -Z. "Wall" (x 0..6, y 0..4, z 0..1) hides "Back" (x 1..3, z -6..-4).
// "Side" sits to the right in the open. "Lamp" is a helper (no geometry, no children).
// "Rig" is the parent of "Arm", which is the parent of "Hand".
std::unique_ptr<NativeEditorController> make_controller() {
    EditorDocument document("Selection filters");
    EditorObject wall(1, "Wall");
    fill_box(wall, {0, 0, 0}, {6, 4, 1});
    document.add_object(std::move(wall));
    EditorObject back(2, "Back");
    fill_box(back, {0, 0, 0}, {2, 2, 2});
    back.transform.position = {1.0F, 0.0F, -6.0F};
    document.add_object(std::move(back));
    EditorObject side(3, "Side");
    fill_box(side, {0, 0, 0}, {1, 1, 1});
    side.transform.position = {9.0F, 0.0F, 0.0F};
    document.add_object(std::move(side));
    EditorObject lamp(4, "Lamp");
    lamp.transform.position = {9.0F, 4.0F, 0.0F};
    document.add_object(std::move(lamp));
    EditorObject rig(5, "Rig");
    fill_box(rig, {0, 0, 0}, {0, 0, 0});
    rig.transform.position = {-20.0F, 0.0F, 0.0F};
    document.add_object(std::move(rig));
    EditorObject arm(6, "Arm");
    fill_box(arm, {0, 0, 0}, {0, 0, 0});
    arm.parent = 5;
    arm.transform.position = {-22.0F, 0.0F, 0.0F};
    document.add_object(std::move(arm));
    EditorObject hand(7, "Hand");
    fill_box(hand, {0, 0, 0}, {0, 0, 0});
    hand.parent = 6;
    hand.transform.position = {-24.0F, 0.0F, 0.0F};
    document.add_object(std::move(hand));
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    controller->resize(1280, 720);
    controller->camera().position = {4.0F, 2.0F, 22.0F};
    controller->camera().target = {4.0F, 2.0F, 0.0F};
    return controller;
}

std::pair<int, int> screen(NativeEditorController& c, Float3 point) {
    const ScreenPoint p = project_world_to_screen(c.camera(), c.layout().viewport, point);
    check(p.visible, "test point visible");
    return {static_cast<int>(std::lround(p.x)), static_cast<int>(std::lround(p.y))};
}

void click(NativeEditorController& c, Float3 point, std::uint32_t modifiers = 0) {
    const auto [x, y] = screen(c, point);
    c.pointer_down(PointerButton::Primary, x, y, modifiers);
    c.pointer_up(PointerButton::Primary, x, y, modifiers);
}

// Box from world point a to world point b (screen order decides window vs crossing).
void box(NativeEditorController& c, Float3 a, Float3 b, std::string* label = nullptr) {
    const auto [ax, ay] = screen(c, a);
    const auto [bx, by] = screen(c, b);
    c.pointer_down(PointerButton::Primary, ax, ay);
    c.pointer_move(bx, by);
    if (label) *label = c.marquee_label();
    c.pointer_up(PointerButton::Primary, bx, by);
}

bool selected_exactly(const NativeEditorController& c, std::set<EditorObjectId> ids) {
    return c.workspace().selected_objects() == ids;
}

std::string names(const NativeEditorController& c) {
    std::string out;
    for (const EditorObjectId id : c.workspace().selected_objects()) out += std::to_string(id) + " ";
    return out;
}

void test_front_only_boxes() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    // A crossing box (right to left) over the middle of the wall: Back is behind it.
    std::string label;
    box(c, {3.5F, 2.5F, 2.0F}, {1.5F, 0.5F, 2.0F}, &label);
    check(selected_exactly(c, {1}), "crossing box takes only the visible Wall: " + names(c));
    check(label == "Crossing \xC2\xB7 visible only", "label names the rule: " + label);
    // A window box (left to right) around everything: Back is inside but hidden.
    box(c, {-1.0F, 6.0F, 2.0F}, {12.0F, -2.0F, 2.0F}, &label);
    check(c.workspace().is_selected(1) && c.workspace().is_selected(3) && !c.workspace().is_selected(2),
          "window box keeps visible objects only: " + names(c));
    check(label.rfind("Window", 0) == 0, "window label");
    c.viewport_settings().xraySelection = true;
    box(c, {3.5F, 2.5F, 2.0F}, {1.5F, 0.5F, 2.0F}, &label);
    check(selected_exactly(c, {1, 2}), "with X-Ray the crossing box selects through: " + names(c));
    check(label == "Crossing \xC2\xB7 X-Ray (through)", "X-Ray label: " + label);
}

void test_masks() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.workspace().document().find_object(1)->flags.locked = true;
    const MenuAction* lockedItem = std::as_const(c.workspace().menus()).find("select.mask_locked");
    check(lockedItem && lockedItem->checkable && lockedItem->checked, "Selectable: Locked Objects starts checked");
    click(c, {2.0F, 1.5F, 2.0F});
    check(selected_exactly(c, {1}), "locked objects are clickable by default");
    check(c.dispatch_action("select.mask_locked") && !c.selection_mask().locked, "mask out locked objects");
    lockedItem = std::as_const(c.workspace().menus()).find("select.mask_locked");
    check(lockedItem && !lockedItem->checked, "the menu unchecks it");
    click(c, {3.0F, 2.5F, 2.0F});  // a different spot: not a double-click
    check(selected_exactly(c, {2}), "a click on the locked wall reaches Back behind it: " + names(c));
    std::string label;
    c.viewport_settings().xraySelection = true;
    box(c, {3.5F, 2.5F, 2.0F}, {1.5F, 0.5F, 2.0F}, &label);
    check(selected_exactly(c, {2}), "boxes skip the locked wall too: " + names(c));
    check(label.find("skipping locked") != std::string::npos, "the box label says what it skips: " + label);
    c.viewport_settings().xraySelection = false;
    box(c, {3.5F, 2.5F, 2.0F}, {1.5F, 0.5F, 2.0F});
    check(c.workspace().selected_objects().empty(), "front-only: the masked wall still hides Back");

    // Helpers and prefabs, through Invert.
    c.workspace().document().find_object(1)->flags.locked = false;
    (void)c.dispatch_action("select.mask_all");
    check(c.selection_mask().everything(), "Make Everything Selectable");
    c.workspace().select_object(1);
    check(c.dispatch_action("edit.select_invert") && !c.workspace().is_selected(1) && c.workspace().is_selected(4),
          "invert includes the helper Lamp: " + names(c));
    (void)c.dispatch_action("select.mask_helpers");
    c.workspace().select_object(1);
    (void)c.dispatch_action("edit.select_invert");
    check(!c.workspace().is_selected(4) && c.workspace().is_selected(2), "with helpers masked, invert skips Lamp: " + names(c));
    EditorPrefabLink link;
    link.instanceId = 1;
    c.workspace().document().find_object(3)->prefabLink = link;
    (void)c.dispatch_action("select.mask_prefabs");
    c.workspace().select_object(1);
    (void)c.dispatch_action("edit.select_invert");
    check(!c.workspace().is_selected(3), "with prefabs masked, invert skips the instance: " + names(c));
    check(c.status().text.find("Inverted") != std::string::npos, "status: " + c.status().text);
}

void test_object_grow_shrink_connected() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.workspace().select_object(5);
    check(c.dispatch_action("edit.select_grow") && selected_exactly(c, {5, 6}), "grow adds the children");
    check(c.dispatch_action("edit.select_grow") && selected_exactly(c, {5, 6, 7}), "grow again adds grandchildren");
    check(!c.dispatch_action("edit.select_grow"), "nothing more to grow into");
    check(c.dispatch_action("edit.select_shrink") && selected_exactly(c, {5}), "shrink drops children of selected parents");
    c.workspace().select_object(6);
    check(c.dispatch_action("edit.select_connected") && selected_exactly(c, {5, 6, 7}), "connected takes the whole hierarchy");
    c.workspace().document().find_object(7)->flags.visible = false;
    c.workspace().select_object(5);
    (void)c.dispatch_action("edit.select_connected");
    check(selected_exactly(c, {5, 6}), "connected skips hidden objects: " + names(c));
}

void test_voxel_domain() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.workspace().select_object(1);
    (void)c.dispatch_action("select.domain_voxels");
    c.select_voxels(1, {Int3{3, 2, 1}}, SelectionOperation::Replace);
    check(c.dispatch_action("edit.select_grow") && c.voxel_selection().size() > 1, "Edit > Grow works on voxels");
    check(c.dispatch_action("edit.select_shrink"), "Edit > Shrink works on voxels");
    c.select_voxels(1, {Int3{3, 2, 1}}, SelectionOperation::Replace);
    check(c.dispatch_action("edit.select_connected") && c.voxel_selection().size() == 7U * 5U * 2U,
          "connected takes the whole piece: " + std::to_string(c.voxel_selection().size()));
    check(c.dispatch_action("edit.select_invert") && c.voxel_selection().empty(), "invert of everything is nothing");
}

void test_polygon_domain_and_keys() {
    EditorDocument document("Mesh");
    EditorObject mesh(1, "Mesh");
    mesh.polygon = make_editor_polygon_cube(1);
    document.add_object(std::move(mesh));
    auto owner = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    NativeEditorController& c = *owner;
    c.resize(1600, 900);
    c.workspace().select_object(1);
    c.camera().position = {0.5F, 0.5F, 5.0F};
    c.camera().target = {0.5F, 0.5F, 0.5F};
    (void)c.dispatch_action("polygon.face_mode");
    c.select_polygon_components({0}, SelectionOperation::Replace);
    check(c.dispatch_action("edit.select_connected") && c.polygon_selection().components.size() == 12, "connected = the shell");
    c.select_polygon_components({0}, SelectionOperation::Replace);
    c.key_down("i", true, true, false);
    check(c.polygon_selection().components.size() == 11, "Ctrl+Shift+I inverts mesh components");
    c.select_polygon_components({0}, SelectionOperation::Replace);
    c.key_down(".", false, false, false);
    check(c.polygon_selection().components.size() > 1, "'.' grows");
    c.key_down(",", false, false, false);
    check(c.polygon_selection().components.size() < 12, "',' shrinks");
}

void test_selection_operation_modes() {
    auto owner = make_controller();
    auto& c = *owner;
    c.viewport_settings().xraySelection = true;
    const Float3 a{3.5F, 2.5F, 2.0F}, b{1.5F, 0.5F, 2.0F};
    box(c, a, b);
    check(selected_exactly(c, {1, 2}), "baseline crossing box selects wall and back");
    check(c.dispatch_action("select.operation_toggle"), "enable box toggle");
    check(c.selection_operation() == SelectionOperation::Toggle, "operation mode stored");
    const MenuAction* item = std::as_const(c.workspace().menus()).find("select.operation_toggle");
    check(item && item->checked, "operation mode checked in Edit menu");
    std::string label;
    box(c, a, b, &label);
    check(label.find("Toggle") != std::string::npos, "marquee shows selected set operation");
    check(selected_exactly(c, {}), "toggle box removes both hits");
    box(c, a, b);
    check(selected_exactly(c, {1, 2}), "toggle box restores both hits");
    check(c.dispatch_action("select.operation_intersect"), "enable intersection mode");
    box(c, {11.0F, 2.0F, 2.0F}, {8.0F, -1.0F, 2.0F});
    check(selected_exactly(c, {}), "intersect with disjoint box clears prior selection");
    check(c.dispatch_action("select.operation_add"), "enable explicit add");
    c.workspace().select_object(1);
    click(c, {2.0F, 1.5F, 2.0F});
    check(selected_exactly(c, {1}), "explicit Add must not turn into Shift-click removal");
    check(c.dispatch_action("select.operation_replace"), "restore replace mode");

    // Hierarchy Ctrl+Shift must intersect, not subtract. This was different from
    // Ctrl+Shift in viewport, polygon, and voxel selection before the closeout.
    c.workspace().apply_selection({1, 2}, SelectionOperation::Replace);
    const auto order = c.hierarchy_order();
    const auto pos = std::find(order.begin(), order.end(), EditorObjectId{2});
    check(pos != order.end(), "Back object visible in hierarchy");
    if (pos != order.end()) {
        const UiRect row = c.layout().hierarchyRows[static_cast<std::size_t>(pos - order.begin())];
        const int x = row.x + 32, y = row.y + row.height / 2;
        c.pointer_down(PointerButton::Primary, x, y, 3U);
        c.pointer_up(PointerButton::Primary, x, y, 3U);
        check(selected_exactly(c, {2}), "Ctrl+Shift hierarchy row intersects, not subtracts: " + names(c));
        c.pointer_down(PointerButton::Primary, x, y, 1U);
        c.pointer_up(PointerButton::Primary, x, y, 1U);
        check(selected_exactly(c, {}), "Shift hierarchy row toggles a selected object off");
        c.pointer_down(PointerButton::Primary, x, y, 1U);
        c.pointer_up(PointerButton::Primary, x, y, 1U);
        check(selected_exactly(c, {2}), "Shift hierarchy row toggles an object on");
        check(c.dispatch_action("select.operation_add"), "enable explicit add for hierarchy");
        c.pointer_down(PointerButton::Primary, x, y, 0U);
        c.pointer_up(PointerButton::Primary, x, y, 0U);
        check(selected_exactly(c, {2}), "persistent Add plain hierarchy click keeps a selected row: " + names(c));
        c.pointer_down(PointerButton::Primary, x, y, 1U);
        c.pointer_up(PointerButton::Primary, x, y, 1U);
        check(selected_exactly(c, {}), "Shift hierarchy row in Add mode still toggles off");
        check(c.dispatch_action("select.operation_replace"), "restore replace mode after hierarchy add");
    }
}

void test_projected_triangle_crossing() {
    EditorObject triangle(100, "Diagonal triangle");
    triangle.polygon = make_editor_polygon_plane(100);
    auto& mesh = *triangle.polygon;
    mesh.vertices[0].position = {0.0F, 0.0F, 0.0F};
    mesh.vertices[1].position = {4.0F, 0.0F, 0.0F};
    mesh.vertices[2].position = {0.0F, 4.0F, 0.0F};
    mesh.indices = {0, 1, 2};
    mesh.bounds.minimum = {0.0F, 0.0F, 0.0F};
    mesh.bounds.maximum = {4.0F, 4.0F, 0.0F};
    EditorCamera camera;
    camera.position = {2.0F, 2.0F, 12.0F};
    camera.target = {2.0F, 2.0F, 0.0F};
    camera.projection = EditorProjection::Orthographic;
    camera.orthographicHeight = 8.0F;
    const UiRect viewport{0, 0, 800, 800};
    const auto rect_at = [&](Float3 world) {
        const ScreenPoint p = project_world_to_screen(camera, viewport, world);
        check(p.visible, "triangle crossing test point projects visibly");
        return UiRect{static_cast<int>(p.x) - 4, static_cast<int>(p.y) - 4, 9, 9};
    };
    check(!object_matches_screen_rect(triangle, camera, viewport, rect_at({3.25F, 3.25F, 0.0F}), false),
          "crossing box must not hit empty corner of projected triangle AABB");
    check(object_matches_screen_rect(triangle, camera, viewport, rect_at({0.5F, 0.5F, 0.0F}), false),
          "crossing box intersects real projected triangle");
    check(!object_matches_screen_rect(triangle, camera, viewport, rect_at({0.5F, 0.5F, 0.0F}), true),
          "small containment window must not select whole triangle");
    check(object_matches_screen_rect(triangle, camera, viewport, viewport, true),
          "whole viewport contains projected triangle");
}
}  // namespace

int main() {
    test_front_only_boxes();
    test_selection_operation_modes();
    test_projected_triangle_crossing();
    test_masks();
    test_object_grow_shrink_connected();
    test_voxel_domain();
    test_polygon_domain_and_keys();
    if (g_failures == 0) std::printf("selection filter tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
