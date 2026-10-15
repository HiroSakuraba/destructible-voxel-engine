// Closed groups, group bounds and an editable pivot (artist worklist ART-035/036):
//   - a click on a piece of a closed group selects the outermost closed group;
//   - Open Group / double-click lets clicks reach the pieces, Close Group returns;
//   - window boxes take a group only when all its pieces are inside, crossing boxes when any is;
//   - Move, Rotate and nudge carry the pieces; Scale asks to open the group;
//   - Edit Group Pivot moves only the pivot; new groups put it at the bottom centre;
//   - a parent with its own geometry is not a group; imported models are closed groups.
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_project.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
bool near(float a, float b, float tolerance = 1.0e-3F) { return std::abs(a - b) <= tolerance; }

void fill_box(EditorObject& object, Int3 min, Int3 max, MaterialId material) {
    for (int x = min.x; x <= max.x; ++x)
        for (int y = min.y; y <= max.y; ++y)
            for (int z = min.z; z <= max.z; ++z) (void)object.voxels->set_voxel({x, y, z}, material);
}

EditorObject cube(EditorObjectId id, std::string name, Float3 position, std::optional<EditorObjectId> parent = std::nullopt) {
    EditorObject object(id, std::move(name));
    object.voxelSizeMeters = 0.25F;
    object.transform.position = position;
    object.parent = parent;
    fill_box(object, {0, 0, 0}, {3, 3, 3}, 1);  // 1 m cube
    return object;
}

// Outer (20, empty) holds Chair group (10, empty: Seat 11 + Back 12) and Lamp (21).
// Table (30) has its own voxels and a child Vase (31): a plain parent, not a group.
EditorDocument make_scene() {
    EditorDocument document("Groups");
    EditorObject outer(20, "Outer");
    document.add_object(std::move(outer));
    EditorObject chair(10, "Chair");
    chair.parent = 20;
    document.add_object(std::move(chair));
    document.add_object(cube(11, "Seat", {0.0F, 0.0F, 0.0F}, 10));
    document.add_object(cube(12, "Back", {0.0F, 1.0F, 0.0F}, 10));
    document.add_object(cube(21, "Lamp", {3.0F, 0.0F, 0.0F}, 20));
    document.add_object(cube(30, "Table", {-4.0F, 0.0F, 0.0F}));
    document.add_object(cube(31, "Vase", {-4.0F, 1.0F, 0.0F}, 30));
    return document;
}

std::unique_ptr<NativeEditorController> make_controller() {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(make_scene()));
    controller->resize(1280, 720);
    (void)controller->dispatch_action("view.frame_all");
    return controller;
}

ScreenPoint screen_of(NativeEditorController& controller, Float3 world) {
    return project_world_to_screen(controller.camera(), controller.layout().viewport, world);
}

// Click the front face (z = 0 side... any visible face) near the centre of a 1 m cube at `origin`.
void click_cube(NativeEditorController& controller, Float3 origin, std::uint32_t modifiers = 0) {
    const ScreenPoint p = screen_of(controller, {origin.x + 0.5F, origin.y + 0.5F, origin.z + 0.5F});
    const int x = static_cast<int>(p.x), y = static_cast<int>(p.y);
    controller.pointer_move(x, y, modifiers);
    controller.pointer_down(PointerButton::Primary, x, y, modifiers);
    controller.pointer_up(PointerButton::Primary, x, y, modifiers);
}

struct Canvas final : IEditorCanvas {
    void fill(UiRect, EditorColor) const override {}
    void outline(UiRect, EditorColor) const override {}
    void line(int, int, int, int, EditorColor, int) const override { ++lines; }
    void text(int, int, std::string_view value, EditorColor) const override { texts.emplace_back(value); }
    [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
    [[nodiscard]] bool has(std::string_view needle) const {
        for (const std::string& t : texts) if (t.find(needle) != std::string::npos) return true;
        return false;
    }
    mutable int lines{};
    mutable std::vector<std::string> texts;
};

std::optional<EditorObjectId> primary(NativeEditorController& controller) { return controller.workspace().selected_object(); }

void test_click_selects_outermost_group() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    check(controller.is_group(20) && controller.is_group(10), "empty parents with children are groups");
    check(!controller.is_group(30) && !controller.is_group(11), "a parent with voxels and a plain piece are not");
    check(controller.viewport_selection_target(11) == 20, "a seat click resolves to the outermost closed group");

