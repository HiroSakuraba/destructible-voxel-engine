// Pivot editing for any object (artist worklist ART-028): Pivot to Centre / Bottom Centre / Point
// Under Cursor and Edit Pivot for voxel objects and meshes move the origin while the geometry stays
// put; voxel origins land on voxel corners; one undo step each.
#include "dve/editor_native.hpp"
#include "dve/voxel_metric.hpp"

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
bool near(Float3 a, Float3 b, float tolerance = 1.0e-3F) { return length(subtract(a, b)) <= tolerance; }
std::string text(Float3 v) { return std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z); }

// 1 "Crate": 3 x 3 x 3 cells of 0.5 m at (4, 0, 0). 2 "Box": unit mesh cube scaled 2 at (0, 0, 0).
// 3 "Tag": a child of Box (not attached), at (0, 3, 0).
std::unique_ptr<NativeEditorController> make_controller() {
    EditorDocument document("Pivot");
    EditorObject crate(1, "Crate");
    crate.voxelSizeMeters = 0.5F;
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 3; ++y)
            for (int z = 0; z < 3; ++z) (void)crate.voxels->set_voxel({x, y, z}, 1);
    crate.transform.position = {4.0F, 0.0F, 0.0F};
    document.add_object(std::move(crate));
    EditorObject box(2, "Box");
    box.polygon = make_editor_polygon_cube(2);
    box.voxelScale = {2.0F, 2.0F, 2.0F};
    document.add_object(std::move(box));
    EditorObject tag(3, "Tag");
    tag.voxelSizeMeters = 0.25F;
    (void)tag.voxels->set_voxel({0, 0, 0}, 1);
    tag.parent = 2;
    tag.transform.position = {0.0F, 3.0F, 0.0F};
    document.add_object(std::move(tag));
    auto c = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    c->resize(1600, 900);
    c->camera().position = {2.0F, 4.0F, 10.0F};
    c->camera().target = {2.0F, 1.0F, 0.0F};
    return c;
}

EditorObjectBounds bounds(NativeEditorController& c, EditorObjectId id) { return object_world_bounds(*c.workspace().document().find_object(id)); }
Float3 origin(NativeEditorController& c, EditorObjectId id) { return c.workspace().document().find_object(id)->transform.position; }
bool same_bounds(const EditorObjectBounds& a, const EditorObjectBounds& b) {
    return a.valid && b.valid && near(a.minimum, b.minimum) && near(a.maximum, b.maximum);
}

void test_commands() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.workspace().select_object(2);
    const EditorObjectBounds box = bounds(c, 2);
    const Float3 tagOrigin = origin(c, 3);
    check(c.dispatch_action("edit.pivot_bottom_center"), "mesh: Pivot to Bottom Centre: " + c.status().text);
    check(near(origin(c, 2), {1.0F, 0.0F, 1.0F}), "origin at the bottom centre: " + text(origin(c, 2)));
    check(same_bounds(bounds(c, 2), box), "the mesh did not move");
    check(near(origin(c, 3), tagOrigin), "its child stays put");
    check(c.dispatch_action("edit.undo") && near(origin(c, 2), {0.0F, 0.0F, 0.0F}) && same_bounds(bounds(c, 2), box), "one undo");

    c.workspace().select_object(1);
    const EditorObjectBounds crate = bounds(c, 1);
    check(c.dispatch_action("edit.pivot_center"), "voxels: Pivot to Centre");
    // Centre is 0.75 m in: the nearest voxel corner is 0.5 or 1.0 m.
    const Float3 o = origin(c, 1);
    const auto on_corner = [](float v) { return std::abs(v * 2.0F - std::round(v * 2.0F)) < 1.0e-4F; };
    check(on_corner(o.x) && on_corner(o.y) && on_corner(o.z) && std::abs(o.x - 4.75F) <= 0.26F &&
          std::abs(o.y - 0.75F) <= 0.26F && std::abs(o.z - 0.75F) <= 0.26F, "a voxel corner near the centre: " + text(o));
    check(same_bounds(bounds(c, 1), crate), "the voxels did not move in the world");
    check(c.workspace().document().find_object(1)->voxels->occupied_voxel_count() == 27, "same cells");
    check(!c.dispatch_action("edit.pivot_center"), "already there: no new step");

    // Both at once.
    c.workspace().select_object(1);
    c.workspace().add_to_selection(2);
    check(c.dispatch_action("edit.pivot_bottom_center") && near(origin(c, 2), {1.0F, 0.0F, 1.0F}), "several objects in one step");
}

