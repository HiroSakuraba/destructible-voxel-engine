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
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
bool close_to(Float3 a, Float3 b, float tolerance = 0.002F) { return length(subtract(a, b)) <= tolerance; }
std::string text(Float3 v) { return std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z); }

// windows.h defines `near` (and min/max) as macros, hence close_to.
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
    check(snapped && snapped->kind == SnapTargetKind::Vertex && close_to(snapped->point, corner),
          "snaps to the vertex: " + (snapped ? std::string(snap_target_name(snapped->kind)) + " " + text(snapped->point) : "none"));
    check(close_to(c.workspace().document().find_object(3)->transform.position, add(corner, offset)), "the pivot sits on the vertex");
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
          close_to(c.drag_snap_target()->point, {5.5F, 0.5F, 1.0F}), "voxel face centre: " +
          (c.drag_snap_target() ? std::string(snap_target_name(c.drag_snap_target()->kind)) + " " + text(c.drag_snap_target()->point) : "none"));
    std::tie(x, y) = screen(c, {6.0F, 1.0F, 1.0F});
    c.pointer_move(x + 2, y);
    check(c.drag_snap_target() && c.drag_snap_target()->kind == SnapTargetKind::VoxelCorner &&
          close_to(c.drag_snap_target()->point, {6.0F, 1.0F, 1.0F}), "voxel corner");
    // Releasing V goes back to the axis.
    c.key_up("v", false, false, false);
    check(!c.drag_snap_held() && !c.drag_snap_target(), "release V");
    check(std::abs(c.workspace().document().find_object(3)->transform.position.y - start.y) < 0.001F, "back on the X axis");
    c.key_down("v", false, false, false);
    std::tie(x, y) = screen(c, corner);
    c.pointer_move(x, y);
    c.pointer_up(PointerButton::Primary, x, y);
    check(close_to(c.workspace().document().find_object(3)->transform.position, add(corner, offset)), "release commits the snapped move");
    check(c.dispatch_action("edit.undo") && close_to(c.workspace().document().find_object(3)->transform.position, start), "one undo step");
    c.key_up("v", false, false, false);

    // Esc cancels a snapped drag.
    check(grab(c), "grab again");
    c.key_down("v", false, false, false);
    c.pointer_move(x, y);
    c.key_down("escape", false, false, false);
    check(close_to(c.workspace().document().find_object(3)->transform.position, start), "Esc restores the start");
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
    check(c.placement_target() && close_to(*c.placement_target(), {2.0F, 2.0F, 2.0F}), "the placement snaps to the vertex");
    c.pointer_down(PointerButton::Primary, x + 4, y - 3);
    c.pointer_up(PointerButton::Primary, x + 4, y - 3);
    check(close_to(c.workspace().document().find_object(3)->transform.position, add(start, subtract({2.0F, 2.0F, 2.0F}, pivot))),
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
        found = face && face->kind == SnapTargetKind::FaceCenter && close_to(face->point, centre, 0.01F);
    }
    check(found, "face centres can be found on their own");
    c.key_down("escape", false, false, false);
}

bool same(const RigidTransform& a, const RigidTransform& b) {
    return a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z &&
           a.rotation.x == b.rotation.x && a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z && a.rotation.w == b.rotation.w;
}

struct TextCanvas final : IEditorCanvas {
    void fill(UiRect, EditorColor) const override {}
    void outline(UiRect, EditorColor) const override {}
    void line(int, int, int, int, EditorColor, int) const override {}
    void text(int, int, std::string_view value, EditorColor) const override { texts.emplace_back(value); }
    [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
    mutable std::vector<std::string> texts;
    [[nodiscard]] bool contains(std::string_view needle) const {
        for (const std::string& t : texts) if (t.find(needle) != std::string::npos) return true;
        return false;
    }
};

// A drag that ends where it started, with or without V, adds no undo step.
void test_unmoved_drag_adds_no_undo_step() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    check(grab(c), "grab");
    c.key_down("enter", false, false, false);
    check(!c.workspace().commands().can_undo(), "a drag that did not move adds no undo step");
    check(!c.gizmo_dragging(), "drag finished");
}

