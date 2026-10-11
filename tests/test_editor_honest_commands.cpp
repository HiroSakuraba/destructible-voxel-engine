// Commands that used to report success while doing nothing (artist worklist ART-114 and the
// items it lists) now do their job, or say plainly that they are not available:
//   Flood Fill, Drop to Surface, Rebuild Collision + View > Collision Shapes (merged proxy, the
//   Inspector count matches Play), shading modes, X-Ray Selection, Follow Selection, Validate
//   Project, File > Open Scene / Open Project / Save As / Import Model, and the honest fallbacks.
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_project.hpp"
#include "dve/fragment.hpp"

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

// Floor: 4 m square slab (top at y = 0.25). Block: 1 m cube floating at y = 5 above it. Two
// regions of material 1 in the floor separated by a stripe of material 2.
EditorDocument make_scene() {
    EditorDocument document("Honest commands");
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
    void fill(UiRect, EditorColor) const override { ++fills; }
    void outline(UiRect, EditorColor) const override { ++outlines; }
    void line(int, int, int, int, EditorColor, int) const override { ++lines; }
    void text(int, int, std::string_view value, EditorColor) const override { texts.emplace_back(value); }
    [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
    [[nodiscard]] bool has(std::string_view needle) const {
        for (const std::string& t : texts) if (t.find(needle) != std::string::npos) return true;
        return false;
    }
    mutable int fills{}, outlines{}, lines{};
    mutable std::vector<std::string> texts;
};

void test_flood_fill() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    EditorDocument& document = controller.workspace().document();
    controller.set_active_material(4);
    check(controller.dispatch_action("voxel.fill") && controller.flood_fill_armed(), "Flood Fill arms a click");
    controller.key_down("escape", false, false, false);
    check(!controller.flood_fill_armed(), "Esc cancels the armed fill");

    // Click a floor voxel left of the stripe: only that connected material-1 region changes.
    (void)controller.dispatch_action("view.frame_all");
    check(controller.dispatch_action("voxel.fill"), "arm again");
    const Float3 world{0.25F * 2.5F, 0.25F, 0.25F * 3.5F};  // top of voxel (2,0,3)
    const ScreenPoint p = project_world_to_screen(controller.camera(), controller.layout().viewport, world);
    controller.pointer_move(static_cast<int>(p.x), static_cast<int>(p.y));
    controller.pointer_down(PointerButton::Primary, static_cast<int>(p.x), static_cast<int>(p.y));
    controller.pointer_up(PointerButton::Primary, static_cast<int>(p.x), static_cast<int>(p.y));
    const EditorObject* floor = document.find_object(1);
    check(!controller.flood_fill_armed(), "the click uses up the armed fill");
    check(floor->voxels->material_at({0, 0, 0}) == 4 && floor->voxels->material_at({7, 0, 15}) == 4,
          "the clicked region is repainted");
    check(floor->voxels->material_at({8, 0, 4}) == 2 && floor->voxels->material_at({9, 0, 4}) == 1 &&
              floor->voxels->material_at({15, 0, 15}) == 1,
          "the stripe and the region beyond it are untouched");
    check(controller.dispatch_action("edit.undo") && document.find_object(1)->voxels->material_at({0, 0, 0}) == 1,
          "one undo restores");
    controller.set_active_material(2);
    check(!controller.flood_fill_at(1, {8, 0, 0}), "filling a region with its own material is refused");
    check(controller.status().text.find("already uses") != std::string::npos, "and says why");
    check(!controller.flood_fill_at(1, {0, 5, 0}), "an empty cell is refused");
}