    click_cube(controller, {0.0F, 0.0F, 0.0F});
    check(primary(controller) == 20U && controller.workspace().selection_count() == 1, "clicking the seat selects Outer");
    click_cube(controller, {-4.0F, 1.0F, 0.0F});
    check(primary(controller) == 31U, "the vase on a table (a parent with geometry) is picked directly");

    // Open Outer: the next click reaches the Chair group, not the seat.
    controller.workspace().select_object(20);
    check(controller.dispatch_action("edit.open_group") && controller.is_group_open(20), "Open Group opens Outer");
    click_cube(controller, {0.0F, 0.0F, 0.0F});
    check(primary(controller) == 10U, "inside Outer a seat click selects the Chair group");
    click_cube(controller, {3.0F, 0.0F, 0.0F});
    check(primary(controller) == 21U, "and a lamp click selects the lamp");

    // Close Group from a piece inside: Outer closes and is selected.
    check(controller.dispatch_action("edit.close_group") && !controller.is_group_open(20) && primary(controller) == 20U,
          "Close Group closes Outer and selects it");
    check(!controller.dispatch_action("edit.close_group"), "nothing left to close");
}

void test_double_click_opens_one_level() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    click_cube(controller, {0.0F, 0.0F, 0.0F});
    check(primary(controller) == 20U, "first click selects Outer");
    click_cube(controller, {0.0F, 0.0F, 0.0F});
    check(controller.is_group_open(20) && primary(controller) == 10U, "double-click opens Outer and selects the Chair");
    click_cube(controller, {0.0F, 0.0F, 0.0F});
    click_cube(controller, {0.0F, 0.0F, 0.0F});
    check(controller.is_group_open(10) && primary(controller) == 11U, "another double-click opens the Chair and selects the seat");

    Canvas canvas;
    render_native_editor(canvas, controller, 1280, 720);
    check(canvas.has("Chair (open)") && canvas.has("[open]"), "open groups are labelled in the viewport and hierarchy");

    // Close Group from the seat closes the Chair (nearest open group) and its insides only.
    check(controller.dispatch_action("edit.close_group") && !controller.is_group_open(10) && controller.is_group_open(20),
          "Close Group closes the nearest open group");
    check(primary(controller) == 10U, "and selects it");
    check(controller.dispatch_action("edit.close_all_groups") && controller.open_groups().empty(), "Close All Groups");
    check(!controller.dispatch_action("edit.close_all_groups"), "Close All Groups is refused when none are open");
}

