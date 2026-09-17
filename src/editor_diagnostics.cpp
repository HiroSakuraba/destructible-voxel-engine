#include "dve/editor_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <set>
#include <vector>

namespace dve::editor {
namespace {

constexpr std::array<Int3, 6> kNeighbors{{
    {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
}};

Int3 plus(Int3 a, Int3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

std::vector<std::pair<Int3, MaterialId>> occupied_voxels(const VoxelObject& object) {
    std::vector<std::pair<Int3, MaterialId>> result;
    result.reserve(static_cast<std::size_t>(object.occupied_voxel_count()));
    for (const auto& [key, brick] : object.bricks()) {
        brick.occupancy().for_each_set([&](std::uint16_t index) {
            const Int3 voxel = global_from_local(key, local_from_index_unchecked(index));
            result.emplace_back(voxel, brick.material(index));
        });
    }
    return result;
}

void extend_bounds(EditorObjectBounds& destination, const EditorObjectBounds& source) noexcept {
    if (!source.valid) return;
    if (!destination.valid) { destination = source; return; }
    destination.minimum = {
        std::min(destination.minimum.x, source.minimum.x),
        std::min(destination.minimum.y, source.minimum.y),
        std::min(destination.minimum.z, source.minimum.z)};
    destination.maximum = {
        std::max(destination.maximum.x, source.maximum.x),
        std::max(destination.maximum.y, source.maximum.y),
        std::max(destination.maximum.z, source.maximum.z)};
}

bool finite3(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

} // namespace

EditorObjectDiagnostics analyze_editor_object(
    const EditorObject& object,
    const EditorMaterialLibrary& materials) {
    EditorObjectDiagnostics result;
    result.objectId = object.id;
    result.worldBounds = object_world_bounds(object);
    result.occupiedVoxels = object.voxels ? object.voxels->occupied_voxel_count() : 0;
    if (!object.voxels || result.occupiedVoxels == 0) {
        result.finite = true;
        result.collisionProxyValid = true;
        return result;
    }

    const auto voxels = occupied_voxels(*object.voxels);
    std::set<Int3> occupied;
    occupied.clear();
    for (const auto& [voxel, material] : voxels) {
        occupied.insert(voxel);
        ++result.materialVoxelCounts[material];
    }
    for (const auto& [key, brick] : object.voxels->bricks()) {
        if (!brick.empty()) ++result.occupiedBricks;
        (void)key;
    }

    for (const auto& [voxel, material] : voxels) {
        (void)material;
        bool surface = false;
        for (const Int3 offset : kNeighbors) {
            if (!occupied.contains(plus(voxel, offset))) { surface = true; break; }
        }
        if (surface) ++result.surfaceVoxels;
    }

    std::set<Int3> unvisited = occupied;
    while (!unvisited.empty()) {
        ++result.connectedComponents;
        bool anchored = false;
        std::deque<Int3> queue;
        queue.push_back(*unvisited.begin());
        unvisited.erase(unvisited.begin());
        while (!queue.empty()) {
            const Int3 voxel = queue.front();
            queue.pop_front();
            anchored = anchored || object.flags.anchored || object.anchors.contains(voxel);
            for (const Int3 offset : kNeighbors) {
                const Int3 neighbor = plus(voxel, offset);
                const auto it = unvisited.find(neighbor);
                if (it != unvisited.end()) {
                    queue.push_back(*it);
                    unvisited.erase(it);
                }
            }
        }
        if (anchored) ++result.anchoredComponents;
    }
    result.detachedComponents = result.connectedComponents - result.anchoredComponents;

    const double voxelSize = static_cast<double>(object.voxelSizeMeters);
    const double voxelVolume = voxelSize * voxelSize * voxelSize;
    double mass = 0.0;
    double weightedX = 0.0, weightedY = 0.0, weightedZ = 0.0;
    struct MassPoint { Float3 point; double mass; };
    std::vector<MassPoint> points;
    points.reserve(voxels.size());
    for (const auto& [voxel, material] : voxels) {
        const EditorMaterialEntry* entry = materials.find(material);
        const double density = entry ? std::max(0.0, static_cast<double>(entry->definition.densityKilogramsPerCubicMeter)) : 1000.0;
        const double voxelMass = density * voxelVolume;
        const Float3 point{
            static_cast<float>((static_cast<double>(voxel.x) + 0.5) * voxelSize),
            static_cast<float>((static_cast<double>(voxel.y) + 0.5) * voxelSize),
            static_cast<float>((static_cast<double>(voxel.z) + 0.5) * voxelSize)};
        points.push_back({point, voxelMass});
        mass += voxelMass;
        weightedX += voxelMass * point.x;
        weightedY += voxelMass * point.y;
        weightedZ += voxelMass * point.z;
    }
    result.massKilograms = mass;
    if (mass > 0.0) {
        result.centerOfMassLocalMeters = {
            static_cast<float>(weightedX / mass),
            static_cast<float>(weightedY / mass),
            static_cast<float>(weightedZ / mass)};
        result.centerOfMassWorldMeters = transform_point(object.transform, result.centerOfMassLocalMeters);
        double ixx = 0.0, iyy = 0.0, izz = 0.0;
        const double cubeOwn = voxelSize * voxelSize / 6.0;
        for (const MassPoint& point : points) {
            const double dx = static_cast<double>(point.point.x - result.centerOfMassLocalMeters.x);
            const double dy = static_cast<double>(point.point.y - result.centerOfMassLocalMeters.y);
            const double dz = static_cast<double>(point.point.z - result.centerOfMassLocalMeters.z);
            ixx += point.mass * (dy*dy + dz*dz + cubeOwn);
            iyy += point.mass * (dx*dx + dz*dz + cubeOwn);
            izz += point.mass * (dx*dx + dy*dy + cubeOwn);
        }
        result.inertiaDiagonalKilogramMetersSquared = {
            static_cast<float>(ixx), static_cast<float>(iyy), static_cast<float>(izz)};
    }

    if (object.flags.collisionEnabled) {
        const std::vector<VoxelBox> boxes = build_object_box_proxy(*object.voxels);
        result.collisionBoxes = boxes.size();
        result.collisionProxyValid = validate_box_proxy(*object.voxels, boxes);
    } else {
        result.collisionProxyValid = true;
    }
    result.finite = std::isfinite(result.massKilograms) && finite3(result.centerOfMassLocalMeters) &&
                    finite3(result.centerOfMassWorldMeters) && finite3(result.inertiaDiagonalKilogramMetersSquared);
    if (!result.collisionProxyValid) result.warning = "collision proxy does not exactly cover occupied voxels";
    else if (result.detachedComponents > 0 && object.flags.structural)
        result.warning = "structural object contains detached components";
    return result;
}

EditorSelectionDiagnostics analyze_editor_selection(
    const EditorDocument& document,
    const EditorMaterialLibrary& materials,
    const std::set<EditorObjectId>& selection) {
    EditorSelectionDiagnostics result;
    double weightedX = 0.0, weightedY = 0.0, weightedZ = 0.0;
    result.finite = true;
    for (EditorObjectId id : selection) {
        const EditorObject* object = document.find_object(id);
        if (!object) continue;
        const EditorObjectDiagnostics diagnostics = analyze_editor_object(*object, materials);
        ++result.objectCount;
        result.occupiedVoxels += diagnostics.occupiedVoxels;
        result.connectedComponents += diagnostics.connectedComponents;
        result.detachedComponents += diagnostics.detachedComponents;
        result.collisionBoxes += diagnostics.collisionBoxes;
        result.massKilograms += diagnostics.massKilograms;
        weightedX += diagnostics.massKilograms * diagnostics.centerOfMassWorldMeters.x;
        weightedY += diagnostics.massKilograms * diagnostics.centerOfMassWorldMeters.y;
        weightedZ += diagnostics.massKilograms * diagnostics.centerOfMassWorldMeters.z;
        extend_bounds(result.worldBounds, diagnostics.worldBounds);
        result.finite = result.finite && diagnostics.finite;
    }
    if (result.massKilograms > 0.0) {
        result.centerOfMassWorldMeters = {
            static_cast<float>(weightedX / result.massKilograms),
            static_cast<float>(weightedY / result.massKilograms),
            static_cast<float>(weightedZ / result.massKilograms)};
    }
    result.finite = result.finite && std::isfinite(result.massKilograms) && finite3(result.centerOfMassWorldMeters);
    return result;
}

} // namespace dve::editor
