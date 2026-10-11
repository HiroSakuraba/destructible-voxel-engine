// Join and Separate Islands (artist worklist ART-061 Separate Islands, ART-062 Join).
//   Separate: face-connected pieces, largest stays in the original, the rest become siblings in
//             place with their own materials and anchors; one undo; refusals with reasons.
//   Join:     on-grid operands copy exactly and are removed, their children move to the target;
//             off-grid operands block until R (resample) or G (group instead); Tab changes the
//             target and asks again; one undo; the preview closes when the selection changes.
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_voxel_join.hpp"

#include <cmath>
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

// "Rubble": a 3x3x3 block (material 1), a 2x2x2 block (material 2) apart from it, a single voxel
// touching the big block only along an edge (diagonal: a separate piece under face
// connectivity), and an anchor in each of the first two pieces.
EditorObject make_rubble(EditorObjectId id) {
    EditorObject rubble(id, "Rubble");
    rubble.voxelSizeMeters = 0.25F;
    rubble.transform.position = {2.0F, 1.0F, -3.0F};
    fill_box(rubble, {0, 0, 0}, {2, 2, 2}, 1);
    fill_box(rubble, {6, 0, 0}, {7, 1, 1}, 2);
    (void)rubble.voxels->set_voxel({3, 3, 0}, 3);
    rubble.anchors.insert({0, 0, 0});
    rubble.anchors.insert({6, 0, 0});
    Component script;
    script.id = 1;
    script.type = "dve.script";
    script.properties.emplace("asset", std::string("scripts/rubble.lua"));
    script.properties.emplace("startup", false);
    rubble.components.push_back(std::move(script));
    rubble.tags = {"debris"};
    return rubble;
}

EditorObject make_slab(EditorObjectId id, std::string name, Float3 position, float voxelSize, MaterialId material) {
    EditorObject slab(id, std::move(name));
    slab.voxelSizeMeters = voxelSize;
    slab.transform.position = position;
    fill_box(slab, {0, 0, 0}, {3, 0, 3}, material);
    return slab;
}

std::uint64_t total_voxels(const EditorDocument& document) {
    std::uint64_t total = 0;
    for (const auto& [id, object] : document.objects())
        if (object.voxels) total += object.voxels->occupied_voxel_count();
    return total;
}

void test_islands() {
    const EditorObject rubble = make_rubble(1);
    const auto islands = find_voxel_islands(*rubble.voxels);
    check(islands.size() == 3, "three face-connected pieces (an edge contact is not a connection), got " +
          std::to_string(islands.size()));
    if (islands.size() == 3)
        check(islands[0].size() == 27 && islands[1].size() == 8 && islands[2].size() == 1, "pieces are ordered largest first");
    const auto again = find_voxel_islands(*rubble.voxels);
    check(again.size() == islands.size() && again[1].front().voxel == islands[1].front().voxel, "deterministic");
}

