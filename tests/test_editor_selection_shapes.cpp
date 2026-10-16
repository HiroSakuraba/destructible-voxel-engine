// Lasso and paint selection (artist worklist ART-018): Edit > Selection Shape: Box / Lasso / Paint,
// Shift+Q cycles; set operations, domains, masks and X-Ray behave as for boxes.
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

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

// "Wall" (x 0..7, y 0..5, z 0..2) hides "Back" behind it; "Side" stands alone to the right.
std::unique_ptr<NativeEditorController> make_controller() {
    EditorDocument document("Selection shapes");
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

// A closed loop through the given world points, with in-between moves.
void lasso(NativeEditorController& c, const std::vector<Float3>& loop, std::uint32_t modifiers = 0, std::string* label = nullptr,
           std::size_t* drawnLines = nullptr) {
    std::vector<std::pair<int, int>> points;
    for (const Float3 p : loop) points.push_back(screen(c, p));
    c.pointer_down(PointerButton::Primary, points[0].first, points[0].second, modifiers);
    for (std::size_t i = 1; i <= points.size(); ++i) {
        const auto [ax, ay] = points[i - 1];
        const auto [bx, by] = points[i % points.size()];
        for (int s = 1; s <= 8; ++s) c.pointer_move(ax + (bx - ax) * s / 8, ay + (by - ay) * s / 8, modifiers);
    }
    if (label) *label = c.marquee_label();
    if (drawnLines) {
        struct Canvas final : IEditorCanvas {
            void fill(UiRect, EditorColor) const override {}
            void outline(UiRect, EditorColor) const override {}
            void line(int, int, int, int, EditorColor, int width) const override { if (width == 2) ++lines; }
            void text(int, int, std::string_view, EditorColor) const override {}
            [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
            mutable std::size_t lines{};
        } canvas;
        render_native_editor(canvas, c, 1280, 720);
        *drawnLines = canvas.lines;
    }
    c.pointer_up(PointerButton::Primary, points[0].first, points[0].second, modifiers);
}

std::string ids(const NativeEditorController& c) {
    std::string out;
    for (const EditorObjectId id : c.workspace().selected_objects()) out += std::to_string(id) + " ";
    return out;
}

void test_shape_setting() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    check(c.selection_shape() == SelectionShape::Box, "Box is the default");
    c.key_down("q", false, true, false);
    check(c.selection_shape() == SelectionShape::Lasso, "Shift+Q -> Lasso");
    const MenuAction* lassoItem = std::as_const(c.workspace().menus()).find("select.shape_lasso");
    check(lassoItem && lassoItem->checked && !lassoItem->radioGroup.empty(), "the menu radio checks Lasso");
    c.key_down("q", false, true, false);
    check(c.selection_shape() == SelectionShape::Paint, "Shift+Q -> Paint");
    const int radius = c.selection_paint_radius();
    c.key_down("]", true, false, false);
    check(c.selection_paint_radius() > radius, "Ctrl+] grows the paint brush");
    c.key_down("[", true, false, false);
    c.key_down("[", true, false, false);
    check(c.selection_paint_radius() < radius, "Ctrl+[ shrinks it");
    c.key_down("q", false, true, false);
    check(c.selection_shape() == SelectionShape::Box, "Shift+Q wraps to Box");
}

void test_lasso_objects() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    (void)c.dispatch_action("select.shape_lasso");
    // A triangle around Side only.
    std::string label;
    std::size_t lines = 0;
    lasso(c, {{8.0F, -1.0F, 2.0F}, {12.0F, -1.0F, 2.0F}, {10.0F, 3.5F, 2.0F}}, 0, &label, &lines);
    check(c.workspace().selected_objects() == std::set<EditorObjectId>{3}, "lasso around Side selects it: " + ids(c));
    check(label == "Lasso \xC2\xB7 visible only", "lasso label: " + label);
    check(lines >= 10, "the loop is drawn while dragging: " + std::to_string(lines));
    // A loop over the middle of the wall: Back is behind it.
    const std::vector<Float3> middle{{1.5F, 0.5F, 2.0F}, {3.5F, 0.5F, 2.0F}, {3.5F, 2.5F, 2.0F}, {1.5F, 2.5F, 2.0F}};
    lasso(c, middle);
    check(c.workspace().selected_objects() == std::set<EditorObjectId>{1}, "visible only: the wall: " + ids(c));
    check(c.dispatch_action("view.xray") && c.viewport_settings().xraySelection, "X-Ray on");
    lasso(c, middle);
    check(c.workspace().selected_objects() == std::set<EditorObjectId>{1, 2}, "X-Ray: through to Back: " + ids(c) + c.status().text);
    check(c.dispatch_action("view.xray") && !c.viewport_settings().xraySelection, "X-Ray off");
    lasso(c, middle);
    lasso(c, {{8.0F, -1.0F, 2.0F}, {12.0F, -1.0F, 2.0F}, {10.0F, 3.5F, 2.0F}}, 1U);
    check(c.workspace().selected_objects() == std::set<EditorObjectId>{1, 3}, "Shift-lasso adds: " + ids(c));
    lasso(c, middle, 2U);
    check(c.workspace().selected_objects() == std::set<EditorObjectId>{3}, "Ctrl-lasso removes: " + ids(c));
    // Masks: a locked wall is skipped but still hides Back.
    c.workspace().document().find_object(1)->flags.locked = true;
    (void)c.dispatch_action("select.mask_locked");
    lasso(c, middle);
    check(c.workspace().selected_objects().empty(), "masked wall skipped, Back still hidden: " + ids(c) + c.status().text);
    // A tiny loop is a click.
    (void)c.dispatch_action("select.mask_all");
    const auto [x, y] = screen(c, {10.0F, 1.0F, 2.0F});
    c.pointer_down(PointerButton::Primary, x, y);
    c.pointer_move(x + 2, y + 1);
    c.pointer_up(PointerButton::Primary, x, y);
    check(c.workspace().selected_objects() == std::set<EditorObjectId>{3}, "a tiny lasso clicks: " + ids(c));
}

