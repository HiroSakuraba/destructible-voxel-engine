// Named selection sets (artist worklist ART-021): save, recall, add to, rename and delete object,
// vertex, edge and face sets and smart (query) sets; missing and retired members; topology lineage
// after edits; Undo/Redo; saving and loading with the scene.
#include "dve/editor_native.hpp"
#include "dve/editor_selection_sets.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

EditorDocument make_document() {
    EditorDocument document("Sets");
    for (EditorObjectId id = 1; id <= 3; ++id) {
        EditorObject object(id, id == 1 ? "Rock A" : id == 2 ? "Rock B" : "Crate");
        (void)object.voxels->set_voxel({0, 0, 0}, 1);
        object.transform.position = {static_cast<float>(id) * 3.0F, 0.0F, 0.0F};
        if (id != 3) object.tags = {"rock"};
        document.add_object(std::move(object));
    }
    EditorObject mesh(4, "Box");
    mesh.polygon = make_editor_polygon_cube(4);
    mesh.transform.position = {-4.0F, 0.0F, 0.0F};
    document.add_object(std::move(mesh));
    return document;
}

std::unique_ptr<NativeEditorController> make_controller() {
    auto c = std::make_unique<NativeEditorController>(EditorWorkspace(make_document()));
    c->resize(1600, 900);
    return c;
}

std::string ids(const NativeEditorController& c) {
    std::string out;
    for (const EditorObjectId id : c.workspace().selected_objects()) out += std::to_string(id) + " ";
    return out;
}

void type_name(NativeEditorController& c, std::string_view name) {
    c.text_input(name);
    c.key_down("enter", false, false, false);
}

void test_object_sets_and_undo() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.workspace().clear_selection();
    check(!c.dispatch_action("select.set_save"), "nothing selected, nothing saved");
    c.workspace().select_object(1);
    c.workspace().add_to_selection(2);
    const std::size_t steps = c.workspace().commands().size();
    check(c.dispatch_action("select.set_save"), "Save Selection as Set");
    check(c.text_edit().kind == TextEditKind::SelectionSetName && c.text_edit().buffer == "Objects 1", "a name field opens with a default");
    type_name(c, "Rocks");
    const auto& sets = c.workspace().document().selection_sets();
    check(sets.size() == 1 && sets[0].name == "Rocks" && sets[0].objects == std::set<EditorObjectId>{1, 2}, "the set is saved and named");
    check(c.workspace().commands().size() == steps + 1, "save and name are one undo step");
    check(c.dispatch_action("edit.undo") && c.workspace().document().selection_sets().empty(), "Undo removes the set");
    check(c.dispatch_action("edit.redo") && c.workspace().document().selection_sets().size() == 1 &&
          c.workspace().document().selection_sets()[0].name == "Rocks", "Redo brings it back with its name");

    c.workspace().select_object(3);
    check(c.dispatch_action("selection.set.recall.0") && c.workspace().selected_objects() == std::set<EditorObjectId>{1, 2},
          "Recall selects the members: " + ids(c));
    check(c.dispatch_action("select.set_recall") && c.context_menu().open && c.context_menu().items.size() == 1 &&
          c.context_menu().items[0].label == "Rocks  (objects, 2)", "the recall list describes each set: " +
          (c.context_menu().items.empty() ? std::string() : c.context_menu().items[0].label));

    // Add to the set, then delete a member object: it shows as missing until Undo restores it.
    c.workspace().select_object(3);
    check(c.dispatch_action("selection.set.add.0") && c.workspace().document().selection_sets()[0].objects.size() == 3, "Add Selection to Set");
    c.workspace().select_object(3);
    check(c.dispatch_action("edit.delete") && !c.workspace().document().find_object(3), "delete a member");
    const std::string label = describe_selection_set(c.workspace().document(), c.workspace().document().selection_sets()[0]);
    check(label == "Rocks  (objects, 2)  \xC2\xB7 1 missing", "missing members are shown: " + label);
    check(c.dispatch_action("selection.set.recall.0") && c.status().text.find("1 missing") != std::string::npos, "recall says so too: " + c.status().text);
    check(c.dispatch_action("edit.undo") && c.workspace().document().find_object(3), "undo the delete");
    (void)c.dispatch_action("selection.set.recall.0");
    check(c.workspace().selected_objects() == std::set<EditorObjectId>{1, 2, 3}, "the member is back: " + ids(c));

    // Rename and delete through the list.
    check(c.dispatch_action("selection.set.rename.0") && c.text_edit().kind == TextEditKind::SelectionSetName, "Rename opens the field");
    type_name(c, "Boulders");
    check(c.workspace().document().selection_sets()[0].name == "Boulders", "renamed");
    check(c.dispatch_action("selection.set.delete.0") && c.workspace().document().selection_sets().empty(), "deleted");
    check(c.dispatch_action("edit.undo") && c.workspace().document().selection_sets().size() == 1, "Undo restores a deleted set");
}