void test_separate_core() {
    EditorDocument document("Separate");
    document.add_object(make_rubble(1));
    EditorObject child(2, "Lamp");
    child.parent = 1;
    document.add_object(std::move(child));
    EditorObject parent(3, "Ruins");
    document.add_object(std::move(parent));
    (void)document.reparent(1, 3);
    const std::uint64_t before = total_voxels(document);
    const std::size_t objectsBefore = document.objects().size();

    SeparateIslandsBuild build = build_separate_islands_command(document, {1});
    check(build.command && build.newObjects.size() == 2 && build.separatedObjects == 1, "two new pieces");
    EditorCommandStack stack;
    check(build.command && stack.execute(document, std::move(build.command)).success, "separate executes");
    const EditorObject* original = document.find_object(1);
    check(original && original->voxels->occupied_voxel_count() == 27, "the largest piece stays in the original");
    check(original && original->anchors.size() == 1 && original->anchors.contains({0, 0, 0}), "its anchor stays");
    check(original && original->components.size() == 1, "the original keeps its components");
    check(document.find_object(2) && document.find_object(2)->parent == std::optional<EditorObjectId>{1},
          "the original keeps its children");
    check(total_voxels(document) == before, "no voxel is lost or duplicated");
    for (std::size_t i = 0; i < build.newObjects.size(); ++i) {
        const EditorObject* piece = document.find_object(build.newObjects[i]);
        check(piece != nullptr, "piece exists");
        if (!piece) continue;
        check(piece->name == "Rubble (piece " + std::to_string(i + 2) + ")", "pieces are named after the original: " + piece->name);
        check(piece->parent == std::optional<EditorObjectId>{3}, "pieces are siblings of the original");
        check(piece->transform.position.x == 2.0F && piece->transform.position.z == -3.0F && piece->voxelSizeMeters == 0.25F,
              "pieces keep the transform and voxel size, so nothing moves");
        check(piece->components.empty(), "pieces do not copy components (scripts stay on the original)");
        check(piece->tags == std::vector<std::string>{"debris"}, "pieces keep tags");
    }
    const EditorObject* second = build.newObjects.empty() ? nullptr : document.find_object(build.newObjects[0]);
    check(second && second->voxels->occupied_voxel_count() == 8 && second->voxels->material_at({6, 0, 0}) == 2 &&
              second->anchors.contains({6, 0, 0}),
          "the second piece keeps its voxel coordinates, material and anchor");
    check(stack.undo(document).success, "undo");
    check(document.objects().size() == objectsBefore && document.find_object(1)->voxels->occupied_voxel_count() == 36 &&
              document.find_object(1)->anchors.size() == 2,
          "undo restores one object with every voxel and anchor");

    // Refusals.
    EditorDocument solid("Solid");
    solid.add_object(make_slab(1, "Slab", {}, 0.25F, 1));
    SeparateIslandsBuild none = build_separate_islands_command(solid, {1});
    check(!none.command && !none.skipped.empty() && none.skipped.front().find("one piece") != std::string::npos,
          "a single piece is left alone with a reason");
    EditorObject locked = make_rubble(2);
    locked.flags.locked = true;
    solid.add_object(std::move(locked));
    check(separate_islands_problem(solid, 2).find("locked") != std::string::npos, "locked objects are refused");
}

std::unique_ptr<NativeEditorController> make_controller(EditorDocument document) {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    controller->resize(1280, 720);
    return controller;
}

void select(NativeEditorController& controller, std::vector<EditorObjectId> ids) {
    controller.workspace().clear_selection();
    for (const EditorObjectId id : ids) controller.workspace().add_to_selection(id);  // last = active
}

void test_separate_rules() {
    // Slice's Separate rules (#97): prefab instances and attached objects are refused.
    EditorDocument document("Rules");
    EditorObject prefab = make_rubble(1);
    prefab.prefabLink = EditorPrefabLink{};
    prefab.prefabLink->prefabAsset = "prefabs/rubble.dprefab";
    document.add_object(std::move(prefab));
    document.add_object(EditorObject(2, "Crane"));
    EditorObject hook = make_rubble(3);
    hook.parent = 2;
    hook.attachment = EditorAttachment{};
    hook.attachment->socket = "hook";
    document.add_object(std::move(hook));
    check(separate_islands_problem(document, 1).find("prefab") != std::string::npos, "a prefab instance is refused");
    check(separate_islands_problem(document, 3).find("attached") != std::string::npos, "an attached object is refused");
    SeparateIslandsBuild build = build_separate_islands_command(document, {1, 3});
    check(!build.command && build.skipped.size() == 2, "neither is separated, each with a reason");

    // Huge island counts are capped: kMaxSeparateIslands + 1 single voxels, two cells apart.
    EditorDocument dust("Dust");
    EditorObject cloud(1, "Dust");
    for (std::size_t i = 0; i <= kMaxSeparateIslands; ++i)
        (void)cloud.voxels->set_voxel({static_cast<int>(i % 32) * 2, 0, static_cast<int>(i / 32) * 2}, 1);
    dust.add_object(std::move(cloud));
    SeparateIslandsBuild capped = build_separate_islands_command(dust, {1});
    check(!capped.command && !capped.skipped.empty() && capped.skipped.front().find("at most") != std::string::npos,
          "more than kMaxSeparateIslands pieces is refused with a reason");
    check(dust.objects().size() == 1, "no objects were added");
}

