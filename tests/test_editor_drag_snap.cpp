// Point snapping (artist worklist ART-025, ART-026): hold V while dragging the Move gizmo to snap
// the pivot to mesh vertices, edge midpoints, face centres, voxel corners or a surface; the
// Snap to Mesh Vertex / Edge / Face placement modes; source and target are drawn.
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/voxel_metric.hpp"

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
bool near(Float3 a, Float3 b, float tolerance = 0.002F) { return length(subtract(a, b)) <= tolerance; }
std::string text(Float3 v) { return std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z); }

// "Box": a unit mesh cube at x 0..1 (scaled 2x, so 0..2). "Wall": voxels 4..8 x 0..2 x 0..1 m.
// "Mover": a small voxel object, selected, out to the left.
std::unique_ptr<NativeEditorController> make_controller() {
    EditorDocument document("Snap");
    EditorObject box(1, "Box");
    box.polygon = make_editor_polygon_cube(1);
    box.voxelScale = {2.0F, 2.0F, 2.0F};
    document.add_object(std::move(box));
    EditorObject wall(2, "Wall");
    wall.voxelSizeMeters = 1.0F;
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 2; ++y) (void)wall.voxels->set_voxel({x, y, 0}, 1);
    wall.transform.position = {4.0F, 0.0F, 0.0F};
    document.add_object(std::move(wall));
    EditorObject mover(3, "Mover");
    mover.voxelSizeMeters = 0.5F;
    (void)mover.voxels->set_voxel({0, 0, 0}, 1);
    mover.transform.position = {-4.0F, 0.0F, 0.0F};
    document.add_object(std::move(mover));
    auto c = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    c->resize(1600, 900);
    c->camera().position = {2.0F, 4.0F, 12.0F};
    c->camera().target = {1.0F, 1.0F, 0.0F};
    c->workspace().select_object(3);
    c->set_active_tool(EditorToolId::Translate);
    return c;
}

std::pair<int, int> screen(NativeEditorController& c, Float3 p) {
    const ScreenPoint s = project_world_to_screen(c.camera(), c.layout().viewport, p);
    check(s.visible, "visible: " + text(p));
    return {static_cast<int>(std::lround(s.x)), static_cast<int>(std::lround(s.y))};
}

// Grab the X handle of the Move gizmo.
bool grab(NativeEditorController& c) {
    for (const GizmoScreenAxis& axis : c.gizmo_axes())
        if (axis.axis == 1) {
            const int x = static_cast<int>(axis.start.x + (axis.end.x - axis.start.x) * 0.7F);
            const int y = static_cast<int>(axis.start.y + (axis.end.y - axis.start.y) * 0.7F);
            c.pointer_move(x, y);
            c.pointer_down(PointerButton::Primary, x, y);
            return c.gizmo_dragging();
        }
    return false;
}

Float3 pivot_offset(NativeEditorController& c) {
    // The object's position relative to the pivot the drag started from.
    return subtract(c.workspace().document().find_object(3)->transform.position, c.drag_snap_source());
}

void test_snap_drag() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    const Float3 start = c.workspace().document().find_object(3)->transform.position;
    check(grab(c), "grab the X handle");
    const Float3 offset = pivot_offset(c);
    c.key_down("v", false, false, false);
    check(c.drag_snap_held(), "V held");
    // A cube corner: (1,1,1) scaled by 2.
    const Float3 corner{2.0F, 2.0F, 2.0F};
    auto [x, y] = screen(c, corner);
    c.pointer_move(x + 3, y + 2);  // close, not exact
    const auto snapped = c.drag_snap_target();
    check(snapped && snapped->kind == SnapTargetKind::Vertex && near(snapped->point, corner),
          "snaps to the vertex: " + (snapped ? std::string(snap_target_name(snapped->kind)) + " " + text(snapped->point) : "none"));
    check(near(c.workspace().document().find_object(3)->transform.position, add(corner, offset)), "the pivot sits on the vertex");
    // Draws the target, its label and the line from the source.
    struct Canvas final : IEditorCanvas {
        void fill(UiRect, EditorColor) const override {}
        void outline(UiRect, EditorColor) const override {}
        void line(int, int, int, int, EditorColor, int) const override {}
        void text(int, int, std::string_view value, EditorColor) const override { texts.emplace_back(value); }
        [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
        mutable std::vector<std::string> texts;
    } canvas;
    render_native_editor(canvas, c, 1600, 900);
    bool labelled = false;
    for (const std::string& t : canvas.texts) labelled = labelled || t == "snap: vertex";
    check(labelled, "the target is labelled in the viewport");

    // An edge midpoint of the cube's front top edge: (1,2,2).
    std::tie(x, y) = screen(c, {1.0F, 2.0F, 2.0F});
    c.pointer_move(x, y);
    check(c.drag_snap_target() && c.drag_snap_target()->kind == SnapTargetKind::EdgeMidpoint, "edge midpoint");
    // Middle of the wall's front face of cell (1,0,0): a voxel face centre (cell corner priority
    // needs the cursor near a corner).
    std::tie(x, y) = screen(c, {5.5F, 0.5F, 1.0F});
    c.pointer_move(x, y);
    check(c.drag_snap_target() && c.drag_snap_target()->kind == SnapTargetKind::FaceCenter &&
          near(c.drag_snap_target()->point, {5.5F, 0.5F, 1.0F}), "voxel face centre: " +
          (c.drag_snap_target() ? std::string(snap_target_name(c.drag_snap_target()->kind)) + " " + text(c.drag_snap_target()->point) : "none"));
    std::tie(x, y) = screen(c, {6.0F, 1.0F, 1.0F});
    c.pointer_move(x + 2, y);
    check(c.drag_snap_target() && c.drag_snap_target()->kind == SnapTargetKind::VoxelCorner &&
          near(c.drag_snap_target()->point, {6.0F, 1.0F, 1.0F}), "voxel corner");
    // Releasing V goes back to the axis.
    c.key_up("v", false, false, false);
    check(!c.drag_snap_held() && !c.drag_snap_target(), "release V");
    check(std::abs(c.workspace().document().find_object(3)->transform.position.y - start.y) < 0.001F, "back on the X axis");
    c.key_down("v", false, false, false);
    std::tie(x, y) = screen(c, corner);
    c.pointer_move(x, y);
    c.pointer_up(PointerButton::Primary, x, y);
    check(near(c.workspace().document().find_object(3)->transform.position, add(corner, offset)), "release commits the snapped move");
    check(c.dispatch_action("edit.undo") && near(c.workspace().document().find_object(3)->transform.position, start), "one undo step");
    c.key_up("v", false, false, false);

    // Esc cancels a snapped drag.
    check(grab(c), "grab again");
    c.key_down("v", false, false, false);
    c.pointer_move(x, y);
    c.key_down("escape", false, false, false);
    check(near(c.workspace().document().find_object(3)->transform.position, start), "Esc restores the start");
    c.key_up("v", false, false, false);
}

