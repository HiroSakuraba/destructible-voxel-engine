#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "dve/geometry_build.hpp"
#include "dve/polygon_asset.hpp"

namespace dve {

enum class MaterialDisplacementPolicy : std::uint8_t {
    Disabled,
    VisualOnly,
    CollisionAffecting,
};

struct MaterialVertexDisplacementSettings {
    MaterialDisplacementPolicy policy{MaterialDisplacementPolicy::Disabled};
    float scaleMeters{0.0F};
    float referencePlane{0.5F};
    float fadeStartMeters{20.0F};
    float fadeEndMeters{60.0F};
    std::uint32_t offlineSubdivisionLevels{};
    bool affectDepth{true};
    bool affectShadows{true};
};

struct PolygonDisplacementStats {
    std::uint64_t sourceVertices{};
    std::uint64_t outputVertices{};
    std::uint64_t sourceTriangles{};
    std::uint64_t outputTriangles{};
    std::uint64_t displacedVisualVertices{};
    std::uint64_t displacedCollisionVertices{};
    std::uint64_t sharedMaterialVertices{};
    std::uint64_t missingHeightBindings{};
    float maximumAbsoluteDisplacement{};
};

struct PolygonDisplacementResult {
    CookedPolygonAsset visualAsset;
    std::optional<CookedPolygonAsset> collisionAsset;
    PolygonDisplacementStats stats{};
};

[[nodiscard]] bool validate_material_vertex_displacement_settings(
    const MaterialVertexDisplacementSettings& settings,
    GeometryKind geometry,
    std::string* error = nullptr) noexcept;

[[nodiscard]] float material_displacement_lod_fade(
    float cameraDistanceMeters,
    const MaterialVertexDisplacementSettings& settings) noexcept;

// Deterministic offline subdivision and CPU vertex displacement. The visual asset is always
// independent of the source. collisionAsset is produced only when at least one material requests
// CollisionAffecting. Voxel-derived surfaces are restricted to VisualOnly by validation.
[[nodiscard]] std::optional<PolygonDisplacementResult> build_displaced_polygon_asset(
    const CookedPolygonAsset& source,
    std::span<const MaterialVertexDisplacementSettings> materialSettings,
    float cameraDistanceMeters,
    std::uint64_t maximumVertices = 16ULL * 1024ULL * 1024ULL,
    std::string* error = nullptr);

} // namespace dve
