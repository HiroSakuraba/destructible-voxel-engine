// Scatter Objects: deterministic pseudo-random placement of copies on a voxel surface.
//   core:       spacing, staying on the surface, gaps, seeds, alignment, independent copies,
//               one undoable command;
//   controller: Create > Scatter Objects preview (scene unchanged), keys, commit, Esc, reasons.
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_scatter.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}
bool near(float a, float b, float tolerance = 1.0e-3F) { return std::abs(a - b) <= tolerance; }

// A 10 m x 10 m ground slab (40 x 40 voxels of 0.25 m, one voxel thick, top at y = 0.25), with an
// optional square hole in the middle, and a 2x2x2 "Rock" off to the side.
EditorDocument make_scene(bool hole = false) {
    EditorDocument document("Scatter test");
    EditorObject ground(1, "Ground");
    ground.voxelSizeMeters = 0.25F;
    for (int x = 0; x < 40; ++x)
        for (int z = 0; z < 40; ++z) {
            if (hole && x >= 12 && x < 28 && z >= 12 && z < 28) continue;
            ground.voxels->set_voxel({x, 0, z}, 1);
        }
    document.add_object(std::move(ground));
    EditorObject rock(2, "Rock");
    rock.voxelSizeMeters = 0.25F;
    rock.transform.position = {40.0F, 0.0F, 40.0F};
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) rock.voxels->set_voxel({x, y, z}, 2);
    document.add_object(std::move(rock));
    return document;
}

void test_plan_spacing_and_surface() {
    const EditorDocument document = make_scene();
    ScatterSettings settings;
    settings.count = 30;
    settings.minSpacingMeters = 1.0F;
    settings.seed = 7;
    const ScatterPlan plan = plan_scatter(document, 1, 1, settings);
    check(plan.error.empty(), "plan has no error: " + plan.error);
    check(plan.samples.size() == 30, "30 copies fit on 10 x 10 m at 1 m spacing, got " + std::to_string(plan.samples.size()));
    for (std::size_t i = 0; i < plan.samples.size(); ++i) {
        const ScatterSample& a = plan.samples[i];
        check(a.position.x >= 0.0F && a.position.x <= 10.0F && a.position.z >= 0.0F && a.position.z <= 10.0F,
              "spot inside the ground footprint");
        check(near(a.position.y, 0.25F), "spot on the ground's top surface, y=" + std::to_string(a.position.y));
        check(near(a.normal.y, 1.0F), "flat ground has an up normal");
        for (std::size_t j = i + 1; j < plan.samples.size(); ++j) {
            const ScatterSample& b = plan.samples[j];
            const float dx = a.position.x - b.position.x;
            const float dz = a.position.z - b.position.z;
            check(dx * dx + dz * dz >= 1.0F - 1.0e-4F, "spots keep the minimum spacing");
        }
    }
    const ScatterPlan again = plan_scatter(document, 1, 1, settings);
    bool same = again.samples.size() == plan.samples.size();
    for (std::size_t i = 0; same && i < plan.samples.size(); ++i)
        same = near(again.samples[i].position.x, plan.samples[i].position.x, 0.0F) &&
               near(again.samples[i].position.z, plan.samples[i].position.z, 0.0F);
    check(same, "the same seed gives exactly the same layout");
    settings.seed = 8;
    const ScatterPlan other = plan_scatter(document, 1, 1, settings);
    check(!other.samples.empty() && !near(other.samples.front().position.x, plan.samples.front().position.x, 0.0F),
          "a different seed gives a different layout");
}