// The status bar snap summary says point snapping replaces the grid step while V is held.
void test_snap_readout() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    TextCanvas idle;
    render_native_editor(idle, c, 1600, 900);
    check(idle.contains("Move "), "the snap summary is drawn");
    check(!idle.contains("point (V)"), "no point snap before V");
    check(grab(c), "grab");
    c.key_down("v", false, false, false);
    const auto [x, y] = screen(c, {2.0F, 2.0F, 2.0F});
    c.pointer_move(x, y);
    TextCanvas held;
    render_native_editor(held, c, 1600, 900);
    check(held.contains("Move point (V): vertex"), "the summary names the point snap and its target");
    c.key_down("escape", false, false, false);
    c.key_up("v", false, false, false);
    TextCanvas after;
    render_native_editor(after, c, 1600, 900);
    check(!after.contains("point (V)"), "released V restores the grid summary");
}

// Multi-select: every selected object moves by the same snap; none of them is a target; undo
// restores each exact transform (including a surface-normal turn) and the selection.
void test_multi_select_and_exact_undo() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    {
        EditorObject second(4, "Second");
        second.voxelSizeMeters = 0.5F;
        (void)second.voxels->set_voxel({0, 0, 0}, 1);
        second.transform.position = {-4.0F, 0.0F, 2.0F};
        second.transform.rotation = normalize(quaternion_from_axis_angle({0.0F, 1.0F, 0.0F}, 0.3F));
        c.workspace().document().add_object(std::move(second));
        // Attached to Mover: it follows Mover, so it is never a target either.
        EditorObject rider(5, "Rider");
        rider.polygon = make_editor_polygon_cube(1);
        rider.parent = 3;
        rider.attachment = EditorAttachment{};
        rider.attachment->localTransform.position = {0.0F, 3.0F, 0.0F};
        c.workspace().document().add_object(std::move(rider));
        c.workspace().document().refresh_attachment_world_transforms();
    }
    c.workspace().select_object(3);
    c.workspace().toggle_selection(4);
    check(c.workspace().selection_count() == 2, "two selected");
    const RigidTransform before3 = c.workspace().document().find_object(3)->transform;
    const RigidTransform before4 = c.workspace().document().find_object(4)->transform;
    // Aim at the rider's corner: it moves with Mover, so it must not be what we snap to.
    const EditorObject* rider = c.workspace().document().find_object(5);
    const Float3 riderCorner = voxel_to_world(rider->transform, rider->voxelScale, Float3{1.0F, 1.0F, 1.0F});
    const auto [rx, ry] = screen(c, riderCorner);
    const auto riderSnap = c.find_snap_target(rx, ry);
    check(!riderSnap || (riderSnap->object != 3 && riderSnap->object != 4 && riderSnap->object != 5),
          "an attached child of the selection is not a target");
    check(grab(c), "grab both");
    c.key_down("v", false, false, false);
    const Float3 corner{2.0F, 2.0F, 2.0F};
    const auto [x, y] = screen(c, corner);
    c.pointer_move(x, y);
    const auto t = c.drag_snap_target();
    check(t && t->object == 1 && t->kind == SnapTargetKind::Vertex, "snaps to the box, not to a selected object");
    const Float3 d3 = subtract(c.workspace().document().find_object(3)->transform.position, before3.position);
    const Float3 d4 = subtract(c.workspace().document().find_object(4)->transform.position, before4.position);
    check(close_to(d3, d4) && length(d3) > 0.5F, "both move by the same snap");
    c.pointer_up(PointerButton::Primary, x, y);
    c.key_up("v", false, false, false);
    check(c.dispatch_action("edit.undo"), "undo");
    check(same(c.workspace().document().find_object(3)->transform, before3) &&
          same(c.workspace().document().find_object(4)->transform, before4), "undo restores the exact transforms");
    check(c.workspace().selection_count() == 2 && c.workspace().is_selected(3) && c.workspace().is_selected(4), "and the selection");
    check(!c.workspace().commands().can_undo(), "it was one undo step");

    // Align Up to Surface Normal: a turned snap undoes exactly too.
    check(c.dispatch_action("transform.align_surface"), "align on");
    check(grab(c), "grab again");
    c.key_down("v", false, false, false);
    // The box's front face (+Z), away from vertices and edge midpoints.
    const auto [fx, fy] = screen(c, {0.35F, 0.65F, 2.0F});
    c.pointer_move(fx, fy);
    c.pointer_up(PointerButton::Primary, fx, fy);
    c.key_up("v", false, false, false);
    if (c.workspace().commands().can_undo()) {
        check(c.dispatch_action("edit.undo"), "undo the turned snap");
        check(same(c.workspace().document().find_object(3)->transform, before3) &&
              same(c.workspace().document().find_object(4)->transform, before4), "undo restores the exact turned transforms");
    }
}