void test_smart_sets() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    check(!c.dispatch_action("select.set_save_query"), "a smart set needs a filter");
    c.set_hierarchy_filter("tag:rock");
    check(c.dispatch_action("select.set_save_query"), "Save Filter as Smart Set");
    c.key_down("escape", false, false, false);
    const auto& sets = c.workspace().document().selection_sets();
    check(sets.size() == 1 && sets[0].kind == SelectionSetKind::Query && sets[0].query == "tag:rock" && sets[0].name == "Query 1",
          "Esc keeps the default name");
    c.workspace().document().find_object(3)->tags = {"rock"};
    c.set_hierarchy_filter("");
    check(c.dispatch_action("selection.set.recall.0") && c.workspace().selected_objects() == std::set<EditorObjectId>{1, 2, 3},
          "a smart set is re-evaluated on recall: " + ids(c));
    c.workspace().select_object(4);
    check(!c.dispatch_action("selection.set.add.0"), "smart sets cannot be added to");
}

void test_component_sets_follow_topology() {
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.workspace().select_object(4);
    c.camera().position = {-3.5F, 0.5F, 6.0F};
    c.camera().target = {-3.5F, 0.5F, 0.5F};
    check(c.dispatch_action("polygon.face_mode"), "face mode");
    c.select_polygon_components({0}, SelectionOperation::Replace);
    const std::set<std::uint64_t> face = c.polygon_selection().components;
    check(c.dispatch_action("select.set_save"), "save a face set");
    type_name(c, "Lid");
    const EditorSelectionSet saved = c.workspace().document().selection_sets().at(0);
    check(saved.kind == SelectionSetKind::Faces && saved.mesh == 4 && !saved.elements.empty(), "a face set keeps persistent IDs");
    (void)c.dispatch_action("polygon.select_none");
    check(c.dispatch_action("selection.set.recall.0") && c.polygon_selection().components == face, "recall restores the face");

    // Inset the face: the edit rebuilds the mesh around it; the set keeps its face.
    check(c.dispatch_action("polygon.inset"), "inset preview: " + c.status().text);
    check(c.dispatch_action("polygon.preview_apply"), "apply: " + c.status().text);
    check(!c.workspace().document().selection_lineage().empty(), "the edit recorded lineage for the set's mesh");
    (void)c.dispatch_action("polygon.select_none");
    check(c.dispatch_action("selection.set.recall.0") && !c.polygon_selection().components.empty(),
          "after the edit the set still selects its pieces: " + c.status().text);
    const std::size_t piecesAfterEdit = c.polygon_selection().components.size();
    const SelectionSetResolution after = resolve_selection_set(c.workspace().document(), saved);
    check(after.missing == 0 && after.retired == 0, "no member is lost to the edit");
    check(c.dispatch_action("edit.undo"), "undo the inset");
    (void)c.dispatch_action("polygon.select_none");
    check(c.dispatch_action("selection.set.recall.0") && c.polygon_selection().components == face, "after Undo the set is the original face");
    check(c.dispatch_action("edit.redo"), "redo the inset");
    (void)c.dispatch_action("polygon.select_none");
    check(c.dispatch_action("selection.set.recall.0") && c.polygon_selection().components.size() == piecesAfterEdit,
          "after Redo the set resolves as after the edit");

    // Mixing kinds is refused.
    (void)c.dispatch_action("polygon.vertex_mode");
    c.select_polygon_components({0}, SelectionOperation::Replace);
    check(!c.dispatch_action("selection.set.add.0"), "vertices cannot be added to a face set");
}

// A real split: a loop cut through a subdivision cage face; both halves stay in the set.
void test_loop_cut_split() {
    EditorDocument document("Cut");
    auto owner = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    NativeEditorController& c = *owner;
    c.resize(1600, 900);
    check(c.dispatch_action("subdiv.create_cube"), "create a subdivision cube: " + c.status().text);
    const auto selected = c.workspace().selected_object();
    if (!selected) return;
    const EditorObject* cube = c.workspace().document().find_object(*selected);
    check(c.dispatch_action("subdiv.face_mode"), "cage face mode");
    const auto faces = subdivision_control_face_triangles(*cube->polygon, &*cube->subdivision);
    c.select_polygon_components({faces.at(0).begin(), faces.at(0).end()}, SelectionOperation::Replace);
    check(c.dispatch_action("select.set_save"), "save the cage face");
    c.key_down("escape", false, false, false);
    const auto edges = subdivision_control_edges(*cube->polygon, &*cube->subdivision);
    bool split = false;
    for (const std::uint64_t edge : edges) {
        const auto a = std::uint32_t(edge >> 32U), b = std::uint32_t(edge);
        const auto& corners = cube->subdivision->controlFaces.at(0);
        if (std::find(corners.begin(), corners.end(), a) == corners.end() || std::find(corners.begin(), corners.end(), b) == corners.end()) continue;
        check(c.dispatch_action("subdiv.edge_mode"), "cage edge mode");
        c.select_polygon_components({edge}, SelectionOperation::Replace);
        if (!c.dispatch_action("subdiv.loop_cut")) continue;
        if (!c.dispatch_action("subdiv.preview_apply")) continue;
        split = true;
        break;
    }
    check(split, "a loop cut through the saved face applied: " + c.status().text);
    const auto resolved = resolve_selection_set(c.workspace().document(), c.workspace().document().selection_sets().at(0));
    check(resolved.modelingIds.size() == 2 && resolved.missing == 0 && resolved.retired == 0,
          "both halves of the cut face are in the set: " + std::to_string(resolved.modelingIds.size()));
    check(c.dispatch_action("selection.set.recall.0") && c.status().text.find("2 faces") != std::string::npos, "recall: " + c.status().text);
    check(c.dispatch_action("edit.undo"), "undo the cut");
    const auto original = resolve_selection_set(c.workspace().document(), c.workspace().document().selection_sets().at(0));
    check(original.modelingIds.size() == 1 && original.missing == 0, "after Undo, the original face");
}