void test_separate_isolation() {
    EditorDocument document("Isolated");
    document.add_object(make_rubble(1));
    document.add_object(make_rubble(2));
    auto owner = make_controller(std::move(document));
    NativeEditorController& controller = *owner;
    controller.workspace().select_object(1);
    check(controller.toggle_isolation(), "isolate Rubble 1");
    // Rubble 2 is isolated out; select both (as the outliner can) and separate.
    select(controller, {2, 1});
    check(controller.dispatch_action("voxel.separate_islands"), "separate both");
    std::size_t visible = 0, hidden = 0;
    for (const auto& [id, object] : controller.workspace().document().objects()) {
        if (id <= 2) continue;
        const bool fromOne = object.name.starts_with("Rubble (piece") && !controller.is_isolated_out(id);
        (fromOne ? visible : hidden) += 1;
    }
    check(visible == 2 && hidden == 2, "pieces of the isolated original stay visible; pieces of the isolated-out one do not (" +
          std::to_string(visible) + " visible, " + std::to_string(hidden) + " hidden)");
}

void test_separate_controller() {
    EditorDocument document("Separate UI");
    document.add_object(make_rubble(1));
    auto owner = make_controller(std::move(document));
    NativeEditorController& controller = *owner;
    controller.workspace().select_object(1);
    check(controller.separate_islands_disabled_reason().empty(), "Separate Islands is available");
    check(controller.dispatch_action("voxel.separate_islands"), "Tools > Voxel > Separate Islands");
    check(controller.workspace().selection_count() == 3 && controller.workspace().selected_object() == std::optional<EditorObjectId>{1},
          "all pieces are selected and the original stays active");
    check(controller.dispatch_action("edit.undo") && controller.workspace().document().objects().size() == 1, "one undo step");
}

EditorDocument make_join_scene() {
    EditorDocument document("Join");
    document.add_object(make_slab(1, "Floor A", {0.0F, 0.0F, 0.0F}, 0.25F, 1));
    document.add_object(make_slab(2, "Floor B", {1.0F, 0.0F, 0.0F}, 0.25F, 2));    // next to A, on A's grid
    document.add_object(make_slab(3, "Coarse", {0.0F, 0.0F, 2.0F}, 0.5F, 3));      // double voxel size
    EditorObject turned = make_slab(4, "Turned", {3.0F, 0.0F, 3.0F}, 0.25F, 4);    // 45 degrees off the grid
    turned.transform.rotation = quaternion_from_axis_angle({0.0F, 1.0F, 0.0F}, 0.785398F);
    document.add_object(std::move(turned));
    EditorObject lamp(5, "Lamp");
    lamp.parent = 2;
    document.add_object(std::move(lamp));
    EditorObject text(6, "Sign");
    document.add_object(std::move(text));
    return document;
}

void test_join_on_grid() {
    auto owner = make_controller(make_join_scene());
    NativeEditorController& controller = *owner;
    EditorDocument& document = controller.workspace().document();
    select(controller, {1});
    check(!controller.voxel_join_disabled_reason().empty(), "one object cannot be joined");
    select(controller, {1, 6});
    check(controller.voxel_join_disabled_reason().find("no voxels") != std::string::npos, "an object without voxels is refused");
    select(controller, {2, 1});  // A active
    const std::uint64_t voxelsBefore = total_voxels(document);
    const std::size_t objectsBefore = document.objects().size();
    check(controller.dispatch_action("voxel.join"), "Join opens a preview");
    check(controller.voxel_join().active() && !controller.voxel_join().has_mismatch(), "B is on A's grid");
    check(document.objects().size() == objectsBefore, "nothing changes while previewing");
    const auto lines = controller.voxel_join().describe(document);
    check(lines.size() > 3 && lines[1].starts_with("Target: 'Floor A'") && lines[2].find("on the grid: exact") != std::string::npos,
          "the preview names the target and the exact operand");
    check(controller.dispatch_action("voxel.join_commit"), "commit");
    const EditorObject* joined = document.find_object(1);
    check(!document.find_object(2), "the joined object is removed");
    check(joined && joined->voxels->occupied_voxel_count() == 32, "the target holds both slabs (32 voxels)");
    check(joined && joined->voxels->material_at({4, 0, 0}) == 2, "B's voxels keep their material in A's grid");
    check(document.find_object(5) && document.find_object(5)->parent == std::optional<EditorObjectId>{1},
          "B's child moved under the target");
    check(total_voxels(document) == voxelsBefore, "no voxel lost or duplicated");
    check(controller.dispatch_action("edit.undo"), "undo");
    check(document.find_object(2) && document.find_object(1)->voxels->occupied_voxel_count() == 16 &&
              document.find_object(5)->parent == std::optional<EditorObjectId>{2},
          "one undo restores B, A and the child's parent");
}

