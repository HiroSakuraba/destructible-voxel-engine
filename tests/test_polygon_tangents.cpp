#include "dve/polygon_tangents.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace dve;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void near3(Float4 actual, Float3 expected, float sign, const char* message) {
    if (std::abs(actual.x - expected.x) > 1e-5F || std::abs(actual.y - expected.y) > 1e-5F ||
        std::abs(actual.z - expected.z) > 1e-5F || actual.w != sign)
        throw std::runtime_error(message);
}

CookedPolygonAsset quad(Float2 (*uv)(float, float)) {
    CookedPolygonAsset asset;
    const Float3 positions[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    for (const auto& p : positions) {
        PolygonVertex v;
        v.position = p;
        v.normal = {0, 0, 1};
        v.texcoord = uv(p.x, p.y);
        asset.vertices.push_back(v);
    }
    asset.indices = {0, 1, 2, 0, 2, 3};
    return asset;
}
} // namespace

int main() {
    try {
        // UV aligned with position: tangent is +X, orientation preserving.
        auto aligned = quad([](float x, float y) { return Float2{x, y}; });
        require(compute_polygon_tangents_mikktspace(aligned), "aligned quad must process");
        for (const auto& v : aligned.vertices)
            near3(v.tangent, {1, 0, 0}, 1.0F, "aligned quad tangent must be +X with sign +1");

        // Mirrored U: tangent flips to -X and the handedness sign goes
        // negative — the pair the shader needs to rebuild the bitangent.
        auto mirrored = quad([](float x, float y) { return Float2{1.0F - x, y}; });
        require(compute_polygon_tangents_mikktspace(mirrored), "mirrored quad must process");
        for (const auto& v : mirrored.vertices)
            near3(v.tangent, {-1, 0, 0}, -1.0F, "mirrored quad tangent must be -X with sign -1");

        // U along +Y: tangent follows U. Swapping U and V mirrors the frame,
        // so the handedness sign is -1 (checked against MikkTSpace output).
        auto rotated = quad([](float x, float y) { return Float2{y, x}; });
        require(compute_polygon_tangents_mikktspace(rotated), "rotated quad must process");
        for (const auto& v : rotated.vertices)
            near3(v.tangent, {0, 1, 0}, -1.0F, "rotated quad tangent must be +Y with sign -1");

        // Tangents stay unit length and perpendicular to the normal on a
        // slanted triangle with non-uniform UV scale.
        CookedPolygonAsset slanted;
        const Float3 n = {0.0F, 0.57735027F, 0.81649658F};
        slanted.vertices = {{{0, 0, 0}, n, {1, 0, 0, 1}, {0, 0}},
                            {{2, 0, 0}, n, {1, 0, 0, 1}, {1, 0}},
                            {{0, 1, 0.5F}, n, {1, 0, 0, 1}, {0, 0.25F}}};
        slanted.indices = {0, 1, 2};
        require(compute_polygon_tangents_mikktspace(slanted), "slanted triangle must process");
        for (const auto& v : slanted.vertices) {
            const float length = std::sqrt(v.tangent.x * v.tangent.x + v.tangent.y * v.tangent.y +
                                           v.tangent.z * v.tangent.z);
            require(std::abs(length - 1.0F) < 1e-5F, "tangent must be unit length");
            const float dot = v.tangent.x * n.x + v.tangent.y * n.y + v.tangent.z * n.z;
            require(std::abs(dot) < 1e-5F, "tangent must be perpendicular to the normal");
        }

        // Refusals leave the asset untouched.
        CookedPolygonAsset empty;
        empty.vertices = {{{0, 0, 0}, {0, 0, 1}, {9, 9, 9, 9}, {0, 0}}};
        require(!compute_polygon_tangents_mikktspace(empty), "index-less asset must refuse");
        require(empty.vertices[0].tangent.x == 9.0F, "refusal must not touch tangents");
        CookedPolygonAsset broken = quad([](float x, float y) { return Float2{x, y}; });
        broken.indices = {0, 1, 7};
        require(!compute_polygon_tangents_mikktspace(broken), "out-of-range index must refuse");

        std::cout << "polygon tangents tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
