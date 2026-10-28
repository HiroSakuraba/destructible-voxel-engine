// Subdivision surface editor workflow: create/level/undo history, selection restore, locked,
// hidden and Play refusals with honest menu reasons, and the scene format bump (v10 only when a
// subdivision object exists; older polygon scenes still save as v7 and load; v9 scenes migrate).
// Also: edge-crease save/load, control-quad/triangulate undo and no-op honesty.
#include "dve/editor_native.hpp"
#include "dve/editor_subdivision.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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

void uv_smoothing_completion() {
    auto control=make_editor_polygon_cube(1);
    SubdivisionSettings settings{2,false};
    freeze_subdivision_topology(control,settings);
    const auto linear=evaluate_subdivision(control,settings);
    check(bool(linear.mesh),"linear subdivision UVs evaluate");
    settings.uvMode=SubdivisionUvMode::SmoothInterior;
    const auto smooth=evaluate_subdivision(control,settings);
    check(bool(smooth.mesh),"smooth-interior subdivision UVs evaluate");
    if(linear.mesh && smooth.mesh) {
        check(linear.mesh->indices.size()==smooth.mesh->indices.size(),
              "UV smoothing does not alter subdivision topology");
        check(linear.mesh->vertices.size()==smooth.mesh->vertices.size(),
              "UV smoothing preserves evaluated vertex count");
        for(std::size_t i=0;i<linear.mesh->vertices.size();++i){
            const auto& a=linear.mesh->vertices[i];const auto& b=smooth.mesh->vertices[i];
            near(a.position.x,b.position.x,"subdivision UV option does not move X");
            near(a.position.y,b.position.y,"subdivision UV option does not move Y");
            near(a.position.z,b.position.z,"subdivision UV option does not move Z");
        }
    }
    EditorDocument doc("Smooth UV save");
    EditorObject surface(1,"Surface");
    surface.polygon=control;
    surface.subdivision=settings;
    doc.add_object(std::move(surface));
    const auto root=std::filesystem::temp_directory_path()/"dve_subdivision_uv_mode_test";
    std::filesystem::remove_all(root);std::filesystem::create_directories(root);
    const auto path=root/"smooth.dvescene";
    const auto saved=doc.save_transactional(path);
    check(saved.success,"opt-in smooth UV scene saves");
    check(first_line(path)=="DVE_EDITOR_SCENE 15","smooth UV writes scene format 15");
    std::string error;
    auto loaded=EditorDocument::load(path,&error);
    check(bool(loaded),"smooth UV scene reloads: "+error);
    if(loaded){
        const auto* object=loaded->find_object(1);
        check(object && object->subdivision &&
              object->subdivision->uvMode==SubdivisionUvMode::SmoothInterior,
              "UV smoothing survives scene round trip");
    }
    std::filesystem::remove_all(root);
    // Invalid enum must be refused rather than deserializing an arbitrary mode.
    settings.uvMode=static_cast<SubdivisionUvMode>(55);
    const auto invalid=evaluate_subdivision(control,settings);
    check(!invalid.mesh && !invalid.error.empty(),"invalid UV smoothing mode refused");
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
        check(first_line(path) == "DVE_EDITOR_SCENE 10", "subdivision scene uses format 10, got " + first_line(path));
        std::string error; const auto loaded = EditorDocument::load(path, &error);
        check(bool(loaded), "format 10 scene loads: " + error);
        const auto* object = loaded->find_object(1);
        check(object && object->subdivision && object->subdivision->level == 3 && object->subdivision->preserveSharpEdges,
              "subdivision settings round-trip");
        check(object->polygon && object->polygon->indices.size() == make_editor_polygon_cube(1).indices.size(),
              "the control cage (not the evaluated surface) is reloaded");
        (void)cageHash;
    }
    std::filesystem::remove_all(root);
}

std::string read_all(const std::filesystem::path& path) { std::ifstream in(path); std::stringstream b; b << in.rdbuf(); return b.str(); }

