#pragma once
#include "dve/polygon_asset.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
namespace dve::editor {
enum class SubdivisionUvMode : std::uint8_t {
    Linear,       // exact piecewise-linear source UV interpolation
    SmoothInterior // relax only UV chart interiors; preserve seams and island boundaries
};
struct SubdivisionSettings {
    std::uint32_t level{2};
    bool preserveSharpEdges{};
    // Each face stores one triangle or two quad triangles as oriented attribute-vertex triples.
    // Resolve triples against the current cage, independent of triangle ordering.
    std::vector<std::vector<std::uint32_t>> controlFaces;
    // Canonical cage attribute-vertex edge keys; finite semi-sharp strengths in (0,4].
    std::map<std::uint64_t,float> edgeCreases;
    // Stored after existing fields to keep aggregate {level, sharp} callers valid.
    SubdivisionUvMode uvMode{SubdivisionUvMode::Linear};
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
[[nodiscard]] SubdivisionEvaluation evaluate_subdivision(const CookedPolygonAsset& cage, const SubdivisionSettings& settings);
// Saved control-face records override legacy pairing. Legacy/newly emitted triangles are
// paired once by connectivity and material, then frozen. Attributes retain UV discontinuities.
[[nodiscard]] std::vector<std::vector<std::uint32_t>> subdivision_control_face_triangles(const CookedPolygonAsset& cage, const SubdivisionSettings* settings=nullptr);
[[nodiscard]] std::set<std::uint64_t> subdivision_control_edges(const CookedPolygonAsset& cage, const SubdivisionSettings* settings=nullptr);
// Freeze legacy pairing once; subsequent topology changes preserve surviving faces and creases.
void freeze_subdivision_topology(const CookedPolygonAsset&, SubdivisionSettings&);
[[nodiscard]] SubdivisionSettings remap_subdivision_topology(const CookedPolygonAsset& before,
    const CookedPolygonAsset& after, const SubdivisionSettings&);
}
