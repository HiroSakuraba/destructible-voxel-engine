// Follow-ups to #101 (honest commands):
//   1. Long jobs (Import Model, Reimport, large Flood Fills, Validate Project, Rebuild Collision)
//      run on a worker with a progress overlay and Esc/Cancel; cancel leaves the scene unchanged
//      with no undo entry; results apply as one undo step; nothing can edit the scene meanwhile.
//   2. Import options panel (voxel size, scale, fill, meshes, estimate, hard cap) and Reimport.
//   3. Polygon/material shortcut-only commands are menu items, disabled with their reason.
//   4. Lighting Only shades with the scene's sun (N.L) plus sky/ground ambient.
#include "dve/editor_lighting_preview.hpp"
#include "dve/editor_model_import.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_runtime_settings.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <cstdint>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

EditorDocument make_scene() {
    EditorDocument document("Followups");
    EditorObject floor(1, "Floor");
    floor.voxelSizeMeters = 0.25F;
    fill_box(floor, {0, 0, 0}, {15, 0, 15}, 1);
    fill_box(floor, {8, 0, 0}, {8, 0, 15}, 2);
    document.add_object(std::move(floor));
    EditorObject block(2, "Block");
    block.voxelSizeMeters = 0.25F;
    block.transform.position = {1.0F, 5.0F, 1.0F};
    fill_box(block, {0, 0, 0}, {3, 3, 3}, 3);
    document.add_object(std::move(block));
    return document;
}

std::unique_ptr<NativeEditorController> make_controller(EditorDocument document = make_scene()) {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    controller->resize(1280, 720);
    return controller;
}

struct Canvas final : IEditorCanvas {
    void fill(UiRect, EditorColor color) const override { colors.insert(color & 0xFFFFFFU); }
    void outline(UiRect, EditorColor) const override {}
    void line(int, int, int, int, EditorColor, int) const override {}
    void text(int, int, std::string_view value, EditorColor) const override { texts.emplace_back(value); }
    [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
    [[nodiscard]] bool has(std::string_view needle) const {
        for (const std::string& t : texts) if (t.find(needle) != std::string::npos) return true;
        return false;
    }
    mutable std::vector<std::string> texts;
    mutable std::set<unsigned> colors;
};

std::size_t count_material(const EditorObject& object, MaterialId material) {
    std::size_t count = 0;
    for (int x = 0; x <= 15; ++x)
        for (int z = 0; z <= 15; ++z) count += object.voxels->material_at({x, 0, z}) == material ? 1U : 0U;
    return count;
}

// --- 1. Long jobs ----------------------------------------------------------------------------

void test_flood_fill_job_commits_one_undo_step() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.set_long_job_threshold(0);  // everything goes through the worker
    controller.set_active_material(4);
    const std::size_t undoBefore = controller.workspace().commands().size();
    check(controller.flood_fill_at(1, {2, 0, 3}), "a flood fill starts as a long job");
    check(controller.long_job_active(), "the job is running");
    const LongJobView view = controller.long_job_view();
    check(view.kind == LongJobKind::FloodFill && view.label.find("Flood Fill") != std::string::npos, "the job is labelled");
    // The progress overlay is drawn with the label, a percentage and Cancel.
    Canvas canvas;
    render_native_editor(canvas, controller, 1280, 720);
    check(canvas.has("Flood Fill Floor") && canvas.has("%") && canvas.has("Cancel") && canvas.has("Esc cancels"),
          "the progress overlay shows the job, its percent and how to cancel");
    const NativeLongJobLayout layout = controller.long_job_layout();
    check(layout.panel.x >= controller.layout().viewport.x &&
              layout.panel.x + layout.panel.width <= controller.layout().viewport.x + controller.layout().viewport.width &&
              layout.cancelButton.x + layout.cancelButton.width <= layout.panel.x + layout.panel.width,
          "the overlay and its Cancel button fit the viewport");
    // While it runs, nothing edits the scene.
    check(!controller.dispatch_action("edit.undo") && controller.status().text.find("Busy") != std::string::npos,
          "Undo waits for the job");
    check(!controller.dispatch_action("physics.play"), "Play cannot start during a long job");
    check(!controller.dispatch_action("build.validate"), "a second long job is refused");
    controller.key_down("delete", false, false, false);
    check(controller.workspace().document().find_object(1) != nullptr, "keys do not edit during the job");
    check(controller.dispatch_action("view.lighting_only") && controller.dispatch_action("view.lit"),
          "shading modes still switch during a job");
    check(controller.wait_for_long_job(), "the job finishes");
    check(!controller.long_job_active(), "and is gone");
    const EditorObject* floor = controller.workspace().document().find_object(1);
    check(floor && count_material(*floor, 4) == 8 * 16, "the left region was filled: " + controller.status().text);
    check(controller.workspace().commands().size() == undoBefore + 1, "the fill is one undo step");
    check(controller.dispatch_action("edit.undo") && count_material(*controller.workspace().document().find_object(1), 4) == 0,
          "one undo restores it");
}