void test_plan_limits_and_errors() {
    const EditorDocument document = make_scene();
    ScatterSettings crowded;
    crowded.count = 50;
    crowded.minSpacingMeters = 5.0F;
    const ScatterPlan sparse = plan_scatter(document, 1, 1, crowded);
    check(sparse.samples.size() < 10 && sparse.tooClose > 0, "wide spacing limits how many fit and reports why");

    const EditorDocument holed = make_scene(true);
    ScatterSettings settings;
    settings.count = 60;
    settings.minSpacingMeters = 0.5F;
    const ScatterPlan plan = plan_scatter(holed, 1, 1, settings);
    check(plan.missedSurface > 0, "spots over the hole are reported as off the surface");
    for (const ScatterSample& s : plan.samples)
        check(!(s.position.x > 3.0F && s.position.x < 7.0F && s.position.z > 3.0F && s.position.z < 7.0F),
              "no copy floats over the hole");

    check(!plan_scatter(document, 99, 1, settings).error.empty(), "a missing target is an error");
    check(!plan_scatter(document, 1, 0, settings).error.empty(), "no sources is an error");
    EditorDocument empty = make_scene();
    EditorObject marker(3, "Empty");
    empty.add_object(std::move(marker));
    check(!plan_scatter(empty, 3, 1, settings).error.empty(), "a target without voxels is an error");
}

void test_build_and_undo() {
    EditorDocument document = make_scene();
    ScatterSettings settings;
    settings.count = 12;
    settings.minSpacingMeters = 1.5F;
    const ScatterPlan plan = plan_scatter(document, 1, 1, settings);
    const std::vector<ScatterSource> sources{{&document, {2}, "Rock"}};
    ScatterBuildResult build = build_scatter_command(document, 1, plan, sources, settings);
    check(build.command != nullptr, "build makes a command: " + build.error);
    if (!build.command) return;
    const std::size_t before = document.objects().size();
    EditorCommandStack stack;
    check(stack.execute(document, std::move(build.command)).success, "scatter command executes");
    check(document.objects().size() == before + 1 + plan.samples.size(), "one group plus one object per copy");
    const EditorObject* group = document.find_object(build.groupId);
    check(group && group->name == "Scatter (" + std::to_string(plan.samples.size()) + ")", "group is named with its count");
    std::size_t copies = 0;
    for (const auto& [id, object] : document.objects()) {
        if (!object.parent || *object.parent != build.groupId) continue;
        ++copies;
        check(object.voxels && object.voxels->occupied_voxel_count() == 8, "a copy has the rock's voxels");
        check(!object.prefabLink.has_value(), "copies are independent");
        const EditorObjectBounds bounds = object_world_bounds(object);
        check(near(bounds.minimum.y, 0.25F), "a copy's bottom sits on the ground, y=" + std::to_string(bounds.minimum.y));
    }
    check(copies == plan.samples.size(), "every copy is under the group");
    check(stack.undo(document).success && document.objects().size() == before, "one undo removes every copy and the group");
    check(stack.redo(document).success && document.objects().size() == before + 1 + plan.samples.size(), "redo restores them");
}

void test_align_to_surface() {
    for (const bool align : {false, true}) {
        EditorDocument document = make_scene();
        EditorObject* ground = document.find_object(1);
        ground->transform.rotation = quaternion_from_axis_angle({1.0F, 0.0F, 0.0F}, 0.5F);  // ~29 degree slope
        ScatterSettings settings;
        settings.count = 5;
        settings.alignToSurface = align;
        const ScatterPlan plan = plan_scatter(document, 1, 1, settings);
        check(!plan.samples.empty(), "spots found on a slope");
        if (plan.samples.empty()) continue;
        check(plan.samples.front().normal.y < 0.95F, "the slope normal is tilted");
        ScatterBuildResult build = build_scatter_command(document, 1, plan, {{&document, {2}, "Rock"}}, settings);
        EditorCommandStack stack;
        check(build.command && stack.execute(document, std::move(build.command)).success, "build on a slope");
        for (const auto& [id, object] : document.objects()) {
            if (!object.parent || *object.parent != build.groupId) continue;
            const Float3 up = rotate(object.transform.rotation, {0.0F, 1.0F, 0.0F});
            if (align) check(near(up.x, plan.samples.front().normal.x, 1.0e-3F) && near(up.y, plan.samples.front().normal.y, 1.0e-3F) &&
                                 near(up.z, plan.samples.front().normal.z, 1.0e-3F), "aligned copy's up follows the normal");
            else check(near(up.y, 1.0F), "unaligned copy stays upright");
            break;
        }
    }
}