// A closed group moves as one: its own pieces are never snap or placement targets.
void test_closed_group_is_not_its_own_target() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    {
        EditorObject crate(10, "Crate");
        c.workspace().document().add_object(std::move(crate));
        EditorObject lid(11, "Lid");
        lid.polygon = make_editor_polygon_cube(1);
        lid.parent = 10;
        lid.transform.position = {-2.0F, 0.0F, 0.0F};
        c.workspace().document().add_object(std::move(lid));
        EditorObject base(12, "Base");
        base.voxelSizeMeters = 0.5F;
        (void)base.voxels->set_voxel({0, 0, 0}, 1);
        base.parent = 10;
        base.transform.position = {-2.0F, -1.0F, 0.0F};
        c.workspace().document().add_object(std::move(base));
    }
    check(c.is_group(10), "Crate is a group");
    c.workspace().select_object(10);
    // Straight at the lid's top corner: without the group's pieces excluded this snapped to it.
    const auto [x, y] = screen(c, {-1.0F, 1.0F, 1.0F});
    const auto t = c.find_snap_target(x, y);
    check(!t || (t->object != 10 && t->object != 11 && t->object != 12), "a closed group's pieces are not targets");
    check(c.dispatch_action("transform.place_mesh_vertex"), "Snap to Mesh Vertex for the group");
    c.pointer_move(x, y);
    check(!c.placement_target() || !close_to(*c.placement_target(), {-1.0F, 1.0F, 1.0F}), "placement does not land on its own piece");
    c.key_down("escape", false, false, false);
    check(c.dispatch_action("transform.place_surface"), "Place on Surface for the group");
    c.pointer_move(x, y);
    if (const auto p = c.placement_target()) check(p->x > -0.9F || p->y < -1.1F || p->z > 1.1F || p->x < -2.1F, "surface placement skips its own pieces");
    c.key_down("escape", false, false, false);
    // Moving a piece of a locked group is refused before any snapping.
    c.workspace().document().find_object(11)->flags.locked = true;
    check(!grab(c), "a group with a locked piece is refused");
}

// V only means snap where snapping can happen; menus say why placement cannot start.
void test_gating_and_menus() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.set_active_tool(EditorToolId::Rotate);
    c.key_down("v", false, false, false);
    check(!c.drag_snap_held(), "Rotate does not take V");
    c.key_up("v", false, false, false);
    c.set_active_tool(EditorToolId::Translate);
    check(c.dispatch_action("select.domain_voxels"), "voxel domain");
    c.key_down("v", false, false, false);
    check(!c.drag_snap_held(), "the voxel domain does not take V (cell moves have their own steps)");
    c.key_up("v", false, false, false);
    check(c.dispatch_action("select.domain_objects"), "object domain");
    c.workspace().select_object(3);

    c.workspace().select_object(std::nullopt);
    check(!c.dispatch_action("transform.place_mesh_vertex"), "nothing selected: refused");
    check(c.status().error && c.status().text.find("Select the objects to place first") != std::string::npos,
          "with the reason: " + c.status().text);
    const MenuAction* action = std::as_const(c.workspace().menus()).find("transform.place_mesh_face");
    check(action && !action->enabled && !action->disabledReason.empty(), "the menu entry is disabled with a reason");
    c.workspace().select_object(3);
    check(c.dispatch_action("transform.place_mesh_edge"), "enabled once something is selected");
    c.key_down("escape", false, false, false);
    // Hidden objects are never targets.
    c.workspace().document().find_object(1)->flags.visible = false;
    const auto [x, y] = screen(c, {2.0F, 2.0F, 2.0F});
    const auto hidden = c.find_snap_target(x, y);
    check(!hidden || hidden->object != 1, "a hidden object is not a target");
}
}  // namespace

int main() {
    test_snap_drag();
    test_surface_snap_and_align();
    test_mesh_placement_modes();
    test_unmoved_drag_adds_no_undo_step();
    test_snap_readout();
    test_multi_select_and_exact_undo();
    test_closed_group_is_not_its_own_target();
    test_gating_and_menus();
    if (g_failures == 0) std::printf("drag snap tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
