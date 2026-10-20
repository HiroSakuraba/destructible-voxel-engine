// CAD edit guard (exact CAD, first batch): ask before an edit changes the triangles of a CAD object.
//
// A CAD object's triangles are cut from its exact B-rep. Editing them directly (bevel, extrude,
// Manifold Booleans, moving vertices, ...) makes them disagree with the exact source, so exact
// Booleans, CAD Detail, measuring and STEP export would silently use the old shape. Instead the
// editor asks first: Enter continues and the object becomes a plain mesh (its exact link is
// switched off as its own undo step, so Undo brings the link back); Escape cancels. Edits that
// keep the triangles matching the exact shape (object transforms, exact Booleans, materials, UVs,
// selection, CAD Detail) are not asked about.
#include "dve/editor_native.hpp"

#include <algorithm>
#include <array>
#include <memory>

#include "dve/editor_exact_cad.hpp"

namespace dve::editor {

bool NativeEditorController::is_cad_mesh_edit(std::string_view a, bool polygonsDomain) {
    static constexpr std::array<std::string_view, 14> kPolygon{
        "polygon.bevel", "polygon.extrude", "polygon.inset", "polygon.recalculate_normals", "polygon.flip_normals",
        "polygon.weld", "polygon.dissolve_vertices", "polygon.loop_cut", "polygon.dissolve_edges", "polygon.clean_unused",
        "polygon.extract", "polygon.slice", "polygon.join", "polygon.separate"};
    static constexpr std::array<std::string_view, 11> kMesh{
        "mesh.bevel_edges", "mesh.chamfer_edges", "mesh.fillet_edges", "mesh.subdivide", "mesh.split_edges",
        "mesh.flip_diagonal", "mesh.bridge", "mesh.cap", "mesh.hard_normals", "mesh.smooth_normals", "subdiv.from_polygon"};
    const auto in = [&](const auto& list) { return std::find(list.begin(), list.end(), a) != list.end(); };
    if (in(kPolygon) || in(kMesh)) return true;
    if (a.starts_with("mesh.boolean_")) return true;  // Manifold Booleans; mesh.exact_boolean_* stay exact
    if (a.starts_with("mirror.mesh_") || a.starts_with("mirror.faces_") || a.starts_with("mirror.live_")) return true;
    if (polygonsDomain && (a == "edit.delete" || a == "edit.cut" || a == "edit.duplicate")) return true;
    // Direct component edits (gizmo drags, typed or snapped moves of vertices/edges/faces).
    return polygonsDomain && (a == "gizmo" || a == "transform");
}

std::vector<EditorObjectId> NativeEditorController::cad_mesh_edit_targets() const {
    std::vector<EditorObjectId> candidates;
    if (selectionDomain_ == SelectionDomain::Polygons) candidates.push_back(polygonSelection_.object);
    else candidates.assign(workspace_.selected_objects().begin(), workspace_.selected_objects().end());
    std::vector<EditorObjectId> targets;
    for (const EditorObjectId id : candidates)
        if (const EditorObject* object = workspace_.document().find_object(id); object && object->polygon && exact_brep_source(*object))
            targets.push_back(id);
    return targets;
}

bool NativeEditorController::cad_link_dropped(EditorObjectId id) const {
    const EditorObject* object = workspace_.document().find_object(id);
    if (!object || exact_brep_source(*object)) return false;
    return std::any_of(object->components.begin(), object->components.end(),
                       [](const Component& c) { return c.type == kExactBrepComponent && !c.enabled; });
}

bool NativeEditorController::ask_before_cad_mesh_edit(std::string_view actionId) {
    if (cadEditConfirmed_) return false;
    if (pendingDestructiveAction_ == PendingDestructiveAction::DropCadLink) return true;  // already asking
    const bool polygons = selectionDomain_ == SelectionDomain::Polygons;
    if (!is_cad_mesh_edit(actionId, polygons)) return false;
    std::vector<EditorObjectId> targets = cad_mesh_edit_targets();
    if (targets.empty()) return false;
    // Locked objects and other refusals are left to the action itself: asking first would drop
    // the link for an edit that then does nothing.
    for (const EditorObjectId id : targets)
        if (const EditorObject* object = workspace_.document().find_object(id); object && object->flags.locked) return false;
    pendingCadAction_ = std::string(actionId);
    pendingCadObjects_ = std::move(targets);
    if (!workspace_.preferences().confirmDestructiveActions) {
        // Confirmations switched off: drop the link and let the edit run now.
        pendingCadAction_.clear();
        return !drop_cad_link(std::exchange(pendingCadObjects_, {}));
    }
    const EditorObject* first = workspace_.document().find_object(pendingCadObjects_.front());
    const std::string who = pendingCadObjects_.size() == 1 && first ? first->name
                                                                    : std::to_string(pendingCadObjects_.size()) + " CAD objects";
    pendingCadMessage_ = "Editing triangles drops the exact CAD link of " + who + " (it becomes a plain mesh).\n"
                         "Enter continues, Escape cancels. Undo brings the link back.";
    pendingDestructiveAction_ = PendingDestructiveAction::DropCadLink;
    set_status("Editing triangles drops the exact CAD link: Enter continues as a plain mesh, Escape cancels", false, 0.0F);
    return true;
}

bool NativeEditorController::drop_cad_link(const std::vector<EditorObjectId>& objects) {
    auto compound = std::make_unique<CompoundCommand>("Drop exact CAD link");
    for (const EditorObjectId id : objects) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object) continue;
        for (const Component& component : object->components)
            if (component.type == kExactBrepComponent && component.enabled)
                compound->add(std::make_unique<SetComponentEnabledCommand>(id, component.id, true, false, "Drop exact CAD link"));
    }
    if (compound->empty()) return true;
    const CommandResult dropped = workspace_.commands().execute(workspace_.document(), std::move(compound));
    if (!dropped.success) { set_status("Could not drop the CAD link: " + dropped.message, true); return false; }
    refresh_menu_state();
    return true;
}

void NativeEditorController::drop_cad_link_and_continue() {
    const std::string action = std::exchange(pendingCadAction_, {});
    if (!drop_cad_link(std::exchange(pendingCadObjects_, {}))) return;
    if (action == "gizmo" || action == "transform") {
        set_status("Now a plain mesh (Undo restores the CAD link): drag again to edit", false, 4.0F);
        return;
    }
    cadEditConfirmed_ = true;
    const bool ran = dispatch_action(action);
    cadEditConfirmed_ = false;
    if (ran && !status_.error) set_status(status_.text + " (now a plain mesh; Undo twice restores the CAD link)", false, 4.0F);
}

}  // namespace dve::editor
