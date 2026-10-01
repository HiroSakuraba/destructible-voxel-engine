#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dve/asset_cooker.hpp"

namespace dve {

struct MeshRemeshOptions {
    // meshoptimizer's experimental voxel remesher accepts resolutions from 4 to 256.
    std::uint32_t resolution{64U};
    bool shell{};
    float creaseAngleRadians{1.04719755F}; // 60 degrees
    float normalSmoothing{}; // meshoptimizer recommends [0, 5]
    std::uint64_t maximumOutputTriangles{4'000'000U};
};

struct RemeshedTriangleMesh {
    std::vector<Float3> positions;
    // Three position indices per triangle.
    std::vector<std::uint32_t> indices;
    // One normal per index/corner, which allows hard edges without duplicating positions.
    std::vector<Float3> cornerNormals;
};

// Rebuilds a triangle mesh at voxel resolution. This operation intentionally returns geometry
// only: source materials, UVs, tangents, and other vertex attributes are not preserved.
[[nodiscard]] bool remesh_triangle_mesh(
    const std::vector<Float3>& positions,
    const std::vector<std::uint32_t>& indices,
    const MeshRemeshOptions& options,
    RemeshedTriangleMesh& output,
    std::string* error = nullptr);

} // namespace dve