void test_drop_to_surface() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    EditorDocument& document = controller.workspace().document();
    EditorObject child(3, "Lamp");
    child.parent = 2;
    child.transform.position = {1.5F, 6.5F, 1.5F};
    document.add_object(std::move(child));
    controller.workspace().select_object(2);
    check(controller.dispatch_action("view.drop_to_surface"), "Drop to Surface runs");
    const EditorObjectBounds block = object_world_bounds(*document.find_object(2));
    check(near(block.minimum.y, 0.25F), "the block lands on the floor top, bottom y=" + std::to_string(block.minimum.y));
    check(near(document.find_object(3)->transform.position.y, 6.5F - 4.75F), "its unattached child moves with it");
    check(!controller.dispatch_action("view.drop_to_surface") &&
              controller.status().text.find("Already resting") != std::string::npos,
          "dropping again says it is already resting");
    check(controller.dispatch_action("edit.undo") && near(object_world_bounds(*document.find_object(2)).minimum.y, 5.0F),
          "one undo restores the height");
    // Nothing below: falls to the grid floor (y = 0).
    document.find_object(2)->transform.position = {30.0F, 3.0F, 30.0F};
    check(controller.dispatch_action("view.drop_to_surface") &&
              near(object_world_bounds(*document.find_object(2)).minimum.y, 0.0F),
          "with nothing below, it drops to the grid");
    controller.workspace().clear_selection();
    check(!controller.dispatch_action("view.drop_to_surface"), "nothing selected: refused");
}

void test_collision() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    EditorDocument& document = controller.workspace().document();
    controller.workspace().select_object(1);
    check(controller.dispatch_action("voxel.rebuild_collision"), "Rebuild Collision runs");
    const std::size_t merged = build_merged_object_box_proxy(*document.find_object(1)->voxels).size();
    check(controller.status().text.find(std::to_string(merged) + " merged boxes") != std::string::npos,
          "it reports the merged box count: " + controller.status().text);
    check(controller.collision_proxy(1).size() == merged, "the cached proxy is Play's merged proxy");
    check(controller.selection_diagnostics().collisionBoxes == merged, "the Inspector count now matches Play");
    (void)document.find_object(1)->voxels->set_voxel({0, 3, 0}, 1);
    check(controller.collision_proxy(1).size() == build_merged_object_box_proxy(*document.find_object(1)->voxels).size(),
          "the proxy follows voxel edits");
    document.find_object(1)->flags.collisionEnabled = false;
    check(controller.collision_proxy(1).empty(), "collision off: no proxy");
    document.find_object(1)->flags.collisionEnabled = true;

    // View > Collision Shapes draws the proxy boxes (12 edges each), not one bounds box.
    (void)controller.dispatch_action("view.frame_all");
    Canvas plain;
    render_native_editor(plain, controller, 1280, 720);
    (void)controller.dispatch_action("view.collision");
    Canvas shown;
    render_native_editor(shown, controller, 1280, 720);
    check(controller.viewport_settings().showCollision, "Collision Shapes is on");
    check(shown.lines - plain.lines >= static_cast<int>(controller.collision_proxy(1).size()) * 8,
          "the proxy boxes are drawn (" + std::to_string(shown.lines - plain.lines) + " more lines)");
    check(!controller.dispatch_action("voxel.rebuild_brickmaps"), "Rebuild Brickmaps no longer pretends");
}

void test_shading_and_xray() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.workspace().select_object(2);
    (void)controller.dispatch_action("view.frame_all");
    Canvas lit;
    render_native_editor(lit, controller, 1280, 720);
    check(!lit.has("View: "), "no shading tag in Lit");
    check(controller.dispatch_action("view.wireframe"), "Wireframe");
    Canvas wire;
    render_native_editor(wire, controller, 1280, 720);
    check(wire.has("View: Wireframe"), "the viewport says which mode is on");
    check(wire.fills < lit.fills && wire.outlines > lit.outlines, "Wireframe outlines voxels instead of filling them");
    const MenuAction* item = std::as_const(controller.workspace().menus()).find("view.wireframe");
    check(item && item->checked, "the View menu checks the active mode");
    for (const char* mode : {"view.unlit", "view.material_ids", "view.normals", "view.lighting_only", "view.voxel_debug"}) {
        check(controller.dispatch_action(mode), std::string("mode ") + mode);
        Canvas canvas;
        render_native_editor(canvas, controller, 1280, 720);
        check(canvas.has("View: ") && canvas.fills >= lit.fills - 4, std::string(mode) + " draws the voxels and its tag");
    }
    check(controller.dispatch_action("view.overdraw"), "Overdraw");
    Canvas overdraw;
    render_native_editor(overdraw, controller, 1280, 720);
    check(overdraw.has("View: Overdraw") && overdraw.fills > 10, "Overdraw paints a heat map");
    (void)controller.dispatch_action("view.lit");

    Canvas before;
    render_native_editor(before, controller, 1280, 720);
    check(controller.dispatch_action("view.xray"), "X-Ray Selection on");
    Canvas xray;
    render_native_editor(xray, controller, 1280, 720);
    check(xray.outlines > before.outlines, "X-Ray draws the selection again on top");
}

