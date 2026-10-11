// Voxel selection (artist worklist ART-043): the Objects / Voxels selection domain (M), click /
// box / brush gestures with Shift-add and Ctrl-remove, grow / shrink / invert / select all, and
// the edits on a selection: delete, extract, copy / cut / paste, move with the gizmo, and Paint,
// Remove and Flood Fill limited to the selection.
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_voxel_selection.hpp"

#include <cmath>
#include <utility>
#include <cstdio>
#include <memory>
#include <set>
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

// "Rocks": piece A = 4x4x4 cube of material 1 at the origin, piece B = 2x2x2 of material 2 at
// x 8..9 (not touching A); an anchor in each.
EditorObject make_rocks(EditorObjectId id = 1) {
    EditorObject rocks(id, "Rocks");
    rocks.voxelSizeMeters = 0.25F;
    fill_box(rocks, {0, 0, 0}, {3, 3, 3}, 1);
    fill_box(rocks, {8, 0, 0}, {9, 1, 1}, 2);
    rocks.anchors.insert({0, 0, 0});
    rocks.anchors.insert({8, 0, 0});
    return rocks;
}

void test_core() {
    const EditorObject rocks = make_rocks();
    const VoxelObject& v = *rocks.voxels;
    check(voxel_connected_piece(v, {1, 1, 1}).size() == 64, "the clicked piece is the 4x4x4 cube");
    check(voxel_connected_piece(v, {9, 1, 1}).size() == 8, "the other piece is separate");
    check(voxel_connected_piece(v, {5, 0, 0}).empty(), "empty cell: nothing");
    check(all_occupied_voxels(v).size() == 72, "all voxels");
    check(grow_voxel_selection(v, {{1, 1, 1}}).size() == 7, "grow adds the 6 face neighbours");
    check(shrink_voxel_selection(voxel_connected_piece(v, {0, 0, 0})).size() == 8, "shrinking the cube leaves its 2x2x2 core");
    check(invert_voxel_selection(v, voxel_connected_piece(v, {0, 0, 0})).size() == 8, "invert selects the other piece");
    check(voxels_in_cube(v, {0, 0, 0}, 1).size() == 8, "a radius-1 brush at a corner holds 8 voxels");

    EditorDocument document("core");
    document.add_object(make_rocks());
    EditorCommandStack stack;
    const std::set<Int3> pieceB = voxel_connected_piece(*document.find_object(1)->voxels, {8, 0, 0});

    // Delete restores voxels and anchors exactly on undo.
    std::string error;
    auto remove = build_delete_voxels_command(*document.find_object(1), pieceB, &error);
    check(remove && stack.execute(document, std::move(remove)).success, "delete executes");
    check(document.find_object(1)->voxels->occupied_voxel_count() == 64 && !document.find_object(1)->anchors.contains({8, 0, 0}),
          "piece B and its anchor are gone");
    check(stack.undo(document).success && document.find_object(1)->voxels->material_at({9, 1, 1}) == 2 &&
              document.find_object(1)->anchors.contains({8, 0, 0}),
          "undo restores voxels and anchor");

    // Move by +2 X: lands at x 10..11, the old cells empty, the anchor moves with it.
    auto move = build_move_voxels_command(*document.find_object(1), pieceB, {2, 0, 0}, &error);
    check(move && stack.execute(document, std::move(move)).success, "move executes");
    const EditorObject* moved = document.find_object(1);
    check(moved->voxels->material_at({11, 1, 1}) == 2 && moved->voxels->material_at({8, 0, 0}) == kAirMaterial &&
              moved->anchors.contains({10, 0, 0}) && !moved->anchors.contains({8, 0, 0}),
          "voxels and anchor moved");
    check(stack.undo(document).success && document.find_object(1)->voxels->material_at({8, 0, 0}) == 2 &&
              document.find_object(1)->anchors.contains({8, 0, 0}),
          "undo moves them back");
    // Moving into occupied cells overwrites them.
    auto into = build_move_voxels_command(*document.find_object(1), pieceB, {-6, 0, 0}, &error);
    check(into && stack.execute(document, std::move(into)).success, "move into the cube");
    check(document.find_object(1)->voxels->material_at({2, 0, 0}) == 2 && document.find_object(1)->voxels->occupied_voxel_count() == 64,
          "moved voxels replace what was there");
    check(stack.undo(document).success && document.find_object(1)->voxels->occupied_voxel_count() == 72, "undo");
    check(!build_move_voxels_command(*document.find_object(1), pieceB, {0, 0, 0}, &error), "a zero move is refused");

    // Extract makes a sibling with the cells; nothing moves in the world.
    ExtractVoxelsBuild extract = build_extract_voxels_command(document, 1, pieceB);
    check(extract.command && stack.execute(document, std::move(extract.command)).success, "extract executes");
    const EditorObject* part = document.find_object(extract.newObject);
    check(part && part->name == "Rocks (extract)" && part->voxels->occupied_voxel_count() == 8 &&
              part->voxels->material_at({8, 0, 0}) == 2 && part->anchors.contains({8, 0, 0}) && part->voxelSizeMeters == 0.25F,
          "the new object holds the cells at the same coordinates");
    check(document.find_object(1)->voxels->occupied_voxel_count() == 64, "the original lost them");
    check(stack.undo(document).success && !document.find_object(extract.newObject) &&
              document.find_object(1)->voxels->occupied_voxel_count() == 72,
          "undo puts them back");
    check(!build_extract_voxels_command(document, 1, all_occupied_voxels(*document.find_object(1)->voxels)).command,
          "extracting every voxel is refused (that is the whole object)");
}

