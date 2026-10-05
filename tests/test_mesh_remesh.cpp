#include "dve/mesh_remesh.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures{};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_closed_cube_remesh() {
    const std::vector<dve::Float3> positions{
        {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
        {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1},
    };
    const std::vector<std::uint32_t> indices{
        0,2,1, 0,3,2, // -Z
        4,5,6, 4,6,7, // +Z
        0,1,5, 0,5,4, // -Y
        3,7,6, 3,6,2, // +Y
        0,4,7, 0,7,3, // -X
        1,2,6, 1,6,5, // +X
    };
    dve::MeshRemeshOptions options;
    options.resolution = 16U;
    dve::RemeshedTriangleMesh output;
    std::string error;
    require(dve::remesh_triangle_mesh(positions, indices, options, output, &error),
            "closed cube remesh succeeds");
    if (failures != 0 && output.indices.empty()) return;
    require(!output.positions.empty() && output.indices.size() >= indices.size(),
            "remesh emits a non-empty replacement mesh");
    require(output.indices.size() % 3U == 0U && output.cornerNormals.size() == output.indices.size(),
            "output indices and per-corner normals are aligned");

    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> directedEdges;
    for (std::size_t i = 0; i < output.indices.size(); i += 3U) {
        for (std::size_t edge = 0; edge < 3U; ++edge) {
            const std::uint32_t a = output.indices[i + edge];
            const std::uint32_t b = output.indices[i + (edge + 1U) % 3U];
            ++directedEdges[{a,b}];
        }
    }
    for (const auto& [edge, count] : directedEdges)
        require(directedEdges[{edge.second, edge.first}] == count,
                "remeshed closed cube has a matching opposite half-edge");

    for (const dve::Float3 normal : output.cornerNormals) {
        const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
        require(std::isfinite(length) && std::abs(length - 1.0F) < 1.0e-3F,
                "generated corner normal is finite and normalized");
    }
}

void test_rejects_invalid_input() {
    dve::MeshRemeshOptions options;
    dve::RemeshedTriangleMesh output;
    std::string error;
    const std::vector<dve::Float3> positions{{0,0,0}, {1,0,0}, {0,1,0}};
    const std::vector<std::uint32_t> invalidIndices{0,1,4};
    require(!dve::remesh_triangle_mesh(positions, invalidIndices, options, output, &error) &&
            !error.empty() && output.indices.empty(), "out-of-range indices fail without partial output");
    options.resolution = 3U;
    const std::vector<std::uint32_t> validIndices{0,1,2};
    require(!dve::remesh_triangle_mesh(positions, validIndices, options, output, &error),
            "resolution below upstream supported range is rejected");
}

} // namespace

int main() {
    test_closed_cube_remesh();
    test_rejects_invalid_input();
    if (failures != 0) return 1;
    std::cout << "mesh remesh: all checks passed\n";
    return 0;
}