void test_follow_selection() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.workspace().clear_selection();
    check(!controller.dispatch_action("view.follow_selection"), "nothing to follow: refused");
    controller.workspace().select_object(2);
    check(controller.dispatch_action("view.follow_selection") && controller.follow_selection(), "Follow Selection on");
    controller.update(0.016F);
    const Float3 eye = controller.camera().position;
    EditorDocument& document = controller.workspace().document();
    const RigidTransform before = document.find_object(2)->transform;
    RigidTransform after = before;
    after.position.x += 3.0F;
    check(controller.workspace().commands().execute(document, std::make_unique<TransformObjectsCommand>(
              std::vector<ObjectTransformChange>{{2, before, after}})).success, "move the block");
    controller.update(0.016F);
    check(near(controller.camera().position.x, eye.x + 3.0F, 1.0e-2F), "the camera moved with the selection");
    check(controller.dispatch_action("view.follow_selection") && !controller.follow_selection(), "toggle off");
}

void test_validate_project() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "dve_honest_validate";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "scripts");
    std::ofstream(root / "scripts" / "ok.lua") << "-- ok\n";
    EditorDocument document = make_scene();
    document.find_object(2)->sourceAsset = "models/gone.obj";
    Component script;
    script.id = 1;
    script.type = "dve.script";
    script.properties.emplace("asset", std::string("scripts/missing.lua"));
    script.properties.emplace("startup", false);
    document.find_object(2)->components.push_back(std::move(script));
    EditorObject empty(9, "Leftover");
    document.add_object(std::move(empty));
    auto owner = make_controller(std::move(document));
    NativeEditorController& controller = *owner;
    controller.configure_ai_assistant(root);
    const std::size_t found = controller.validate_project();
    check(found == 3, "three problems found, got " + std::to_string(found));
    bool missingScript = false, missingSource = false, emptyObject = false;
    for (const EditorProblem& problem : controller.workspace().problems().problems()) {
        missingScript = missingScript || (problem.code == "validate.component" && problem.severity == ProblemSeverity::Error);
        missingSource = missingSource || (problem.code == "validate.missing_file" && problem.objectId == std::optional<EditorObjectId>{2});
        emptyObject = emptyObject || (problem.code == "validate.empty_object" && problem.objectId == std::optional<EditorObjectId>{9});
    }
    check(missingScript && missingSource && emptyObject, "missing script, missing source and empty object are listed");
    check(controller.bottom_tab() == BottomPanelTab::Problems, "Problems opens");
    // Fix them; validation then passes and replaces its earlier entries.
    EditorDocument& doc = controller.workspace().document();
    doc.find_object(2)->sourceAsset.clear();
    doc.find_object(2)->components.clear();
    (void)doc.remove_object(9);
    check(controller.validate_project() == 0, "a clean scene validates");
    check(controller.workspace().problems().problems().size() == 1 &&
              controller.workspace().problems().problems().front().code == "validate.ok",
          "only the passed note remains");
    std::filesystem::remove_all(root);
}

void write_project(const std::filesystem::path& root, const std::string& name, const std::vector<std::string>& scenes) {
    std::filesystem::create_directories(root / "scenes");
    EditorProject project;
    project.name = name;
    project.root = root;
    std::string error;
    check(project.save(&error), "project saves: " + error);
    for (const std::string& scene : scenes) {
        EditorDocument document(scene);
        EditorObject object(1, scene + " object");
        object.voxelSizeMeters = 0.25F;
        (void)object.voxels->set_voxel({0, 0, 0}, 1);
        document.add_object(std::move(object));
        check(document.save_transactional(root / "scenes" / (scene + ".dvescene")).success, "scene saves");
    }
}

