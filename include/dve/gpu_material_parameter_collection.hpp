#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "dve/material_parameter_collection.hpp"

namespace dve {

struct alignas(16) GpuFloat4 {
    float x{};
    float y{};
    float z{};
    float w{};
};

struct alignas(16) GpuMaterialParameterCollection {
    std::array<GpuFloat4, kMaximumMaterialGlobalScalars / 4> scalarGroups{};
    std::array<GpuFloat4, kMaximumMaterialGlobalVectors> vectors{};
    std::uint32_t scalarCount{};
    std::uint32_t vectorCount{};
    std::uint32_t reserved0{};
    std::uint32_t reserved1{};
};

static_assert(sizeof(GpuFloat4) == 16);
static_assert(alignof(GpuMaterialParameterCollection) == 16);
static_assert(sizeof(GpuMaterialParameterCollection) == 400);

[[nodiscard]] GpuMaterialParameterCollection pack_gpu_material_parameter_collection(
    const MaterialParameterCollection& collection) noexcept;

} // namespace dve
