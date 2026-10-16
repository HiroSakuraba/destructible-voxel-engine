// Select by property (artist worklist ART-019): the query language, the hierarchy filter,
// Edit > Select Matching Filter and Edit > Select Similar.
#include "dve/editor_native.hpp"
#include "dve/editor_selection_query.hpp"

#include <cstdio>
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

EditorObject voxel_object(EditorObjectId id, std::string name, MaterialId material) {
    EditorObject object(id, std::move(name));
    (void)object.voxels->set_voxel({0, 0, 0}, material);
    return object;
}

// 1 Rock A (voxels, mat 1, tag rock, layer 2) under 5 Cliff; 2 Rock B (voxels, mat 1, tag rock);
// 3 Crate (voxels, mat 2, locked); 4 Pipe (mesh, tag metal); 5 Cliff (group); 6 Lamp (helper,
// spawn component); 7 Tree (prefab instance, voxels mat 3); 8 Tree 2 (same prefab, hidden).
EditorDocument make_document() {
    EditorDocument document("Query");
    EditorObject rockA = voxel_object(1, "Rock A", 1);
    rockA.tags = {"rock"};
    rockA.layer = 2;
    rockA.parent = 5;
    document.add_object(std::move(rockA));
    EditorObject rockB = voxel_object(2, "Rock B", 1);
    rockB.tags = {"Rock", "big"};
    document.add_object(std::move(rockB));
    EditorObject crate = voxel_object(3, "Crate", 2);
    crate.flags.locked = true;
    document.add_object(std::move(crate));
    EditorObject pipe(4, "Pipe");
    pipe.polygon = make_editor_polygon_cube(4);
    pipe.tags = {"metal"};
    document.add_object(std::move(pipe));
    document.add_object(EditorObject(5, "Cliff"));
    EditorObject lamp(6, "Lamp");
    Component spawn;
    spawn.type = "dve.spawn";
    lamp.components.push_back(spawn);
    document.add_object(std::move(lamp));
    EditorPrefabLink link;
    link.prefabAsset = "prefabs/tree.dveprefab";
    link.instanceId = 1;
    EditorObject tree = voxel_object(7, "Tree", 3);
    tree.prefabLink = link;
    document.add_object(std::move(tree));
    EditorObject tree2 = voxel_object(8, "Tree 2", 3);
    link.instanceId = 2;
    tree2.prefabLink = link;
    tree2.flags.visible = false;
    document.add_object(std::move(tree2));
    return document;
}

std::set<EditorObjectId> query(const EditorDocument& document, std::string_view text) {
    std::set<EditorObjectId> ids;
    const auto parsed = parse_selection_query(text);
    check(parsed.has_value(), "parses: " + std::string(text));
    if (!parsed) return ids;
    for (const auto& [id, object] : document.objects())
        if (selection_query_matches(document, object, *parsed)) ids.insert(id);
    return ids;
}

std::string list(const std::set<EditorObjectId>& ids) {
    std::string out;
    for (const EditorObjectId id : ids) out += std::to_string(id) + " ";
    return out;
}

