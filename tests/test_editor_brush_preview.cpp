// Voxel brush preview and the sphere brush (artist worklist ART-044):
//   - the Add, Remove and Paint tools show exactly the cells a click would change;
//   - Voxel > Brush Shape: Cube / Sphere (and a toggle) change the brush, the menu checks it;
//   - the sphere applies to the edit, selection-limited Remove/Paint and the selection brush.
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <utility>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

void fill_box(EditorObject& object, Int3 min, Int3 max, MaterialId material) {
    for (int x = min.x; x <= max.x; ++x)
        for (int y = min.y; y <= max.y; ++y)
            for (int z = min.z; z <= max.z; ++z) (void)object.voxels->set_voxel({x, y, z}, material);
}

// A 20 x 6 x 20 block of material 1 (0.25 m voxels), top at y = 1.5 m.
EditorDocument make_scene() {
    EditorDocument document("Brush");
    EditorObject block(1, "Block");
    block.voxelSizeMeters = 0.25F;
    fill_box(block, {0, 0, 0}, {19, 5, 19}, 1);
    document.add_object(std::move(block));
    return document;
}

std::unique_ptr<NativeEditorController> make_controller() {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(make_scene()));
    controller->resize(1280, 720);
    controller->camera().position = {2.5F, 7.0F, 7.5F};
    controller->camera().target = {2.5F, 1.5F, 2.5F};
    return controller;
}

struct Canvas final : IEditorCanvas {
    void fill(UiRect, EditorColor) const override {}
    void outline(UiRect, EditorColor) const override { ++outlines; }
    void line(int, int, int, int, EditorColor, int) const override {}
    void text(int, int, std::string_view value, EditorColor) const override { texts.emplace_back(value); }
    [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
    [[nodiscard]] bool has(std::string_view needle) const {
        for (const std::string& t : texts) if (t.find(needle) != std::string::npos) return true;
        return false;
    }
    mutable int outlines{};
    mutable std::vector<std::string> texts;
};

// Hover the top face of voxel (10, 5, 10).
std::pair<int, int> hover_top(NativeEditorController& controller) {
    const ScreenPoint p = project_world_to_screen(controller.camera(), controller.layout().viewport,
                                                  {10.5F * 0.25F, 1.5F, 10.5F * 0.25F});
    const int x = static_cast<int>(std::lround(p.x)), y = static_cast<int>(std::lround(p.y));
    controller.pointer_move(x, y);
    return {x, y};
}

void click(NativeEditorController& controller, std::pair<int, int> at) {
    controller.pointer_down(PointerButton::Primary, at.first, at.second);
    controller.pointer_up(PointerButton::Primary, at.first, at.second);
}

std::uint64_t occupied(NativeEditorController& controller) {
    return controller.workspace().document().find_object(1)->voxels->occupied_voxel_count();
}

void set_radius(NativeEditorController& controller, int radius) {
    while (controller.voxel_brush_radius() < radius) (void)controller.dispatch_action("voxel.brush_increase");
    while (controller.voxel_brush_radius() > radius) (void)controller.dispatch_action("voxel.brush_decrease");
}

void test_add_preview_matches_click() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.set_active_material(1);  // the block's own material: only the 18 empty cells change
    controller.set_active_tool(EditorToolId::AddVoxel);
    set_radius(controller, 1);
    const auto at = hover_top(controller);
    const auto preview = controller.voxel_brush_preview();
    check(preview && preview->operation == VoxelToolOperation::Add, "Add shows a preview under the cursor");
    if (!preview) return;
    check(preview->center.y == 6 && preview->count == 18, "a radius-1 cube on the top face adds 18 cells: " + std::to_string(preview->count));
    Canvas canvas;
    render_native_editor(canvas, controller, 1280, 720);
    check(canvas.has("+18  cube r1") && canvas.outlines >= 18, "the cells are outlined and counted at the cursor");
    const std::uint64_t before = occupied(controller);
    const std::vector<Int3> cells = preview->cells;
    click(controller, at);
    check(occupied(controller) == before + 18, "the click adds exactly the previewed cells");
    bool all = true;
    for (const Int3 cell : cells) all = all && controller.workspace().document().find_object(1)->voxels->material_at(cell) != kAirMaterial;
    check(all, "every previewed cell is now filled");
}

void test_sphere() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    check(controller.dispatch_action("voxel.brush_shape_sphere") && controller.voxel_brush_shape() == BrushShape::Sphere,
          "Brush Shape: Sphere");
    const MenuAction* checkedSphere = std::as_const(controller.workspace().menus()).find("voxel.brush_shape_sphere");
    check(checkedSphere && checkedSphere->checked, "the menu checks Sphere");
    set_radius(controller, 3);
    controller.set_active_tool(EditorToolId::RemoveVoxel);
    const auto at = hover_top(controller);
    const auto sphere = controller.voxel_brush_preview();
    (void)controller.dispatch_action("voxel.brush_shape_toggle");
    const auto cube = controller.voxel_brush_preview();
    check(controller.voxel_brush_shape() == BrushShape::Cube, "the toggle switches back to Cube");
    check(sphere && cube && sphere->count < cube->count && sphere->count > 0,
          "a sphere removes fewer cells than the cube of the same radius");
    // r = 3 sphere centred on a top-layer voxel: the layers at or below y = 5 hold 29 + 25 + 21 + 1
    // cells (the block is 6 deep, so nothing is cut off below).
    if (sphere) check(sphere->count == 76, "radius-3 sphere count: " + std::to_string(sphere->count));
    (void)controller.dispatch_action("voxel.brush_shape_sphere");
    const std::uint64_t before = occupied(controller);
    click(controller, at);
    check(sphere && occupied(controller) == before - sphere->count, "the sphere click removes exactly the previewed cells");
    check(controller.dispatch_action("edit.undo") && occupied(controller) == before, "one undo");
}

