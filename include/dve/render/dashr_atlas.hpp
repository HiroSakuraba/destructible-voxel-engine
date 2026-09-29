#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/render/mesh_rhi_mirror.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

inline constexpr std::uint32_t kDashrAtlasTargetCount = 4U;
inline constexpr std::uint32_t kDashrEdgeFillRadiusPixels = 2U;

struct DashrAtlasShaderBytecode {
    std::vector<std::byte> atlasVertex;
    std::vector<std::byte> atlasFragment;
    std::vector<std::byte> edgeFillCompute;
    [[nodiscard]] bool valid() const noexcept {
        return !atlasVertex.empty() && !atlasFragment.empty() && !edgeFillCompute.empty();
    }
};

struct DashrAtlasResources {
    std::uint32_t resolution{};
    std::array<rhi::TextureHandle, kDashrAtlasTargetCount> rawTextures{};
    std::array<rhi::TextureViewHandle, kDashrAtlasTargetCount> rawViews{};
    std::array<rhi::TextureHandle, kDashrAtlasTargetCount> filledTextures{};
    std::array<rhi::TextureViewHandle, kDashrAtlasTargetCount> filledViews{};

    // xy = point-sampled destination UV, z = filterable signed seam-region value,
    // w = validity/reserved. The same texture is bound twice with different samplers.
    rhi::TextureHandle seamTexture{};
    rhi::TextureViewHandle seamView{};

    rhi::SamplerHandle linearClampSampler{};
    rhi::SamplerHandle pointClampSampler{};

    rhi::BindGroupLayoutHandle edgeFillLayout{};
    rhi::BindGroupHandle edgeFillGroup{};
    rhi::ComputePipelineHandle edgeFillPipeline{};

    // Consumer layout: filled deformation atlas 0..3, seam-linear 4, seam-point 5.
    rhi::BindGroupLayoutHandle consumerLayout{};
    rhi::BindGroupHandle consumerGroup{};

    rhi::GraphicsPipelineHandle atlasPipeline{};
    bool published{};

    [[nodiscard]] bool valid() const noexcept;
};

struct DashrAtlasUpdateStats {
    std::uint64_t trianglesRasterized{};
    std::uint64_t edgeFillDispatches{};
    std::uint64_t renderPasses{};
    std::uint64_t textureTransitions{};
};

[[nodiscard]] bool create_dashr_atlas_resources(
    rhi::IDevice& device,
    const DashrAtlasShaderBytecode& bytecode,
    std::uint32_t resolution,
    DashrAtlasResources& resources,
    std::string* error = nullptr);

[[nodiscard]] bool destroy_dashr_atlas_resources(
    rhi::IDevice& device,
    DashrAtlasResources& resources,
    std::string* error = nullptr);

// Upload a precomputed RGBA seam map. The input must contain exactly resolution^2
// float4 texels in row-major order.
[[nodiscard]] bool upload_dashr_seam_map(
    rhi::IDevice& device,
    DashrAtlasResources& resources,
    std::span<const Float4> seamTexels,
    std::string* error = nullptr);

// Rasterize the current polygon differential into four RGBA16F UV-space targets,
// then dilate valid samples by a bounded two-pixel edge fill. The current mesh
// mirror is the deformation source; once live GPU skinning publishes deformed
// position/normal/tangent data, the same pass consumes that buffer unchanged.
[[nodiscard]] bool record_dashr_atlas_update(
    rhi::IDevice& device,
    DashrAtlasResources& resources,
    const MeshRhiMirror& mirror,
    const CookedPolygonAsset& asset,
    DashrAtlasUpdateStats& stats,
    rhi::FenceHandle* fence = nullptr,
    std::string* error = nullptr);

} // namespace dve::render