void type(NativeEditorController& controller, std::string_view text) { controller.text_input(text); }

void test_file_pickers() {
    const std::filesystem::path base = std::filesystem::temp_directory_path() / "dve_honest_files";
    std::filesystem::remove_all(base);
    write_project(base / "Alpha", "Alpha Project", {"Forest", "Cave"});
    write_project(base / "Beta", "Beta Project", {"Desert"});
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.configure_ai_assistant(base / "Alpha");

    // Open Scene.
    controller.workspace().document().mark_clean();  // (unsaved changes ask first; tested below)
    check(controller.dispatch_action("file.open_scene") && controller.file_picker().open, "Open Scene opens a picker");
    check(controller.file_picker().entries.size() == 2, "it lists the project's two scenes");
    type(controller, "cav");
    check(controller.file_picker_matches().size() == 1, "typing filters");
    Canvas canvas;
    render_native_editor(canvas, controller, 1280, 720);
    check(canvas.has("Open Scene") && canvas.has("scenes/Cave.dvescene"), "the picker is drawn");
    controller.key_down("return", false, false, false);
    check(!controller.file_picker().open && controller.workspace().document().path().filename() == "Cave.dvescene",
          "Enter opens the scene");
    check(controller.workspace().document().find_object(1) && controller.workspace().document().find_object(1)->name == "Cave object",
          "the scene's objects are loaded");
    auto project = EditorProject::load(base / "Alpha" / "project.dveproject");
    check(project && !project->recentScenes.empty() && project->recentScenes.front() == "scenes/Cave.dvescene",
          "it is remembered as recent");

    // Save As: a new name saves a copy and becomes the scene's path; an existing name asks first.
    check(controller.dispatch_action("file.save_as") && controller.file_picker().query == "Cave", "Save As starts from the current name");
    for (int i = 0; i < 4; ++i) controller.key_down("backspace", false, false, false);
    type(controller, "Forest");
    controller.key_down("return", false, false, false);
    check(controller.file_picker().open && controller.file_picker().confirmReplace.has_value(), "an existing name asks before replacing");
    controller.key_down("backspace", false, false, false);
    type(controller, "t2");
    controller.key_down("return", false, false, false);
    check(!controller.file_picker().open && std::filesystem::exists(base / "Alpha" / "scenes" / "Forest2.dvescene") &&
              controller.workspace().document().path().filename() == "Forest2.dvescene",
          "a new name saves under scenes/ and becomes the current scene");
    // Save on an unsaved new scene opens Save As instead of failing.
    (void)controller.dispatch_action("file.new_scene");
    check(controller.dispatch_action("file.save") && controller.file_picker().open &&
              controller.file_picker().kind == FilePickerKind::SaveSceneAs,
          "Save on a new scene asks for a name");
    controller.key_down("escape", false, false, false);

    // Unsaved changes: opening asks first.
    (void)controller.dispatch_action("file.open_scene");
    type(controller, "Forest.dve");
    controller.workspace().document().mark_dirty();
    controller.key_down("return", false, false, false);
    check(controller.pending_destructive_confirmation(), "unsaved changes: asks before opening");
    controller.key_down("escape", false, false, false);
    check(controller.workspace().document().dirty(), "Esc keeps the current scene");

    // Open Project.
    controller.workspace().document().mark_clean();
    check(controller.dispatch_action("file.open_project"), "Open Project opens a picker");
    bool sawBeta = false;
    for (const FilePickerEntry& entry : controller.file_picker().entries) sawBeta = sawBeta || entry.label == "Beta Project";
    check(sawBeta, "sibling projects are listed by name");
    type(controller, "beta");
    controller.key_down("return", false, false, false);
    check(std::filesystem::equivalent(controller.project_root(), base / "Beta") &&
              controller.workspace().document().path().filename() == "Desert.dvescene",
          "the project opens with its scene");
    std::filesystem::remove_all(base);
}