void test_lasso_voxels() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.workspace().select_object(1);
    (void)c.dispatch_action("select.domain_voxels");
    c.select_voxels(1, {Int3{0, 0, 1}}, SelectionOperation::Replace);
    (void)c.dispatch_action("select.shape_lasso");
    // A triangle on the front face: fewer cells than its bounding box.
    lasso(c, {{0.1F, 0.1F, 2.0F}, {6.9F, 0.1F, 2.0F}, {0.1F, 4.9F, 2.0F}});
    const std::size_t triangle = c.voxel_selection().size();
    check(triangle > 5 && triangle < 35, "lasso selects front cells inside the triangle: " + std::to_string(triangle) + " " + c.status().text);
    bool front = true;
    for (const Int3 cell : c.voxel_selection()) front = front && cell.z == 1;
    check(front, "visible only: just the front layer");
    check(c.voxel_selection().contains({0, 0, 1}) && !c.voxel_selection().contains({6, 4, 1}), "inside versus outside the loop");
}

void test_lasso_polygons() {
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
    (void)c.dispatch_action("polygon.vertex_mode");
    (void)c.dispatch_action("select.shape_lasso");
    // Around the two left front corners only.
    lasso(c, {{-0.3F, -0.3F, 1.0F}, {0.3F, -0.3F, 1.0F}, {0.3F, 1.3F, 1.0F}, {-0.3F, 1.3F, 1.0F}});
    check(c.polygon_selection().components.size() >= 2 && c.polygon_selection().components.size() <= 4,
          "lasso picks the visible corners inside: " + std::to_string(c.polygon_selection().components.size()));
    const std::size_t front = c.polygon_selection().components.size();
    check(c.dispatch_action("view.xray") && c.viewport_settings().xraySelection, "X-Ray on");
    lasso(c, {{-0.3F, -0.3F, 1.0F}, {0.3F, -0.3F, 1.0F}, {0.3F, 1.3F, 1.0F}, {-0.3F, 1.3F, 1.0F}});
    check(c.polygon_selection().components.size() > front, "X-Ray adds the corners behind");

    // Paint over the front face in face mode.
    check(c.dispatch_action("view.xray") && !c.viewport_settings().xraySelection, "X-Ray off");
    (void)c.dispatch_action("polygon.face_mode");
    (void)c.dispatch_action("polygon.select_none");
    (void)c.dispatch_action("select.shape_paint");
    const auto [ax, ay] = screen(c, {0.2F, 0.5F, 1.0F});
    const auto [bx, by] = screen(c, {0.8F, 0.5F, 1.0F});
    c.pointer_down(PointerButton::Primary, ax, ay);
    check(c.paint_selecting(), "a paint stroke starts");
    for (int s = 1; s <= 10; ++s) c.pointer_move(ax + (bx - ax) * s / 10, ay + (by - ay) * s / 10);
    c.pointer_up(PointerButton::Primary, bx, by);
    check(!c.paint_selecting(), "and ends on release");
    check(c.polygon_selection().components.size() == 2, "painting the front face takes its two triangles: " +
          std::to_string(c.polygon_selection().components.size()));
}

