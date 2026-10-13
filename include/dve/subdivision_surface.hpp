#pragma once
#include "dve/polygon_asset.hpp"
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>
namespace dve::editor {
struct SubdivisionSettings {
    std::uint32_t level{2};
    bool preserveSharpEdges{};
    auto operator<=>(const SubdivisionSettings&) const = default;
};
inline constexpr std::uint32_t kMaxSubdivisionLevel = 4;
inline constexpr std::size_t kMaxSubdivisionControlTriangles = 50'000;
inline constexpr std::size_t kMaxSubdivisionFaces = 125'000;
struct SubdivisionEvaluation {
    std::optional<CookedPolygonAsset> mesh;
    std::vector<std::uint32_t> controlTriangles; // One cage triangle per evaluated triangle
    std::string error;
};
[[nodiscard]] SubdivisionEvaluation evaluate_subdivision(const CookedPolygonAsset& cage, SubdivisionSettings settings);
// Consecutive triangle pairs sharing an oppositely wound edge and material become control
// quads by connectivity. This remains stable under cage deformation. Other triangles remain
// Catmull-Clark control faces. Corner attributes retain the source triangle UV discontinuities.
[[nodiscard]] std::vector<std::vector<std::uint32_t>> subdivision_control_face_triangles(const CookedPolygonAsset& cage);
[[nodiscard]] std::set<std::uint64_t> subdivision_control_edges(const CookedPolygonAsset& cage);
}