void test_query_language() {
    const EditorDocument document = make_document();
    check(looks_like_selection_query("type:mesh") && looks_like_selection_query("rock -tag:big") && !looks_like_selection_query("rock a"),
          "query syntax is recognised");
    check(query(document, "rock") == std::set<EditorObjectId>{1, 2}, "a bare word matches names: " + list(query(document, "rock")));
    check(query(document, "tag:rock") == std::set<EditorObjectId>{1, 2}, "tags ignore case");
    check(query(document, "tag:rock -tag:big") == std::set<EditorObjectId>{1}, "negation");
    check(query(document, "tag:metal,big") == std::set<EditorObjectId>{2, 4}, "commas mean or: " + list(query(document, "tag:metal,big")));
    check(query(document, "type:mesh") == std::set<EditorObjectId>{4}, "type:mesh");
    check(query(document, "type:group") == std::set<EditorObjectId>{5}, "type:group");
    check(query(document, "type:helper") == std::set<EditorObjectId>{6}, "type:helper");
    check(query(document, "layer:2") == std::set<EditorObjectId>{1}, "layer");
    check(query(document, "material:1") == std::set<EditorObjectId>{1, 2}, "material by palette id");
    check(query(document, "locked:yes") == std::set<EditorObjectId>{3}, "locked:yes");
    check(query(document, "visible:no") == std::set<EditorObjectId>{8}, "visible:no");
    check(query(document, "prefab:tree") == std::set<EditorObjectId>{7, 8}, "prefab by asset name");
    check(query(document, "component:spawn") == std::set<EditorObjectId>{6}, "component type");
    check(query(document, "parent:cliff") == std::set<EditorObjectId>{1}, "parent path");
    check(query(document, "type:voxels -locked:yes tree") == std::set<EditorObjectId>{7, 8}, "terms combine with and");
    std::string error;
    check(!parse_selection_query("colour:red", &error) && error.find("Unknown filter key") != std::string::npos, "unknown key: " + error);
    check(!parse_selection_query("locked:maybe", &error) && error.find("yes or no") != std::string::npos, "yes/no keys: " + error);
}

void test_editor_actions() {
    auto owner = std::make_unique<NativeEditorController>(EditorWorkspace(make_document()));
    NativeEditorController& c = *owner;
    c.resize(1280, 720);
    check(!c.dispatch_action("select.matching"), "Select Matching needs a filter");
    c.set_hierarchy_filter("tag:rock");
    const auto rows = c.hierarchy_order();
    check(std::find(rows.begin(), rows.end(), 2) != rows.end() && std::find(rows.begin(), rows.end(), 4) == rows.end(),
          "the hierarchy filter understands queries");
    check(std::find(rows.begin(), rows.end(), 5) != rows.end(), "and still shows the parents of matches");
    check(c.dispatch_action("select.matching") && c.workspace().selected_objects() == std::set<EditorObjectId>{1, 2},
          "Select Matching Filter selects the matches: " + list(c.workspace().selected_objects()));
    c.set_hierarchy_filter("crate");
    check(c.dispatch_action("select.matching") && c.workspace().selected_objects() == std::set<EditorObjectId>{3}, "plain names still work");
    c.set_hierarchy_filter("colour:red");
    check(!c.dispatch_action("select.matching") && c.status().text.find("Unknown filter key") != std::string::npos, "a bad query says why");

    c.workspace().select_object(1);
    check(c.dispatch_action("select.similar_material") && c.workspace().selected_objects() == std::set<EditorObjectId>{1, 2},
          "Same Material: " + list(c.workspace().selected_objects()));
    c.workspace().select_object(1);
    check(c.dispatch_action("select.similar_type") && c.workspace().selected_objects() == std::set<EditorObjectId>{1, 2, 3, 7},
          "Same Type skips the hidden tree: " + list(c.workspace().selected_objects()));
    (void)c.dispatch_action("select.mask_locked");
    c.workspace().select_object(1);
    (void)c.dispatch_action("select.similar_type");
    check(!c.workspace().is_selected(3), "Same Type respects the selection masks");
    c.workspace().select_object(2);
    // Codex review: Rock A sits inside the closed group Cliff, which the viewport cannot reach.
    check(c.dispatch_action("select.similar_tag") && c.workspace().selected_objects() == std::set<EditorObjectId>{2},
          "Same Tag skips pieces of a closed group: " + list(c.workspace().selected_objects()));
    check(c.status().text.find("closed group") != std::string::npos, "and says so: " + c.status().text);
    c.workspace().select_object(5);
    check(c.dispatch_action("edit.open_group"), "open Cliff");
    c.workspace().select_object(2);
    check(c.dispatch_action("select.similar_tag") && c.workspace().selected_objects() == std::set<EditorObjectId>{1, 2},
          "Same Tag reaches inside an open group: " + list(c.workspace().selected_objects()));
    c.workspace().select_object(1);
    check(c.dispatch_action("select.similar_layer") && c.workspace().selected_objects() == std::set<EditorObjectId>{1}, "Same Layer");
    c.workspace().select_object(7);
    check(c.dispatch_action("select.similar_prefab") && c.workspace().selected_objects() == std::set<EditorObjectId>{7},
          "Same Prefab (the other instance is hidden)");
    c.workspace().select_object(4);
    check(!c.dispatch_action("select.similar_prefab"), "Same Prefab needs an instance selected");
    c.workspace().clear_selection();
    check(!c.dispatch_action("select.similar_type"), "Select Similar needs a selection");
}