void test_box_rules() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    const auto drag = [&](Float3 a, Float3 b) {
        const ScreenPoint pa = screen_of(controller, a), pb = screen_of(controller, b);
        controller.workspace().clear_selection();
        controller.pointer_move(static_cast<int>(pa.x), static_cast<int>(pa.y));
        controller.pointer_down(PointerButton::Primary, static_cast<int>(pa.x), static_cast<int>(pa.y));
        controller.pointer_move(static_cast<int>(pb.x), static_cast<int>(pb.y));
        controller.pointer_up(PointerButton::Primary, static_cast<int>(pb.x), static_cast<int>(pb.y));
    };
    // A screen box tightly around the lamp only, from the screen-space corners of its cube.
    float minX = 1e9F, minY = 1e9F, maxX = -1e9F, maxY = -1e9F;
    for (int i = 0; i < 8; ++i) {
        const ScreenPoint p = screen_of(controller, {3.0F + (i & 1), static_cast<float>((i >> 1) & 1), static_cast<float>((i >> 2) & 1)});
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
    }
    const auto box = [&](bool leftToRight) {
        const int left = static_cast<int>(minX) - 4, right = static_cast<int>(maxX) + 4;
        const int top = static_cast<int>(minY) - 4, bottom = static_cast<int>(maxY) + 4;
        controller.workspace().clear_selection();
        const int sx = leftToRight ? left : right, ex = leftToRight ? right : left;
        controller.pointer_move(sx, top);
        controller.pointer_down(PointerButton::Primary, sx, top);
        controller.pointer_move(ex, bottom);
        controller.pointer_up(PointerButton::Primary, ex, bottom);
    };
    (void)drag;
    box(true);
    check(controller.workspace().selection_count() == 0, "a window box around only the lamp does not take Outer");
    box(false);
    check(controller.workspace().selection_count() == 1 && primary(controller) == 20U, "a crossing box over the lamp takes Outer");
    controller.workspace().select_object(20);
    (void)controller.dispatch_action("edit.open_group");
    box(true);
    check(controller.workspace().selection_count() == 1 && primary(controller) == 21U, "with Outer open the window box takes the lamp");
}

void test_move_rotate_nudge_scale() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    EditorDocument& document = controller.workspace().document();
    controller.workspace().select_object(10);
    check(controller.nudge_selection({1.0F, 0.0F, 0.0F}).success, "nudge the Chair group");
    check(near(document.find_object(11)->transform.position.x, 1.0F) && near(document.find_object(12)->transform.position.x, 1.0F),
          "its pieces move with it");
    check(near(document.find_object(21)->transform.position.x, 3.0F), "the lamp (outside the Chair) stays");
    check(controller.dispatch_action("edit.undo") && near(document.find_object(11)->transform.position.x, 0.0F), "one undo");

    // Gizmo: drag the X handle of the Move gizmo on the selected group.
    controller.workspace().select_object(20);
    controller.set_active_tool(EditorToolId::Translate);
    const auto axes = controller.gizmo_axes();
    const GizmoScreenAxis* xAxis = nullptr;
    for (const GizmoScreenAxis& axis : axes) if (axis.axis == 1) xAxis = &axis;
    check(xAxis != nullptr, "the Move gizmo sits on the group");
    if (xAxis) {
        const int sx = static_cast<int>(xAxis->start.x + (xAxis->end.x - xAxis->start.x) * 0.7F);
        const int sy = static_cast<int>(xAxis->start.y + (xAxis->end.y - xAxis->start.y) * 0.7F);
        const int ex = sx + static_cast<int>((xAxis->end.x - xAxis->start.x) * 0.8F);
        const int ey = sy + static_cast<int>((xAxis->end.y - xAxis->start.y) * 0.8F);
        controller.pointer_move(sx, sy);
        controller.pointer_down(PointerButton::Primary, sx, sy);
        controller.pointer_move(ex, ey);
        controller.pointer_up(PointerButton::Primary, ex, ey);
        const float dx = document.find_object(20)->transform.position.x;
        check(dx > 0.1F, "the gizmo moved Outer");
        check(near(document.find_object(11)->transform.position.x, dx) && near(document.find_object(12)->transform.position.x, dx) &&
                  near(document.find_object(21)->transform.position.x, 3.0F + dx),
              "every piece, nested ones too, moved by the same amount");
        check(controller.dispatch_action("edit.undo") && near(document.find_object(21)->transform.position.x, 3.0F),
              "one undo puts them all back");
    }

    // Scale has no meaning for a group's empty transform: it asks to open the group.
    controller.workspace().select_object(20);
    controller.set_active_tool(EditorToolId::Scale);
    const auto scaleAxes = controller.gizmo_axes();
    check(!scaleAxes.empty(), "the Scale gizmo shows on the group");
    if (!scaleAxes.empty()) {
        const GizmoScreenAxis& axis = scaleAxes.front();
        const int sx = static_cast<int>(axis.start.x + (axis.end.x - axis.start.x) * 0.9F);
        const int sy = static_cast<int>(axis.start.y + (axis.end.y - axis.start.y) * 0.9F);
        controller.pointer_move(sx, sy);
        controller.pointer_down(PointerButton::Primary, sx, sy);
        controller.pointer_up(PointerButton::Primary, sx, sy);
        check(controller.status().text.find("Scale works on pieces") != std::string::npos,
              "Scale on a group says to open it: " + controller.status().text);
    }

    // A locked piece blocks moving its group.
    document.find_object(12)->flags.locked = true;
    controller.set_active_tool(EditorToolId::Translate);
    controller.workspace().select_object(10);
    const auto lockedAxes = controller.gizmo_axes();
    check(!lockedAxes.empty(), "the Move gizmo shows on the Chair");
    if (!lockedAxes.empty()) {
        const GizmoScreenAxis& axis = lockedAxes.front();
        const int sx = static_cast<int>(axis.start.x + (axis.end.x - axis.start.x) * 0.7F);
        const int sy = static_cast<int>(axis.start.y + (axis.end.y - axis.start.y) * 0.7F);
        controller.pointer_move(sx, sy);
        controller.pointer_down(PointerButton::Primary, sx, sy);
        controller.pointer_move(sx + 40, sy);
        controller.pointer_up(PointerButton::Primary, sx + 40, sy);
        check(controller.status().text.find("Unlock Back") != std::string::npos && near(document.find_object(11)->transform.position.x, 0.0F),
              "a locked piece blocks moving its group: " + controller.status().text);
    }
}