void test_join_mismatch() {
    auto owner = make_controller(make_join_scene());
    NativeEditorController& controller = *owner;
    EditorDocument& document = controller.workspace().document();
    select(controller, {3, 4, 1});  // A active; Coarse and Turned are off its grid
    check(controller.dispatch_action("voxel.join"), "Join opens");
    const EditorVoxelJoinSession& join = controller.voxel_join();
    check(join.needs_choice(), "off-grid objects need a choice");
    std::size_t mismatched = 0;
    for (const VoxelJoinOperand& operand : join.operands()) {
        if (operand.id == 3) check(operand.mismatch.find("voxel size 0.5 m vs 0.25 m") != std::string::npos,
                                   "names the voxel size mismatch: " + operand.mismatch);
        if (operand.id == 4) check(operand.mismatch.find("grid") != std::string::npos, "names the rotation mismatch: " + operand.mismatch);
        mismatched += operand.mismatch.empty() ? 0 : 1;
    }
    check(mismatched == 2, "both operands are off the grid");
    check(!controller.dispatch_action("voxel.join_commit") && document.find_object(3), "commit waits for a choice");
    // Tab: Coarse becomes the target, and the choice is asked again.
    controller.key_down("r", false, false, false);
    check(!join.needs_choice(), "R chooses resample");
    controller.key_down("tab", false, false, false);
    check(join.target() == 3 && controller.workspace().selected_object() == std::optional<EditorObjectId>{3},
          "Tab makes the next object the target (and the active selection)");
    check(join.needs_choice(), "a new target asks again");
    controller.key_down("tab", false, false, false);
    controller.key_down("tab", false, false, false);
    check(join.target() == 1, "Tab cycles back to the first target");
    controller.key_down("r", false, false, false);
    controller.key_down("return", false, false, false);
    check(!join.active() && !document.find_object(3) && !document.find_object(4), "resampled join commits");
    const EditorObject* joined = document.find_object(1);
    // Coarse: 4x4 voxels of 0.5 m = 2 m square -> 8x8 cells of 0.25 m in A's grid.
    check(joined && joined->voxels->material_at({0, 0, 8}) == 3 && joined->voxels->material_at({7, 0, 15}) == 3,
          "the coarse slab is resampled into A's finer grid");
    check(joined && joined->voxels->occupied_voxel_count() > 16 + 64, "the turned slab was resampled in as well");
    check(controller.dispatch_action("edit.undo") && document.find_object(3) && document.find_object(4) &&
              document.find_object(1)->voxels->occupied_voxel_count() == 16,
          "one undo restores everything");

    // G: group instead, no geometry change.
    select(controller, {3, 1});
    check(controller.dispatch_action("voxel.join"), "Join opens again");
    controller.key_down("g", false, false, false);
    check(!controller.voxel_join().active(), "G closes the preview");
    const EditorObject* a = document.find_object(1);
    const EditorObject* coarse = document.find_object(3);
    check(a && coarse && a->parent && a->parent == coarse->parent && a->voxels->occupied_voxel_count() == 16,
          "G groups the objects instead and changes no voxels");

    // Changing the selection closes the preview.
    select(controller, {2, 1});
    check(controller.dispatch_action("voxel.join"), "Join opens");
    select(controller, {1});
    controller.update(0.016F);
    check(!controller.voxel_join().active(), "a different selection closes the Join preview");
}