std::unique_ptr<NativeEditorController> make_controller() {
    EditorDocument document("Voxel selection");
    document.add_object(make_rocks());
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    controller->resize(1280, 720);
    controller->workspace().select_object(1);
    (void)controller->dispatch_action("view.frame_all");
    return controller;
}

struct Screen { int x; int y; };
Screen voxel_top(const NativeEditorController& controller, Int3 cell) {
    const EditorObject* object = controller.workspace().document().find_object(1);
    const float s = object->voxelSizeMeters;
    const Float3 world = transform_point(object->transform, {(cell.x + 0.5F) * s, (cell.y + 1.0F) * s, (cell.z + 0.5F) * s});
    const ScreenPoint p = project_world_to_screen(controller.camera(), controller.layout().viewport, world);
    return {static_cast<int>(p.x), static_cast<int>(p.y)};
}
void click(NativeEditorController& controller, Screen at, std::uint32_t modifiers = 0) {
    controller.pointer_move(at.x, at.y, modifiers);
    controller.pointer_down(PointerButton::Primary, at.x, at.y, modifiers);
    controller.pointer_up(PointerButton::Primary, at.x, at.y, modifiers);
}

struct Canvas final : IEditorCanvas {
    void fill(UiRect, EditorColor) const override {}
    void outline(UiRect, EditorColor color) const override { if (color == ((255U << 16U) | (196U << 8U) | 64U)) ++amber; }
    void line(int, int, int, int, EditorColor, int) const override {}
    void text(int, int, std::string_view value, EditorColor) const override { texts.emplace_back(value); }
    [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
    [[nodiscard]] bool has(std::string_view needle) const {
        for (const std::string& t : texts) if (t.find(needle) != std::string::npos) return true;
        return false;
    }
    mutable int amber{};
    mutable std::vector<std::string> texts;
};

void test_domain_and_gestures() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    check(controller.selection_domain() == SelectionDomain::Objects, "starts in the Objects domain");
    check(controller.tool_shortcut_text(EditorToolId::Select) == "Q", "tools unchanged");
    controller.key_down("m", false, false, false);
    check(controller.selection_domain() == SelectionDomain::Voxels, "M switches to the Voxels domain");
    const MenuAction* item = std::as_const(controller.workspace().menus()).find("select.domain_voxels");
    check(item && item->checked, "the Edit menu shows the domain");
    const auto hints = controller.viewport_tool_gestures();
    check(!hints.empty() && hints.front().find("click piece") != std::string::npos, "the hint explains the voxel gestures");
    Canvas chip;
    render_native_editor(chip, controller, 1280, 720);
    check(chip.has("Selecting: VOXELS (0)"), "the toolbar shows the domain");