void test_cancel_leaves_scene_unchanged() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.set_long_job_threshold(0);
    controller.set_active_material(4);
    const std::size_t undoBefore = controller.workspace().commands().size();
    const std::uint64_t revision = controller.workspace().document().revision();
    check(controller.flood_fill_at(1, {2, 0, 3}), "start a fill");
    controller.key_down("escape", false, false, false);
    check(!controller.long_job_active() && controller.status().text.find("cancelled") != std::string::npos,
          "Esc cancels the job: " + controller.status().text);
    for (int i = 0; i < 50; ++i) controller.update(0.016F);
    check(controller.workspace().document().revision() == revision, "cancel leaves the scene unchanged");
    check(count_material(*controller.workspace().document().find_object(1), 4) == 0, "no voxels were repainted");
    check(controller.workspace().commands().size() == undoBefore, "cancel adds no undo entry");

    // The Cancel button does the same.
    check(controller.flood_fill_at(1, {2, 0, 3}), "start again");
    const UiRect button = controller.long_job_layout().cancelButton;
    controller.pointer_down(PointerButton::Primary, button.x + 4, button.y + 4);
    controller.pointer_up(PointerButton::Primary, button.x + 4, button.y + 4);
    check(!controller.long_job_active(), "Cancel button cancels");
    check(controller.workspace().commands().size() == undoBefore && count_material(*controller.workspace().document().find_object(1), 4) == 0,
          "still unchanged");
    check(controller.flood_fill_at(1, {2, 0, 3}), "a new job can start after a cancel");
    check(controller.wait_for_long_job() && count_material(*controller.workspace().document().find_object(1), 4) == 8 * 16,
          "and completes");

    // Rapid cancel-and-restart never runs out of room behind cancelled jobs still winding down.
    check(controller.dispatch_action("edit.undo"), "undo the completed fill");
    for (int i = 0; i < 8; ++i) {
        check(controller.flood_fill_at(1, {2, 0, 3}), "restart " + std::to_string(i) + ": " + controller.status().text);
        controller.key_down("escape", false, false, false);
    }
    check(controller.flood_fill_at(1, {2, 0, 3}) && controller.wait_for_long_job(), "a job still runs after many cancels");
}

void test_scene_changed_during_job_is_not_applied() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.set_long_job_threshold(0);
    controller.set_active_material(4);
    check(controller.flood_fill_at(1, {2, 0, 3}), "start a fill");
    // Something outside the UI (live MCP, AI tools) edits the scene meanwhile.
    (void)controller.workspace().commands().execute(controller.workspace().document(),
                                                    std::make_unique<RenameObjectCommand>(2, "Block", "Crate"));
    const std::size_t undo = controller.workspace().commands().size();
    check(controller.wait_for_long_job(), "the job finishes");
    check(count_material(*controller.workspace().document().find_object(1), 4) == 0 &&
              controller.status().text.find("not applied") != std::string::npos,
          "a result computed from an old scene is not applied: " + controller.status().text);
    check(controller.workspace().commands().size() == undo, "and adds no undo entry");
}

