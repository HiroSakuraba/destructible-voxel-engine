#pragma once
// T4: associate actual GPU-resident normal view and format with the shader
// mapping bit. The compression *request* is not an authoritative format.
#include "dve/gpu_material_mapping.hpp"
#include "dve/render/texture_residency.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace dve::render {
// CPU mirror of DecodeMaterialNormalSample()'s BC5 branch in shaders/common/material_mapping.hlsli
// (sampled R,G in [0,1] -> legacy-compatible [0,1] XYZ). Keep both in sync: tests compare the two.
[[nodiscard]] inline std::array<float, 3> decode_bc5_normal_sample(float r, float g) noexcept {
    const float x = r * 2.0F - 1.0F;
    const float y = g * 2.0F - 1.0F;
    const float z = std::sqrt((std::max)(0.0F, 1.0F - (x * x + y * y)));
    const float inverseLength = 1.0F / std::sqrt((std::max)(x * x + y * y + z * z, 1.0e-12F));
    const auto pack = [](float v) { return std::clamp(v * 0.5F + 0.5F, 0.0F, 1.0F); };
    return {pack(x * inverseLength), pack(y * inverseLength), pack(z * inverseLength)};
}

// Must be called on the residency/descriptor owner's thread, with a stable
// ResidentTexture snapshot. The caller must keep the bound texture view alive.
// Returns true for a matching BC5, linear BC7 or linear RGBA8 normal view; false for
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
    // Linear RGB-encoded normals (the legacy path). BC7 normals come from PreferBc7 residency
    // and are NOT BC5: the flag stays clear. sRGB formats would corrupt normals and are rejected.
    return resident->format == rhi::TextureFormat::RGBA8Unorm ||
           resident->format == rhi::TextureFormat::BC7RGBAUnorm;
}
} // namespace dve::render
