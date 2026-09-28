#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "dve/voxel_object.hpp"

namespace dve {

// Options for watertight print export of a voxel model.
struct PrintExportOptions {
    // World-space size of one voxel edge, in meters.
    double voxelSizeMeters = 0.10;
    // Extra uniform scale applied on top of voxelSizeMeters (e.g. to print a
    // miniature at half scale).
    double scale = 1.0;
};

// The solid/air boundary surface of a voxel model as a plain triangle soup.
// Positions are in meters. Vertices are NOT deduplicated: run the soup
// through a manifold builder (or use export_print_stl) before treating it
// as a solid.
struct PrintMeshSoup {
    std::vector<double> positions;    // flat x, y, z per vertex
    std::vector<std::uint32_t> triangles;  // flat index triples, CCW from outside
};

// Extract one quad (two CCW triangles) per exposed voxel face.
[[nodiscard]] PrintMeshSoup extract_print_surface(
    const VoxelObject& object, const PrintExportOptions& options = {});

struct PrintExportResult {
    bool success = false;
    std::string error;  // human-readable when !success
    std::size_t triangleCount = 0;
    double sizeXMeters = 0.0;
    double sizeYMeters = 0.0;
    double sizeZMeters = 0.0;
    double volumeCubicMeters = 0.0;
};

// Full pipeline: extract the surface, verify it is watertight with
// manifold3d, then write a binary STL. Never writes a broken file: a
// non-manifold result comes back as success=false with an error string.
[[nodiscard]] PrintExportResult export_print_stl(
    const VoxelObject& object,
    const std::filesystem::path& path,
    const PrintExportOptions& options = {});

}  // namespace dve