void test_validate_and_collision_jobs() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.set_long_job_threshold(0);
    check(controller.validate_project() == 0 && controller.long_job_active(), "Validate runs as a long job");
    check(controller.wait_for_long_job(), "and finishes");
    check(controller.status().text.find("Validation") != std::string::npos, "Validate reports: " + controller.status().text);
    bool listed = false;
    for (const EditorProblem& problem : controller.workspace().problems().problems())
        if (problem.code.starts_with("validate.")) listed = true;
    check(listed, "Validate's findings are in Problems");

    const std::size_t undo = controller.workspace().commands().size();
    check(controller.rebuild_collision_proxies() && controller.long_job_active(), "Rebuild Collision runs as a long job");
    check(controller.cancel_long_job() && controller.status().text.find("cancelled") != std::string::npos, "and cancels");
    check(controller.rebuild_collision_proxies() && controller.wait_for_long_job(), "run it to the end");
    check(controller.status().text.find("Rebuilt collision for") != std::string::npos, "it reports: " + controller.status().text);
    check(!controller.collision_proxy(1).empty(), "the proxy is cached");
    check(controller.workspace().commands().size() == undo, "Rebuild Collision adds no undo entry");

    // Small work still finishes at once (no overlay for a 2-object scene by default).
    controller.set_long_job_threshold(250'000);
    (void)controller.validate_project();
    check(!controller.long_job_active(), "small Validate finishes at once");
}

// --- 2. Import options and Reimport ----------------------------------------------------------

std::filesystem::path make_import_project() {
    // Unique per run so parallel runs (ctest -j, several build trees) never share it.
    static const std::string unique = std::to_string(std::random_device{}()) + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("dve_followup_import_" + unique);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "models");
    // glTF with two nodes sharing a 1 m cube mesh: "Body" at the origin and "Lid" 2 m up.
    const float positions[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    const std::uint16_t indices[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                       3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
    {
        std::ofstream bin(root / "models" / "chest.bin", std::ios::binary);
        bin.write(reinterpret_cast<const char*>(positions), sizeof positions);
        bin.write(reinterpret_cast<const char*>(indices), sizeof indices);
    }
    std::ofstream(root / "models" / "chest.gltf")
        << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],)"
           R"("nodes":[{"name":"Body","mesh":0},{"name":"Lid","mesh":0,"translation":[0,2,0]}],)"
           R"("meshes":[{"name":"Cube","primitives":[{"attributes":{"POSITION":0},"indices":1}]}],)"
           R"("buffers":[{"byteLength":168,"uri":"chest.bin"}],)"
           R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":96},{"buffer":0,"byteOffset":96,"byteLength":72}],)"
           R"("accessors":[{"bufferView":0,"componentType":5126,"count":8,"type":"VEC3","min":[0,0,0],"max":[1,1,1]},)"
           R"({"bufferView":1,"componentType":5123,"count":36,"type":"SCALAR"}]})";
    return root;
}

std::size_t children_with_voxels(const EditorDocument& document, EditorObjectId group, float* voxelSize = nullptr) {
    std::size_t count = 0;
    for (const auto& [id, object] : document.objects())
        if (object.parent == group && object.voxels && object.voxels->occupied_voxel_count() > 0) {
            ++count;
            if (voxelSize) *voxelSize = object.voxelSizeMeters;
        }
    return count;
}

