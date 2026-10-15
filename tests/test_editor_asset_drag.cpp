// Drag assets from the Assets panel into the viewport (artist worklist ART-076):
//   - prefabs and .dvox voxel assets land with their bottom centre on the surface under the
//     cursor (or the ground), as one undo step, and are selected;
//   - models open the import options and the imported group lands at the drop point;
//   - materials dropped on a voxel object repaint its voxels (or its selected voxels);
//   - a ghost box and a label show what releasing will do; Esc or releasing outside the
//     viewport cancels; unsupported assets say why.
#include "dve/dvox.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_prefab.hpp"
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

void fill_box(VoxelObject& voxels, Int3 min, Int3 max, MaterialId material) {
    for (int x = min.x; x <= max.x; ++x)
        for (int y = min.y; y <= max.y; ++y)
            for (int z = min.z; z <= max.z; ++z) (void)voxels.set_voxel({x, y, z}, material);
}

struct Canvas final : IEditorCanvas {
    void fill(UiRect, EditorColor) const override {}
    void outline(UiRect, EditorColor) const override {}
    void line(int, int, int, int, EditorColor, int) const override {}
    void text(int, int, std::string_view value, EditorColor) const override { texts.emplace_back(value); }
    [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
    [[nodiscard]] bool has(std::string_view needle) const {
        for (const std::string& t : texts) if (t.find(needle) != std::string::npos) return true;
        return false;
    }
    mutable std::vector<std::string> texts;
};

const std::filesystem::path kRoot = std::filesystem::temp_directory_path() / "dve_asset_drag";

// A project with a prefab (a 1 m crate), a .dvox (a 0.5 m block), an OBJ cube, a material
// file named after a library material, and a texture (not placeable).
void make_project() {
    std::filesystem::remove_all(kRoot);
    std::filesystem::create_directories(kRoot / "assets");
    {
        EditorDocument source("Prefab source");
        EditorObject crate(1, "Crate");
        crate.voxelSizeMeters = 0.25F;
        crate.transform.position = {5.0F, 3.0F, 5.0F};  // the prefab's own origin is not its base
        fill_box(*crate.voxels, {0, 0, 0}, {3, 3, 3}, 1);
        source.add_object(std::move(crate));
        const std::array<EditorObjectId, 1> roots{1};
        const auto captured = capture_editor_prefab(source, roots, kRoot / "assets" / "crate.dveprefab", "Crate");
        check(captured.success, "capture the prefab: " + captured.error);
    }
    {
        CookedVoxelAsset asset(7);
        asset.voxelSizeMeters = 0.25F;
        fill_box(asset.object, {2, 2, 2}, {3, 3, 3}, 2);  // 0.5 m block, offset from the asset origin
        std::string error;
        check(write_dvox(kRoot / "assets" / "block.dvox", asset, {}, &error), "write the .dvox: " + error);
    }
    std::ofstream(kRoot / "assets" / "box.obj")
        << "o Box\n"
           "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
           "f 1 3 2\nf 1 4 3\nf 5 6 7\nf 5 7 8\nf 1 2 6\nf 1 6 5\nf 4 8 7\nf 4 7 3\nf 1 5 8\nf 1 8 4\nf 2 3 7\nf 2 7 6\n";
    std::ofstream(kRoot / "assets" / "brick_red.dmat") << "material\n";
    std::ofstream(kRoot / "assets" / "nowhere.dmat") << "material\n";
    std::ofstream(kRoot / "assets" / "notes.lua") << "-- not placeable\n";
}

EditorDocument make_scene() {
    EditorDocument document("Drag");
    EditorObject floor(1, "Floor");
    floor.voxelSizeMeters = 0.5F;
    floor.transform.position = {-5.0F, -0.5F, -5.0F};  // top at y = 0
    fill_box(*floor.voxels, {0, 0, 0}, {19, 0, 19}, 1);
    document.add_object(std::move(floor));
    EditorObject wall(2, "Wall");
    wall.voxelSizeMeters = 0.5F;
    wall.transform.position = {2.0F, 0.0F, 0.0F};
    fill_box(*wall.voxels, {0, 0, 0}, {1, 3, 1}, 3);
    document.add_object(std::move(wall));
    return document;
}

std::unique_ptr<NativeEditorController> make_controller() {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(make_scene()));
    controller->configure_ai_assistant(kRoot);
    controller->resize(1280, 720);
    controller->update(0.016F);
    (void)controller->start_asset_scan(false);
    (void)controller->dispatch_action("window.toggle_assets");
    controller->update(0.016F);
    EditorMaterialEntry brick;
    brick.id = controller->materials().next_available_id();
    brick.definition.name = "Brick Red";
    brick.definition.baseColor = {0.7F, 0.2F, 0.1F, 1.0F};
    check(controller->materials().upsert(brick), "add a Brick Red material");
    controller->camera().position = {0.0F, 8.0F, 12.0F};
    controller->camera().target = {0.0F, 0.0F, 0.0F};
    return controller;
}