void test_point_under_cursor() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.workspace().select_object(2);
    const EditorObjectBounds box = bounds(c, 2);
    const ScreenPoint corner = project_world_to_screen(c.camera(), c.layout().viewport, {2.0F, 2.0F, 2.0F});
    c.pointer_move(static_cast<int>(corner.x) + 2, static_cast<int>(corner.y) + 1);
    check(c.dispatch_action("edit.pivot_to_cursor"), "Pivot to Point Under Cursor: " + c.status().text);
    check(near(origin(c, 2), {2.0F, 2.0F, 2.0F}), "the pivot snaps to the object's own vertex: " + text(origin(c, 2)));
    check(same_bounds(bounds(c, 2), box), "the mesh did not move");
}

void test_edit_pivot_mode() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.workspace().select_object(2);
    const EditorObjectBounds box = bounds(c, 2);
    check(c.dispatch_action("edit.edit_pivot") && c.pivot_editing(), "Edit Pivot on a mesh");
    check(c.active_tool() == EditorToolId::Translate, "the Move tool");
    const auto axes = c.gizmo_axes();
    check(!axes.empty() && near({axes[0].start.x, axes[0].start.y, 0.0F},
                                {project_world_to_screen(c.camera(), c.layout().viewport, {0, 0, 0}).x,
                                 project_world_to_screen(c.camera(), c.layout().viewport, {0, 0, 0}).y, 0.0F}, 1.5F),
          "the gizmo sits on the origin");
    if (axes.empty()) return;
    const GizmoScreenAxis x = axes[0];
    const int sx = static_cast<int>(x.start.x + (x.end.x - x.start.x) * 0.8F), sy = static_cast<int>(x.start.y + (x.end.y - x.start.y) * 0.8F);
    c.pointer_move(sx, sy);
    c.pointer_down(PointerButton::Primary, sx, sy);
    c.pointer_move(sx + 8, sy);
    c.key_down("1", false, false, false);  // exactly 1 m along X
    check(near(origin(c, 2), {0.0F, 0.0F, 0.0F}) && same_bounds(bounds(c, 2), box), "nothing moves while dragging");
    check(c.origin_marker() && near(*c.origin_marker(), {1.0F, 0.0F, 0.0F}), "the marker shows the new pivot");
    c.pointer_up(PointerButton::Primary, sx + 8, sy);
    check(near(origin(c, 2), {1.0F, 0.0F, 0.0F}) && same_bounds(bounds(c, 2), box), "release moves only the origin: " + text(origin(c, 2)));
    check(c.dispatch_action("edit.undo") && near(origin(c, 2), {0.0F, 0.0F, 0.0F}), "one undo");

    // Hold V: snap the pivot onto the object's own corner.
    c.pointer_move(sx, sy);
    c.pointer_down(PointerButton::Primary, sx, sy);
    c.key_down("v", false, false, false);
    const ScreenPoint corner = project_world_to_screen(c.camera(), c.layout().viewport, {2.0F, 2.0F, 2.0F});
    c.pointer_move(static_cast<int>(corner.x), static_cast<int>(corner.y));
    c.pointer_up(PointerButton::Primary, static_cast<int>(corner.x), static_cast<int>(corner.y));
    c.key_up("v", false, false, false);
    check(near(origin(c, 2), {2.0F, 2.0F, 2.0F}) && same_bounds(bounds(c, 2), box), "snapped to its own vertex: " + text(origin(c, 2)));

    // Rotate is refused while editing an object's pivot; Esc and selecting something else end it.
    c.set_active_tool(EditorToolId::Rotate);
    const auto ring = c.gizmo_axes();
    if (!ring.empty()) {
        const int rx = static_cast<int>(ring[0].end.x), ry = static_cast<int>(ring[0].end.y);
        c.pointer_move(rx, ry);
        c.pointer_down(PointerButton::Primary, rx, ry);
        check(!c.gizmo_dragging(), "Rotate does not start: " + c.status().text);
        c.pointer_up(PointerButton::Primary, rx, ry);
    }
    c.workspace().select_object(1);
    c.update(0.016F);
    check(!c.pivot_editing(), "selecting another object ends pivot editing");
}

void test_refusals() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.workspace().document().find_object(2)->flags.locked = true;
    c.workspace().select_object(2);
    check(!c.dispatch_action("edit.pivot_center") && c.status().text.find("Unlock") != std::string::npos,
          "locked objects keep their pivot: " + c.status().text);
    check(!c.dispatch_action("edit.edit_pivot"), "and cannot enter Edit Pivot");
    c.workspace().document().find_object(2)->flags.locked = false;
    std::string error;
    check(c.workspace().document().attach_object(3, 2, true, {}, true, true, &error), "attach the tag: " + error);
    check(!c.dispatch_action("edit.pivot_center") && c.status().text.find("socket-attached") != std::string::npos,
          "socket-attached children are refused: " + c.status().text);
}
}  // namespace

int main() {
    test_commands();
    test_point_under_cursor();
    test_edit_pivot_mode();
    test_refusals();
    if (g_failures == 0) std::printf("pivot edit tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
