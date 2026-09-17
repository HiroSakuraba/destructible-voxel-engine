#pragma once

#include <cassert>
#include <compare>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace dve {

constexpr std::int32_t kBrickDim = 8;
constexpr std::int32_t kBrickVoxelCount = kBrickDim * kBrickDim * kBrickDim;
using MaterialId = std::uint8_t;
constexpr MaterialId kAirMaterial = 0;

// Shared by asset import, master-material resolution, and GPU shading records.
enum class MaterialShadingModel : std::uint8_t {
    StandardPBR,
    Unlit,
    Emissive,
    Subsurface,
    TwoSidedFoliage,
    ClearCoat,
};

enum class MaterialBlendMode : std::uint8_t { Opaque, Masked, Translucent };

enum class MaterialLayerBlendMode : std::uint8_t {
    Lerp,
    Multiply,
    Additive,
};

struct Int3 {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};
    auto operator<=>(const Int3&) const = default;
};

struct Float3 {
    float x{};
    float y{};
    float z{};
};

struct BrickKey {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};
    auto operator<=>(const BrickKey&) const = default;
};

[[nodiscard]] constexpr std::int32_t floor_div(std::int32_t value, std::int32_t divisor) {
    const std::int32_t q = value / divisor;
    const std::int32_t r = value % divisor;
    return (r != 0 && ((r < 0) != (divisor < 0))) ? q - 1 : q;
}

[[nodiscard]] constexpr std::int32_t floor_mod(std::int32_t value, std::int32_t divisor) {
    return value - floor_div(value, divisor) * divisor;
}

[[nodiscard]] constexpr BrickKey brick_key_from_voxel(Int3 voxel) {
    return {
        floor_div(voxel.x, kBrickDim),
        floor_div(voxel.y, kBrickDim),
        floor_div(voxel.z, kBrickDim),
    };
}

[[nodiscard]] constexpr Int3 local_voxel_from_global(Int3 voxel) {
    return {
        floor_mod(voxel.x, kBrickDim),
        floor_mod(voxel.y, kBrickDim),
        floor_mod(voxel.z, kBrickDim),
    };
}

[[nodiscard]] constexpr Int3 brick_origin(BrickKey key) {
    return {key.x * kBrickDim, key.y * kBrickDim, key.z * kBrickDim};
}

[[nodiscard]] constexpr bool valid_local_voxel(Int3 local) noexcept {
    return local.x >= 0 && local.x < kBrickDim && local.y >= 0 && local.y < kBrickDim &&
           local.z >= 0 && local.z < kBrickDim;
}

[[nodiscard]] constexpr std::uint16_t voxel_index_unchecked(Int3 local) noexcept {
    assert(valid_local_voxel(local));
    return static_cast<std::uint16_t>(local.x + kBrickDim * local.y + kBrickDim * kBrickDim * local.z);
}

[[nodiscard]] constexpr Int3 local_from_index_unchecked(std::uint16_t index) noexcept {
    assert(index < kBrickVoxelCount);
    return {
        static_cast<std::int32_t>(index & 7U),
        static_cast<std::int32_t>((index >> 3U) & 7U),
        static_cast<std::int32_t>(index >> 6U),
    };
}

// Checked API-boundary helpers. Inner loops must use the unchecked forms after proving bounds.
[[nodiscard]] constexpr std::uint16_t voxel_index(Int3 local) {
    if (!valid_local_voxel(local)) {
        throw std::out_of_range("local voxel coordinate is outside an 8^3 brick");
    }
    return voxel_index_unchecked(local);
}

[[nodiscard]] constexpr Int3 local_from_index(std::uint16_t index) {
    if (index >= kBrickVoxelCount) {
        throw std::out_of_range("voxel index is outside an 8^3 brick");
    }
    return local_from_index_unchecked(index);
}

[[nodiscard]] constexpr Int3 global_from_local(BrickKey key, Int3 local) {
    const Int3 origin = brick_origin(key);
    return {origin.x + local.x, origin.y + local.y, origin.z + local.z};
}

[[nodiscard]] constexpr Int3 min_components(Int3 a, Int3 b) {
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
}

[[nodiscard]] constexpr Int3 max_components(Int3 a, Int3 b) {
    return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}

constexpr Int3 kInt3Max{
    std::numeric_limits<std::int32_t>::max(),
    std::numeric_limits<std::int32_t>::max(),
    std::numeric_limits<std::int32_t>::max(),
};
constexpr Int3 kInt3Min{
    std::numeric_limits<std::int32_t>::min(),
    std::numeric_limits<std::int32_t>::min(),
    std::numeric_limits<std::int32_t>::min(),
};

} // namespace dve
