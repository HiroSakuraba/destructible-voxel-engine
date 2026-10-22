#pragma once

// Normal maps on voxel materials (VoxelMaterialDefinition::normalMap).
//
// Voxels have no UVs, so the map is projected triplanar in object space: each axis projects the
// object-space position (metres / normalMapTileMeters) onto the plane across it, the three samples
// are swizzled from tangent space onto that axis (the same swizzle as the polygon renderer's
// triplanar normal maps) and blended by |normal|^sharpness. On an axis-aligned voxel face only
// the face's own projection contributes. A flat texel (128, 128, 255) leaves the normal as it is.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/types.hpp"

namespace dve {

// Builds an immutable map (computes contentHash). Null, with *error, when the size is 0 or above
// kMaximumVoxelNormalMapDimension or the texel count does not match.
[[nodiscard]] std::shared_ptr<const VoxelNormalMap> make_voxel_normal_map(std::string name, std::uint32_t width,
                                                                          std::uint32_t height, std::vector<std::uint8_t> rgba8,
                                                                          std::string* error = nullptr);

// Bilinear, repeating sample of the decoded tangent-space vector (2 x rgb - 1, not normalized) at
// (u, v) in repeats; v grows down the image.
[[nodiscard]] Float3 sample_voxel_normal_map(const VoxelNormalMap& map, float u, float v) noexcept;

// The material's normal at an object-space point (metres) with object-space geometric normal
// `normal`; returns `normal` unchanged without a map or with strength 0.
[[nodiscard]] Float3 voxel_normal_map_normal(const VoxelMaterialDefinition& material, Float3 positionMeters,
                                             Float3 normal) noexcept;

} // namespace dve