    // Click a voxel of the cube: the whole connected cube is selected.
    click(controller, voxel_top(controller, {1, 3, 1}));
    check(controller.voxel_selection().size() == 64 && controller.voxel_selection_object() == 1, "click selects the piece");
    Canvas highlight;
    render_native_editor(highlight, controller, 1280, 720);
    check(highlight.amber > 10, "selected voxels are outlined");
    // Shift-click adds the other piece; Ctrl-click removes it again.
    click(controller, voxel_top(controller, {8, 1, 0}), 1U);
    check(controller.voxel_selection().size() == 72, "Shift-click adds a piece");
    click(controller, voxel_top(controller, {8, 1, 0}), 2U);
    check(controller.voxel_selection().size() == 64, "Ctrl-click removes it");
    // A plain click on empty space clears.
    const UiRect viewport = controller.layout().viewport;
    click(controller, {viewport.x + 6, viewport.y + 60});
    check(controller.voxel_selection().empty(), "clicking empty space clears");

    // Box over everything: only voxels facing the camera; with X-Ray every voxel in the box.
    const auto box = [&](std::uint32_t modifiers) {
        controller.pointer_move(viewport.x + 4, viewport.y + 40, modifiers);
        controller.pointer_down(PointerButton::Primary, viewport.x + 4, viewport.y + 40, modifiers);
        controller.pointer_move(viewport.x + viewport.width - 4, viewport.y + viewport.height - 4, modifiers);
        controller.pointer_up(PointerButton::Primary, viewport.x + viewport.width - 4, viewport.y + viewport.height - 4, modifiers);
    };
    box(0U);
    const std::size_t front = controller.voxel_selection().size();
    check(front > 8 && front < 72, "a box takes the visible voxels (" + std::to_string(front) + ")");
    check(!controller.voxel_selection().contains({1, 1, 1}), "hidden interior voxels are not taken");
    (void)controller.dispatch_action("view.xray");
    box(0U);
    check(controller.voxel_selection().size() == 72, "with X-Ray the box takes every voxel");
    (void)controller.dispatch_action("view.xray");

    // Ctrl+Shift-drag brush: radius 0 paints single voxels into the selection.
    (void)controller.dispatch_action("voxel.select_none");
    check(controller.voxel_selection().empty(), "Deselect Voxels");
    const Screen a = voxel_top(controller, {0, 3, 0});
    const Screen b = voxel_top(controller, {3, 3, 0});
    controller.pointer_move(a.x, a.y, 3U);
    controller.pointer_down(PointerButton::Primary, a.x, a.y, 3U);
    for (int i = 1; i <= 10; ++i) controller.pointer_move(a.x + (b.x - a.x) * i / 10, a.y + (b.y - a.y) * i / 10, 3U);
    controller.pointer_up(PointerButton::Primary, b.x, b.y, 3U);
    const std::size_t brushed = controller.voxel_selection().size();
    check(brushed >= 3 && brushed <= 8, "Ctrl+Shift-drag brushes a row of top voxels (" + std::to_string(brushed) + ")");

    // Grow / Shrink / Invert / Select All.
    check(controller.dispatch_action("voxel.select_grow") && controller.voxel_selection().size() > brushed, "Grow");
    check(controller.dispatch_action("edit.select_all") && controller.voxel_selection().size() == 72, "Ctrl+A selects every voxel in the Voxels domain");
    check(controller.dispatch_action("voxel.select_shrink") && controller.voxel_selection().size() == 8, "Shrink keeps the cube's core");
    check(controller.dispatch_action("voxel.select_invert") && controller.voxel_selection().size() == 64, "Invert");

    controller.key_down("m", false, false, false);
    check(controller.selection_domain() == SelectionDomain::Objects, "M goes back to objects");
}

