#include "dve/gpu_material_buffer.hpp"

namespace dve {
namespace {
static_assert(static_cast<int>(MaterialShadingModel::StandardPBR) == 0);
static_assert(static_cast<int>(MaterialShadingModel::Unlit) == 1);
static_assert(static_cast<int>(MaterialShadingModel::Emissive) == 2);
static_assert(static_cast<int>(MaterialShadingModel::Subsurface) == 3);
static_assert(static_cast<int>(MaterialShadingModel::TwoSidedFoliage) == 4);
static_assert(static_cast<int>(MaterialShadingModel::ClearCoat) == 5);
static_assert(static_cast<int>(MaterialBlendMode::Opaque) == 0);
static_assert(static_cast<int>(MaterialBlendMode::Masked) == 1);
static_assert(static_cast<int>(MaterialBlendMode::Translucent) == 2);

GpuMaterialRecord fallback_record() {
    GpuMaterialRecord fallback;
    // Same neutral dielectric presentation as the built-in Standard Surface preset.
    fallback.baseColorR = fallback.baseColorG = fallback.baseColorB = 0.214F;
    fallback.roughness = kStandardSurfaceRoughness;
    fallback.metallic = 0.0F;
    fallback.specular = kStandardSurfaceSpecular;
    return fallback;
}
} // namespace

GpuMaterialRecord pack_gpu_material_record(const VoxelMaterialDefinition& definition) noexcept {
    GpuMaterialRecord record;
    record.baseColorR = definition.baseColor.x;
    record.baseColorG = definition.baseColor.y;
    record.baseColorB = definition.baseColor.z;
    record.baseColorA = definition.baseColor.w;
    record.emissiveR = definition.emissive.x;
    record.emissiveG = definition.emissive.y;
    record.emissiveB = definition.emissive.z;
    record.metallic = definition.metallic;
    record.roughness = definition.roughness;
    record.specular = definition.specular;
    record.shadingModel = static_cast<std::uint32_t>(definition.shadingModel);
    record.blendMode = static_cast<std::uint32_t>(definition.blendMode);
    record.subsurfaceScatterDistanceMeters = definition.subsurfaceScatterDistanceMeters;
    record.subsurfaceColorR = definition.subsurfaceColor.x;
    record.subsurfaceColorG = definition.subsurfaceColor.y;
    record.subsurfaceColorB = definition.subsurfaceColor.z;
    record.clearCoat = definition.clearCoat;
    record.clearCoatRoughness = definition.clearCoatRoughness;
    record.foliageColorR = definition.foliageColor.x;
    record.foliageColorG = definition.foliageColor.y;
    record.foliageColorB = definition.foliageColor.z;
    record.foliageTransmittance = definition.foliageTransmittance;
    record.foliageWrap = definition.foliageWrap;
    return record;
}

GpuMaterialTable build_gpu_material_table(const MaterialLibrary& library) {
    GpuMaterialTable table;
    table.fill(fallback_record());
    for (std::uint32_t materialId = 0; materialId <= 255; ++materialId) {
        if (const VoxelMaterialDefinition* definition = library.resolved(static_cast<MaterialId>(materialId))) {
            table[materialId] = pack_gpu_material_record(*definition);
        }
    }
    return table;
}

std::optional<GpuMaterialTable> build_gpu_material_table(
    const std::vector<VoxelMaterialDefinition>& materials, std::string* error) {
    const auto resolved = resolve_voxel_material_layers(materials, error);
    if (!resolved) return std::nullopt;
    GpuMaterialTable table;
    table.fill(fallback_record());
    for (std::size_t materialId = 0; materialId < resolved->size(); ++materialId) {
        table[materialId] = pack_gpu_material_record((*resolved)[materialId]);
    }
    return table;
}

} // namespace dve