void creases_and_migration() {
    const auto root = std::filesystem::temp_directory_path() / "dve_subdivision_crease_test";
    std::filesystem::remove_all(root); std::filesystem::create_directories(root);
    EditorDocument doc("Creases");
    EditorObject surface(1, "Surface"); surface.polygon = make_editor_polygon_cube(1);
    SubdivisionSettings settings{2, false};
    freeze_subdivision_topology(*surface.polygon, settings);
    const auto edges = subdivision_control_edges(*surface.polygon, &settings);
    check(edges.size() >= 2, "cube has control edges");
    settings.edgeCreases[*edges.begin()] = 4.0F; settings.edgeCreases[*std::next(edges.begin())] = 0.5F;
    const auto creased = evaluate_subdivision(*surface.polygon, settings);
    auto plain = settings; plain.edgeCreases.clear();
    const auto smooth = evaluate_subdivision(*surface.polygon, plain);
    check(creased.mesh && smooth.mesh, "creased and smooth surfaces evaluate: " + creased.error);
    bool differs = false;
    for (std::size_t i = 0; i < creased.mesh->vertices.size() && i < smooth.mesh->vertices.size(); ++i) {
        const auto a = creased.mesh->vertices[i].position, b = smooth.mesh->vertices[i].position;
        differs = differs || a.x != b.x || a.y != b.y || a.z != b.z;
    }
    check(differs, "edge creases change the evaluated surface");
    surface.subdivision = settings; doc.add_object(std::move(surface));
    const auto path = root / "creased.dvescene";
    const auto saved = doc.save_transactional(path);
    check(saved.success, "creased save: " + saved.error);
    std::string error; auto loaded = EditorDocument::load(path, &error);
    check(loaded && loaded->find_object(1) && loaded->find_object(1)->subdivision, "creased scene loads: " + error);
    check(*loaded->find_object(1)->subdivision == settings, "control faces and crease strengths round-trip exactly");

    // Hand-write the legacy format 9 form of the same scene (no topology records).
    std::istringstream lines(read_all(path)); std::ostringstream legacy; std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("DVE_EDITOR_SCENE", 0) == 0) line = "DVE_EDITOR_SCENE 9";
        if (line.rfind("control_face", 0) == 0 || line.rfind("edge_crease", 0) == 0) continue;
        legacy << line << '\n';
    }
    const auto legacyPath = root / "legacy.dvescene";
    { std::ofstream out(legacyPath); out << legacy.str(); }
    error.clear(); auto migrated = EditorDocument::load(legacyPath, &error);
    check(migrated && migrated->find_object(1) && migrated->find_object(1)->subdivision, "format 9 scene loads: " + error);
    const auto& m = *migrated->find_object(1)->subdivision;
    check(m.level == 2 && m.edgeCreases.empty(), "format 9 migration keeps level and has no creases");
    SubdivisionSettings expected{2, false}; freeze_subdivision_topology(*migrated->find_object(1)->polygon, expected);
    check(!m.controlFaces.empty() && m.controlFaces == expected.controlFaces, "format 9 legacy pairing is frozen on load");
    check(editor_surface_mesh(*migrated->find_object(1))->indices.size() == smooth.mesh->indices.size(),
          "migrated surface evaluates like the legacy pairing");
    std::filesystem::remove_all(root);
}

void control_face_undo() {
    auto owner = controller(); auto& c = *owner;
    check(c.dispatch_action("subdiv.from_polygon"), "convert");
    const auto id = *c.workspace().selected_object();
    const auto groupsOf = [&] {
        const auto* o = c.workspace().document().find_object(id);
        return subdivision_control_face_triangles(*o->polygon, &*o->subdivision);
    };
    const auto before = groupsOf();
    std::size_t quad = before.size();
    for (std::size_t i = 0; i < before.size(); ++i) if (before[i].size() == 2) { quad = i; break; }
    check(quad < before.size(), "cube cage starts with control quads");
    check(c.dispatch_action("subdiv.face_mode"), "face mode");
    const std::set<std::uint64_t> picked(before[quad].begin(), before[quad].end());
    c.select_polygon_components(picked, SelectionOperation::Replace); c.update(0);
    check(c.polygon_selection().components == picked, "quad selected");
    const auto history = c.workspace().commands().size();

    check(c.dispatch_action("subdiv.make_quad"), "make quad on an existing quad is an honest no-op");
    check(c.workspace().commands().size() == history, "no-op make quad adds no undo step");

    check(c.dispatch_action("subdiv.triangulate_faces"), "triangulate selected quad");
    check(groupsOf().size() == before.size() + 1, "quad becomes two control triangles");
    check(c.workspace().commands().size() == history + 1, "triangulate is one undo step");
    check(c.dispatch_action("subdiv.triangulate_faces"), "re-triangulating is an honest no-op");
    check(c.workspace().commands().size() == history + 1, "no-op triangulate adds no undo step");

    check(c.dispatch_action("subdiv.make_quad"), "rebuild the quad from the two triangles");
    check(groupsOf().size() == before.size(), "two triangles become one control quad");
    check(c.workspace().commands().size() == history + 2, "make quad is one undo step");

    check(c.dispatch_action("edit.undo"), "undo make quad");
    check(groupsOf().size() == before.size() + 1, "undo restores the triangles");
    c.select_polygon_components({}, SelectionOperation::Replace);
    check(c.dispatch_action("edit.undo"), "undo triangulate");
    check(groupsOf().size() == before.size(), "undo restores the quad");
    check(c.polygon_selection().components == picked, "undo restores the face selection");

    check(c.dispatch_action("subdiv.edge_mode"), "edge mode");
    const auto* o = c.workspace().document().find_object(id);
    const auto edges = subdivision_control_edges(*o->polygon, &*o->subdivision);
    c.select_polygon_components({*edges.begin()}, SelectionOperation::Replace); c.update(0);
    check(c.dispatch_action("subdiv.crease_2"), "set crease 2");
    check(c.workspace().document().find_object(id)->subdivision->edgeCreases.at(*edges.begin()) == 2.0F, "crease stored");
    const auto h2 = c.workspace().commands().size(); // history truncated to the crease step
    check(c.dispatch_action("subdiv.crease_2") && c.workspace().commands().size() == h2, "same crease adds no undo step");
    check(c.dispatch_action("edit.undo"), "undo crease");
    check(c.workspace().document().find_object(id)->subdivision->edgeCreases.empty(), "undo clears crease");
    check(groupsOf().size() == before.size() && c.workspace().commands().can_undo(), "crease was exactly one undo step");

    c.workspace().document().find_object(id)->flags.locked = true; c.update(0);
    const auto* crease = action(c, "subdiv.crease_1");
    check(crease && !crease->enabled && !crease->disabledReason.empty(), "locked: crease disabled with a reason");
    check(!c.dispatch_action("subdiv.crease_1") && !c.dispatch_action("subdiv.triangulate_faces"), "locked: topology edits refused");
}
} // namespace

int main() {
    try { create_level_undo(); refusals(); scene_format(); uv_smoothing_completion(); creases_and_migration(); control_face_undo(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Subdivision editor regression checks passed\n";
}
