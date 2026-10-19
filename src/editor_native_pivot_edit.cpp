// Pivot editing for any object (artist worklist ART-028). An object's pivot is its origin: moving
// it moves the object's position and shifts the geometry the other way, so nothing moves in the
// world. Meshes and subdivision cages move exactly; voxel objects keep whole cells, so their
// origin lands on the nearest voxel corner. Every change is one undo step and is saved.
//
//   Edit > Edit Pivot (now for one voxel object or mesh as well as a group): Move drags only the
//     origin, with the usual snapping (V), typed values and Esc; the new origin is drawn.
//   Edit > Pivot to Centre / Pivot to Bottom Centre: for groups and objects.
//   Edit > Pivot to Point Under Cursor: the nearest snap target on any object, the selection's
//     own vertices and corners included.
#include "dve/editor_native.hpp"

#include <cmath>

#include "dve/component.hpp"
#include "dve/editor_exact_cad.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/voxel_metric.hpp"

namespace dve::editor {

std::string NativeEditorController::origin_edit_refusal(EditorObjectId id) const {
    const EditorDocument& document = workspace_.document();
    const EditorObject* object = document.find_object(id);
    if (!object) return "Select an object";
    if (object->flags.locked) return "Unlock " + object->name + " before moving its pivot";
    if (object->prefabLink) return object->name + " is part of a prefab instance; unpack it or edit the prefab";
    if (object->text3d || object->gaborVolume) return "3D text and volumes keep their origin; move the object instead";
    if (!object->polygon && !(object->voxels && object->voxels->occupied_voxel_count() > 0))
        return object->name + " has no geometry to keep in place";
    if (find_component_by_type(std::span<const Component>(object->components), kExactBrepComponent))
        return object->name + " carries exact CAD geometry; its origin is fixed by the import";
    for (const EditorObjectId child : document.children_of(id))
        if (const EditorObject* attached = document.find_object(child); attached && attached->attachment)
            return "Moving the pivot needs children that are not socket-attached (" + attached->name + " is)";
    return {};
}

bool NativeEditorController::origin_drag() const {
    if (!pivotEditing_ || workspace_.selection_count() != 1 || !workspace_.selected_object()) return false;
    return !is_group(*workspace_.selected_object());
}

std::unique_ptr<IEditorCommand> NativeEditorController::make_move_origin_command(EditorObjectId id, Float3 worldPoint, Float3* placed,
                                                                                 std::string* error) const {
    const EditorObject* object = workspace_.document().find_object(id);
    if (const std::string refusal = origin_edit_refusal(id); !refusal.empty()) { if (error) *error = refusal; return nullptr; }
    const RigidTransform& pose = object->transform;
    // The offset in the object's own (unscaled) frame.
    const Float3 local = divide_components(rotate(conjugate(pose.rotation), subtract(worldPoint, pose.position)), object->voxelScale);
    Float3 shift = local;  // what every point of the geometry loses, in local metric units
    auto compound = std::make_unique<CompoundCommand>("Move pivot");
    if (object->polygon) {
        CookedPolygonAsset after = *object->polygon;
        for (PolygonVertex& vertex : after.vertices) vertex.position = subtract(vertex.position, shift);
        refresh_editor_polygon(after, false);
        compound->add(std::make_unique<ReplacePolygonCommand>(id, *object->polygon, std::move(after), "Move pivot (geometry)"));
    } else {
        // Whole cells only: the origin lands on the nearest voxel corner.
        const float size = object->voxelSizeMeters;
        // Nearest corner; on an exact tie keep the smaller step, so asking again changes nothing.
        const auto step = [&](float value) {
            const float cellsAway = value / size;
            if (std::abs(std::abs(cellsAway - std::trunc(cellsAway)) - 0.5F) < 1.0e-4F) return static_cast<int>(std::trunc(cellsAway));
            return static_cast<int>(std::lround(cellsAway));
        };
        const Int3 cells{step(local.x), step(local.y), step(local.z)};
        shift = {static_cast<float>(cells.x) * size, static_cast<float>(cells.y) * size, static_cast<float>(cells.z) * size};
        if (cells.x == 0 && cells.y == 0 && cells.z == 0) { if (error) *error = "The pivot is already on that voxel corner"; return nullptr; }
        const VoxelObjectState before = capture_voxel_object_state(*object);
        VoxelObjectState after = before;
        for (SparseVoxelStateEntry& entry : after.voxels) entry.voxel = {entry.voxel.x - cells.x, entry.voxel.y - cells.y, entry.voxel.z - cells.z};
        for (Int3& anchor : after.anchors) anchor = {anchor.x - cells.x, anchor.y - cells.y, anchor.z - cells.z};
        compound->add(std::make_unique<ReplaceVoxelObjectStateCommand>(id, before, std::move(after), "Move pivot (voxels)"));
    }
    if (shift.x == 0.0F && shift.y == 0.0F && shift.z == 0.0F) { if (error) *error = "The pivot is already there"; return nullptr; }
    RigidTransform moved = pose;
    moved.position = add(pose.position, rotate(pose.rotation, multiply_components(shift, object->voxelScale)));
    std::vector<ObjectTransformChange> changes{{id, pose, moved}};
    compound->add(std::make_unique<TransformObjectsCommand>(std::move(changes), "Move pivot (origin)"));
    if (placed) *placed = moved.position;
    return compound;
}

bool NativeEditorController::move_selected_origins(std::string_view where) {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) { set_status("Stop Play or Simulate before editing a pivot", true); return false; }
    std::optional<Float3> cursorPoint;
    if (where == "cursor") {
        const auto target = find_snap_target(hover_x(), hover_y(), std::nullopt, true);
        if (!target) { set_status("Point at a vertex, edge, face or surface to put the pivot there", true); return false; }
        cursorPoint = target->point;
    }
    auto compound = std::make_unique<CompoundCommand>(where == "cursor" ? "Pivot to point" : where == "bottom" ? "Pivot to bottom centre" : "Pivot to centre");
    std::size_t moved = 0;
    std::string lastError;
    for (const EditorObjectId id : workspace_.selected_objects()) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object) continue;
        Float3 point{};
        if (cursorPoint) point = *cursorPoint;
        else {
            const EditorObjectBounds bounds = object_world_bounds(*object);
            if (!bounds.valid) continue;
            point = {(bounds.minimum.x + bounds.maximum.x) * 0.5F, where == "bottom" ? bounds.minimum.y : (bounds.minimum.y + bounds.maximum.y) * 0.5F,
                     (bounds.minimum.z + bounds.maximum.z) * 0.5F};
        }
        std::string error;
        auto command = make_move_origin_command(id, point, nullptr, &error);
        if (!command) { lastError = error; continue; }
        compound->add(std::move(command));
        ++moved;
    }
    if (moved == 0) { set_status(lastError.empty() ? "Select a voxel object or mesh first" : lastError, true); return false; }
    const auto selection = capture_selection_snapshot();
    const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(compound));
    if (result.success) workspace_.commands().set_last_selection_transition(selection, selection);
    set_status(result.success ? std::string("Moved the pivot of ") + std::to_string(moved) + " object" + (moved == 1 ? "" : "s") +
                                    (where == "cursor" ? " to the point" : where == "bottom" ? " to the bottom centre" : " to the centre") +
                                    " (geometry stays put)"
                              : result.message, !result.success);
    return result.success;
}

std::optional<Float3> NativeEditorController::origin_marker() const {
    if (!pivotEditing_ || workspace_.selection_count() != 1 || !workspace_.selected_object()) return std::nullopt;
    if (gizmoDragging_ && originDragPoint_) return originDragPoint_;
    if (const EditorObject* object = workspace_.document().find_object(*workspace_.selected_object())) return object->transform.position;
    return std::nullopt;
}

}  // namespace dve::editor