void test_import_options_panel() {
    const std::filesystem::path root = make_import_project();
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.configure_ai_assistant(root);
    controller.update(0.016F);
    const std::size_t before = controller.workspace().document().objects().size();
    const std::size_t undoBefore = controller.workspace().commands().size();
    check(controller.open_import_options("models/chest.gltf") && controller.import_options().open, "the options panel opens");
    const ImportOptionsState& options = controller.import_options();
    check(options.nodes.size() == 2, "both meshes are listed");
    const auto rows = controller.import_option_rows();
    check(rows.size() == 5 && rows[0].first == "Voxel size" && rows[1].first == "Scale" && rows[2].first == "Fill" &&
              rows[3].first.find("[x]") == 0,
          "rows: voxel size, scale, fill, then the meshes");
    const std::uint64_t estimate = options.estimate.voxels;
    check(estimate > 0 && options.estimate.refusal.empty(), "an estimate is shown");
    // Smaller voxels: more voxels. Left/Right steps the voxel size.
    controller.key_down("left", false, false, false);
    check(options.settings.voxelSizeMeters < 0.1F && options.estimate.voxels > estimate, "a smaller voxel size raises the estimate");
    // Typing a value: digits, then Enter applies it (without importing).
    controller.text_input("0.25");
    controller.key_down("return", false, false, false);
    check(controller.import_options().open && std::abs(options.settings.voxelSizeMeters - 0.25F) < 1e-6F, "a typed voxel size applies");
    // Scale doubles the size in each axis.
    const std::uint64_t atScale1 = options.estimate.voxels;
    check(controller.set_import_option(ImportOptionsRow::Scale, "2") && options.estimate.voxels > atScale1, "scale raises the estimate");
    check(!controller.set_import_option(ImportOptionsRow::Scale, "-1") && options.messageIsError, "a bad scale is refused with a reason");
    check(controller.set_import_option(ImportOptionsRow::Scale, "1"), "scale back to 1");
    // Leave the lid out.
    check(controller.set_import_option(ImportOptionsRow::Node, "off", 1) && options.estimate.includedNodes == 1, "a mesh can be left out");
    // The hard cap: tiny voxels on a big scale is refused before anything is voxelized.
    check(controller.set_import_option(ImportOptionsRow::VoxelSize, "0.005") && controller.set_import_option(ImportOptionsRow::Scale, "100"),
          "set a huge import");
    check(!options.estimate.refusal.empty() && options.estimate.refusal.find("Too large to import") != std::string::npos,
          "the estimate is over the cap: " + options.estimate.refusal);
    check(!controller.accept_import_options() && controller.import_options().open, "Import is refused over the cap");
    check(controller.status().text.find("limit") != std::string::npos && controller.status().error,
          "the refusal says the limit and what to change");
    Canvas canvas;
    render_native_editor(canvas, controller, 1280, 720);
    check(canvas.has("Import models/chest.gltf") && canvas.has("Estimate: up to") && canvas.has("Too large"),
          "the panel draws its title, the estimate and the refusal");
    const NativeImportOptionsLayout layout = controller.import_options_layout();
    check(layout.importButton.x >= layout.panel.x && layout.cancelButton.x + layout.cancelButton.width <= layout.panel.x + layout.panel.width &&
              layout.estimateBox.y + layout.estimateBox.height <= layout.importButton.y && layout.rows.size() == 5,
          "the panel's rows, estimate and buttons fit inside it");
    check(controller.set_import_option(ImportOptionsRow::VoxelSize, "0.1") && controller.set_import_option(ImportOptionsRow::Scale, "1"), "back to sane");
    check(controller.workspace().document().objects().size() == before && controller.workspace().commands().size() == undoBefore,
          "nothing was added while choosing");
    check(controller.accept_import_options(), "Import: " + controller.status().text);
    const EditorDocument& document = controller.workspace().document();
    const auto group = controller.workspace().selected_object();
    const EditorObject* groupObject = group ? document.find_object(*group) : nullptr;
    check(groupObject && groupObject->name == "Imported chest", "the group is selected");
    check(groupObject && children_with_voxels(document, *group) == 1, "only the included mesh was imported");
    const auto stored = groupObject ? read_import_settings(*groupObject) : std::nullopt;
    check(stored && stored->excludedNodes.contains("Lid") && std::abs(stored->voxelSizeMeters - 0.1F) < 1e-6F,
          "the options are stored on the group for Reimport");
    check(controller.workspace().commands().size() == undoBefore + 1, "Import is one undo step");
    check(controller.dispatch_action("edit.undo") && controller.workspace().document().objects().size() == before, "one undo removes it");

    // Esc closes the panel without importing.
    check(controller.open_import_options("models/chest.gltf"), "open again");
    controller.key_down("escape", false, false, false);
    check(!controller.import_options().open && controller.workspace().document().objects().size() == before, "Esc closes without importing");
    // Not during Play.
    check(controller.dispatch_action("physics.play"), "Play");
    check(!controller.open_import_options("models/chest.gltf"), "no import during Play");
    check(controller.dispatch_action("physics.stop"), "Stop");
    std::filesystem::remove_all(root);
}

