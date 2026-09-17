#pragma once

#include <optional>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/navigation_mesh.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/voxel_object.hpp"

namespace dve {

struct NavigationGeometrySettings {
    Matrix4 worldTransform{Matrix4::identity()};
    std::uint16_t area{1U};
    std::uint16_t flags{0xFFFFU};
    std::uint64_t sourceIdBase{};
};

[[nodiscard]] std::vector<NavigationTriangle> navigation_triangles_from_polygon_asset(
    const CookedPolygonAsset& asset,
    const NavigationGeometrySettings& settings = {});

[[nodiscard]] std::vector<NavigationTriangle> navigation_triangles_from_voxel_object(
    const VoxelObject& object,
    float voxelSizeMeters,
    const NavigationGeometrySettings& settings = {},
    std::optional<NavigationBounds> worldRegion = std::nullopt);

} // namespace dve
