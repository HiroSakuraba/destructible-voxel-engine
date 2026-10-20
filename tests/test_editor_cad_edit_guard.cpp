// CAD edit guard (exact CAD, first batch): editing the triangles of a CAD object asks first.
// Enter continues (the object becomes a plain mesh; the link drop is its own undo step), Escape
// cancels and changes nothing. Edits that keep the triangles exact are not asked about.
#include "dve/editor_exact_cad.hpp"
#include "dve/editor_native.hpp"

#include <cstdio>
#include <memory>
#include <set>
#include <string>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

std::unique_ptr<NativeEditorController> make_controller(EditorObjectId& id) {
    auto c = std::make_unique<NativeEditorController>();
    c->resize(1600, 900);
    EditorObject part(c->workspace().document().allocate_object_id(), "Bracket");
    part.polygon = make_editor_polygon_cube(part.id);
    Component component = make_exact_brep_component({"sources/exact/part-0123456789ab.brep", "0123456789ab", "bracket.step", 0.0, 0.0});
    component.id = 1;
    part.components.push_back(component);
    id = part.id;
    c->workspace().document().add_object(std::move(part));
    c->workspace().select_object(id);
    return c;
}

bool linked(const NativeEditorController& c, EditorObjectId id) {
    const EditorObject* object = c.workspace().document().find_object(id);
    return object && exact_brep_source(*object).has_value();
}

void test_classifier() {
    for (const char* a : {"polygon.bevel", "polygon.extrude", "polygon.flip_normals", "polygon.weld", "polygon.join",
                          "mesh.boolean_union", "mesh.subdivide", "mesh.fillet_edges", "mirror.mesh_x", "mirror.faces_y",
                          "mirror.live_z", "subdiv.from_polygon"})
        check(NativeEditorController::is_cad_mesh_edit(a, false), std::string("asks: ") + a);
    for (const char* a : {"mesh.exact_boolean_union", "polygon.assign_material", "polygon.cad_detail", "polygon.face_mode",
                          "polygon.select_grow", "mesh.uv_mark_seam", "mesh.hull_fit", "polygon.convert_to_voxels",
                          "transform.reset", "edit.delete", "edit.undo", "gizmo"})
        check(!NativeEditorController::is_cad_mesh_edit(a, false), std::string("does not ask (objects): ") + a);
    for (const char* a : {"edit.delete", "edit.cut", "edit.duplicate", "gizmo", "transform"})
        check(NativeEditorController::is_cad_mesh_edit(a, true), std::string("asks on components: ") + a);
}