// Press on the asset's row in the Assets panel.
bool press_asset(NativeEditorController& controller, std::string_view relative, int& x, int& y) {
    const EditorAssetRecord* record = controller.asset_database().find_path(std::filesystem::path(relative));
    if (!record) return false;
    controller.asset_browser_state().query.text = std::filesystem::path(relative).filename().string();
    controller.asset_browser_state().firstVisible = 0;
    controller.resize(1280, 720);  // lays the rows out again for the filtered list
    const auto rows = controller.asset_browser_rows();
    for (std::size_t i = 0; i < rows.size() && i < controller.layout().assetRows.size(); ++i) {
        if (rows[i]->id != record->id) continue;
        const UiRect row = controller.layout().assetRows[i];
        x = row.x + 20; y = row.y + row.height / 2;
        controller.pointer_move(x, y);
        controller.pointer_down(PointerButton::Primary, x, y);
        return true;
    }
    return false;
}

ScreenPoint screen_of(NativeEditorController& controller, Float3 world) {
    return project_world_to_screen(controller.camera(), controller.layout().viewport, world);
}

// Drag an asset to a world point and release; returns whether the drag became active.
bool drag_asset(NativeEditorController& controller, std::string_view relative, Float3 world, bool release = true) {
    int x = 0, y = 0;
    if (!press_asset(controller, relative, x, y)) return false;
    controller.pointer_move(x + 4, y - 30);
    const ScreenPoint p = screen_of(controller, world);
    const int tx = static_cast<int>(std::lround(p.x)), ty = static_cast<int>(std::lround(p.y));
    controller.pointer_move(tx, ty);
    const bool active = controller.asset_drag().active;
    if (release) controller.pointer_up(PointerButton::Primary, tx, ty);
    return active;
}

const EditorObject* newest(const NativeEditorController& controller) {
    const auto selected = controller.workspace().selected_object();
    return selected ? controller.workspace().document().find_object(*selected) : nullptr;
}

void test_prefab_and_voxel_asset() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    const std::size_t before = controller.workspace().document().objects().size();
    check(drag_asset(controller, "assets/crate.dveprefab", {-2.0F, 0.0F, 1.0F}), "dragging the prefab starts a drag");
    const EditorObject* crate = newest(controller);
    check(crate && crate->name.find("Crate") != std::string::npos, "the placed prefab is selected");
    if (crate) {
        const EditorObjectBounds bounds = object_world_bounds(*crate);
        check(near(bounds.minimum.y, 0.0F, 0.06F), "it stands on the floor: bottom y=" + std::to_string(bounds.minimum.y));
        check(near((bounds.minimum.x + bounds.maximum.x) * 0.5F, -2.0F, 0.15F) && near((bounds.minimum.z + bounds.maximum.z) * 0.5F, 1.0F, 0.15F),
              "centred on the drop point");
        check(crate->prefabLink.has_value(), "it is a prefab instance");
    }
    check(controller.dispatch_action("edit.undo") && controller.workspace().document().objects().size() == before, "one undo removes it");

    check(drag_asset(controller, "assets/block.dvox", {0.0F, 0.0F, 2.0F}), "dragging the .dvox starts a drag");
    const EditorObject* block = newest(controller);
    check(block && block->voxels && block->voxels->occupied_voxel_count() == 8 && block->sourceAsset == "assets/block.dvox",
          "the voxel asset becomes a new object that remembers its source");
    if (block) {
        const EditorObjectBounds bounds = object_world_bounds(*block);
        check(near(bounds.minimum.y, 0.0F, 0.06F) && near(bounds.maximum.y - bounds.minimum.y, 0.5F) &&
                  near((bounds.minimum.x + bounds.maximum.x) * 0.5F, 0.0F, 0.15F),
              "its bottom centre is on the drop point");
        check(block->voxels->material_at({2, 2, 2}) == 2, "materials come along");
    }
    // Dropping onto the wall's top lands on the wall.
    check(drag_asset(controller, "assets/block.dvox", {2.5F, 2.0F, 0.5F}), "drag onto the wall");
    if (const EditorObject* onWall = newest(controller))
        check(near(object_world_bounds(*onWall).minimum.y, 2.0F, 0.06F), "it lands on the wall's top");
    check(controller.dispatch_action("edit.undo") && controller.dispatch_action("edit.undo") &&
              controller.workspace().document().objects().size() == before,
          "each drop is one undo step");
}

