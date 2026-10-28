#pragma once
// T3: CPU BC5_UNORM normal-map codec. Independent of Binomial/Basis.
#include "dve/render/texture_residency.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dve::render {
// Encode a full RGBA8 tangent-space normal mip chain to GPU-native BC5 RG blocks.
// Z is reconstructed as +sqrt(max(0,1-x*x-y*y)) in compatible shaders;
// signed/negative-Z tangent normals cannot round-trip and are rejected.
[[nodiscard]] bool encode_bc5_normal_mips(
    const PolygonTextureMipChain& source,
    std::vector<std::vector<std::byte>>& blocks, std::string* error = nullptr);

// Decode one 16-byte BC5 block to unsigned normalized R/G bytes.
// Reference decoder for correctness and image-quality measurements, not runtime sampling.
[[nodiscard]] std::array<std::array<std::uint8_t,2>,16> decode_bc5_rg_block(
    std::span<const std::byte,16> block) noexcept;

// Quality of the reference BC5 normal reconstruction against the input RGBA8
// normal map, in degrees. Returns -1 on malformed input or payload.
[[nodiscard]] double bc5_mean_normal_angle_degrees(
    const PolygonTextureMip& source, std::span<const std::byte> blocks) noexcept;

[[nodiscard]] TextureBc7Encoder make_bc5_normal_encoder();
} // namespace dve::render