void test_paint_objects_and_voxels() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    (void)c.dispatch_action("select.shape_paint");
    const auto [ax, ay] = screen(c, {5.0F, 1.0F, 2.0F});
    const auto [bx, by] = screen(c, {10.0F, 1.0F, 2.0F});
    c.pointer_move(ax, ay);
    struct Canvas final : IEditorCanvas {
        void fill(UiRect, EditorColor) const override {}
        void outline(UiRect, EditorColor) const override {}
        void line(int, int, int, int, EditorColor, int) const override { ++lines; }
        void text(int, int, std::string_view, EditorColor) const override {}
        [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
        mutable std::size_t lines{};
    } before, during;
    render_native_editor(before, c, 1280, 720);
    c.pointer_down(PointerButton::Primary, ax, ay);
    for (int s = 1; s <= 12; ++s) c.pointer_move(ax + (bx - ax) * s / 12, ay + (by - ay) * s / 12);
    c.pointer_up(PointerButton::Primary, bx, by);
    check(c.workspace().selected_objects() == std::set<EditorObjectId>{1, 3}, "painting across Wall and Side selects both: " + ids(c));
    check(before.lines >= 32, "the brush circle is drawn at the cursor");
    c.pointer_down(PointerButton::Primary, bx, by, 2U);
    c.pointer_up(PointerButton::Primary, bx, by, 2U);
    check(c.workspace().selected_objects() == std::set<EditorObjectId>{1}, "Ctrl-paint removes: " + ids(c));

    // Voxels: Paint is the Selection Brush.
    c.workspace().select_object(1);
    (void)c.dispatch_action("select.domain_voxels");
    (void)c.dispatch_action("select.shape_paint");
    const auto [vx, vy] = screen(c, {3.5F, 2.5F, 2.0F});
    c.pointer_move(vx, vy);
    c.pointer_down(PointerButton::Primary, vx, vy);
    c.pointer_up(PointerButton::Primary, vx, vy);
    check(!c.voxel_selection().empty(), "paint in the voxel domain uses the Selection Brush: " + c.status().text);
    (void)during;
}

// Codex review: Paint chosen outside the voxel domain stays Paint when switching to voxels (and
// back again), and Ctrl+[ / Ctrl+] size the voxel Selection Brush there.
void test_paint_survives_domain_switch() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.workspace().select_object(1);
    const auto brushChecked = [&] {
        const MenuAction* item = std::as_const(c.workspace().menus()).find("voxel.selection_brush");
        return item && item->checked;
    };
    check(c.dispatch_action("select.shape_paint"), "Paint in the object domain");
    check(!brushChecked(), "no voxel brush while selecting objects");
    (void)c.dispatch_action("select.domain_voxels");
    check(c.selection_domain() == SelectionDomain::Voxels, "now selecting voxels");
    check(c.selection_shape() == SelectionShape::Paint && brushChecked(), "Paint carries into the voxel domain as the Selection Brush");
    const int radius = c.voxel_brush_radius();
    const int pixels = c.selection_paint_radius();
    c.key_down("]", true, false, false);
    check(c.voxel_brush_radius() == radius + 1, "Ctrl+] grows the voxel Selection Brush: " + c.status().text);
    check(c.selection_paint_radius() == pixels, "and leaves the pixel brush alone");
    c.key_down("[", true, false, false);
    check(c.voxel_brush_radius() == radius, "Ctrl+[ shrinks it again");
    (void)c.dispatch_action("select.domain_objects");
    check(!brushChecked(), "leaving voxels turns the brush off");
    (void)c.dispatch_action("select.domain_voxels");
    check(brushChecked(), "and returning turns it back on");
    // Switching the brush off from the Voxel menu is honest about the shape too.
    (void)c.dispatch_action("voxel.selection_brush");
    check(!brushChecked() && c.selection_shape() == SelectionShape::Box, "brush off -> Box shape");
    (void)c.dispatch_action("voxel.selection_brush");
    check(brushChecked() && c.selection_shape() == SelectionShape::Paint, "brush on -> Paint shape");
}
}  // namespace

int main() {
    test_shape_setting();
    test_lasso_objects();
    test_lasso_voxels();
    test_lasso_polygons();
    test_paint_objects_and_voxels();
    test_paint_survives_domain_switch();
    if (g_failures == 0) std::printf("selection shape tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
