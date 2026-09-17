#include "dve/gpu_material_parameter_collection.hpp"

namespace dve {

GpuMaterialParameterCollection pack_gpu_material_parameter_collection(
    const MaterialParameterCollection& collection) noexcept {
    GpuMaterialParameterCollection result;
    result.scalarCount = static_cast<std::uint32_t>(collection.scalar_count());
    result.vectorCount = static_cast<std::uint32_t>(collection.vector_count());
    for (std::size_t slot = 0; slot < collection.scalar_count() && slot < kMaximumMaterialGlobalScalars; ++slot) {
        GpuFloat4& group = result.scalarGroups[slot / 4];
        const float value = collection.scalar_at(slot);
        switch (slot % 4) {
        case 0: group.x = value; break;
        case 1: group.y = value; break;
        case 2: group.z = value; break;
        default: group.w = value; break;
        }
    }
    for (std::size_t slot = 0; slot < collection.vector_count() && slot < kMaximumMaterialGlobalVectors; ++slot) {
        const Float4 value = collection.vector_at(slot);
        result.vectors[slot] = {value.x, value.y, value.z, value.w};
    }
    return result;
}

} // namespace dve
