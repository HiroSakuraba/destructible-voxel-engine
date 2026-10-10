// Editor-level tests for authored voxel Booleans (ART-060, with the ART-068 undo, ART-113
// preview/commit/cancel and ART-114 disabled-reason contracts it relies on).
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_voxel_boolean.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x + " (line " + std::to_string(__LINE__) + ")"); } while (false)

namespace {
using namespace dve;
using namespace dve::editor;

constexpr EditorObjectId kWall = 10;
constexpr EditorObjectId kCutter = 11;
constexpr EditorObjectId kEmpty = 12;
constexpr EditorObjectId kFar = 13;
constexpr EditorObjectId kChild = 14;

void fill_box(VoxelObject& object, Int3 lo, Int3 hi, MaterialId material) {
    for (int z = lo.z; z < hi.z; ++z)
        for (int y = lo.y; y < hi.y; ++y)
            for (int x = lo.x; x < hi.x; ++x) (void)object.set_voxel({x, y, z}, material);
}

EditorDocument make_document() {
    EditorDocument document("Boolean test");
    EditorObject wall(kWall, "Wall");
    fill_box(*wall.voxels, {0, 0, 0}, {8, 4, 2}, 1);
    wall.anchors = {{0, 0, 0}, {4, 0, 0}, {7, 0, 1}};
    document.add_object(std::move(wall));

    EditorObject cutter(kCutter, "Cutter");
    fill_box(*cutter.voxels, {0, 0, 0}, {2, 2, 4}, 2);
    cutter.transform = make_rigid_transform({0.3F, 0.0F, -0.1F}, {}); // x 3..4, z -1..2 in wall grid
    cutter.anchors = {{0, 0, 0}};
    document.add_object(std::move(cutter));

    document.add_object(EditorObject(kEmpty, "Empty"));

    EditorObject far(kFar, "Far");
    fill_box(*far.voxels, {0, 0, 0}, {2, 2, 2}, 3);
    far.transform = make_rigid_transform({5.0F, 0.0F, 0.0F}, {});
    document.add_object(std::move(far));
    document.mark_clean();
    return document;
}

struct ObjectSnapshot {
    std::string name;
    std::optional<EditorObjectId> parent;
    RigidTransform transform{};
    EditorObjectFlags flags{};
    float voxelSize{};
    std::vector<SparseVoxelStateEntry> voxels;
    std::vector<Int3> anchors;
    bool operator==(const ObjectSnapshot& o) const {
        const auto same_entries = [](const auto& a, const auto& b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
                return x.voxel == y.voxel && x.material == y.material;
            });
        };
        return name == o.name && parent == o.parent && flags == o.flags && voxelSize == o.voxelSize &&
               transform.position.x == o.transform.position.x && transform.position.y == o.transform.position.y &&
               transform.position.z == o.transform.position.z && transform.rotation.w == o.transform.rotation.w &&
               same_entries(voxels, o.voxels) && anchors == o.anchors;
    }
};

std::map<EditorObjectId, ObjectSnapshot> snapshot(const EditorDocument& document) {
    std::map<EditorObjectId, ObjectSnapshot> result;
    for (const auto& [id, object] : document.objects()) {
        const VoxelObjectState state = capture_voxel_object_state(object);
        result[id] = {object.name, object.parent, object.transform, object.flags, object.voxelSizeMeters,
                      state.voxels, state.anchors};
    }
    return result;
}

const MenuAction& action(const NativeEditorController& controller, std::string_view id) {
    const MenuAction* found = std::as_const(controller.workspace().menus()).find(id);
    if (!found) throw std::runtime_error("missing action " + std::string(id));
    return *found;
}

void select(NativeEditorController& controller, std::vector<EditorObjectId> ids) {
    controller.workspace().clear_selection();
    for (EditorObjectId id : ids) controller.workspace().add_to_selection(id); // last = active
    controller.update(0.0F);
}

