#include "dve/mesh_remesh.hpp"

#include "meshoptimizer.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace dve {
namespace {

bool fail(std::string* error, const char* message) {
    if (error != nullptr) *error = message;
    return false;
}

} // namespace

bool remesh_triangle_mesh(
    const std::vector<Float3>& positions,
    const std::vector<std::uint32_t>& indices,
    const MeshRemeshOptions& options,
    RemeshedTriangleMesh& output,
    std::string* error) {
    output = {};
    if (positions.empty() || indices.empty() || indices.size() % 3U != 0U)
        return fail(error, "remeshing requires vertices and a non-empty triangle index buffer");
    if (options.resolution < 4U || options.resolution > 256U)
        return fail(error, "remesh resolution must be in [4, 256]");
    if (!std::isfinite(options.creaseAngleRadians) || options.creaseAngleRadians < 0.0F ||
        options.creaseAngleRadians > 3.1415927F)
        return fail(error, "normal crease angle must be finite and in [0, pi]");
    if (!std::isfinite(options.normalSmoothing) || options.normalSmoothing < 0.0F ||
        options.normalSmoothing > 5.0F)
        return fail(error, "normal smoothing must be finite and in [0, 5]");
    if (options.maximumOutputTriangles == 0U)
        return fail(error, "maximumOutputTriangles must be positive");
    for (const Float3 position : positions)
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
            return fail(error, "remesh input contains a non-finite vertex");
    for (const std::uint32_t index : indices)
        if (index >= positions.size()) return fail(error, "remesh input index is outside the vertex buffer");

    try {
        std::vector<float> sourcePositions;
        sourcePositions.reserve(positions.size() * 3U);
        for (const Float3 position : positions) {
            sourcePositions.push_back(position.x);
            sourcePositions.push_back(position.y);
            sourcePositions.push_back(position.z);
        }

        const unsigned int remeshOptions = static_cast<unsigned int>(meshopt_RemeshSolve) |
            (options.shell ? static_cast<unsigned int>(meshopt_RemeshShell) : 0U);
        const std::size_t triangleCapacity = meshopt_remesh(
            nullptr, 0U, indices.data(), indices.size(), sourcePositions.data(), positions.size(),
            sizeof(float) * 3U, static_cast<int>(options.resolution), remeshOptions);
        if (triangleCapacity == 0U) return fail(error, "remesher produced no triangles");
        if (triangleCapacity > options.maximumOutputTriangles ||
            triangleCapacity > std::numeric_limits<std::size_t>::max() / 9U)
            return fail(error, "remesh output exceeds maximumOutputTriangles");

        std::vector<float> unindexedPositions(triangleCapacity * 9U);
        const std::size_t triangleCount = meshopt_remesh(
            unindexedPositions.data(), triangleCapacity, indices.data(), indices.size(),
            sourcePositions.data(), positions.size(), sizeof(float) * 3U,
            static_cast<int>(options.resolution), remeshOptions);
        if (triangleCount == 0U || triangleCount > triangleCapacity)
            return fail(error, "remesher returned an invalid triangle count");
        const std::size_t cornerCount = triangleCount * 3U;

        // The remesher emits independent triangle corners. Reindex exact-equal positions so
        // generated normals can be averaged across the remeshed surface's shared edges.
        std::vector<unsigned int> remap(cornerCount);
        const std::size_t vertexCount = meshopt_generateVertexRemap(
            remap.data(), nullptr, cornerCount, unindexedPositions.data(), cornerCount,
            sizeof(float) * 3U);
        if (vertexCount == 0U) return fail(error, "remesher output could not be reindexed");
        std::vector<float> indexedPositions(vertexCount * 3U);
        std::vector<unsigned int> indexedCorners(cornerCount);
        meshopt_remapVertexBuffer(indexedPositions.data(), unindexedPositions.data(), cornerCount,
                                  sizeof(float) * 3U, remap.data());
        meshopt_remapIndexBuffer(indexedCorners.data(), nullptr, cornerCount, remap.data());

        std::vector<float> cornerNormals(cornerCount * 3U);
        meshopt_generateNormals(cornerNormals.data(), indexedCorners.data(), cornerCount,
                                indexedPositions.data(), vertexCount, sizeof(float) * 3U,
                                options.creaseAngleRadians, options.normalSmoothing);

        output.positions.reserve(vertexCount);
        for (std::size_t i = 0; i < vertexCount; ++i)
            output.positions.push_back({indexedPositions[i * 3U], indexedPositions[i * 3U + 1U],
                                        indexedPositions[i * 3U + 2U]});
        output.indices.assign(indexedCorners.begin(), indexedCorners.end());
        output.cornerNormals.reserve(cornerCount);
        for (std::size_t i = 0; i < cornerCount; ++i)
            output.cornerNormals.push_back({cornerNormals[i * 3U], cornerNormals[i * 3U + 1U],
                                            cornerNormals[i * 3U + 2U]});
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        output = {};
        return false;
    }
}

} // namespace dve