void test_join_rules() {
    auto owner = make_controller(make_join_scene());
    NativeEditorController& controller = *owner;
    EditorDocument& document = controller.workspace().document();
    // An operand with a script would lose it: refused, but it may be the target.
    Component script;
    script.id = 1;
    script.type = "dve.script";
    script.properties.emplace("asset", std::string("scripts/b.lua"));
    document.find_object(2)->components.push_back(std::move(script));
    select(controller, {2, 1});
    check(controller.voxel_join_disabled_reason().find("components") != std::string::npos,
          "an operand with components is refused: " + controller.voxel_join_disabled_reason());
    select(controller, {1, 2});
    check(controller.voxel_join_disabled_reason().empty(), "the object with components can be the target");
    document.find_object(2)->components.clear();
    // A locked child of an operand would make the move fail: refused up front, by name.
    document.find_object(5)->flags.locked = true;
    select(controller, {2, 1});
    check(controller.voxel_join_disabled_reason().find("'Lamp'") != std::string::npos, "a locked child is named");
    document.find_object(5)->flags.locked = false;
    // Hidden participants are refused.
    document.find_object(2)->flags.visible = false;
    check(controller.voxel_join_disabled_reason().find("hidden") != std::string::npos, "a hidden object is refused");
    document.find_object(2)->flags.visible = true;
    // Isolated-out participants are refused.
    select(controller, {1});
    check(controller.toggle_isolation(), "isolate Floor A");
    select(controller, {2, 1});
    check(controller.voxel_join_disabled_reason().find("isolated") != std::string::npos, "an isolated-out object is refused");
    check(controller.toggle_isolation(), "isolation off");
    // Flag differences are stated; the commit reports the collision rebuild.
    document.find_object(2)->flags.collisionEnabled = false;
    check(controller.dispatch_action("voxel.join"), "Join opens");
    bool stated = false;
    for (const std::string& line : controller.voxel_join().describe(document))
        stated = stated || line.find("the target's settings apply") != std::string::npos;
    check(stated, "the preview says the target's flags apply");
    check(controller.dispatch_action("voxel.join_commit"), "commit");
    check(controller.status().text.find("collision rebuilt") != std::string::npos,
          "the status reports the collision rebuild: " + controller.status().text);
}

void test_join_preview_panel() {
    auto owner = make_controller(make_join_scene());
    NativeEditorController& controller = *owner;
    select(controller, {3, 1});
    (void)controller.dispatch_action("view.frame_all");
    check(controller.dispatch_action("voxel.join"), "Join opens");
    struct Texts final : IEditorCanvas {
        void fill(UiRect, EditorColor) const override {}
        void outline(UiRect, EditorColor) const override {}
        void line(int, int, int, int, EditorColor, int) const override {}
        void text(int, int, std::string_view value, EditorColor) const override { all.emplace_back(value); }
        [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
        [[nodiscard]] bool has(std::string_view needle) const {
            for (const std::string& t : all) if (t.find(needle) != std::string::npos) return true;
            return false;
        }
        mutable std::vector<std::string> all;
    } canvas;
    render_native_editor(canvas, controller, 1280, 720);
    check(canvas.has("Join 2 voxel objects") && canvas.has("R resample") && canvas.has("Off grid"),
          "the viewport shows the Join panel, the choice and the off-grid label");
    // Many operands: the panel stays inside the viewport and keeps the key line.
    {
        EditorDocument many("Many");
        for (EditorObjectId id = 1; id <= 60; ++id)
            many.add_object(make_slab(id, "Slab " + std::to_string(id), {static_cast<float>(id), 0.0F, 0.0F}, 0.25F, 1));
        auto big = make_controller(std::move(many));
        std::vector<EditorObjectId> ids;
        for (EditorObjectId id = 1; id <= 60; ++id) ids.push_back(id);
        select(*big, ids);
        check(big->dispatch_action("voxel.join"), "Join of 60 objects opens");
        struct Rows final : IEditorCanvas {
            void fill(UiRect, EditorColor) const override {}
            void outline(UiRect, EditorColor) const override {}
            void line(int, int, int, int, EditorColor, int) const override {}
            void text(int, int y, std::string_view value, EditorColor) const override {
                if (value.starts_with("Enter join")) keyY = y;
            }
            [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
            mutable int keyY = -1;
        } rows;
        render_native_editor(rows, *big, 1280, 720);
        const UiRect viewport = big->layout().viewport;
        check(rows.keyY > viewport.y && rows.keyY < viewport.y + viewport.height,
              "the key line of a 60-object Join panel is inside the viewport (y=" + std::to_string(rows.keyY) + ")");
    }
    controller.workspace().select_object(1);
    check(controller.dispatch_action("voxel.slice"), "Slice opens");
    check(!controller.voxel_join().active(), "starting Slice closes Join (one preview at a time)");
}

} // namespace

int main() {
    test_islands();
    test_separate_core();
    test_separate_controller();
    test_separate_rules();
    test_separate_isolation();
    test_join_on_grid();
    test_join_mismatch();
    test_join_rules();
    test_join_preview_panel();
    if (g_failures != 0) {
        std::printf("dve_editor_voxel_join_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("dve_editor_voxel_join_tests: PASS\n");
    return 0;
}