void test_import_as_long_job_and_cancel() {
    const std::filesystem::path root = make_import_project();
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.configure_ai_assistant(root);
    controller.update(0.016F);
    controller.set_long_job_threshold(0);
    const std::size_t before = controller.workspace().document().objects().size();
    const std::size_t undoBefore = controller.workspace().commands().size();
    // Reading the file is a job too; the panel opens when it is read.
    check(controller.open_import_options("models/chest.gltf") && controller.long_job_view().kind == LongJobKind::ReadModel, "reading runs as a job");
    check(controller.wait_for_long_job() && controller.import_options().open, "the panel opens after reading");
    check(controller.accept_import_options() && controller.long_job_view().kind == LongJobKind::ImportModel, "the import runs as a job");
    check(!controller.import_options().open, "the panel closed");
    check(controller.cancel_long_job(), "cancel the import");
    for (int i = 0; i < 20; ++i) controller.update(0.016F);
    check(controller.workspace().document().objects().size() == before && controller.workspace().commands().size() == undoBefore,
          "a cancelled import adds nothing and no undo entry");
    check(controller.open_import_options("models/chest.gltf") && controller.wait_for_long_job(), "read again");
    check(controller.accept_import_options() && controller.wait_for_long_job(), "import runs to the end: " + controller.status().text);
    check(controller.workspace().document().objects().size() == before + 3, "group plus two parts");
    check(controller.workspace().commands().size() == undoBefore + 1, "one undo step");
    std::filesystem::remove_all(root);
}

void test_reimport_keeps_transform_name_parent() {
    const std::filesystem::path root = make_import_project();
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.configure_ai_assistant(root);
    controller.update(0.016F);
    check(!controller.reimport_disabled_reason().empty(), "Reimport needs a selection");
    controller.workspace().select_object(2);
    check(controller.reimport_disabled_reason().find("not imported") != std::string::npos, "a plain object is not reimportable");
    check(controller.open_import_options("models/chest.gltf") && controller.accept_import_options(), "import");
    EditorDocument& document = controller.workspace().document();
    const EditorObjectId group = *controller.workspace().selected_object();
    // The artist renames, moves and reparents the group.
    (void)controller.workspace().commands().execute(document, std::make_unique<RenameObjectCommand>(group, "Imported chest", "Treasure"));
    RigidTransform moved = document.find_object(group)->transform;
    moved.position = {3.0F, 0.0F, -2.0F};
    (void)controller.workspace().commands().execute(document, std::make_unique<TransformObjectCommand>(group, document.find_object(group)->transform, moved));
    (void)document.reparent(group, EditorObjectId{2});
    // A part selected: Reimport finds its group.
    EditorObjectId part = 0;
    for (const auto& [id, object] : document.objects()) if (object.parent == group) part = id;
    controller.workspace().select_object(part);
    check(controller.reimport_disabled_reason().empty(), "Reimport is available from a part: " + controller.reimport_disabled_reason());
    const std::size_t undoBefore = controller.workspace().commands().size();
    check(controller.dispatch_action("file.reimport") && controller.import_options().open && controller.import_options().reimport,
          "Reimport opens the options with the stored settings");
    check(controller.set_import_option(ImportOptionsRow::VoxelSize, "0.25"), "change the voxel size");
    check(controller.accept_import_options(), "Reimport: " + controller.status().text);
    const EditorObject* after = document.find_object(group);
    check(after && after->name == "Treasure", "the name is kept");
    check(after && after->parent == EditorObjectId{2}, "the parent is kept");
    check(after && std::abs(after->transform.position.x - 3.0F) < 1e-4F && std::abs(after->transform.position.z + 2.0F) < 1e-4F,
          "the transform is kept");
    float voxelSize = 0.0F;
    check(children_with_voxels(document, group, &voxelSize) == 2 && std::abs(voxelSize - 0.25F) < 1e-6F, "the parts are rebuilt with the new settings");
    bool partsFollow = true;
    for (const auto& [id, object] : document.objects())
        if (object.parent == group && object.transform.position.x < 2.0F) partsFollow = false;
    check(partsFollow, "rebuilt parts sit where the moved group is");
    check(controller.workspace().commands().size() == undoBefore + 1, "Reimport is one undo step");
    check(controller.dispatch_action("edit.undo"), "undo the reimport");
    voxelSize = 0.0F;
    check(children_with_voxels(document, group, &voxelSize) == 2 && std::abs(voxelSize - 0.1F) < 1e-6F && document.find_object(group)->name == "Treasure",
          "one undo restores the previous parts");
    // Locked and hidden groups are refused before anything happens.
    controller.workspace().select_object(group);
    (void)controller.workspace().commands().execute(document, std::make_unique<SetObjectFlagsCommand>(group, document.find_object(group)->flags,
        [&] { EditorObjectFlags f = document.find_object(group)->flags; f.locked = true; return f; }()));
    check(controller.reimport_disabled_reason().find("Unlock") != std::string::npos && !controller.dispatch_action("file.reimport"),
          "a locked group is refused");
    (void)controller.dispatch_action("edit.undo");
    (void)controller.workspace().commands().execute(document, std::make_unique<SetObjectFlagsCommand>(group, document.find_object(group)->flags,
        [&] { EditorObjectFlags f = document.find_object(group)->flags; f.visible = false; return f; }()));
    check(controller.reimport_disabled_reason().find("hidden") != std::string::npos, "a hidden group is refused");
    std::filesystem::remove_all(root);
}