std::unique_ptr<NativeEditorController> make_controller() {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace{make_document()});
    controller->resize(1280, 800);
    controller->update(0.0F);
    return controller;
}

class RecordingCanvas final : public IEditorCanvas {
public:
    void fill(UiRect, EditorColor) const override { ++fills; }
    void outline(UiRect, EditorColor) const override { ++outlines; }
    void line(int, int, int, int, EditorColor, int) const override {}
    void text(int, int, std::string_view value, EditorColor) const override { texts.emplace_back(value); }
    [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
    mutable std::size_t fills{};
    mutable std::size_t outlines{};
    mutable std::vector<std::string> texts;
    [[nodiscard]] bool contains(std::string_view needle) const {
        return std::any_of(texts.begin(), texts.end(), [&](const std::string& t) { return t.find(needle) != std::string::npos; });
    }
};

void test_selection_reasons() {
    EditorDocument document = make_document();
    auto r = evaluate_voxel_boolean_selection(document, {}, std::nullopt);
    CHECK(!r.valid && r.reason.find("Select two or more voxel objects") != std::string::npos);
    r = evaluate_voxel_boolean_selection(document, {kWall}, kWall);
    CHECK(!r.valid && r.reason.find("Ctrl+click a second voxel object") != std::string::npos);
    r = evaluate_voxel_boolean_selection(document, {kWall, kEmpty}, kWall);
    CHECK(!r.valid && r.reason.find("'Empty' has no voxels") != std::string::npos);
    r = evaluate_voxel_boolean_selection(document, {kWall, kCutter}, kWall);
    CHECK(r.valid && r.target == kWall && r.operands == std::vector<EditorObjectId>{kCutter});
    r = evaluate_voxel_boolean_selection(document, {kWall, kCutter, kFar}, kCutter);
    CHECK(r.valid && r.target == kCutter && (r.operands == std::vector<EditorObjectId>{kWall, kFar}));
    r = evaluate_voxel_boolean_selection(document, {kWall, kCutter}, std::nullopt);
    CHECK(r.valid && r.target == kWall);

    document.find_object(kWall)->flags.locked = true;
    r = evaluate_voxel_boolean_selection(document, {kWall, kCutter}, kWall);
    CHECK(!r.valid && r.reason.find("locked") != std::string::npos && r.reason.find("Inspector") != std::string::npos);
    document.find_object(kWall)->flags.locked = false;

    document.find_object(kCutter)->flags.locked = true;
    CHECK(evaluate_voxel_boolean_selection(document, {kWall, kCutter}, kWall, VoxelBooleanOperandPolicy::Hide).valid);
    r = evaluate_voxel_boolean_selection(document, {kWall, kCutter}, kWall, VoxelBooleanOperandPolicy::Delete);
    CHECK(!r.valid && r.reason.find("set operands to Hide or Keep") != std::string::npos);
    document.find_object(kCutter)->flags.locked = false;

    EditorObject child(kChild, "Child");
    fill_box(*child.voxels, {0, 0, 0}, {1, 1, 1}, 1);
    child.parent = kCutter;
    document.add_object(std::move(child));
    r = evaluate_voxel_boolean_selection(document, {kWall, kCutter}, kWall, VoxelBooleanOperandPolicy::Delete);
    CHECK(!r.valid && r.reason.find("child objects") != std::string::npos);
    r = evaluate_voxel_boolean_selection(document, {kChild, kCutter}, kChild, VoxelBooleanOperandPolicy::Delete);
    CHECK(!r.valid && r.reason.find("is a child of operand") != std::string::npos);
    CHECK(evaluate_voxel_boolean_selection(document, {kChild, kCutter}, kChild, VoxelBooleanOperandPolicy::Hide).valid);
}

void test_command_enable_disable_and_palette() {
    const auto owner = make_controller();
    NativeEditorController& controller = *owner;
    select(controller, {kWall});
    const MenuAction& unionAction = action(controller, "voxel.boolean_union");
    CHECK(!unionAction.enabled);
    CHECK(unionAction.disabledReason.find("Ctrl+click a second voxel object") != std::string::npos);
    CHECK(unionAction.menu == "Tools" && unionAction.section == "Voxel Boolean");
    CHECK(!action(controller, "voxel.boolean_commit").enabled);
    CHECK(!action(controller, "voxel.boolean_cancel").enabled);
    CHECK(!controller.dispatch_action("voxel.boolean_union"));
    CHECK(controller.status().error && !controller.voxel_boolean().active());

    select(controller, {kCutter, kEmpty});
    CHECK(!action(controller, "voxel.boolean_difference").enabled);
    CHECK(action(controller, "voxel.boolean_difference").disabledReason.find("has no voxels") != std::string::npos);

    select(controller, {kCutter, kWall});
    CHECK(action(controller, "voxel.boolean_union").enabled);
    CHECK(action(controller, "voxel.boolean_difference").enabled);
    CHECK(action(controller, "voxel.boolean_intersection").enabled);
    CHECK(action(controller, "voxel.boolean_union").disabledReason.empty());
    CHECK(action(controller, "voxel.boolean_operands_hide").checked);

    // Reachable from the Tools menu and the Command Center by CSG vocabulary.
    const auto tools = controller.menu_actions("Tools");
    CHECK(std::any_of(tools.begin(), tools.end(), [](const MenuAction& a) { return a.id == "voxel.boolean_difference"; }));
    const auto search = controller.workspace().menus().search("csg subtract", 16);
    CHECK(std::any_of(search.begin(), search.end(), [](const MenuAction& a) { return a.id == "voxel.boolean_difference"; }));
}

void test_preview_does_not_touch_document_and_cancel_restores() {
    const auto owner = make_controller();
    NativeEditorController& controller = *owner;
    select(controller, {kCutter, kWall}); // Wall is active => target A
    controller.frame_selection();
    const auto before = snapshot(controller.workspace().document());
    const std::size_t history = controller.workspace().commands().size();
    const std::uint64_t wallRevision = controller.workspace().document().find_object(kWall)->voxels->revision();

    CHECK(controller.dispatch_action("voxel.boolean_difference"));
    CHECK(controller.voxel_boolean().active());
    CHECK(controller.voxel_boolean().target() == kWall);
    CHECK(controller.voxel_boolean().operands() == std::vector<EditorObjectId>{kCutter});
    CHECK(controller.voxel_boolean().result().stats.removedVoxels == 2 * 2 * 2);
    CHECK(action(controller, "voxel.boolean_commit").enabled);
    CHECK(action(controller, "voxel.boolean_cancel").enabled);
    CHECK(snapshot(controller.workspace().document()) == before);
    CHECK(controller.workspace().document().find_object(kWall)->voxels->revision() == wallRevision);

    // Preview visuals: A/B labels, the explanation panel and changed-cell markers.
    const auto markers = controller.voxel_boolean_preview_markers();
    CHECK(!markers.empty());
    CHECK(std::all_of(markers.begin(), markers.end(), [](const VoxelBooleanPreviewMarker& m) {
        return m.kind == VoxelBooleanPreviewMarker::Kind::Removed;
    }));
    RecordingCanvas canvas;
    render_native_editor(canvas, controller, 1280, 800);
    CHECK(canvas.contains("Boolean Difference"));
    CHECK(canvas.contains("A target"));
    CHECK(canvas.contains("B operand"));
    CHECK(canvas.contains("'Wall'"));
    CHECK(canvas.contains("Enter commit"));

    // Operation chooser.
    controller.key_down("1", false, false, false);
    CHECK(controller.voxel_boolean().operation() == VoxelBooleanOperation::Union);
    CHECK(controller.voxel_boolean().result().stats.addedVoxels == 2 * 2 * 2); // z = -1 and 2 slices
    controller.key_down("3", false, false, false);
    CHECK(controller.voxel_boolean().operation() == VoxelBooleanOperation::Intersection);
    CHECK(controller.voxel_boolean_preview_lines().front().find("Intersection") != std::string::npos);

    // Esc restores exactly: nothing was written, no undo entry.
    controller.key_down("Escape", false, false, false);
    CHECK(!controller.voxel_boolean().active());
    CHECK(snapshot(controller.workspace().document()) == before);
    CHECK(controller.workspace().commands().size() == history);
    CHECK(!controller.workspace().document().dirty());
    CHECK(controller.status().text.find("unchanged") != std::string::npos);

    // Cancel through the command as well; a selection change also cancels.
    CHECK(controller.dispatch_action("voxel.boolean_union"));
    CHECK(controller.dispatch_action("voxel.boolean_cancel"));
    CHECK(!controller.voxel_boolean().active());
    CHECK(controller.dispatch_action("voxel.boolean_union"));
    select(controller, {kWall});
    CHECK(!controller.voxel_boolean().active());
    CHECK(snapshot(controller.workspace().document()) == before);
}

void test_commit_undo_redo_exact_hide_policy() {
    const auto owner = make_controller();
    NativeEditorController& controller = *owner;
    select(controller, {kCutter, kWall});
    const auto before = snapshot(controller.workspace().document());
    const std::size_t history = controller.workspace().commands().size();
    CHECK(controller.dispatch_action("voxel.boolean_difference"));
    controller.key_down("Return", false, false, false);
    CHECK(!controller.voxel_boolean().active());
    CHECK(controller.workspace().commands().size() == history + 1U); // exactly one undo step
    CHECK(controller.workspace().commands().undo_label().find("Boolean Difference") != std::string_view::npos);
    CHECK(controller.status().text.find("collision rebuilt") != std::string::npos);
    CHECK(controller.voxel_boolean().last_commit()->collisionBoxes > 0U);

    const EditorDocument& document = controller.workspace().document();
    const EditorObject* wall = document.find_object(kWall);
    CHECK(wall->voxels->occupied_voxel_count() == 64U - 8U);
    for (int x = 3; x <= 4; ++x) for (int y = 0; y < 2; ++y) for (int z = 0; z < 2; ++z) CHECK(!wall->voxels->occupied_at({x, y, z}));
    CHECK(wall->voxels->material_at({0, 0, 0}) == 1 && wall->voxels->material_at({3, 2, 0}) == 1);
    CHECK(wall->anchors.contains({0, 0, 0}) && !wall->anchors.contains({4, 0, 0}) && wall->anchors.contains({7, 0, 1}));
    CHECK(!document.find_object(kCutter)->flags.visible);                  // Hide policy
    CHECK(document.find_object(kCutter)->voxels->occupied_voxel_count() == 16U);
    CHECK(controller.workspace().selection_count() == 1U && controller.workspace().selected_object() == kWall);
    CHECK(document.dirty());
    const auto after = snapshot(document);

    CHECK(controller.dispatch_action("edit.undo"));
    CHECK(snapshot(controller.workspace().document()) == before);
    CHECK(controller.dispatch_action("edit.redo"));
    CHECK(snapshot(controller.workspace().document()) == after);
    CHECK(controller.dispatch_action("edit.undo"));
    CHECK(snapshot(controller.workspace().document()) == before);
    CHECK(controller.workspace().document().find_object(kCutter)->flags.visible);
}

void test_collision_off_target_reports_no_proxy() {
    const auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.workspace().document().find_object(kWall)->flags.collisionEnabled = false;
    select(controller, {kCutter, kWall});
    CHECK(controller.dispatch_action("voxel.boolean_difference"));
    CHECK(controller.dispatch_action("voxel.boolean_commit"));
    CHECK(!controller.voxel_boolean().last_commit()->collisionEnabled);
    CHECK(controller.voxel_boolean().last_commit()->collisionBoxes == 0U);
    CHECK(controller.status().text.find("collision off on target") != std::string::npos);
}

void test_delete_and_keep_policies() {
    const auto owner = make_controller();
    NativeEditorController& controller = *owner;
    select(controller, {kCutter, kWall});
    const auto before = snapshot(controller.workspace().document());
    CHECK(controller.dispatch_action("voxel.boolean_operands_delete"));
    CHECK(action(controller, "voxel.boolean_operands_delete").checked);
    CHECK(controller.dispatch_action("voxel.boolean_union"));
    CHECK(controller.voxel_boolean().result().stats.anchorsTransferred == 1U); // cutter (0,0,0) -> wall (3,0,-1)
    CHECK(controller.dispatch_action("voxel.boolean_commit"));
    const EditorDocument& document = controller.workspace().document();
    CHECK(document.find_object(kCutter) == nullptr);
    const EditorObject* wall = document.find_object(kWall);
    CHECK(wall->voxels->occupied_voxel_count() == 64U + 8U);
    CHECK(wall->voxels->material_at({3, 0, -1}) == 2 && wall->voxels->material_at({3, 0, 0}) == 1); // A keeps overlap
    CHECK(wall->anchors.contains({3, 0, -1}) && wall->anchors.size() == 4U);
    const auto after = snapshot(document);
    CHECK(controller.dispatch_action("edit.undo"));
    CHECK(snapshot(controller.workspace().document()) == before);  // operand restored with its id, voxels, anchors
    CHECK(controller.dispatch_action("edit.redo"));
    CHECK(snapshot(controller.workspace().document()) == after);
    CHECK(controller.dispatch_action("edit.undo"));

    // Keep policy + operand material on overlap (M), with S swapping A/B.
    select(controller, {kCutter, kWall});
    CHECK(controller.dispatch_action("voxel.boolean_operands_keep"));
    CHECK(controller.dispatch_action("voxel.boolean_union"));
    controller.key_down("m", false, false, false);
    CHECK(controller.voxel_boolean().overlap_material() == VoxelBooleanOverlapMaterial::TakeOperand);
    CHECK(controller.voxel_boolean().result().stats.recoloredVoxels == 8U);
    controller.key_down("s", false, false, false);
    CHECK(controller.voxel_boolean().active() && controller.voxel_boolean().target() == kCutter);
    controller.update(0.0F); // the swap updated the active selection, so the preview survives
    CHECK(controller.voxel_boolean().active() && controller.voxel_boolean().target() == kCutter);
    controller.key_down("s", false, false, false);
    CHECK(controller.voxel_boolean().target() == kWall);
    controller.key_down("Return", false, false, false);
    CHECK(controller.workspace().document().find_object(kCutter)->flags.visible);
    CHECK(controller.workspace().document().find_object(kWall)->voxels->material_at({3, 0, 0}) == 2);
    CHECK(controller.dispatch_action("edit.undo"));
    CHECK(snapshot(controller.workspace().document()) == before);
}

void test_empty_result_and_failure_restore() {
    const auto owner = make_controller();
    NativeEditorController& controller = *owner;
    select(controller, {kFar, kWall});
    const auto before = snapshot(controller.workspace().document());
    const std::size_t history = controller.workspace().commands().size();
    CHECK(controller.dispatch_action("voxel.boolean_intersection"));
    CHECK(controller.voxel_boolean().active() && !controller.voxel_boolean().can_commit());
    CHECK(controller.status().error && controller.status().text.find("Intersection would be empty") != std::string::npos);
    CHECK(!action(controller, "voxel.boolean_commit").enabled);
    CHECK(action(controller, "voxel.boolean_commit").disabledReason.find("does not overlap") != std::string::npos);
    const auto lines = controller.voxel_boolean_preview_lines();
    CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string& l) { return l.starts_with("Cannot commit"); }));
    controller.key_down("Return", false, false, false);
    CHECK(controller.voxel_boolean().active()); // still open so the user can switch or cancel
    CHECK(controller.status().error);
    CHECK(controller.workspace().commands().size() == history);
    CHECK(snapshot(controller.workspace().document()) == before);
    controller.key_down("2", false, false, false); // Difference: no overlap => no change
    CHECK(!controller.voxel_boolean().can_commit());
    controller.key_down("1", false, false, false); // Union of separate pieces is allowed with a warning
    CHECK(controller.voxel_boolean().can_commit());
    CHECK(std::any_of(controller.voxel_boolean().result().diagnostics.begin(), controller.voxel_boolean().result().diagnostics.end(),
                      [](const VoxelBooleanDiagnostic& d) { return d.code == VoxelBooleanDiagnosticCode::NoOverlap; }));
    controller.key_down("Escape", false, false, false);

    // A failure at commit time (target locked while previewing) leaves the scene untouched.
    select(controller, {kCutter, kWall});
    CHECK(controller.dispatch_action("voxel.boolean_difference"));
    controller.workspace().document().find_object(kWall)->flags.locked = true;
    const CommandResult result = controller.commit_voxel_boolean();
    CHECK(!result.success && result.message.find("locked") != std::string::npos);
    controller.workspace().document().find_object(kWall)->flags.locked = false;
    auto expected = before;
    CHECK(snapshot(controller.workspace().document()) == expected);
    CHECK(controller.workspace().commands().size() == history);
    CHECK(controller.voxel_boolean().active());
    controller.key_down("Escape", false, false, false);
}