void test_pivot() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    EditorDocument& document = controller.workspace().document();
    controller.workspace().select_object(10);
    check(controller.dispatch_action("edit.pivot_bottom_center"), "Pivot to Bottom Centre");
    const Float3 pivot = document.find_object(10)->transform.position;
    check(near(pivot.x, 0.5F) && near(pivot.y, 0.0F) && near(pivot.z, 0.5F), "the pivot sits under the chair's centre");
    check(near(document.find_object(11)->transform.position.x, 0.0F), "the pieces did not move");
    check(near(controller.current_pivot().x, 0.5F), "the gizmo uses the group's pivot");
    check(controller.dispatch_action("edit.pivot_center") && near(document.find_object(10)->transform.position.y, 1.0F),
          "Pivot to Centre");
    check(controller.dispatch_action("edit.undo") && near(document.find_object(10)->transform.position.y, 0.0F), "undo");

    check(controller.dispatch_action("edit.edit_pivot") && controller.pivot_editing(), "Edit Group Pivot starts");
    check(controller.nudge_selection({0.0F, 0.0F, 2.0F}).success, "nudge while editing the pivot");
    check(near(document.find_object(10)->transform.position.z, 2.5F) && near(document.find_object(11)->transform.position.z, 0.0F),
          "only the pivot moved");
    Canvas canvas;
    render_native_editor(canvas, controller, 1280, 720);
    check(canvas.has("Editing pivot"), "the viewport says the pivot is being edited");
    controller.key_down("escape", false, false, false);
    check(!controller.pivot_editing(), "Esc finishes");
    check(controller.nudge_selection({1.0F, 0.0F, 0.0F}).success && near(document.find_object(11)->transform.position.x, 1.0F),
          "afterwards the group moves its pieces again");

    controller.workspace().select_object(11);
    check(!controller.dispatch_action("edit.edit_pivot"), "Edit Group Pivot needs a group");
    controller.workspace().select_object(10);
    (void)controller.dispatch_action("edit.edit_pivot");
    controller.workspace().select_object(21);
    controller.update(0.016F);
    check(!controller.pivot_editing(), "selecting something else ends pivot editing");
}