void test_import_closes_previews() {
    const std::filesystem::path root = make_import_project();
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.configure_ai_assistant(root);
    controller.update(0.016F);
    controller.workspace().select_object(2);
    check(controller.begin_voxel_slice() && controller.voxel_slice().active(), "a Slice preview is open");
    check(controller.open_import_options("models/chest.gltf"), "open import options");
    check(!controller.voxel_slice().active(), "one preview at a time: the Slice preview closed");
    check(!controller.dispatch_action("voxel.fill") || !controller.import_options().open, "a command closes the modal panel first");
    std::filesystem::remove_all(root);
}

void test_import_estimate_helpers() {
    ImportedScene scene;
    scene.meshes.push_back({"cube", {}, {}});
    for (const Float3 p : {Float3{0, 0, 0}, Float3{1, 1, 1}}) scene.meshes.back().vertices.push_back({p});
    ImportedNode node;
    node.name = "Cube";
    node.mesh = 0;
    scene.nodes.push_back(node);
    EditorImportSettings settings;
    settings.voxelSizeMeters = 0.1F;
    const EditorImportEstimate a = estimate_import(scene, settings);
    check(a.voxels == 11ULL * 11ULL * 11ULL, "a 1 m cube at 0.1 m is up to 11^3 cells: " + std::to_string(a.voxels));
    settings.scale = 2.0F;
    check(estimate_import(scene, settings).voxels == 21ULL * 21ULL * 21ULL, "scale 2 doubles each axis");
    const ImportedScene scaled = scaled_import_scene(scene, 2.0F);
    check(std::abs(transform_point(scaled.nodes[0].worldTransform, {1, 1, 1}).x - 2.0F) < 1e-5F, "scaling scales node transforms");
    settings.excludedNodes.insert("Cube");
    check(!estimate_import(scene, settings).refusal.empty(), "nothing included is refused");
    EditorObject holder(1, "g");
    holder.components.push_back(make_import_settings_component(settings, 1));
    const auto round = read_import_settings(holder);
    check(round && round->excludedNodes == settings.excludedNodes && std::abs(round->scale - 2.0F) < 1e-6F, "settings round-trip through the component");
    check(is_model_source("a/B.GLB") && !is_model_source("a/b.png"), "model extensions");
}

// --- 3. Shortcut-only commands ----------------------------------------------------------------

void type(NativeEditorController& controller, std::string_view text) { controller.text_input(text); }

