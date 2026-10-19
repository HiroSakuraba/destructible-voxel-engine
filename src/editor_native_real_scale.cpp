// Real scale (artist worklist ART-027). Scaling with the gizmo (axis handles or the centre square
// for uniform) or the inspector Scale field stretches voxel objects and meshes through their
// per-axis scale, keeping every voxel; it is an ordinary undoable transform. Rebuilding the voxels
// at the new size is the separate, explicit Voxel > Resample Voxels to Scale.
#include "dve/editor_native.hpp"
#include "dve/editor_tools.hpp"

#include <cmath>

#include "dve/editor_number_field.hpp"

namespace dve::editor {

bool NativeEditorController::resample_voxels_to_scale() {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) { set_status("Stop Play before resampling", true); return false; }
    if (longJob_) { set_status(long_job_busy_reason(), true); return false; }
    auto compound = std::make_unique<CompoundCommand>("Resample voxels to scale");
    std::vector<ObjectVoxelSizeChange> metrics;
    std::size_t count = 0;
    for (const EditorObjectId id : workspace_.selected_objects()) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object || !object->voxels || object->polygon || object->text3d || object->gaborVolume ||
            object->voxels->occupied_voxel_count() == 0) continue;
        const Float3 factors = object->voxelScale;
        if (factors.x == 1.0F && factors.y == 1.0F && factors.z == 1.0F) continue;
        if (object->flags.locked) { set_status("Unlock " + object->name + " before resampling it", true); return false; }
        if (object->prefabLink) { set_status(object->name + " is part of a prefab instance; unpack it or edit the prefab", true); return false; }
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

std::string NativeEditorController::transform_numeric_label() const {
    if (!gizmoDragging_ || transformNumeric_.empty()) return {};
    const bool rotate = activeTool_ == EditorToolId::Rotate, scale = activeTool_ == EditorToolId::Scale;
    const NumberFieldUnit unit = rotate ? NumberFieldUnit::Degrees : scale ? NumberFieldUnit::Count : NumberFieldUnit::Meters;
    std::string label = std::string(rotate ? "Rotate " : scale ? "Scale " : "Move ") +
                        (gizmoAxis_ == 1 ? "X" : gizmoAxis_ == 2 ? "Y" : gizmoAxis_ == 3 ? "Z" : "XYZ") + ": " + transformNumeric_;
    const NumberFieldParse parsed = parse_number_field(transformNumeric_, unit, 0.0);
    if (!parsed.ok) return label + "\xE2\x80\xA6";  // still typing
    const std::string shown = format_number_field(parsed.value, unit) + (rotate ? "\xC2\xB0" : scale ? "\xC3\x97" : " m");
    if (shown.rfind(transformNumeric_, 0) != 0) label += " = " + shown;
    return label + "   (X/Y/Z axis, Enter applies, Esc cancels)";
}

}  // namespace dve::editor