// Review fixes: a pivot move that changes nothing adds no undo step; locked groups and Play refuse.
void test_pivot_refusals() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    EditorDocument& document = controller.workspace().document();
    controller.workspace().select_object(10);
    check(controller.dispatch_action("edit.pivot_bottom_center"), "first Pivot to Bottom Centre");
    const std::size_t steps = controller.workspace().commands().size();
    check(!controller.dispatch_action("edit.pivot_bottom_center") && controller.workspace().commands().size() == steps,
          "again: already there, no undo step");
    document.find_object(10)->flags.locked = true;
    const Float3 before = document.find_object(10)->transform.position;
    check(!controller.dispatch_action("edit.pivot_center") && near(document.find_object(10)->transform.position.y, before.y),
          "a locked group's pivot is refused");
    check(!controller.dispatch_action("edit.edit_pivot") && !controller.pivot_editing(), "Edit Pivot refuses a locked group");
    check(controller.status().text.find("Unlock Chair") != std::string::npos, "and says why: " + controller.status().text);
    document.find_object(10)->flags.locked = false;
    std::string error;
    check(controller.workspace().set_mode(EditorMode::Simulate, &error), "enter Simulate");
    check(!controller.dispatch_action("edit.pivot_center") && controller.workspace().commands().size() == steps,
          "no pivot edit during Simulate");
    check(controller.workspace().set_mode(EditorMode::Edit, &error), "back to Edit");
}

void test_new_group_pivot() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    EditorDocument& document = controller.workspace().document();
    controller.workspace().select_object(30);
    controller.workspace().add_to_selection(21);
    check(controller.dispatch_action("edit.group"), "group the table and the lamp");
    const auto group = primary(controller);
    check(group && controller.is_group(*group), "the new group is a group and is selected");
    if (group) {
        // Table spans x -4..-3, its vase sits on it (y up to 2); lamp spans x 3..4, y 0..1.
        const Float3 pivot = document.find_object(*group)->transform.position;
        check(near(pivot.x, 0.0F) && near(pivot.y, 0.0F) && near(pivot.z, 0.5F),
              "its pivot is at the bottom centre: " + std::to_string(pivot.x) + "," + std::to_string(pivot.y) + "," + std::to_string(pivot.z));
        click_cube(controller, {3.0F, 0.0F, 0.0F});
        check(primary(controller) == *group, "and it is closed: a lamp click selects it");
    }
}

void test_import_group_closed() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "dve_closed_group_import";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "models");
    std::ofstream(root / "models" / "crate.obj")
        << "o Crate\n"
           "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
           "f 1 3 2\nf 1 4 3\nf 5 6 7\nf 5 7 8\nf 1 2 6\nf 1 6 5\nf 4 8 7\nf 4 7 3\nf 1 5 8\nf 1 8 4\nf 2 3 7\nf 2 7 6\n";
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.configure_ai_assistant(root);
    controller.update(0.016F);
    check(controller.start_asset_scan(false), "scan assets");
    const EditorAssetRecord* record = controller.asset_database().find_path("models/crate.obj");
    if (record) {
        controller.asset_browser_state().selectedId = record->id;
        controller.update(0.016F);
        (void)controller.dispatch_action("file.import");
        (void)controller.accept_import_options();
        const auto group = primary(controller);
        check(group && controller.is_group(*group), "an imported model is a closed group");
        check(!controller.dispatch_action("edit.edit_pivot") &&
                  controller.status().text.find("model origin") != std::string::npos,
              "its pivot stays at the model origin (Reimport places parts from it)");
    } else check(false, "the model is indexed");
    std::filesystem::remove_all(root);
}

void test_pruning() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.workspace().select_object(20);
    (void)controller.dispatch_action("edit.open_group");
    check(controller.dispatch_action("edit.ungroup"), "ungroup Outer");
    controller.update(0.016F);
    check(!controller.is_group_open(20), "a dissolved group is no longer open");
}
}  // namespace

int main() {
    test_pivot_refusals();
    test_click_selects_outermost_group();
    test_double_click_opens_one_level();
    test_box_rules();
    test_move_rotate_nudge_scale();
    test_pivot();
    test_new_group_pivot();
    test_import_group_closed();
    test_pruning();
    if (g_failures == 0) std::printf("closed group tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