void test_import_model() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "dve_honest_import";
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
    check(!controller.dispatch_action("file.import"), "Import Model needs a selected model");
    check(controller.start_asset_scan(false), "scan assets");
    const EditorAssetRecord* record = controller.asset_database().find_path("models/crate.obj");
    check(record != nullptr, "the model is indexed");
    if (!record) { std::filesystem::remove_all(root); return; }
    controller.asset_browser_state().selectedId = record->id;
    controller.update(0.016F);
    const std::size_t before = controller.workspace().document().objects().size();
    check(controller.dispatch_action("file.import"), "Import Model imports the selected model: " + controller.status().text);
    const EditorDocument& document = controller.workspace().document();
    check(document.objects().size() >= before + 2, "a group and at least one voxel object were added");
    const auto active = controller.workspace().selected_object();
    const EditorObject* group = active ? document.find_object(*active) : nullptr;
    check(group && group->name == "Imported crate", "the import group is selected");
    bool voxels = false;
    for (const auto& [id, object] : document.objects())
        if (object.parent == active && object.voxels && object.voxels->occupied_voxel_count() > 0) voxels = true;
    check(voxels, "the imported part has voxels");
    check(controller.status().text.find("Imported crate.obj") != std::string::npos, "a summary is reported");
    check(controller.dispatch_action("edit.undo") && controller.workspace().document().objects().size() == before, "one undo removes it");
    std::filesystem::remove_all(root);
}

void test_honest_fallbacks() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    check(!controller.dispatch_action("material.assign") && controller.status().error &&
              controller.status().text.find("Paint tool") != std::string::npos,
          "Material > Assign says what to use instead");
    check(!controller.dispatch_action("polygon.extrude") && controller.status().text.find("Not in this editor yet") != std::string::npos,
          "polygon tools say they are not available");
    check(!controller.dispatch_action("no.such.action") && controller.status().text.find("Not available") != std::string::npos,
          "unknown actions are not reported as done");
}

// Review fixes: Play/preview gating for Flood Fill, the #99/#100 visibility rule for Drop to
// Surface and Flood Fill, Save As confirmation/path validation, menu reasons, unshadowed handlers.
void test_fill_and_drop_gating() {
    {
        auto owner = make_controller();
        NativeEditorController& controller = *owner;
        controller.set_active_material(4);
        controller.workspace().document().find_object(1)->flags.visible = false;
        check(!controller.flood_fill_at(1, {0, 0, 0}) && controller.status().text.find("hidden") != std::string::npos,
              "Flood Fill refuses a hidden object");
        controller.workspace().document().find_object(1)->flags.visible = true;
        // Arming the fill closes an open Join preview (one preview at a time).
        controller.workspace().select_object(1);
        controller.workspace().add_to_selection(2);
        check(controller.begin_voxel_join() && controller.voxel_join().active(), "Join preview opens: " + controller.status().text);
        check(controller.dispatch_action("voxel.fill") && !controller.voxel_join().active(),
              "arming Flood Fill closes the Join preview");
        controller.workspace().clear_selection();
        check(controller.dispatch_action("voxel.fill") && controller.flood_fill_armed(), "armed");
        check(controller.dispatch_action("physics.simulate"), "Simulate starts");
        {
            check(!controller.flood_fill_armed(), "starting Simulate disarms the fill");
            check(!controller.flood_fill_at(1, {0, 0, 0}) && controller.workspace().document().find_object(1)->voxels->material_at({0, 0, 0}) == 1,
                  "no fill while Simulate runs");
            const MenuAction* fill = std::as_const(controller.workspace().menus()).find("voxel.fill");
            check(fill && !fill->enabled && !fill->disabledReason.empty(), "Flood Fill is disabled with a reason in Play");
            (void)controller.dispatch_action("physics.stop");
        }
    }
    {
        auto owner = make_controller();
        NativeEditorController& controller = *owner;
        EditorDocument& document = controller.workspace().document();
        controller.workspace().select_object(2);
        document.find_object(2)->flags.visible = false;
        check(!controller.dispatch_action("view.drop_to_surface") && near(object_world_bounds(*document.find_object(2)).minimum.y, 5.0F),
              "Drop to Surface refuses a hidden selected object");
        document.find_object(2)->flags.visible = true;
        // Isolate the floor, then select the block (isolated out of view): refused, nothing moves.
        controller.workspace().select_object(1);
        check(controller.dispatch_action("view.isolate_selection"), "isolate the floor");
        controller.workspace().select_object(2);
        check(!controller.dispatch_action("view.drop_to_surface") &&
                  controller.status().text.find("isolated") != std::string::npos &&
                  near(object_world_bounds(*document.find_object(2)).minimum.y, 5.0F),
              "Drop to Surface refuses an isolated-out object");
        controller.workspace().clear_selection();
        controller.refresh_menu_state();
        const MenuAction* drop = std::as_const(controller.workspace().menus()).find("view.drop_to_surface");
        check(drop && !drop->enabled && drop->disabledReason.find("Select") != std::string::npos,
              "View > Drop Selection to Surface is disabled with a reason when nothing is selected");
    }
}