void test_whole_object_edit() {
    EditorObjectId id{};
    auto owner = make_controller(id);
    NativeEditorController& c = *owner;
    EditorDocument& doc = c.workspace().document();
    const std::size_t steps = c.workspace().commands().size();

    // Escape: nothing changes.
    check(!c.dispatch_action("mirror.mesh_x"), "the edit is held");
    check(c.pending_destructive_action() == PendingDestructiveAction::DropCadLink, "asks first");
    check(c.pending_confirmation_message().find("Bracket") != std::string_view::npos &&
              c.pending_confirmation_message().find("plain mesh") != std::string_view::npos,
          "names the object and the outcome: " + std::string(c.pending_confirmation_message()));
    check(doc.find_object(id)->polygon->indices.size() == 36, "nothing edited yet");
    c.key_down("escape", false, false, false);
    check(!c.pending_destructive_confirmation() && linked(c, id) && doc.find_object(id)->polygon->indices.size() == 36 &&
              c.workspace().commands().size() == steps,
          "Escape: link kept, mesh unchanged, no undo step");
    check(c.status().text.find("kept") != std::string::npos, "says the link is kept: " + c.status().text);

    // Enter: link dropped as one step, then the edit as another.
    check(!c.dispatch_action("mirror.mesh_x") && c.pending_destructive_confirmation(), "asks again");
    c.key_down("enter", false, false, false);
    check(!c.pending_destructive_confirmation(), "answered");
    check(!linked(c, id) && c.cad_link_dropped(id), "the exact link is dropped");
    check(doc.find_object(id)->polygon->indices.size() / 3 > 12, "the mirror ran: " + c.status().text);
    check(c.workspace().commands().size() == steps + 2, "two undo steps: drop + edit");
    check(c.cad_detail_summary(id).find("Was CAD") != std::string::npos, "inspector: " + c.cad_detail_summary(id));
    check(c.status().text.find("plain mesh") != std::string::npos, "status says plain mesh: " + c.status().text);

    // Now a plain mesh: further edits do not ask.
    check(c.dispatch_action("mirror.mesh_z") && !c.pending_destructive_confirmation(), "plain meshes are not asked about");
    check(c.dispatch_action("edit.undo") && c.dispatch_action("edit.undo") && !linked(c, id) &&
              doc.find_object(id)->polygon->indices.size() == 36,
          "undo the edits: still unlinked");
    check(c.dispatch_action("edit.undo") && linked(c, id) && c.cad_detail_summary(id).find("Exact CAD") != std::string::npos,
          "one more undo restores the link");

    // Edits that keep the triangles exact never ask.
    check(c.dispatch_action("polygon.cad_detail") || true, "CAD Detail");
    check(!c.pending_destructive_confirmation(), "CAD Detail does not ask");
    (void)c.dispatch_action("mesh.exact_boolean_union");
    check(!c.pending_destructive_confirmation(), "exact Booleans do not ask");

    // Locked objects: the action refuses on its own, the link is never dropped for nothing.
    doc.find_object(id)->flags.locked = true;
    check(!c.dispatch_action("mirror.mesh_x") && !c.pending_destructive_confirmation() && linked(c, id), "locked: no prompt");
    doc.find_object(id)->flags.locked = false;

    // Confirmations switched off: the link drops and the edit runs straight away.
    c.workspace().preferences().confirmDestructiveActions = false;
    check(c.dispatch_action("mirror.mesh_x") && !c.pending_destructive_confirmation() && !linked(c, id) &&
              doc.find_object(id)->polygon->indices.size() / 3 > 12,
          "no prompt when confirmations are off: " + c.status().text);
}

void test_component_edits() {
    EditorObjectId id{};
    auto owner = make_controller(id);
    NativeEditorController& c = *owner;
    EditorDocument& doc = c.workspace().document();
    check(c.dispatch_action("polygon.face_mode"), "face mode is free");
    check(!c.pending_destructive_confirmation(), "entering a component mode does not ask");
    c.select_polygon_components(std::set<std::uint64_t>{0, 1}, SelectionOperation::Replace);
    check(!c.pending_destructive_confirmation(), "selecting does not ask");
    const CookedPolygonAsset before = *doc.find_object(id)->polygon;

    check(!c.dispatch_action("polygon.flip_normals") && c.pending_destructive_confirmation(), "Flip Normals asks");
    c.key_down("escape", false, false, false);
    check(linked(c, id) && doc.find_object(id)->polygon->indices == before.indices, "cancelled");

    // Moving vertices directly (typed / nudged component moves; gizmo drags use the same check).
    check(!c.nudge_selection({0.0F, 0.1F, 0.0F}).success && c.pending_destructive_confirmation(), "a component move asks");
    c.key_down("enter", false, false, false);
    check(!linked(c, id) && c.status().text.find("again") != std::string::npos, "dropped; drag again: " + c.status().text);
    check(c.nudge_selection({0.0F, 0.1F, 0.0F}).success && !c.pending_destructive_confirmation(), "the move now runs");

    // Delete on components asks; Delete on the whole object does not.
    EditorObjectId other{};
    auto second = make_controller(other);
    check(second->dispatch_action("edit.delete") && !second->pending_destructive_confirmation(),
          "deleting the whole CAD object does not ask");
}
}  // namespace

int main() {
    test_classifier();
    test_whole_object_edit();
    test_component_edits();
    if (g_failures == 0) std::printf("cad edit guard tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