void test_edits() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    EditorDocument& document = controller.workspace().document();
    (void)controller.dispatch_action("select.domain_voxels");
    // Delete with nothing selected never deletes the object.
    check(!controller.dispatch_action("edit.delete") && document.find_object(1), "Delete with no voxels selected does nothing");
    controller.select_voxels(1, voxel_connected_piece(*document.find_object(1)->voxels, {8, 0, 0}), SelectionOperation::Replace);
    check(controller.dispatch_action("edit.delete") && document.find_object(1)->voxels->occupied_voxel_count() == 64 &&
              controller.voxel_selection().empty(),
          "Delete removes the selected voxels");
    check(controller.dispatch_action("edit.undo") && document.find_object(1)->voxels->occupied_voxel_count() == 72, "undo");

    // Copy, then Paste: a new object in place, selected, Objects domain.
    controller.select_voxels(1, voxel_connected_piece(*document.find_object(1)->voxels, {8, 0, 0}), SelectionOperation::Replace);
    check(controller.dispatch_action("edit.copy"), "Copy voxels");
    const std::size_t objects = document.objects().size();
    check(controller.dispatch_action("edit.paste"), "Paste voxels");
    const auto pasted = controller.workspace().selected_object();
    const EditorObject* copy = pasted ? document.find_object(*pasted) : nullptr;
    check(document.objects().size() == objects + 1 && copy && copy->name == "Rocks (copy)" &&
              copy->voxels->occupied_voxel_count() == 8 && copy->voxels->material_at({9, 1, 1}) == 2,
          "the paste is a new object holding the copied voxels in place");
    check(controller.selection_domain() == SelectionDomain::Objects, "and is selected as an object, ready to move");
    check(controller.dispatch_action("edit.undo") && document.objects().size() == objects, "undo the paste");

    // Extract.
    controller.workspace().select_object(1);
    (void)controller.dispatch_action("select.domain_voxels");
    controller.select_voxels(1, voxel_connected_piece(*document.find_object(1)->voxels, {8, 0, 0}), SelectionOperation::Replace);
    check(controller.dispatch_action("voxel.extract_selection"), "Extract Selected Voxels");
    const auto extracted = controller.workspace().selected_object();
    check(extracted && *extracted != 1 && document.find_object(*extracted)->voxels->occupied_voxel_count() == 8 &&
              document.find_object(1)->voxels->occupied_voxel_count() == 64,
          "the selection becomes its own object, selected");
    check(controller.dispatch_action("edit.undo") && document.find_object(1)->voxels->occupied_voxel_count() == 72, "undo extract");

    // Cut + paste moves voxels into a new object.
    controller.workspace().select_object(1);
    (void)controller.dispatch_action("select.domain_voxels");
    controller.select_voxels(1, voxel_connected_piece(*document.find_object(1)->voxels, {8, 0, 0}), SelectionOperation::Replace);
    check(controller.dispatch_action("edit.cut") && document.find_object(1)->voxels->occupied_voxel_count() == 64, "Cut removes them");
    check(controller.dispatch_action("edit.paste"), "Paste after cut");
}

