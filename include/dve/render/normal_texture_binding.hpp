#pragma once
// T4: associate actual GPU-resident normal view and format with the shader
// mapping bit. The compression *request* is not an authoritative format.
#include "dve/gpu_material_mapping.hpp"
#include "dve/render/texture_residency.hpp"

namespace dve::render {
// Must be called on the residency/descriptor owner's thread, with a stable
// ResidentTexture snapshot. The caller must keep the bound texture view alive.
// Returns true for a matching BC5 or linear RGBA8 normal view; false for
// mismatched, unbound or unsupported resources. Clears BC5 flag on any failure.
[[nodiscard]] inline bool configure_bound_normal_format(
    GpuPolygonMaterialMappingRecord& mapping,
    const ResidentTexture* resident,
    rhi::TextureViewHandle actuallyBoundView) noexcept {
    mapping.mappingFlags &= ~kGpuMaterialMappingNormalBc5;
    if (mapping.normalTexture == kNoGpuTextureIndex ||
        !resident || !resident->texture || !resident->view ||
        !actuallyBoundView || resident->view != actuallyBoundView ||
        resident->semantic != TextureSemantic::Normal)
        return false;
    if (resident->format == rhi::TextureFormat::BC5RGUnorm) {
        mapping.mappingFlags |= kGpuMaterialMappingNormalBc5;
        return true;
    }
    return resident->format == rhi::TextureFormat::RGBA8Unorm;
}
} // namespace dve::render
