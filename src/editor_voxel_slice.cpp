#include "dve/editor_voxel_slice.hpp"
#include "dve/editor_workspace.hpp"
#include <algorithm>
#include <cmath>

namespace dve::editor {
namespace {
bool finite(Float3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
bool same(Float3 a, Float3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool same(Quaternion a, Quaternion b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
std::string selection_reason(const EditorObject* o) {
    if (!o || !o->voxels || o->text3d || o->gaborVolume || !o->voxels->occupied_voxel_count())
        return "Select one occupied voxel object to slice.";
    if (o->flags.locked)
        return "Unlock the object before slicing.";
    return {};
}
} // namespace
bool EditorVoxelSliceSession::begin(const EditorDocument& doc, EditorObjectId id, Float3 point, Float3 normal,
                                    std::string* error) {
    cancel();
    const auto* o = doc.find_object(id);
    const std::string reason = selection_reason(o);
    if (!reason.empty()) {
        if (error)
            *error = reason;
        return false;
    }
    // Keep synchronous previews bounded. Larger operations require a cancellable
    // worker.
    if (o->voxels->occupied_voxel_count() > 250000) {
        if (error)
            *error = "Slice preview supports up to 250,000 occupied cells; split "
                     "this object first.";
        return false;
    }
    target_ = id;
    output_ = VoxelSliceOutput::Front;
    return configure(doc, point, normal, output_, error);
}
bool EditorVoxelSliceSession::configure(const EditorDocument& doc, Float3 point, Float3 normal,
                                        VoxelSliceOutput output, std::string* error) {
    if (!finite(point) || !finite(normal) || length(normal) < 1e-6F || !std::isfinite(length(normal))) {
        if (error)
            *error = "Slice plane needs finite coordinates and a nonzero normal.";
        return false;
    }
    const auto* o = doc.find_object(target_);
    const std::string reason = selection_reason(o);
    if (!reason.empty()) {
        if (error)
            *error = reason;
        return false;
    }
    if (active_ && !target_unchanged(doc)) {
        if (error)
            *error = "Slice target changed; cancel and reopen the preview.";
        return false;
    }
    if (o->voxels->occupied_voxel_count() > 250000) {
        if (error)
            *error = "Slice preview cell limit exceeded.";
        return false;
    }
    point_ = point;
    normal_ = normalize(normal);
    output_ = output;
    voxelRevision_ = o->voxels->revision();
    transform_ = o->transform;
    voxelSize_ = o->voxelSizeMeters;
    const auto state = capture_voxel_object_state(*o);
    anchors_ = state.anchors;
    cells_.clear();
    cells_.reserve(state.voxels.size());
    for (const auto& cell : state.voxels) {
        const Float3 world =
            transform_point(o->transform, {(static_cast<float>(cell.voxel.x) + 0.5F) * voxelSize_,
                                           (static_cast<float>(cell.voxel.y) + 0.5F) * voxelSize_,
                                           (static_cast<float>(cell.voxel.z) + 0.5F) * voxelSize_});
        cells_.push_back({cell.voxel, cell.material, world, dot(subtract(world, point_), normal_) >= 0});
    }
    active_ = true;
    return true;
}
bool EditorVoxelSliceSession::target_unchanged(const EditorDocument& doc) const {
    const auto* o = doc.find_object(target_);
    return active_ && o && o->voxels && !o->text3d && !o->gaborVolume && !o->flags.locked &&
           o->voxels->revision() == voxelRevision_ && o->voxelSizeMeters == voxelSize_ &&
           same(o->transform.position, transform_.position) &&
           same(o->transform.rotation, transform_.rotation) &&
           std::equal(o->anchors.begin(), o->anchors.end(), anchors_.begin(), anchors_.end());
}
std::string EditorVoxelSliceSession::blocked_reason(const EditorDocument& doc) const {
    if (!active_)
        return "No Slice preview is open.";
    const auto* o = doc.find_object(target_);
    const auto reason = selection_reason(o);
    if (!reason.empty())
        return reason;
    if (!target_unchanged(doc))
        return "Slice target changed; cancel and reopen the preview.";
    if (output_ == VoxelSliceOutput::Separate &&
        (!o->components.empty() || o->prefabLink || o->attachment || !doc.children_of(target_).empty()))
        return "Separate requires an object without components, prefab links, "
               "attachments, or children; resolve those first.";
    const auto front = std::count_if(cells_.begin(), cells_.end(), [](const auto& c) { return c.front; });
    if (front == 0 || front == static_cast<std::ptrdiff_t>(cells_.size()))
        return "Move the plane through the object so both sides contain cells.";
    return {};
}
std::vector<std::string> EditorVoxelSliceSession::describe(const EditorDocument& doc) const {
    if (!active_)
        return {};
    const auto front = std::count_if(cells_.begin(), cells_.end(), [](const auto& c) { return c.front; });
    std::vector<std::string> lines = {
        "Voxel Slice (cell centres)",
        "Front: " + std::to_string(front) +
            "  Back: " + std::to_string(cells_.size() - static_cast<std::size_t>(front)),
        std::string("Output: ") + (output_ == VoxelSliceOutput::Front  ? "Keep front"
                                   : output_ == VoxelSliceOutput::Back ? "Keep back"
                                                                       : "Keep both as separate objects"),
        "X/Y/Z: plane axis  Arrows: move  Shift: fine",
        "1: front  2: back  3: separate  Enter: apply  Esc: cancel"};
    if (const auto reason = blocked_reason(doc); !reason.empty())
        lines.push_back("Cannot commit: " + reason);
    return lines;
}
CommandResult EditorVoxelSliceSession::commit(EditorWorkspace& workspace) {
    auto& doc = workspace.document();
    if (const auto reason = blocked_reason(doc); !reason.empty())
        return CommandResult::fail(reason);
    auto* o = doc.find_object(target_);
    std::vector<VoxelBooleanChange> changes;
    std::vector<Int3> keptAnchors;
    EditorObject separated;
    if (output_ == VoxelSliceOutput::Separate) {
        separated = clone_editor_object(*o);
        separated.id = doc.allocate_object_id();
        separated.name = o->name + " (back)";
        separated.voxels = std::make_unique<VoxelObject>(separated.id);
        separated.anchors.clear();
        // Split geometry no longer corresponds to the original import source.
        separated.sourceAsset.clear();
        separated.importRecipe.clear();
    }
    for (const auto& cell : cells_) {
        const bool keep = output_ == VoxelSliceOutput::Back ? !cell.front : cell.front;
        if (!keep)
            changes.push_back({cell.voxel, cell.material, kAirMaterial});
        else if (o->anchors.contains(cell.voxel))
            keptAnchors.push_back(cell.voxel);
        if (output_ == VoxelSliceOutput::Separate && !cell.front) {
            (void)separated.voxels->set_voxel(cell.voxel, cell.material);
            if (o->anchors.contains(cell.voxel))
                separated.anchors.insert(cell.voxel);
        }
    }
    auto compound = std::make_unique<CompoundCommand>("Slice voxel object");
    compound->add(std::make_unique<ApplyVoxelBooleanCommand>(target_, std::move(changes), anchors_,
                                                             std::move(keptAnchors), "Slice cells"));
    if (output_ == VoxelSliceOutput::Separate)
        compound->add(std::make_unique<AddObjectCommand>(std::move(separated), "Add back half"));
    const auto result = workspace.commands().execute(doc, std::move(compound));
    if (result.success)
        cancel();
    return result;
}
} // namespace dve::editor
