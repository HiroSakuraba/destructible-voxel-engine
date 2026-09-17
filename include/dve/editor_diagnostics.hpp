#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "dve/collision_proxy.hpp"
#include "dve/editor_document.hpp"
#include "dve/editor_materials.hpp"
#include "dve/editor_viewport.hpp"

namespace dve::editor {

struct EditorObjectDiagnostics {
    EditorObjectId objectId{};
    std::uint64_t occupiedVoxels{};
    std::uint64_t surfaceVoxels{};
    std::size_t occupiedBricks{};
    std::size_t connectedComponents{};
    std::size_t anchoredComponents{};
    std::size_t detachedComponents{};
    std::size_t collisionBoxes{};
    double massKilograms{};
    Float3 centerOfMassLocalMeters{};
    Float3 centerOfMassWorldMeters{};
    Float3 inertiaDiagonalKilogramMetersSquared{};
    EditorObjectBounds worldBounds{};
    std::map<MaterialId, std::uint64_t> materialVoxelCounts;
    bool collisionProxyValid{};
    bool finite{};
    std::string warning;
};

[[nodiscard]] EditorObjectDiagnostics analyze_editor_object(
    const EditorObject& object,
    const EditorMaterialLibrary& materials);

struct EditorSelectionDiagnostics {
    std::size_t objectCount{};
    std::uint64_t occupiedVoxels{};
    std::size_t connectedComponents{};
    std::size_t detachedComponents{};
    std::size_t collisionBoxes{};
    double massKilograms{};
    Float3 centerOfMassWorldMeters{};
    EditorObjectBounds worldBounds{};
    bool finite{};
};

[[nodiscard]] EditorSelectionDiagnostics analyze_editor_selection(
    const EditorDocument& document,
    const EditorMaterialLibrary& materials,
    const std::set<EditorObjectId>& selection);

} // namespace dve::editor
