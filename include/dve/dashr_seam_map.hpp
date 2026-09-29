#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/polygon_asset.hpp"

namespace dve {

struct DashrSeamCookSettings {
    std::uint32_t resolution{512U};
    std::uint32_t radiusPixels{2U};
    std::uint32_t destinationInsetPixels{4U};
    float positionToleranceMeters{1.0e-5F};
    float uvTolerance{1.0e-5F};
    std::uint64_t maximumTriangleEdges{6ULL * 1024ULL * 1024ULL};
};

struct DashrSeamCookStats {
    std::uint64_t triangleEdges{};
    std::uint64_t geometricEdgeGroups{};
    std::uint64_t seamPairs{};
    std::uint64_t markedTexels{};
    std::uint64_t nonManifoldGroupsSkipped{};
    std::uint64_t degenerateUvEdgesSkipped{};
};

struct DashrSeamMap {
    std::uint32_t resolution{};
    // RGBA contract consumed by render::upload_dashr_seam_map:
    // x,y destination UV; z filterable seam-region value (>0 means teleport);
    // w validity. Non-seam texels are (0,0,-1,0).
    std::vector<Float4> texels;
    DashrSeamCookStats stats{};
};

[[nodiscard]] bool validate_dashr_seam_cook_settings(
    const DashrSeamCookSettings& settings,
    std::string* error = nullptr) noexcept;

// Derive teleport pairs from duplicated geometric edges whose UV representations differ.
// Exactly-two-edge manifold groups are paired; open boundaries are ignored and non-manifold
// coincident groups are conservatively skipped.
[[nodiscard]] std::optional<DashrSeamMap> cook_dashr_seam_map(
    const CookedPolygonAsset& asset,
    const DashrSeamCookSettings& settings = {},
    std::string* error = nullptr);

} // namespace dve