void test_paint_selection_and_lock() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.set_active_material(2);
    controller.set_active_tool(EditorToolId::PaintMaterial);
    set_radius(controller, 2);
    (void)controller.dispatch_action("voxel.brush_shape_sphere");
    const auto at = hover_top(controller);
    const auto all = controller.voxel_brush_preview();
    check(all && all->operation == VoxelToolOperation::Paint && all->count > 0 && !all->limitedToSelection, "Paint previews");

    // A selection of a 3 x 1 x 3 patch at the top: only those inside the sphere change.
    std::set<Int3> patch;
    for (int x = 9; x <= 11; ++x)
        for (int z = 9; z <= 11; ++z) patch.insert({x, 5, z});
    patch.insert({0, 0, 0});  // selected but far outside the brush
    controller.select_voxels(1, patch, SelectionOperation::Replace);
    const auto limited = controller.voxel_brush_preview();
    check(limited && limited->limitedToSelection && limited->count == 9, "with a selection only the selected cells in reach");
    Canvas canvas;
    render_native_editor(canvas, controller, 1280, 720);
    check(canvas.has("paint 9  sphere r2  (selection only)"), "the label says so");
    click(controller, at);
    const EditorObject* block = controller.workspace().document().find_object(1);
    check(block->voxels->material_at({11, 5, 11}) == 2 && block->voxels->material_at({12, 5, 10}) == 1 &&
              block->voxels->material_at({0, 0, 0}) == 1,
          "the click repaints exactly those");
    check(!controller.voxel_brush_preview() || controller.voxel_brush_preview()->count == 0,
          "after painting, the same spot previews nothing left to change");

    controller.workspace().document().find_object(1)->flags.locked = true;
    hover_top(controller);
    const auto locked = controller.voxel_brush_preview();
    check(locked && locked->locked && locked->count == 0, "a locked object previews nothing");
}

void test_selection_brush_sphere() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    (void)controller.dispatch_action("select.domain_voxels");
    (void)controller.dispatch_action("voxel.selection_brush");
    set_radius(controller, 2);
    (void)controller.dispatch_action("voxel.brush_shape_sphere");
    controller.set_active_tool(EditorToolId::Select);
    const auto at = hover_top(controller);
    controller.pointer_down(PointerButton::Primary, at.first, at.second);
    controller.pointer_up(PointerButton::Primary, at.first, at.second);
    const auto& selection = controller.voxel_selection();
    check(!selection.empty(), "the selection brush selected something: " + controller.status().text);
    bool insideSphere = true;
    for (const Int3 cell : selection) {
        const int dx = cell.x - 10, dy = cell.y - 5, dz = cell.z - 10;
        insideSphere = insideSphere && dx * dx + dy * dy + dz * dz <= 4;
    }
    check(insideSphere && !selection.contains({12, 5, 12}), "with the sphere it takes no cube corners");
}

// Review fixes: the preview mirrors the command exactly, a no-op click adds no undo step, and a
// different selection of the same size refreshes the preview.
void test_review_fixes() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.set_active_material(2);
    controller.set_active_tool(EditorToolId::AddVoxel);
    set_radius(controller, 1);
    const auto at = hover_top(controller);
    const auto preview = controller.voxel_brush_preview();
    // Add with another material also rewrites the 9 top-layer cells the cube overlaps.
    check(preview && preview->count == 27, "Add with material 2 previews 27 cells: " + std::to_string(preview ? preview->count : 0));
    click(controller, at);
    std::size_t painted = 0;
    const EditorObject* block = controller.workspace().document().find_object(1);
    for (int x = 9; x <= 11; ++x) for (int y = 5; y <= 7; ++y) for (int z = 9; z <= 11; ++z)
        painted += block->voxels->material_at({x, y, z}) == 2 ? 1U : 0U;
    check(painted == 27, "the click changes exactly the 27 previewed cells: " + std::to_string(painted));
    (void)controller.dispatch_action("edit.undo");

    controller.set_active_tool(EditorToolId::PaintMaterial);
    controller.set_active_material(1);
    hover_top(controller);
    const std::size_t steps = controller.workspace().commands().size();
    const bool couldUndo = controller.workspace().commands().can_undo();
    click(controller, at);
    check(controller.workspace().commands().size() == steps && controller.workspace().commands().can_undo() == couldUndo,
          "painting cells that already have the material adds no undo step");

    controller.set_active_material(2);
    controller.select_voxels(1, {Int3{10, 5, 10}}, SelectionOperation::Replace);
    hover_top(controller);
    const auto one = controller.voxel_brush_preview();
    controller.select_voxels(1, {Int3{0, 0, 0}}, SelectionOperation::Replace);
    const auto other = controller.voxel_brush_preview();
    check(one && one->count == 1 && other && other->count == 0, "a different selection of the same size refreshes the preview");
}
}  // namespace

int main() {
    test_review_fixes();
    test_add_preview_matches_click();
    test_sphere();
    test_paint_selection_and_lock();
    test_selection_brush_sphere();
    if (g_failures == 0) std::printf("brush preview tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
