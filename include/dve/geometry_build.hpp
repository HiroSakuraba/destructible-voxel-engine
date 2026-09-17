#pragma once

#include <cstdint>
#include <string_view>

namespace dve {

enum class GeometryBuildMode : std::uint8_t {
    VoxelOnly,
    PolygonOnly,
    Hybrid,
};

enum class GeometryKind : std::uint8_t {
    Voxel,
    Polygon,
};

[[nodiscard]] constexpr GeometryBuildMode compiled_geometry_build_mode() noexcept {
#if defined(DVE_GEOMETRY_MODE_VOXEL)
    return GeometryBuildMode::VoxelOnly;
#elif defined(DVE_GEOMETRY_MODE_POLYGON)
    return GeometryBuildMode::PolygonOnly;
#else
    return GeometryBuildMode::Hybrid;
#endif
}

[[nodiscard]] constexpr bool geometry_kind_supported(GeometryKind kind) noexcept {
    const GeometryBuildMode mode = compiled_geometry_build_mode();
    return mode == GeometryBuildMode::Hybrid ||
           (mode == GeometryBuildMode::VoxelOnly && kind == GeometryKind::Voxel) ||
           (mode == GeometryBuildMode::PolygonOnly && kind == GeometryKind::Polygon);
}

[[nodiscard]] constexpr std::string_view to_string(GeometryBuildMode mode) noexcept {
    switch (mode) {
    case GeometryBuildMode::VoxelOnly: return "voxel";
    case GeometryBuildMode::PolygonOnly: return "polygon";
    case GeometryBuildMode::Hybrid: return "hybrid";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(GeometryKind kind) noexcept {
    return kind == GeometryKind::Voxel ? "voxel" : "polygon";
}

} // namespace dve