void test_surface_snap_and_align() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    check(grab(c), "grab");
    const Float3 offset = pivot_offset(c);
    c.key_down("v", false, false, false);
    // The middle of the box's top face, away from every vertex and edge midpoint, and away from
    // the triangle centres (a face centre needs the cursor within three snap radii).
    const auto [x, y] = screen(c, {0.35F, 2.0F, 1.5F});
    c.pointer_move(x, y);
    const auto t = c.drag_snap_target();
    check(t.has_value(), "something under the cursor");
    if (t) check(t->kind == SnapTargetKind::Surface || t->kind == SnapTargetKind::FaceCenter, std::string("surface or face: ") + snap_target_name(t->kind));
    if (t && t->kind == SnapTargetKind::Surface)
        check(std::abs(c.workspace().document().find_object(3)->transform.position.y - (2.0F + offset.y)) < 0.01F, "lands on the top surface");
    c.key_down("escape", false, false, false);
    c.key_up("v", false, false, false);
}

void test_mesh_placement_modes() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    const Float3 pivot{-3.75F, 0.25F, 0.25F};  // bounds centre of the 0.5 m cell at (-4, 0, 0)
    const Float3 start = c.workspace().document().find_object(3)->transform.position;
    check(c.dispatch_action("transform.place_mesh_vertex"), "Snap to Mesh Vertex");
    const auto [x, y] = screen(c, {2.0F, 2.0F, 2.0F});
    c.pointer_move(x + 4, y - 3);
    check(c.placement_target() && near(*c.placement_target(), {2.0F, 2.0F, 2.0F}), "the placement snaps to the vertex");
    c.pointer_down(PointerButton::Primary, x + 4, y - 3);
    c.pointer_up(PointerButton::Primary, x + 4, y - 3);
    check(near(c.workspace().document().find_object(3)->transform.position, add(start, subtract({2.0F, 2.0F, 2.0F}, pivot))),
          "click places it: " + text(c.workspace().document().find_object(3)->transform.position));
    check(c.dispatch_action("edit.undo"), "undo");
    check(c.dispatch_action("transform.place_mesh_face"), "Snap to Mesh Face Centre");
    // Aim at the centre of a visible triangle of the box.
    const EditorObject* box = c.workspace().document().find_object(1);
    bool found = false;
    for (std::size_t t = 0; t + 2 < box->polygon->indices.size() && !found; t += 3) {
        Float3 centre{};
        for (int k = 0; k < 3; ++k)
            centre = add(centre, voxel_to_world(box->transform, box->voxelScale, box->polygon->vertices[box->polygon->indices[t + std::size_t(k)]].position));
        centre = multiply(centre, 1.0F / 3.0F);
        const ScreenPoint p = project_world_to_screen(c.camera(), c.layout().viewport, centre);
        const auto face = c.find_snap_target(static_cast<int>(std::lround(p.x)), static_cast<int>(std::lround(p.y)), SnapTargetKind::FaceCenter);
        found = face && face->kind == SnapTargetKind::FaceCenter && near(face->point, centre, 0.01F);
    }
    check(found, "face centres can be found on their own");
    c.key_down("escape", false, false, false);
}
}  // namespace

int main() {
    test_snap_drag();
    test_surface_snap_and_align();
    test_mesh_placement_modes();
    if (g_failures == 0) std::printf("drag snap tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