void test_move_and_limited_tools() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    EditorDocument& document = controller.workspace().document();
    (void)controller.dispatch_action("select.domain_voxels");
    controller.select_voxels(1, voxel_connected_piece(*document.find_object(1)->voxels, {8, 0, 0}), SelectionOperation::Replace);
    controller.set_active_tool(EditorToolId::Translate);
    const auto axes = controller.gizmo_axes();
    check(axes.size() == 3, "the Move gizmo sits on the voxel selection");
    const GizmoScreenAxis* xAxis = nullptr;
    for (const GizmoScreenAxis& axis : axes) if (axis.axis == 1) xAxis = &axis;
    if (xAxis) {
        const int sx = static_cast<int>(xAxis->start.x + (xAxis->end.x - xAxis->start.x) * 0.7F);
        const int sy = static_cast<int>(xAxis->start.y + (xAxis->end.y - xAxis->start.y) * 0.7F);
        const float dx = xAxis->end.x - xAxis->start.x;
        const float dy = xAxis->end.y - xAxis->start.y;
        controller.pointer_move(sx, sy);
        controller.pointer_down(PointerButton::Primary, sx, sy);
        // Drag along the axis by about the handle's own length (several voxels).
        controller.pointer_move(sx + static_cast<int>(dx * 0.5F), sy + static_cast<int>(dy * 0.5F));
        const auto preview = controller.voxel_move_preview();
        check(preview && preview->x > 0 && preview->y == 0 && preview->z == 0, "dragging the X handle previews a whole-voxel X offset");
        Canvas ghost;
        render_native_editor(ghost, controller, 1280, 720);
        controller.pointer_up(PointerButton::Primary, sx + static_cast<int>(dx * 0.5F), sy + static_cast<int>(dy * 0.5F));
        const int steps = preview ? preview->x : 0;
        check(steps > 0 && document.find_object(1)->voxels->material_at({8 + steps, 0, 0}) == 2 &&
                  (steps >= 2 ? document.find_object(1)->voxels->material_at({8, 0, 0}) == kAirMaterial : true),
              "release moves the voxels by " + std::to_string(steps));
        check(controller.voxel_selection().contains({8 + steps, 0, 0}), "the selection follows the voxels");
        check(controller.dispatch_action("edit.undo") && document.find_object(1)->voxels->material_at({8, 0, 0}) == 2, "one undo");
    }
    controller.set_active_tool(EditorToolId::Rotate);
    check(controller.gizmo_axes().empty() && controller.viewport_tool_gestures().front().find("objects") != std::string::npos,
          "Rotate has no voxel gizmo and says it works on objects");

    // Paint only touches selected voxels: select two top voxels, paint with a radius-2 brush.
    controller.select_voxels(1, {{0, 3, 0}, {1, 3, 0}}, SelectionOperation::Replace);
    controller.set_active_tool(EditorToolId::PaintMaterial);
    controller.set_active_material(5);
    for (int i = 0; i < 2; ++i) (void)controller.dispatch_action("voxel.brush_increase");
    const Screen at = voxel_top(controller, {0, 3, 0});
    click(controller, at);
    const EditorObject* rocks = document.find_object(1);
    check(rocks->voxels->material_at({0, 3, 0}) == 5 && rocks->voxels->material_at({1, 3, 0}) == 5, "selected voxels are painted");
    check(rocks->voxels->material_at({2, 3, 0}) == 1 && rocks->voxels->material_at({0, 3, 1}) == 1, "unselected voxels in the brush are not");
    // Flood Fill stays inside the selection too.
    controller.select_voxels(1, {{2, 0, 0}, {3, 0, 0}}, SelectionOperation::Replace);
    controller.set_active_material(6);
    check(controller.flood_fill_at(1, {2, 0, 0}), "fill inside the selection");
    check(rocks->voxels->material_at({3, 0, 0}) == 6 && rocks->voxels->material_at({1, 0, 0}) == 1, "the fill stopped at the selection");
    check(!controller.flood_fill_at(1, {0, 1, 0}), "starting outside the selection is refused");

    // The selection drops voxels that disappear (e.g. removed by another edit).
    (void)document.find_object(1)->voxels->set_voxel({2, 0, 0}, kAirMaterial);
    controller.update(0.016F);
    check(!controller.voxel_selection().contains({2, 0, 0}) && controller.voxel_selection().contains({3, 0, 0}), "selection prunes removed voxels");
}