void test_save_as_review_fixes() {
    const std::filesystem::path base = std::filesystem::temp_directory_path() / "dve_honest_saveas";
    std::filesystem::remove_all(base);
    write_project(base / "Gamma", "Gamma Project", {"Alpha", "Bravo"});
    const std::filesystem::path bravo = base / "Gamma" / "scenes" / "Bravo.dvescene";
    const auto bravoTime = std::filesystem::last_write_time(bravo);
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.configure_ai_assistant(base / "Gamma");
    controller.workspace().document().mark_clean();

    // Confirming a replace of one scene must not carry over to another chosen with Up/Down.
    check(controller.dispatch_action("file.save_as"), "Save As opens");
    while (!controller.file_picker().query.empty()) controller.key_down("backspace", false, false, false);
    controller.key_down("return", false, false, false);  // empty name: the selected row (Alpha) is the target
    check(controller.file_picker().open && controller.file_picker().confirmReplace.has_value() &&
              controller.file_picker().confirmReplace->filename() == "Alpha.dvescene",
          "replacing Alpha asks first");
    controller.key_down("down", false, false, false);  // now Bravo is selected
    controller.key_down("return", false, false, false);
    check(controller.file_picker().open && controller.file_picker().confirmReplace &&
              controller.file_picker().confirmReplace->filename() == "Bravo.dvescene" &&
              std::filesystem::last_write_time(bravo) == bravoTime,
          "Bravo is not overwritten on the strength of Alpha's confirmation; it asks again");
    controller.key_down("escape", false, false, false);

    // Path validation: the scene stays inside the project.
    for (const std::string& bad : std::vector<std::string>{"../escape", (base / "elsewhere").string(), "bad:name", "levels/../../out", ".dvescene"}) {
        check(!controller.save_scene_as(bad), "Save As refuses '" + bad + "'");
        check(controller.status().error, "and says why: " + controller.status().text);
    }
    check(!std::filesystem::exists(base / "escape.dvescene") && !std::filesystem::exists(base / "elsewhere.dvescene") &&
              !std::filesystem::exists(base / "out.dvescene"),
          "nothing was written outside the project");
    check(controller.save_scene_as("levels/Night") && std::filesystem::exists(base / "Gamma" / "levels" / "Night.dvescene"),
          "a relative folder saves under the project");
    check(controller.dispatch_action("file.save_as") && controller.file_picker().query == "levels/Night",
          "Save As of a scene outside scenes/ keeps its folder, so Enter targets the same file");
    controller.key_down("escape", false, false, false);

    // A typed absolute path opens (Open Scene); a non-scene file does not.
    check(controller.dispatch_action("file.open_scene"), "Open Scene");
    type(controller, bravo.string());
    controller.key_down("return", false, false, false);
    check(controller.workspace().document().path().filename() == "Bravo.dvescene", "a typed absolute path opens");
    std::ofstream(base / "Gamma" / "notes.txt") << "not a scene\n";
    check(controller.dispatch_action("file.open_scene"), "Open Scene again");
    type(controller, "notes.txt");
    controller.key_down("return", false, false, false);
    check(controller.file_picker().open && controller.file_picker().messageIsError, "a non-scene file is refused");
    controller.key_down("escape", false, false, false);
    std::filesystem::remove_all(base);
}