// Codex review: a mesh's material library can keep unused entries; only drawn ones count.
void test_polygon_materials_used() {
    EditorDocument document("Materials");
    EditorObject pipe(1, "Pipe");
    pipe.polygon = make_editor_polygon_cube(1);
    check(!pipe.polygon->materials.empty() && !pipe.polygon->submeshes.empty(), "the cube has a material and a submesh");
    pipe.polygon->materials[0].name = "Steel";
    for (auto& submesh : pipe.polygon->submeshes) submesh.materialIndex = 0;
    auto stale = pipe.polygon->materials[0];
    stale.name = "Stale";
    pipe.polygon->materials.push_back(stale);
    document.add_object(std::move(pipe));
    check(query(document, "material:steel") == std::set<EditorObjectId>{1}, "the drawn material matches");
    check(query(document, "material:stale").empty(), "an unused library entry does not");
    const auto used = selection_object_materials(*document.find_object(1));
    check(used == std::set<std::string>{"steel"}, "Same Material compares only drawn materials");
}

// Codex review: helpers have no geometry, so they get a viewport marker that clicks and boxes pick.
void test_helper_markers() {
    EditorDocument document("Helpers");
    EditorObject spawn(1, "Spawn");
    spawn.transform.position = {2.0F, 1.0F, 0.0F};
    document.add_object(std::move(spawn));
    auto owner = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    NativeEditorController& c = *owner;
    c.resize(1280, 720);
    c.camera().position = {2.0F, 1.0F, 12.0F};
    c.camera().target = {2.0F, 1.0F, 0.0F};
    const auto markers = c.helper_markers();
    check(markers.size() == 1U && markers.front().id == 1, "the helper has a marker");
    if (markers.empty()) return;
    const int x = static_cast<int>(markers.front().x), y = static_cast<int>(markers.front().y);
    c.pointer_move(x + 2, y + 1);
    c.pointer_down(PointerButton::Primary, x + 2, y + 1);
    c.pointer_up(PointerButton::Primary, x + 2, y + 1);
    check(c.workspace().selected_objects() == std::set<EditorObjectId>{1}, "a click on the marker selects the helper: " + c.status().text);
    c.workspace().clear_selection();
    c.pointer_down(PointerButton::Primary, x - 40, y - 40);
    c.pointer_move(x + 40, y + 40);
    c.pointer_up(PointerButton::Primary, x + 40, y + 40);
    check(c.workspace().selected_objects() == std::set<EditorObjectId>{1}, "a box around the marker selects it");
    c.workspace().clear_selection();
    (void)c.dispatch_action("select.mask_helpers");
    c.pointer_down(PointerButton::Primary, x, y);
    c.pointer_up(PointerButton::Primary, x, y);
    check(c.workspace().selected_objects().empty(), "with helpers masked out the click skips it");
    c.pointer_down(PointerButton::Primary, x - 40, y - 40);
    c.pointer_move(x + 40, y + 40);
    c.pointer_up(PointerButton::Primary, x + 40, y + 40);
    check(c.workspace().selected_objects().empty(), "and so does a box");
}
}  // namespace

int main() {
    test_query_language();
    test_editor_actions();
    test_polygon_materials_used();
    test_helper_markers();
    if (g_failures == 0) std::printf("select by property tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