void test_ghost_and_cancel() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    const std::size_t before = controller.workspace().document().objects().size();
    check(drag_asset(controller, "assets/block.dvox", {0.0F, 0.0F, 2.0F}, false), "drag without releasing");
    const AssetDragState& drag = controller.asset_drag();
    check(drag.ghost.valid && near(drag.ghost.minimum.y, 0.0F, 0.06F) && drag.point.has_value(), "a ghost box sits on the floor");
    Canvas canvas;
    render_native_editor(canvas, controller, 1280, 720);
    check(canvas.has("Release to place block"), "the label says what releasing does");
    controller.key_down("escape", false, false, false);
    check(!controller.asset_drag().armed, "Esc cancels the drag");
    controller.pointer_up(PointerButton::Primary, 600, 300);
    check(controller.workspace().document().objects().size() == before, "nothing was placed");

    // Releasing back over the Assets panel cancels too.
    int x = 0, y = 0;
    check(press_asset(controller, "assets/block.dvox", x, y), "press again");
    controller.pointer_move(x + 40, y);
    controller.pointer_up(PointerButton::Primary, x + 40, y);
    check(controller.workspace().document().objects().size() == before &&
              controller.status().text.find("outside the viewport") != std::string::npos,
          "releasing outside the viewport places nothing");

    // A short click on a row just selects it.
    check(press_asset(controller, "assets/block.dvox", x, y), "click a row");
    controller.pointer_up(PointerButton::Primary, x, y);
    check(!controller.asset_drag().armed && controller.workspace().document().objects().size() == before,
          "a click is not a drag");

    // Unsupported assets say why.
    check(drag_asset(controller, "assets/notes.lua", {0.0F, 0.0F, 0.0F}), "a script can be dragged");
    check(controller.status().error && controller.status().text.find("Drag prefabs") != std::string::npos,
          "but dropping it explains what can be placed: " + controller.status().text);
}

void test_material_drop() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    EditorDocument& document = controller.workspace().document();
    const MaterialId brick = controller.materials().next_available_id() - 1;
    check(drag_asset(controller, "assets/brick_red.dmat", {2.5F, 1.0F, 1.0F}), "drag the material onto the wall");
    const EditorObject* wall = document.find_object(2);
    check(wall->voxels->material_at({0, 0, 0}) == brick && wall->voxels->material_at({1, 3, 1}) == brick,
          "every voxel of the wall is repainted: " + controller.status().text);
    check(document.find_object(1)->voxels->material_at({0, 0, 0}) == 1, "the floor is untouched");
    check(controller.dispatch_action("edit.undo") && document.find_object(2)->voxels->material_at({0, 0, 0}) == 3, "one undo");

    // With voxels of the wall selected, only those change.
    (void)controller.dispatch_action("select.domain_voxels");
    controller.select_voxels(2, {Int3{0, 0, 0}, Int3{0, 1, 0}}, SelectionOperation::Replace);
    check(drag_asset(controller, "assets/brick_red.dmat", {2.5F, 1.0F, 1.0F}), "drag onto the wall again");
    check(document.find_object(2)->voxels->material_at({0, 1, 0}) == brick &&
              document.find_object(2)->voxels->material_at({1, 3, 1}) == 3,
          "only the selected voxels are repainted");

    check(drag_asset(controller, "assets/nowhere.dmat", {2.5F, 1.0F, 1.0F}), "drag an unknown material");
    check(controller.status().error && controller.status().text.find("No material named") != std::string::npos,
          "an unknown material says it is not in the library");

    document.find_object(2)->flags.locked = true;
    (void)controller.dispatch_action("select.domain_objects");
    check(drag_asset(controller, "assets/brick_red.dmat", {2.5F, 1.0F, 1.0F}), "drag onto a locked wall");
    check(controller.status().text.find("Unlock Wall") != std::string::npos, "a locked object is refused");
}

void test_model_drop() {
    auto owner = make_controller();
    NativeEditorController& controller = *owner;
    check(drag_asset(controller, "assets/box.obj", {-1.0F, 0.0F, 2.0F}), "drag the model");
    check(controller.import_options().open && controller.import_options().dropPoint.has_value(),
          "the import options open, remembering the drop point: " + controller.status().text);
    check(controller.accept_import_options(), "import with the defaults");
    const EditorObject* group = newest(controller);
    check(group && group->name == "Imported box", "the import group is selected");
    if (group) {
        EditorObjectBounds bounds;
        for (const auto& [id, object] : controller.workspace().document().objects()) {
            if (object.parent != group->id) continue;
            const EditorObjectBounds b = object_world_bounds(object);
            bounds = bounds.valid ? EditorObjectBounds{{std::min(bounds.minimum.x, b.minimum.x), std::min(bounds.minimum.y, b.minimum.y),
                                                        std::min(bounds.minimum.z, b.minimum.z)},
                                                       {std::max(bounds.maximum.x, b.maximum.x), std::max(bounds.maximum.y, b.maximum.y),
                                                        std::max(bounds.maximum.z, b.maximum.z)}, true}
                                  : b;
        }
        check(bounds.valid && near(bounds.minimum.y, 0.0F, 0.06F) && near((bounds.minimum.x + bounds.maximum.x) * 0.5F, -1.0F, 0.15F),
              "the model's bottom centre lands on the drop point");
        check(near(group->transform.position.x, bounds.minimum.x, 0.15F), "the group moved with it (Reimport keeps the place)");
    }
}
}  // namespace

int main() {
    make_project();
    test_prefab_and_voxel_asset();
    test_ghost_and_cancel();
    test_material_drop();
    test_model_drop();
    std::filesystem::remove_all(kRoot);
    if (g_failures == 0) std::printf("asset drag tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