void test_stepped_hill_normal() {
    // Voxel terrain is a staircase: every top face points straight up. The scatter normal follows
    // the hill's slope instead, so Align to Surface tilts copies on voxel hills.
    EditorDocument document("Hill");
    EditorObject hill(1, "Hill");
    hill.voxelSizeMeters = 0.25F;
    for (int x = 0; x < 40; ++x)
        for (int z = 0; z < 40; ++z)
            for (int y = 0; y <= x / 4; ++y) hill.voxels->set_voxel({x, y, z}, 1);
    document.add_object(std::move(hill));
    ScatterSettings settings;
    settings.count = 10;
    settings.minSpacingMeters = 1.0F;
    const ScatterPlan plan = plan_scatter(document, 1, 1, settings);
    check(plan.samples.size() >= 5, "spots on the hill");
    int tilted = 0;
    for (const ScatterSample& sample : plan.samples)
        if (sample.normal.x < -0.1F && sample.normal.y > 0.8F) ++tilted;
    check(tilted >= static_cast<int>(plan.samples.size()) / 2,
          "most hill normals lean away from the uphill direction, " + std::to_string(tilted) + " of " +
              std::to_string(plan.samples.size()));
}

void test_prefab_like_source_is_independent() {
    EditorDocument document = make_scene();
    EditorDocument templateDocument("Bush prefab");
    EditorObject bush(10, "Bush");
    bush.voxels->set_voxel({0, 0, 0}, 3);
    bush.prefabLink = EditorPrefabLink{};
    bush.prefabLink->prefabAsset = "props/bush.dveprefab";
    templateDocument.add_object(std::move(bush));
    ScatterSettings settings;
    settings.count = 4;
    const ScatterPlan plan = plan_scatter(document, 1, 2, settings);
    const std::vector<ScatterSource> sources{{&document, {2}, "Rock"}, {&templateDocument, {10}, "prefab Bush"}};
    ScatterBuildResult build = build_scatter_command(document, 1, plan, sources, settings);
    EditorCommandStack stack;
    check(build.command && stack.execute(document, std::move(build.command)).success, "mixed sources build");
    for (const auto& [id, object] : document.objects())
        if (object.parent && *object.parent == build.groupId)
            check(!object.prefabLink.has_value(), "prefab copies drop their link");
}

void test_controller_workflow() {
    auto owner = std::make_unique<NativeEditorController>(EditorWorkspace(make_scene()));
    NativeEditorController& controller = *owner;
    controller.resize(1280, 720);
    controller.workspace().clear_selection();
    check(!controller.scatter_disabled_reason().empty(), "nothing selected: Scatter explains what to select");
    controller.workspace().select_object(1);
    check(controller.scatter_disabled_reason().find("Also select") != std::string::npos,
          "only a surface selected: asks for something to scatter");
    controller.workspace().select_object(2);
    controller.workspace().add_to_selection(1);  // the surface last
    check(controller.scatter_disabled_reason().empty(), "rock then ground is a valid scatter selection");
    (void)controller.dispatch_action("view.frame_all");
    check(controller.workspace().selected_object() == std::optional<EditorObjectId>{1},
          "Frame All keeps the active object (it used to become the highest id)");
    const std::size_t before = controller.workspace().document().objects().size();
    const bool dirty = controller.workspace().document().dirty();
    check(controller.dispatch_action("create.scatter"), "Create > Scatter Objects opens the preview");
    check(controller.scatter_active() && !controller.scatter_plan().samples.empty(), "preview has spots");
    check(controller.workspace().document().objects().size() == before && controller.workspace().document().dirty() == dirty,
          "previewing does not change the scene");
    const std::uint32_t count = controller.scatter_settings().count;
    controller.key_down("]", false, false, false);
    check(controller.scatter_settings().count == count + 5, "] adds copies");
    const auto firstSeedSpot = controller.scatter_plan().samples.front().position;
    controller.key_down("n", false, false, false);
    check(!near(controller.scatter_plan().samples.front().position.x, firstSeedSpot.x, 0.0F), "N re-rolls the layout");
    controller.key_down("a", false, false, false);
    check(controller.scatter_settings().alignToSurface, "A toggles align to surface");
    controller.key_down("escape", false, false, false);
    check(!controller.scatter_active() && controller.workspace().document().objects().size() == before, "Esc cancels, nothing added");

    check(controller.dispatch_action("create.scatter"), "reopen the preview");
    const std::size_t planned = controller.scatter_plan().samples.size();
    controller.key_down("return", false, false, false);
    check(!controller.scatter_active(), "Enter commits");
    check(controller.workspace().document().objects().size() == before + 1 + planned, "group plus copies added");
    const auto group = controller.workspace().selected_object();
    check(group && controller.workspace().document().find_object(*group)->name.starts_with("Scatter ("),
          "the new scatter group is selected");
    // Many objects: hierarchy rows stay inside the panel and the wheel scrolls to the last one.
    const NativeEditorLayout& layout = controller.layout();
    check(layout.hierarchyScrollMax > 0, "a scattered scene overflows the hierarchy and can scroll");
    for (const UiRect& row : layout.hierarchyRows)
        if (row.width > 0)
            check(row.y + row.height <= layout.hierarchy.y + layout.hierarchy.height, "hierarchy row stays in its panel");
    check(layout.hierarchyRows.back().width == 0, "the last row starts scrolled out");
    for (int i = 0; i < 40; ++i)
        controller.pointer_wheel(-1.0F, layout.hierarchy.x + 20, layout.hierarchy.y + layout.hierarchy.height / 2);
    check(controller.layout().hierarchyRows.back().width > 0, "scrolling reveals the last row");
    check(controller.dispatch_action("edit.undo") && controller.workspace().document().objects().size() == before,
          "one undo removes the whole scatter");
}