void test_context_only_commands_listed() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.update(0.016F);
    const auto& menus = std::as_const(controller.workspace().menus());
    for (const char* id : {"material.assign", "material.apply", "material.create_instance", "material.preview"}) {
        const MenuAction* action = menus.find(id);
        check(action && action->enabled, std::string(id) + " exposes the implemented material preset workflow");
    }
    const auto inTools = [&] {
        for (const MenuAction& action : controller.menu_actions("Tools")) if (action.id == "polygon.extrude") return true;
        return false;
    };
    check(inTools(), "mesh tools are discoverable in standard menus");
    check(controller.dispatch_action("view.advanced_menus") && inTools(), "Extrude remains available in Advanced Menus");
    (void)controller.dispatch_action("view.advanced_menus");
    check(controller.dispatch_action("help.command_palette"), "Command Center opens");
    type(controller, "extrude");
    bool listed = false;
    for (const CommandPaletteResult& result : controller.command_palette_results(20))
        if (result.id == "polygon.extrude") listed = !result.enabled && result.disabledReason.find("Select polygon triangle faces") != std::string::npos;
    check(listed, "the Command Center shows Extrude disabled with its reason before use");
    controller.key_down("escape", false, false, false);
    check(!controller.dispatch_action("polygon.extrude") && controller.status().error, "running it still explains");
    // The M key still toggles the selection domain (no conflict with Assign Material).
    controller.key_down("m", false, false, false);
    check(controller.selection_domain() == SelectionDomain::Voxels, "M is still the selection domain");
}

// --- 4. Lighting Only --------------------------------------------------------------------------

void test_lighting_only() {
    RenderEnvironment environment;
    environment.sunDirection = {0.0F, 1.0F, 0.0F};
    const Float3 up = lighting_only_color({0, 1, 0}, environment);
    const Float3 side = lighting_only_color({1, 0, 0}, environment);
    const Float3 down = lighting_only_color({0, -1, 0}, environment);
    check(up.x > side.x && side.x > down.x, "faces toward the sun are brighter (N.L), and up-facing ambient beats down-facing");
    check(down.x > 0.05F, "faces away from the sun still get ambient light");
    environment.sunDirection = {1.0F, 0.0F, 0.0F};
    check(lighting_only_color({1, 0, 0}, environment).x > lighting_only_color({0, 1, 0}, environment).x, "it follows the sun direction");
    environment.exposure = 0.0F;
    check(lighting_only_color({1, 0, 0}, environment).x == 0.0F, "exposure scales it");

    // The viewport uses it: Lighting Only draws more than one grey for a lit box (top vs sides).
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    check(controller.dispatch_action("view.frame_all") && controller.dispatch_action("view.lighting_only"), "Lighting Only");
    Canvas canvas;
    render_native_editor(canvas, controller, 1280, 720);
    check(canvas.has("Lighting Only"), "the mode tag shows");
    const RenderEnvironment environmentNow = render_environment_settings(controller.workspace().settings());
    std::size_t faces = 0;
    for (const Float3 normal : {Float3{0, 1, 0}, Float3{1, 0, 0}, Float3{-1, 0, 0}, Float3{0, 0, 1}, Float3{0, 0, -1}}) {
        const Float3 c = lighting_only_color(normal, environmentNow);
        const auto byte = [](float v) { return static_cast<unsigned>(std::clamp(v, 0.0F, 1.0F) * 255.0F + 0.5F); };
        if (canvas.colors.contains((byte(c.x) << 16U) | (byte(c.y) << 8U) | byte(c.z))) ++faces;
    }
    check(faces >= 2, "voxels are drawn with the sun-lit shades of at least two face directions: " + std::to_string(faces));
}

} // namespace

int main() {
    test_flood_fill_job_commits_one_undo_step();
    test_cancel_leaves_scene_unchanged();
    test_scene_changed_during_job_is_not_applied();
    test_validate_and_collision_jobs();
    test_import_options_panel();
    test_import_as_long_job_and_cancel();
    test_reimport_keeps_transform_name_parent();
    test_import_closes_previews();
    test_import_estimate_helpers();
    test_context_only_commands_listed();
    test_lighting_only();
    if (g_failures != 0) {
        std::printf("dve_editor_honest_followups_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("dve_editor_honest_followups_tests: PASS\n");
    return 0;
}