void test_preview_tracks_live_edits_and_stale_commands() {
    const auto owner = make_controller();
    NativeEditorController& controller = *owner;
    select(controller, {kCutter, kWall});
    CHECK(controller.dispatch_action("voxel.boolean_difference"));
    CHECK(controller.voxel_boolean().result().stats.removedVoxels == 8U);
    // Another tool (or the live MCP host) edits the target while the preview is open.
    (void)controller.workspace().document().find_object(kWall)->voxels->set_voxel({3, 0, 0}, kAirMaterial);
    controller.update(0.0F);
    CHECK(controller.voxel_boolean().active());
    CHECK(controller.voxel_boolean().result().stats.removedVoxels == 7U);
    controller.key_down("Escape", false, false, false);

    // A stale ApplyVoxelBooleanCommand refuses to run rather than corrupting the object.
    EditorDocument document = make_document();
    ApplyVoxelBooleanCommand command(kWall, {{{3, 0, 0}, 2, kAirMaterial}}, {{0, 0, 0}, {4, 0, 0}, {7, 0, 1}}, {}, "stale");
    const auto before = snapshot(document);
    CHECK(!command.execute(document).success);
    CHECK(snapshot(document) == before);
}

void test_play_mode_disables() {
    const auto owner = make_controller();
    NativeEditorController& controller = *owner;
    select(controller, {kCutter, kWall});
    CHECK(controller.dispatch_action("voxel.boolean_union"));
    if (controller.dispatch_action("physics.simulate")) {
        CHECK(!controller.voxel_boolean().active());
        controller.update(0.0F);
        CHECK(!action(controller, "voxel.boolean_union").enabled);
        CHECK(action(controller, "voxel.boolean_union").disabledReason.find("Stop Play or Simulate") != std::string::npos);
        (void)controller.dispatch_action("physics.stop");
    }
}

} // namespace

int main() {
    try {
        test_selection_reasons();
        test_command_enable_disable_and_palette();
        test_preview_does_not_touch_document_and_cancel_restores();
        test_commit_undo_redo_exact_hide_policy();
        test_collision_off_target_reports_no_proxy();
        test_delete_and_keep_policies();
        test_empty_result_and_failure_restore();
        test_preview_tracks_live_edits_and_stale_commands();
        test_play_mode_disables();
        std::cout << "editor voxel boolean tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
