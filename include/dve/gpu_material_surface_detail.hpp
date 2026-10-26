#pragma once
#include <cstddef>
#include <cstdint>
#include "dve/material_surface_detail.hpp"
namespace dve {
// Scalar-only 64-byte mirror of SurfaceDetailRecord in material_surface_detail.hlsli.
// Kept separate from the stable 116-byte MaterialRecord used by voxel/GPU tables.
struct GpuMaterialSurfaceDetail {
    std::uint32_t pattern{};
    std::uint32_t space{};
    std::uint32_t seed{1};
    float periodMeters{.02F};
    float rotationX{};
    float rotationY{};
    float rotationZ{};
    float rotationW{1};
    float grainStretch{20};
    float colorVariation{.25F};
    float roughnessVariation{.15F};
    float bumpHeightMeters{.00002F};
    float uvTileMeters{1};
    std::uint32_t reserved0{};
    std::uint32_t reserved1{};
    std::uint32_t reserved2{};
};
static_assert(sizeof(GpuMaterialSurfaceDetail)==64 && alignof(GpuMaterialSurfaceDetail)==4);
[[nodiscard]] GpuMaterialSurfaceDetail pack_gpu_material_surface_detail(const MaterialSurfaceDetail&) noexcept;
}