// Scatter, Slice and Boolean previews draw their panels in the same slot at the top of the
// viewport, so opening one closes the others. Scatter output made while View > Isolate Selection
// is on stays visible (joins the isolated set).
void test_preview_exclusive_and_isolation() {
    auto owner = std::make_unique<NativeEditorController>(EditorWorkspace(make_scene()));
    NativeEditorController& controller = *owner;
    controller.resize(1280, 720);
    controller.workspace().select_object(2);
    controller.workspace().add_to_selection(1);  // the surface last
    check(controller.dispatch_action("create.scatter") && controller.scatter_active(), "scatter preview opens");
    controller.workspace().select_object(1);
    check(controller.dispatch_action("voxel.slice") && controller.voxel_slice().active(), "slice preview opens");
    check(!controller.scatter_active(), "starting Slice closes the scatter preview");
    controller.workspace().select_object(2);
    controller.workspace().add_to_selection(1);
    check(controller.dispatch_action("create.scatter") && controller.scatter_active(), "scatter preview reopens");
    check(!controller.voxel_slice().active(), "starting Scatter closes the slice preview");
    if (controller.dispatch_action("voxel.boolean_union") && controller.voxel_boolean().active())
        check(!controller.scatter_active(), "starting a Boolean closes the scatter preview");
    if (controller.scatter_active()) controller.cancel_scatter();

    controller.workspace().select_object(1);
    controller.workspace().add_to_selection(2);
    check(controller.dispatch_action("view.isolate_selection"), "isolate the ground and rock");
    controller.workspace().select_object(2);
    controller.workspace().add_to_selection(1);
    check(controller.dispatch_action("create.scatter"), "scatter under isolation");
    controller.key_down("return", false, false, false);
    const auto group = controller.workspace().selected_object();
    check(group && *group != 1 && *group != 2, "the committed group is selected");
    std::size_t hidden = 0;
    for (const auto& [id, object] : controller.workspace().document().objects())
        if (controller.is_isolated_out(id)) ++hidden;
    check(hidden == 0, "scatter output made under isolation is visible, hidden=" + std::to_string(hidden));
}

} // namespace

int main() {
    test_plan_spacing_and_surface();
    test_plan_limits_and_errors();
    test_build_and_undo();
    test_align_to_surface();
    test_stepped_hill_normal();
    test_prefab_like_source_is_independent();
    test_controller_workflow();
    test_preview_exclusive_and_isolation();
    if (g_failures != 0) {
        std::printf("dve_editor_scatter_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("dve_editor_scatter_tests: PASS\n");
    return 0;
}