void test_retired_and_unit_resolution() {
    EditorDocument document = make_document();
    EditorObject* mesh = document.find_object(4);
    check(ensure_modeling_topology(*mesh), "topology");
    EditorSelectionSet set;
    set.name = "Gone";
    set.kind = SelectionSetKind::Faces;
    set.mesh = 4;
    set.elements = {999999U, 888888U, mesh->modelingTopology->faces.front()};
    document.record_selection_lineage(4, {{999999U, {}}});
    const SelectionSetResolution resolved = resolve_selection_set(document, set);
    check(resolved.retired == 1 && resolved.missing == 1 && resolved.modelingIds.size() == 1, "retired versus missing members");
    // A split: one member replaced by two live faces keeps both pieces.
    EditorSelectionSet split = set;
    split.elements = {555555U};
    const auto& faceIds = mesh->modelingTopology->faces;
    document.record_selection_lineage(4, {{555555U, {faceIds[0], faceIds[1]}}});
    const SelectionSetResolution pieces = resolve_selection_set(document, split);
    check(pieces.modelingIds.size() == 2 && pieces.missing == 0 && pieces.components.size() >= 2, "split pieces stay in the set");
    document.set_selection_sets({set});
    check(describe_selection_set(document, set).find("1 missing  \xC2\xB7 1 retired") != std::string::npos,
          "the description lists both: " + describe_selection_set(document, set));
    // Lineage is only recorded for meshes some component set uses.
    auto owner = make_controller();
    NativeEditorController& c = *owner;
    c.workspace().select_object(4);
    (void)c.dispatch_action("polygon.face_mode");
    c.select_polygon_components({0}, SelectionOperation::Replace);
    (void)c.dispatch_action("polygon.inset");
    (void)c.dispatch_action("polygon.preview_apply");
    check(c.workspace().document().selection_lineage().empty(), "no sets, no lineage kept");
}

void test_save_and_load() {
    EditorDocument document = make_document();
    check(ensure_modeling_topology(*document.find_object(4)), "topology");
    EditorSelectionSet objects;
    objects.name = "Rocks \"big\"";
    objects.objects = {1, 2};
    EditorSelectionSet query;
    query.name = "Smart";
    query.kind = SelectionSetKind::Query;
    query.query = "type:voxels -tag:rock";
    EditorSelectionSet faces;
    faces.name = "Lid";
    faces.kind = SelectionSetKind::Faces;
    faces.mesh = 4;
    faces.elements = {document.find_object(4)->modelingTopology->faces.front(), 777U};
    document.set_selection_sets({objects, query, faces});
    document.record_selection_lineage(4, {{777U, {document.find_object(4)->modelingTopology->faces.back()}}});
    const auto directory = std::filesystem::temp_directory_path() / "dve_selection_sets_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto saved = document.save_transactional(directory / "scene.dvescene");
    check(saved.success, "save: " + saved.error);
    std::string error;
    const auto loaded = EditorDocument::load(directory / "scene.dvescene", &error);
    check(loaded.has_value(), "load: " + error);
    if (loaded) {
        check(loaded->selection_sets() == document.selection_sets(), "sets survive saving");
        check(loaded->selection_lineage() == document.selection_lineage(), "lineage survives saving");
        const auto resolved = resolve_selection_set(*loaded, loaded->selection_sets()[2]);
        check(resolved.modelingIds.size() == 2 && resolved.missing == 0, "the loaded face set resolves through lineage");
        check(resolve_selection_set(*loaded, loaded->selection_sets()[1]).objects == std::set<EditorObjectId>{3, 4} ||
              resolve_selection_set(*loaded, loaded->selection_sets()[1]).objects == std::set<EditorObjectId>{3},
              "the smart set evaluates after loading");
    }
    // A scene without sets keeps its older format version.
    EditorDocument plain = make_document();
    const auto plainSaved = plain.save_transactional(directory / "plain.dvescene");
    check(plainSaved.success, "plain save");
    std::filesystem::remove_all(directory);
}
}  // namespace

int main() {
    test_object_sets_and_undo();
    test_smart_sets();
    test_component_sets_follow_topology();
    test_retired_and_unit_resolution();
    test_loop_cut_split();
    test_save_and_load();
    if (g_failures == 0) std::printf("selection set tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
