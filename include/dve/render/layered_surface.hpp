#pragma once
#include "dve/polygon_asset.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dve::render {
// Portable bounded layer payload. Header: count, root mapping, root height, reserved,
// four record offsets. Each record contains the stable material (29 words), mapping
// (44), surface recipe (16), then 16 controls/map offsets. Offset zero means no map.
// Images retain their original dimensions, typed mip chains and individual samplers.
// Software bilinear/trilinear sampling avoids adding any hardware sampler descriptors.
inline constexpr std::size_t kMaximumLayerPayloadBytes = 128U * 1024U * 1024U;
inline constexpr unsigned kLayerMaterialWords=29, kLayerMappingWords=44,
    kLayerRecipeWords=16, kLayerControlWords=16;
struct LayeredSurfacePayload { std::vector<std::uint32_t> words = std::vector<std::uint32_t>(8); };
[[nodiscard]] std::optional<LayeredSurfacePayload> build_layered_surface_payload(
    const CookedPolygonAsset&, std::uint32_t materialIndex, std::string* error=nullptr);
// Validates only the requested stack; dormant references are checked too, so unmuting
// cannot turn an accepted stack into a cycle or an unsupported nested stack.
[[nodiscard]] bool validate_layered_surface(const CookedPolygonAsset&, std::uint32_t,
                                           std::string* error=nullptr);
}
