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

// Density Play uses for a polygon whose materials set none (kg/m^3).
inline constexpr double kDefaultPolygonDensity = 1000.0;

struct EditorObjectDiagnostics {
    EditorObjectId objectId{};
    std::uint64_t occupiedVoxels{};
    std::uint64_t surfaceVoxels{};
    std::size_t occupiedBricks{};
    std::size_t connectedComponents{};
    std::size_t anchoredComponents{};
    std::size_t detachedComponents{};
    std::size_t collisionBoxes{};
    // Polygon objects with certified exact collision (dve.collision_convex_set, e.g. exact CAD
    // tubes): the number of convex pieces, and a mass from their exact volume at the polygon's material density (1000 kg/m^3 by default).
    std::size_t collisionConvexPieces{};
    // True when mass, centre of mass and inertia describe the object: it has voxels or exact
    // collision pieces. A polygon without them has no volume model, so its mass is unknown, not zero.
    bool massKnown{};
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
    std::size_t collisionConvexPieces{};
    std::size_t objectsWithoutMass{};   // selected objects that have no volume model (mass unknown)
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
