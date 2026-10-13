// Subdivision surface editor workflow: create/level/undo history, selection restore, locked,
// hidden and Play refusals with honest menu reasons, and the scene format bump (v9 only when a
// subdivision object exists; older polygon scenes still save as v7 and load).
#include "dve/editor_native.hpp"
#include "dve/editor_subdivision.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace dve;
using namespace dve::editor;
namespace {
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::unique_ptr<NativeEditorController> controller() {
    EditorDocument document("Subdivision regression");
    EditorObject object(1, "Mesh"); object.polygon = make_editor_polygon_cube(1);
    document.add_object(std::move(object));
    auto c = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    c->resize(1600, 900); c->workspace().select_object(1);
    c->camera().position = {.5F, .5F, 5}; c->camera().target = {.5F, .5F, .5F};
    c->update(0);
    return c;
}
const MenuAction* action(const NativeEditorController& c, std::string_view id) {
    return std::as_const(c.workspace().menus()).find(id);
}
std::string first_line(const std::filesystem::path& path) { std::ifstream in(path); std::string line; std::getline(in, line); return line; }

void create_level_undo() {
    auto owner = controller(); auto& c = *owner;
    const auto objects = c.workspace().document().objects().size();
    const auto history = c.workspace().commands().size();
    check(c.dispatch_action("subdiv.from_polygon"), "convert polygon to subdivision");
    check(c.workspace().document().objects().size() == objects + 1, "conversion adds one object, source kept");
    check(c.workspace().commands().size() == history + 1, "conversion is one undo step");
    const auto id = *c.workspace().selected_object();
    const auto* surface = c.workspace().document().find_object(id);
    check(id != 1 && surface && surface->is_subdivision() && !surface->is_polygon(), "new object is a subdivision surface");
    check(editor_surface_mesh(*surface)->indices.size() > surface->polygon->indices.size(), "level 2 evaluates a denser surface");

    check(c.dispatch_action("subdiv.level_3"), "set level 3");
    check(c.workspace().document().find_object(id)->subdivision->level == 3, "level 3 applied");
    check(c.workspace().commands().size() == history + 2, "level change is one undo step");
    check(c.dispatch_action("subdiv.level_3"), "re-selecting the current level succeeds");
    check(c.workspace().commands().size() == history + 2, "no-op level change adds no undo step");

    c.workspace().select_object(1);
    check(c.dispatch_action("edit.undo"), "undo level");
    check(c.workspace().document().find_object(id)->subdivision->level == 2, "undo restores level 2");
    check(c.workspace().selected_object() == id, "undo restores the subdivision selection");
    check(c.dispatch_action("edit.undo"), "undo conversion");
    check(!c.workspace().document().find_object(id) && c.workspace().document().objects().size() == objects, "undo removes the surface");
    check(c.workspace().selected_object() == EditorObjectId{1}, "undo restores the source selection");
}

void refusals() {
    auto owner = controller(); auto& c = *owner;
    check(c.dispatch_action("subdiv.from_polygon"), "convert");
    const auto id = *c.workspace().selected_object();
    const auto history = c.workspace().commands().size();

    c.workspace().document().find_object(id)->flags.locked = true; c.update(0);
    const auto* up = action(c, "subdiv.level_up");
    check(up && !up->enabled && !up->disabledReason.empty(), "locked: Increase Level disabled with a reason");
    check(!c.dispatch_action("subdiv.level_up"), "locked: level change refused");
    check(c.workspace().document().find_object(id)->subdivision->level == 2 && c.workspace().commands().size() == history,
          "locked: nothing changed");
    c.workspace().document().find_object(id)->flags.locked = false;

    c.workspace().document().find_object(id)->flags.visible = false; c.update(0);
    check(!c.dispatch_action("subdiv.level_up"), "hidden: level change refused");
    check(!c.dispatch_action("subdiv.bake_polygon"), "hidden: bake refused");
    check(c.workspace().commands().size() == history, "hidden: nothing changed");
    c.workspace().document().find_object(id)->flags.visible = true; c.update(0);

    check(c.dispatch_action("physics.play"), "Play starts");
    c.update(0);
    const auto* create = action(c, "subdiv.create_cube");
    check(create && !create->enabled && !create->disabledReason.empty(), "Play: create disabled with a reason");
    check(!c.dispatch_action("subdiv.create_cube") && !c.dispatch_action("subdiv.level_up"), "Play: subdivision edits refused");
    check(c.workspace().commands().size() == history, "Play: nothing changed");
    check(c.dispatch_action("physics.stop"), "Stop");
}

void scene_format() {
    const auto root = std::filesystem::temp_directory_path() / "dve_subdivision_scene_test";
    std::filesystem::remove_all(root); std::filesystem::create_directories(root);
    {
        EditorDocument plain("Plain");
        EditorObject mesh(1, "Mesh"); mesh.polygon = make_editor_polygon_cube(1); plain.add_object(std::move(mesh));
        const auto path = root / "plain.dvescene";
        const auto saved = plain.save_transactional(path);
        check(saved.success, "plain save: " + saved.error);
        check(first_line(path) == "DVE_EDITOR_SCENE 7", "polygon-only scene keeps format 7, got " + first_line(path));
        std::string error; const auto loaded = EditorDocument::load(path, &error);
        check(loaded && loaded->find_object(1) && loaded->find_object(1)->is_polygon(), "format 7 scene loads: " + error);
    }
    {
        EditorDocument doc("Subdivision");
        EditorObject surface(1, "Surface"); surface.polygon = make_editor_polygon_cube(1);
        surface.subdivision = SubdivisionSettings{3, true}; doc.add_object(std::move(surface));
        const auto cageHash = doc.find_object(1)->polygon->contentHash;
        const auto path = root / "subdiv.dvescene";
        const auto saved = doc.save_transactional(path);
        check(saved.success, "subdivision save: " + saved.error);
        check(first_line(path) == "DVE_EDITOR_SCENE 9", "subdivision scene uses format 9, got " + first_line(path));
        std::string error; const auto loaded = EditorDocument::load(path, &error);
        check(bool(loaded), "format 9 scene loads: " + error);
        const auto* object = loaded->find_object(1);
        check(object && object->subdivision && object->subdivision->level == 3 && object->subdivision->preserveSharpEdges,
              "subdivision settings round-trip");
        check(object->polygon && object->polygon->indices.size() == make_editor_polygon_cube(1).indices.size(),
              "the control cage (not the evaluated surface) is reloaded");
        (void)cageHash;
    }
    std::filesystem::remove_all(root);
}
} // namespace

int main() {
    try { create_level_undo(); refusals(); scene_format(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Subdivision editor regression checks passed\n";
}
