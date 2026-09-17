#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dve/master_material.hpp"

namespace dve {

// Byte-exact mirror of shaders/common/material_table.hlsli. Every field remains an independent
// four-byte scalar because StructuredBuffer aggregate layout is less portable than cbuffer
// layout. Tests check every offset, not only the final size.
struct GpuMaterialRecord {
    float baseColorR{0.0F};
    float baseColorG{0.0F};
    float baseColorB{0.0F};
    float baseColorA{1.0F};
    float emissiveR{0.0F};
    float emissiveG{0.0F};
    float emissiveB{0.0F};
    float metallic{0.0F};
    float roughness{1.0F};
    float specular{0.5F};
    std::uint32_t shadingModel{0};
    std::uint32_t blendMode{0};
    float subsurfaceScatterDistanceMeters{0.0F};
    float subsurfaceColorR{1.0F};
    float subsurfaceColorG{1.0F};
    float subsurfaceColorB{1.0F};
    float clearCoat{0.0F};
    float clearCoatRoughness{0.1F};
    float foliageColorR{0.5F};
    float foliageColorG{0.8F};
    float foliageColorB{0.35F};
    float foliageTransmittance{0.5F};
    float foliageWrap{0.35F};
};

static_assert(sizeof(GpuMaterialRecord) == 23 * sizeof(std::uint32_t));
static_assert(alignof(GpuMaterialRecord) == 4);

using GpuMaterialTable = std::array<GpuMaterialRecord, 256>;

[[nodiscard]] GpuMaterialRecord pack_gpu_material_record(const VoxelMaterialDefinition& definition) noexcept;
[[nodiscard]] GpuMaterialTable build_gpu_material_table(const MaterialLibrary& library);
// Cooked/editor materials can carry layer stacks without a MaterialLibrary. This overload
// resolves those stacks before packing and returns nullopt on a bad reference or cycle.
[[nodiscard]] std::optional<GpuMaterialTable> build_gpu_material_table(
    const std::vector<VoxelMaterialDefinition>& materials,
    std::string* error = nullptr);

} // namespace dve
