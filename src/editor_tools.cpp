#include "dve/editor_tools.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <limits>
#include <set>

namespace dve::editor {
namespace {

std::vector<VoxelChange> changes_for(
    const EditorDocument& document, EditorObjectId id, const std::vector<Int3>& voxels,
    VoxelToolOperation operation, MaterialId material) {
    const EditorObject* object = document.find_object(id);
    if (!object || object->flags.locked) return {};
    std::set<Int3> unique(voxels.begin(), voxels.end());
    std::vector<VoxelChange> result;
    result.reserve(unique.size());
    for (const Int3 voxel : unique) {
        const MaterialId before = object->voxels->material_at(voxel);
        MaterialId after = before;
        switch (operation) {
        case VoxelToolOperation::Add: after = material == kAirMaterial ? 1 : material; break;
        case VoxelToolOperation::Remove: after = kAirMaterial; break;
        case VoxelToolOperation::Paint: if (before != kAirMaterial) after = material == kAirMaterial ? before : material; break;
        }
        if (before != after) result.push_back({voxel, before, after});
    }
    return result;
}

} // namespace

std::vector<Int3> cube_brush_voxels(Int3 center, std::int32_t radius) {
    radius = std::max(0, radius);
    std::vector<Int3> result;
    const std::int64_t side = 2LL * radius + 1LL;
    result.reserve(static_cast<std::size_t>(side * side * side));
    for (std::int32_t z = -radius; z <= radius; ++z)
        for (std::int32_t y = -radius; y <= radius; ++y)
            for (std::int32_t x = -radius; x <= radius; ++x)
                result.push_back({center.x + x, center.y + y, center.z + z});
    return result;
}

std::vector<Int3> sphere_brush_voxels(Int3 center, std::int32_t radius) {
    radius = std::max(0, radius);
    std::vector<Int3> result;
    const std::int64_t radiusSquared = static_cast<std::int64_t>(radius) * radius;
    for (std::int32_t z = -radius; z <= radius; ++z)
        for (std::int32_t y = -radius; y <= radius; ++y)
            for (std::int32_t x = -radius; x <= radius; ++x)
                if (static_cast<std::int64_t>(x) * x + static_cast<std::int64_t>(y) * y + static_cast<std::int64_t>(z) * z <= radiusSquared)
                    result.push_back({center.x + x, center.y + y, center.z + z});
    return result;
}

std::vector<Int3> box_voxels(Int3 minimum, Int3 maximum, bool hollow) {
    const Int3 originalMinimum = minimum;
    const Int3 originalMaximum = maximum;
    minimum = min_components(originalMinimum, originalMaximum);
    maximum = max_components(originalMinimum, originalMaximum);
    std::vector<Int3> result;
    for (std::int32_t z = minimum.z; z <= maximum.z; ++z)
        for (std::int32_t y = minimum.y; y <= maximum.y; ++y)
            for (std::int32_t x = minimum.x; x <= maximum.x; ++x) {
                const bool boundary = x == minimum.x || x == maximum.x || y == minimum.y || y == maximum.y || z == minimum.z || z == maximum.z;
                if (!hollow || boundary) result.push_back({x, y, z});
            }
    return result;
}

std::vector<Int3> line_voxels(Int3 start, Int3 end, std::int32_t thickness) {
    const std::int32_t dx = end.x - start.x, dy = end.y - start.y, dz = end.z - start.z;
    const std::int32_t steps = std::max({std::abs(dx), std::abs(dy), std::abs(dz), 1});
    std::set<Int3> unique;
    const std::int32_t radius = std::max(0, thickness - 1);
    for (std::int32_t i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const Int3 point{
            static_cast<std::int32_t>(std::lround(static_cast<float>(start.x) + static_cast<float>(dx) * t)),
            static_cast<std::int32_t>(std::lround(static_cast<float>(start.y) + static_cast<float>(dy) * t)),
            static_cast<std::int32_t>(std::lround(static_cast<float>(start.z) + static_cast<float>(dz) * t))};
        for (const Int3 voxel : cube_brush_voxels(point, radius)) unique.insert(voxel);
    }
    return {unique.begin(), unique.end()};
}

std::unique_ptr<IEditorCommand> make_brush_command(
    const EditorDocument& document, EditorObjectId objectId, Int3 center, const BrushSettings& settings) {
    const auto voxels = settings.shape == BrushShape::Sphere ? sphere_brush_voxels(center, settings.radius)
                                                             : cube_brush_voxels(center, settings.radius);
    return std::make_unique<VoxelEditCommand>(objectId,
        changes_for(document, objectId, voxels, settings.operation, settings.material),
        settings.operation == VoxelToolOperation::Remove ? "Remove voxels" :
        settings.operation == VoxelToolOperation::Paint ? "Paint material" : "Add voxels",
        settings.strokeId);
}

std::unique_ptr<IEditorCommand> make_box_command(
    const EditorDocument& document, EditorObjectId objectId, Int3 minimum, Int3 maximum,
    VoxelToolOperation operation, MaterialId material, bool hollow) {
    return std::make_unique<VoxelEditCommand>(objectId,
        changes_for(document, objectId, box_voxels(minimum, maximum, hollow), operation, material),
        hollow ? "Create hollow box" : "Edit box");
}

std::unique_ptr<IEditorCommand> make_line_command(
    const EditorDocument& document, EditorObjectId objectId, Int3 start, Int3 end,
    VoxelToolOperation operation, MaterialId material, std::int32_t thickness) {
    return std::make_unique<VoxelEditCommand>(objectId,
        changes_for(document, objectId, line_voxels(start, end, thickness), operation, material),
        "Edit beam");
}

std::unique_ptr<IEditorCommand> make_anchor_brush_command(
    const EditorDocument& document, EditorObjectId objectId, Int3 center,
    std::int32_t radius, bool add) {
    const EditorObject* object = document.find_object(objectId);
    std::vector<Int3> additions, removals;
    if (object && !object->flags.locked) {
        for (const Int3 voxel : sphere_brush_voxels(center, radius)) {
            const bool anchored = object->anchors.contains(voxel);
            if (add && !anchored && object->voxels->occupied_at(voxel)) additions.push_back(voxel);
            if (!add && anchored) removals.push_back(voxel);
        }
    }
    return std::make_unique<SetAnchorsCommand>(objectId, std::move(additions), std::move(removals));
}


std::unique_ptr<IEditorCommand> make_rescale_voxel_object_command(
    const EditorDocument& document, EditorObjectId objectId, Float3 scale) {
    const EditorObject* object = document.find_object(objectId);
    if (!object || object->flags.locked || !object->voxels) return {};
    const auto finite_positive = [](float value) {
        return std::isfinite(value) && value > 0.0F && value <= 64.0F;
    };
    if (!finite_positive(scale.x) || !finite_positive(scale.y) || !finite_positive(scale.z)) return {};

    const VoxelObjectState before = capture_voxel_object_state(*object);
    if (before.voxels.empty()) {
        return std::make_unique<ReplaceVoxelObjectStateCommand>(
            objectId, before, before, "Rescale voxel object");
    }

    Int3 minimum = before.voxels.front().voxel;
    Int3 maximum = minimum;
    for (const SparseVoxelStateEntry& entry : before.voxels) {
        minimum = min_components(minimum, entry.voxel);
        maximum = max_components(maximum, entry.voxel);
    }
    // Scale the integer lattice as a partition of the source bounding box rather
    // than scaling each closed voxel cube independently.  Independent continuous
    // cubes overlap on their shared faces and made a 1-voxel object scaled by 2x
    // cover 3 cells per axis.  The partition mapping below gives every source cell
    // a deterministic half-open interval inside an exact target bounding box.
    const std::array<std::int64_t, 3> sourceDimensions{
        static_cast<std::int64_t>(maximum.x) - minimum.x + 1,
        static_cast<std::int64_t>(maximum.y) - minimum.y + 1,
        static_cast<std::int64_t>(maximum.z) - minimum.z + 1};
    const std::array<double, 3> scales{scale.x, scale.y, scale.z};
    std::array<std::int64_t, 3> targetDimensions{};
    std::array<std::int64_t, 3> targetMinimum{};
    const std::array<std::int64_t, 3> sourceMinimum{minimum.x, minimum.y, minimum.z};
    const std::array<std::int64_t, 3> sourceMaximum{maximum.x, maximum.y, maximum.z};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double scaledDimension =
            static_cast<double>(sourceDimensions[axis]) * scales[axis];
        if (!std::isfinite(scaledDimension) ||
            scaledDimension > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
            return {};
        targetDimensions[axis] = std::max<std::int64_t>(
            1, static_cast<std::int64_t>(std::llround(scaledDimension)));
        const double pivot = 0.5 * static_cast<double>(
            sourceMinimum[axis] + sourceMaximum[axis] + 1);
        const double minimumCoordinate =
            pivot - 0.5 * static_cast<double>(targetDimensions[axis]);
        const auto roundedMinimum = static_cast<std::int64_t>(std::floor(minimumCoordinate));
        const std::int64_t maximumExclusive = roundedMinimum + targetDimensions[axis];
        if (roundedMinimum < std::numeric_limits<std::int32_t>::min() ||
            maximumExclusive > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1)
            return {};
        targetMinimum[axis] = roundedMinimum;
    }

    const auto floor_partition = [](std::int64_t index, std::int64_t target,
                                    std::int64_t source) noexcept {
        return (index * target) / source;
    };
    const auto ceil_partition = [](std::int64_t index, std::int64_t target,
                                   std::int64_t source) noexcept {
        return (index * target + source - 1) / source;
    };

    struct TargetValue { MaterialId material{}; bool anchored{}; };
    std::map<Int3, TargetValue> targets;
    const std::set<Int3> sourceAnchors(before.anchors.begin(), before.anchors.end());
    constexpr std::size_t maximumEditorVoxels = 128U * 1024U * 1024U;
    for (const SparseVoxelStateEntry& source : before.voxels) {
        const std::array<std::int64_t, 3> sourceCoordinate{
            source.voxel.x, source.voxel.y, source.voxel.z};
        std::array<std::int64_t, 3> first{};
        std::array<std::int64_t, 3> last{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const std::int64_t relative = sourceCoordinate[axis] - sourceMinimum[axis];
            first[axis] = targetMinimum[axis] + floor_partition(
                relative, targetDimensions[axis], sourceDimensions[axis]);
            last[axis] = targetMinimum[axis] + ceil_partition(
                relative + 1, targetDimensions[axis], sourceDimensions[axis]);
            // Downscaling can collapse a source interval. Preserve occupied thin
            // features by assigning at least one target cell, clamped to bounds.
            if (last[axis] <= first[axis])
                last[axis] = std::min(targetMinimum[axis] + targetDimensions[axis],
                                      first[axis] + 1);
        }

        const bool sourceAnchored = sourceAnchors.contains(source.voxel);
        for (std::int64_t z = first[2]; z < last[2]; ++z)
            for (std::int64_t y = first[1]; y < last[1]; ++y)
                for (std::int64_t x = first[0]; x < last[0]; ++x) {
                    const Int3 target{static_cast<std::int32_t>(x),
                                      static_cast<std::int32_t>(y),
                                      static_cast<std::int32_t>(z)};
                    auto [it, inserted] = targets.try_emplace(
                        target, TargetValue{source.material, sourceAnchored});
                    if (!inserted) {
                        it->second.anchored = it->second.anchored || sourceAnchored;
                        if (source.material < it->second.material)
                            it->second.material = source.material;
                    }
                    if (targets.size() > maximumEditorVoxels) return {};
                }
    }

    VoxelObjectState after;
    after.voxels.reserve(targets.size());
    for (const auto& [voxel, value] : targets) {
        after.voxels.push_back({voxel, value.material});
        if (value.anchored) after.anchors.push_back(voxel);
    }
    return std::make_unique<ReplaceVoxelObjectStateCommand>(
        objectId, before, std::move(after), "Rescale voxel object");
}

} // namespace dve::editor