void test_menu_reasons_and_handlers() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    controller.refresh_menu_state();
    const auto& menus = std::as_const(controller.workspace().menus());
    for (const char* id : {"voxel.rebuild_brickmaps", "create.polygon", "audio.import", "file.import", "polygon.import",
                           "build.cook", "build.package", "help.docs"}) {
        const MenuAction* action = menus.find(id);
        check(action && !action->enabled && !action->disabledReason.empty(), std::string(id) + " is disabled with a reason");
    }
    for (const char* id : {"voxel.fill", "voxel.rebuild_collision", "view.drop_to_surface", "build.validate"})
        check(menus.find(id) != nullptr, std::string(id) + " is in the menus (and so in the Command Center)");
    const MenuAction* import = menus.find("file.import");
    const MenuAction* openProject = menus.find("file.open_project");
    check(import && import->shortcut.find("I") != std::string::npos, "Import Model shows its shortcut: " + (import ? import->shortcut : ""));
    check(openProject && openProject->shortcut.find("O") != std::string::npos, "Open Project shows its shortcut");
    // The Command Center lists the disabled command with its reason.
    check(controller.dispatch_action("help.command_palette"), "Command Center opens");
    type(controller, "rebuild brickmaps");
    bool listed = false;
    for (const CommandPaletteResult& result : controller.command_palette_results(20))
        if (result.id == "voxel.rebuild_brickmaps") listed = !result.enabled && !result.disabledReason.empty();
    check(listed, "the Command Center shows Rebuild Brickmaps as disabled with its reason");
    {
        // The reason is elided to the Command Center row instead of running past the panel.
        struct Placed final : IEditorCanvas {
            void fill(UiRect, EditorColor) const override {}
            void outline(UiRect, EditorColor) const override {}
            void line(int, int, int, int, EditorColor, int) const override {}
            void text(int x, int, std::string_view value, EditorColor) const override {
                if (value.find("Nothing to rebuild") != std::string_view::npos) right = std::max(right, x + text_width(value));
            }
            [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
            mutable int right{};
        } placed;
        render_native_editor(placed, controller, 1280, 720);
        const UiRect panel = controller.command_palette_layout().panel;
        check(placed.right > 0 && placed.right <= panel.x + panel.width,
              "the disabled reason fits the Command Center (" + std::to_string(placed.right) + " <= " +
                  std::to_string(panel.x + panel.width) + ")");
    }
    controller.key_down("escape", false, false, false);
    // Handlers that a polygon./audio. catch-all used to shadow.
    check(controller.dispatch_action("polygon.lod") && controller.settings_panel().open, "Polygon > LOD Settings opens settings");
    controller.key_down("escape", false, false, false);
    check(controller.dispatch_action("audio.settings"), "Audio > Audio Settings opens settings");
    check(!controller.dispatch_action("create.polygon") && controller.status().error, "Create > Polygon Mesh is refused honestly");
    check(!controller.dispatch_action("polygon.import") && controller.status().text.find("Assets panel") != std::string::npos,
          "Polygon > Import is Import Model (needs a selected model)");
}

} // namespace

int main() {
    test_flood_fill();
    test_drop_to_surface();
    test_collision();
    test_shading_and_xray();
    test_follow_selection();
    test_validate_project();
    test_file_pickers();
    test_import_model();
    test_honest_fallbacks();
    test_fill_and_drop_gating();
    test_save_as_review_fixes();
    test_menu_reasons_and_handlers();
    if (g_failures != 0) {
        std::printf("dve_editor_honest_commands_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("dve_editor_honest_commands_tests: PASS\n");
    return 0;
}