// Review fixes: hidden / isolated objects, scene changes and Play, previews, caps, Extract rules,
// Cut on a locked object, menu reasons in the Voxels domain, and the gizmo following the selection.
void test_review_fixes() {
    {
        // A hidden object's voxels cannot stay selected (Delete must never act on unseen voxels).
        auto owner = make_controller();
        NativeEditorController& controller = *owner;
        EditorDocument& document = controller.workspace().document();
        (void)controller.dispatch_action("select.domain_voxels");
        controller.select_voxels(1, voxel_connected_piece(*document.find_object(1)->voxels, {8, 0, 0}), SelectionOperation::Replace);
        check(controller.voxel_selection().size() == 8, "review: selected");
        document.find_object(1)->flags.visible = false;
        controller.update(0.016F);
        check(controller.voxel_selection().empty(), "review: hiding the object clears its voxel selection");
        controller.select_voxels(1, {{8, 0, 0}}, SelectionOperation::Replace);
        check(controller.voxel_selection().empty(), "review: voxels of a hidden object cannot be selected");
        check(!controller.dispatch_action("edit.delete") && document.find_object(1)->voxels->occupied_voxel_count() == 72,
              "review: Delete does nothing to the hidden object");
        document.find_object(1)->flags.visible = true;

        // Isolation (#95): isolating another object clears the selection too.
        EditorObject other(2, "Other");
        fill_box(other, {0, 0, 0}, {1, 1, 1}, 3);
        other.transform.position = {5.0F, 0.0F, 0.0F};
        document.add_object(std::move(other));
        controller.select_voxels(1, voxel_connected_piece(*document.find_object(1)->voxels, {8, 0, 0}), SelectionOperation::Replace);
        controller.viewport_settings().isolatedObjects = {2};
        controller.update(0.016F);
        check(controller.voxel_selection().empty(), "review: isolating another object clears the selection");
        controller.viewport_settings().isolatedObjects.clear();
    }
    {
        // Menu reasons in the Voxels domain: Delete / Copy / Cut follow the voxel selection.
        auto owner = make_controller();
        NativeEditorController& controller = *owner;
        EditorDocument& document = controller.workspace().document();
        (void)controller.dispatch_action("select.domain_voxels");
        const MenuAction* del = std::as_const(controller.workspace().menus()).find("edit.delete");
        check(del && !del->enabled && del->disabledReason.find("No voxels selected") != std::string::npos,
              "review: Delete is disabled with a reason while no voxels are selected");
        const MenuAction* all = std::as_const(controller.workspace().menus()).find("voxel.select_all");
        check(all && all->enabled, "review: Select All Voxels is enabled with a voxel object");
        controller.select_voxels(1, voxel_connected_piece(*document.find_object(1)->voxels, {8, 0, 0}), SelectionOperation::Replace);
        del = std::as_const(controller.workspace().menus()).find("edit.delete");
        check(del && del->enabled, "review: Delete is enabled once voxels are selected");
        check(!controller.dispatch_action("voxel.select_none") || controller.voxel_selection().empty(), "review: deselect");
        check(!controller.dispatch_action("voxel.select_none"), "review: Deselect with nothing selected does not report success");

        // Cut on a locked object fails and leaves the clipboard alone.
        controller.select_voxels(1, {{0, 0, 0}}, SelectionOperation::Replace);
        check(controller.dispatch_action("edit.copy"), "review: copy one voxel");
        controller.select_voxels(1, voxel_connected_piece(*document.find_object(1)->voxels, {8, 0, 0}), SelectionOperation::Replace);
        document.find_object(1)->flags.locked = true;
        check(!controller.dispatch_action("edit.cut") && document.find_object(1)->voxels->occupied_voxel_count() == 72,
              "review: Cut on a locked object is refused");
        document.find_object(1)->flags.locked = false;
        check(controller.dispatch_action("edit.paste"), "review: paste");
        const auto pasted = controller.workspace().selected_object();
        check(pasted && document.find_object(*pasted)->voxels->occupied_voxel_count() == 1,
              "review: the failed Cut did not replace the clipboard");
    }
    {
        // Scene changes and Play drop the voxel selection (object ids restart per scene).
        auto owner = make_controller();
        NativeEditorController& controller = *owner;
        EditorDocument& document = controller.workspace().document();
        (void)controller.dispatch_action("select.domain_voxels");
        controller.select_voxels(1, voxel_connected_piece(*document.find_object(1)->voxels, {8, 0, 0}), SelectionOperation::Replace);
        (void)controller.dispatch_action("file.new_scene");
        if (controller.pending_destructive_confirmation()) controller.confirm_pending_destructive_action();
        check(controller.voxel_selection().empty() && controller.voxel_selection_object() == 0,
              "review: a new scene clears the voxel selection");
    }
    {
        auto owner = make_controller();
        NativeEditorController& controller = *owner;
        EditorDocument& document = controller.workspace().document();
        (void)controller.dispatch_action("select.domain_voxels");
        controller.select_voxels(1, voxel_connected_piece(*document.find_object(1)->voxels, {8, 0, 0}), SelectionOperation::Replace);
        check(controller.dispatch_action("physics.play"), "review: Play starts");
        check(controller.voxel_selection().empty(), "review: Play clears the voxel selection");
        check(!controller.dispatch_action("voxel.select_all"), "review: voxel selection commands are refused during Play");
        (void)controller.dispatch_action("physics.stop");
    }
    {
        // One preview at a time (#98): a voxel edit closes an open Slice preview.
        auto owner = make_controller();
        NativeEditorController& controller = *owner;
        EditorDocument& document = controller.workspace().document();
        check(controller.dispatch_action("voxel.slice") && controller.voxel_slice().active(), "review: Slice opens");
        (void)controller.dispatch_action("select.domain_voxels");
        controller.select_voxels(1, voxel_connected_piece(*document.find_object(1)->voxels, {8, 0, 0}), SelectionOperation::Replace);
        check(controller.dispatch_action("edit.delete"), "review: delete with a Slice preview open");
        check(!controller.voxel_slice().active(), "review: the voxel edit closed the Slice preview");
    }
    {
        // Caps: pieces, grow and invert stop early; select_voxels refuses an oversized region.
        const EditorObject rocks = make_rocks();
        check(voxel_connected_piece(*rocks.voxels, {0, 0, 0}, 10).size() == 11, "review: a capped piece stops just past the limit");
        check(grow_voxel_selection(*rocks.voxels, voxel_connected_piece(*rocks.voxels, {0, 0, 0}), 64).size() == 64,
              "review: grow below the cap is unchanged by it");
        check(invert_voxel_selection(*rocks.voxels, {}, 5).size() == 6, "review: a capped invert stops just past the limit");
        auto owner = make_controller();
        NativeEditorController& controller = *owner;
        (void)controller.dispatch_action("select.domain_voxels");
        controller.select_voxels(1, {{0, 0, 0}}, SelectionOperation::Replace);
        std::set<Int3> huge;
        for (int i = 0; huge.size() <= kMaxVoxelSelection; ++i) huge.insert({i % 1000, (i / 1000) % 1000, i / 1000000});
        controller.select_voxels(1, huge, SelectionOperation::Replace);
        check(controller.voxel_selection().size() == 1, "review: an oversized selection is refused and the old one kept");
    }
    {
        // Extract follows Separate Islands' rules: prefab instances and attached objects refuse;
        // the part keeps membership/tags components but not import source or other components.
        EditorDocument document("extract rules");
        EditorObject rocks = make_rocks();
        rocks.sourceAsset = "models/rocks.vox";
        Component membership;
        membership.type = "dve.membership";
        Component light;
        light.type = "dve.light";
        rocks.components = {membership, light};
        document.add_object(std::move(rocks));
        const std::set<Int3> pieceB = voxel_connected_piece(*document.find_object(1)->voxels, {8, 0, 0});
        document.find_object(1)->prefabLink = EditorPrefabLink{};
        check(!build_extract_voxels_command(document, 1, pieceB).command, "review: extract from a prefab instance is refused");
        document.find_object(1)->prefabLink.reset();
        document.find_object(1)->attachment = EditorAttachment{};
        check(!build_extract_voxels_command(document, 1, pieceB).command, "review: extract from an attached object is refused");
        document.find_object(1)->attachment.reset();
        ExtractVoxelsBuild build = build_extract_voxels_command(document, 1, pieceB);
        EditorCommandStack stack;
        check(build.command && stack.execute(document, std::move(build.command)).success, "review: extract executes");
        const EditorObject* part = document.find_object(build.newObject);
        bool keepsMembership = false, keepsLight = false;
        if (part)
            for (const Component& c : part->components) {
                keepsMembership = keepsMembership || c.type == "dve.membership";
                keepsLight = keepsLight || c.type == "dve.light";
            }
        check(part && keepsMembership && !keepsLight && part->sourceAsset.empty() && !part->attachment,
              "review: the extracted part keeps membership only and no import source");
    }
    {
        // The Move gizmo follows the selection (its centre is cached per selection change).
        auto owner = make_controller();
        NativeEditorController& controller = *owner;
        EditorDocument& document = controller.workspace().document();
        (void)controller.dispatch_action("select.domain_voxels");
        controller.set_active_tool(EditorToolId::Translate);
        controller.select_voxels(1, voxel_connected_piece(*document.find_object(1)->voxels, {8, 0, 0}), SelectionOperation::Replace);
        const auto first = controller.gizmo_axes();
        controller.select_voxels(1, voxel_connected_piece(*document.find_object(1)->voxels, {0, 0, 0}), SelectionOperation::Replace);
        const auto second = controller.gizmo_axes();
        check(!first.empty() && !second.empty() && std::abs(first.front().start.x - second.front().start.x) > 2.0F,
              "review: the gizmo moves to the new selection");
    }
}

} // namespace

int main() {
    test_core();
    test_domain_and_gestures();
    test_edits();
    test_move_and_limited_tools();
    test_review_fixes();
    if (g_failures != 0) {
        std::printf("dve_editor_voxel_selection_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("dve_editor_voxel_selection_tests: PASS\n");
    return 0;
}
