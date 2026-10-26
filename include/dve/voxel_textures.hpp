#pragma once

// Texture sets on voxel materials: base colour, occlusion/roughness/metallic (ORM, packed as in
// glTF: R occlusion, G roughness, B metallic), height and emission maps, projected triplanar in
// object space like the voxel normal map (dve/voxel_normal_map.hpp), with mip filtering by the
// pixel's footprint.
//
// Each map is a VoxelNormalMap image (RGBA8, top row first) made for its role, which decides how
// its mips are reduced: colour maps (base colour, emission) are sRGB and average in linear light;
// data maps (ORM, height) average raw values. All maps of a material share textureTileMeters (one
// repeat) and textureSharpness (the triplanar blend exponent).
//
// Sampling (sample_voxel_textures) returns multipliers: base colour (linear) x baseColor,
// roughness x G, metallic x B, emission (linear) x emissive; occlusion scales the ambient light and
// the specular occlusion; height is 0 (deepest) .. 1 (highest), for parallax and displacement.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/types.hpp"

namespace dve {

enum class VoxelImageRole : std::uint8_t {
    Normal = 0,   // tangent-space vectors: mips average decoded vectors and renormalise
    Color = 1,    // sRGB colour: mips average linear light
    Data = 2,     // raw channels: mips average the bytes
};

// Builds an image for a role with its mip chain (every level down to 1 x 1). Null, with *error,
// when the size or texel count is wrong.
[[nodiscard]] std::shared_ptr<const VoxelNormalMap> make_voxel_image(std::string name, std::uint32_t width, std::uint32_t height,
                                                                     std::vector<std::uint8_t> rgba8, VoxelImageRole role,
                                                                     std::string* error = nullptr);

// Bilinear, repeating sample of one mip level (0 = full size), raw [0,1] channels; and the same
// with trilinear filtering at a fractional level. Colour images return linear values.
[[nodiscard]] Float4 sample_voxel_image(const VoxelNormalMap& image, float u, float v, float level) noexcept;

struct VoxelTextureSample {
    Float3 baseColor{1.0F, 1.0F, 1.0F};
    float alpha{1.0F};
    float occlusion{1.0F};
    float roughness{1.0F};
    float metallic{1.0F};
    Float3 emissive{1.0F, 1.0F, 1.0F};
    float height{0.5F};
    float level{};   // the mip level used
};

// The material's maps at an object-space point (metres) with object-space geometric normal;
// footprintMeters is the width of the pixel on the surface (0 = full resolution).
[[nodiscard]] VoxelTextureSample sample_voxel_textures(const VoxelMaterialDefinition& material, Float3 positionMeters, Float3 normal,
                                                       float footprintMeters) noexcept;

// Height alone (for parallax and displacement marches), at a mip level.
[[nodiscard]] float sample_voxel_height(const VoxelMaterialDefinition& material, Float3 positionMeters, Float3 normal,
                                        float level) noexcept;

// Applies a sample to a shading copy of the material (base colour, alpha, roughness, metallic,
// emission). Occlusion is the caller's (it scales ambient light, not the material).
void apply_voxel_texture_sample(VoxelMaterialDefinition& material, const VoxelTextureSample& sample) noexcept;

} // namespace dve
