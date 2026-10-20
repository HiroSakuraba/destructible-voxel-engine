// Real scale (artist worklist ART-027). Scaling with the gizmo (axis handles or the centre square
// for uniform) or the inspector Scale field stretches voxel objects and meshes through their
// per-axis scale, keeping every voxel; it is an ordinary undoable transform. Rebuilding the voxels
// at the new size is the separate, explicit Voxel > Resample Voxels to Scale.
#include "dve/editor_native.hpp"
#include "dve/editor_tools.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace dve::editor {

namespace {
bool resample_candidate(const EditorObject& object) {
    if (!object.voxels || object.polygon || object.text3d || object.gaborVolume || object.voxels->occupied_voxel_count() == 0) return false;
    return !(object.voxelScale.x == 1.0F && object.voxelScale.y == 1.0F && object.voxelScale.z == 1.0F);
}
}  // namespace

// Why Voxel > Resample Voxels to Scale cannot run now (empty when it can). The menu item is
// disabled with this reason, and the action refuses with it, so the menu never lies.
std::string NativeEditorController::resample_to_scale_disabled_reason() const {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) return "Stop Play or Simulate before resampling.";
    if (longJob_) return long_job_busy_reason();
    if (gizmoDragging_ || polygonPreviewBefore_ || duplicateSpecial_.active) return "Apply or cancel the active preview first.";
    std::size_t count = 0;
    for (const EditorObjectId id : workspace_.selected_objects()) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object || !resample_candidate(*object)) continue;
        if (object->flags.locked) return "Unlock " + object->name + " before resampling it.";
        if (!object->flags.visible || is_isolated_out(id)) return object->name + " is hidden or isolated out of view.";
        if (object->prefabLink) return object->name + " is part of a prefab instance; unpack it or edit the prefab.";
        ++count;
    }
    if (count == 0) return "Select voxel objects whose Scale is not 1.";
    return {};
}

bool NativeEditorController::resample_voxels_to_scale() {
    if (const std::string reason = resample_to_scale_disabled_reason(); !reason.empty()) { set_status(reason, true); return false; }
    auto compound = std::make_unique<CompoundCommand>("Resample voxels to scale");
    std::vector<ObjectVoxelSizeChange> metrics;
    std::size_t count = 0;
    for (const EditorObjectId id : workspace_.selected_objects()) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object || !resample_candidate(*object)) continue;
        const Float3 factors = object->voxelScale;
        auto command = make_rescale_voxel_object_command(workspace_.document(), id, factors);
        if (!command) { set_status(object->name + ": the resampled object would be too large", true); return false; }
        // Keep the object where it is: the resample maps the occupied box about its centre.
        const auto occupied = object->voxels->occupied_bounds();
        const float size = object->voxelSizeMeters;
        const auto centre_after = [&](int minimum, int maximum, float factor) {
            const double sourceCentre = 0.5 * (static_cast<double>(minimum) + maximum + 1.0);
            const double dimension = static_cast<double>(maximum) - minimum + 1.0;
            const double target = std::max(1.0, static_cast<double>(std::llround(dimension * factor)));
            return static_cast<float>(std::floor(sourceCentre - target * 0.5) + target * 0.5) * size;
        };
        const Float3 before{(static_cast<float>(occupied.minimum.x) + static_cast<float>(occupied.maximum.x) + 1.0F) * size * 0.5F * factors.x,
                            (static_cast<float>(occupied.minimum.y) + static_cast<float>(occupied.maximum.y) + 1.0F) * size * 0.5F * factors.y,
                            (static_cast<float>(occupied.minimum.z) + static_cast<float>(occupied.maximum.z) + 1.0F) * size * 0.5F * factors.z};
        const Float3 after{centre_after(occupied.minimum.x, occupied.maximum.x, factors.x),
                           centre_after(occupied.minimum.y, occupied.maximum.y, factors.y),
                           centre_after(occupied.minimum.z, occupied.maximum.z, factors.z)};
        RigidTransform moved = object->transform;
        moved.position = add(moved.position, rotate(moved.rotation, subtract(before, after)));
        compound->add(std::move(command));
        metrics.push_back({id, object->transform, moved, size, size, factors, Float3{1.0F, 1.0F, 1.0F}});
        ++count;
    }
    if (count == 0) {
        set_status("Nothing to resample: select voxel objects whose Scale is not 1", true);
        return false;
    }
    compound->add(std::make_unique<ScaleVoxelSizeCommand>(std::move(metrics), "Reset scale after resample"));
    const auto selection = capture_selection_snapshot();
    const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(compound));
    if (result.success) workspace_.commands().set_last_selection_transition(selection, selection);
    set_status(result.success ? "Resampled " + std::to_string(count) + " object" + (count == 1 ? "" : "s") +
                                    " to their scale (voxels rebuilt, Scale back to 1; Undo restores)"
                              : result.message, !result.success);
    return result.success;
}

}  // namespace dve::editor
