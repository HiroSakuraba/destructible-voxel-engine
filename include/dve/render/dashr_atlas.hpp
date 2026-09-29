#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

inline constexpr std::uint32_t kDashrAtlasTargetCount = 4U;
inline constexpr std::uint32_t kDashrEdgeFillRadiusPixels = 2U;

// Atlas-specific vertex format. Ordinary DVE polygon tangents are normalized for
// lighting, while DASHR requires the scaled differential dP/du and dP/dv.
// Keeping this as a separate stream avoids changing the normal polygon ABI.
struct GpuDashrSurfaceVertex {
    float positionX{}, positionY{}, positionZ{};
    float dPduX{1.0F}, dPduY{}, dPduZ{};
    float dPdvX{}, dPdvY{1.0F}, dPdvZ{};
    float uvX{}, uvY{};
    float distortionU{1.0F}, distortionV{1.0F};
};
static_assert(sizeof(GpuDashrSurfaceVertex) == 13U * sizeof(float));

struct DashrSurfaceMeshBuildStats {
    std::uint64_t triangles{};
    std::uint64_t degenerateUvTriangles{};
    std::uint64_t fallbackVertices{};
    float minimumDistortionU{1.0F};
    float maximumDistortionU{1.0F};
    float minimumDistortionV{1.0F};
    float maximumDistortionV{1.0F};
};

// Build the surface differential stream from the asset's UV0 parameterization.
// With an empty deformedPositions span, the rest mesh is used and distortion is
// one. Supplying one object-space position per vertex produces a CPU/reference
// animated stream and measures stretch/compression relative to the rest surface.
[[nodiscard]] std::optional<std::vector<GpuDashrSurfaceVertex>>
build_dashr_surface_vertices(
    const CookedPolygonAsset& asset,
    std::span<const Float3> deformedPositions = {},
    DashrSurfaceMeshBuildStats* stats = nullptr,
    std::string* error = nullptr);

struct DashrSurfaceMeshMirrorStats {
    std::size_t vertexCapacityBytes{};
    std::size_t indexCapacityBytes{};
    std::size_t uploadedBytes{};
    std::uint64_t publications{};
    std::uint64_t reallocations{};
};

class DashrSurfaceMeshMirror {
public:
    explicit DashrSurfaceMeshMirror(rhi::IDevice& device) : device_(device) {}
    ~DashrSurfaceMeshMirror();
    DashrSurfaceMeshMirror(const DashrSurfaceMeshMirror&) = delete;
    DashrSurfaceMeshMirror& operator=(const DashrSurfaceMeshMirror&) = delete;

    // Empty deformedPositions publishes the rest surface. A populated span must
    // match asset.vertices exactly. Repeated calls reuse buffer capacity, so a
    // CPU-skinned pose can update the DASHR atlas without reallocating each frame.
    [[nodiscard]] bool upload(
        const CookedPolygonAsset& asset,
        std::span<const Float3> deformedPositions = {},
        DashrSurfaceMeshBuildStats* buildStats = nullptr,
        std::string* error = nullptr);

    void reset() noexcept;

    [[nodiscard]] rhi::BufferHandle vertex_buffer() const noexcept { return vertexBuffer_; }
    [[nodiscard]] rhi::BufferHandle index_buffer() const noexcept { return indexBuffer_; }
    [[nodiscard]] std::uint32_t vertex_count() const noexcept { return vertexCount_; }
    [[nodiscard]] std::uint32_t index_count() const noexcept { return indexCount_; }
    [[nodiscard]] const DashrSurfaceMeshMirrorStats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] bool ensure_buffer(
        rhi::BufferHandle& handle,
        std::size_t& capacity,
        std::size_t requiredBytes,
        rhi::BufferUsage usage,
        std::string_view debugName,
        std::string* error);

    rhi::IDevice& device_;
    rhi::BufferHandle vertexBuffer_{};
    rhi::BufferHandle indexBuffer_{};
    std::size_t vertexBytes_{};
    std::size_t indexBytes_{};
    std::uint32_t vertexCount_{};
    std::uint32_t indexCount_{};
    std::uint64_t assetContentHash_{};
    DashrSurfaceMeshMirrorStats stats_{};
};

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

[[nodiscard]] bool upload_dashr_seam_map(
    rhi::IDevice& device,
    DashrAtlasResources& resources,
    std::span<const Float4> seamTexels,
    std::string* error = nullptr);

// Rasterize the current scaled surface differential into four RGBA16F UV-space
// targets, then dilate valid samples by a bounded two-pixel edge fill.
[[nodiscard]] bool record_dashr_atlas_update(
    rhi::IDevice& device,
    DashrAtlasResources& resources,
    const DashrSurfaceMeshMirror& mirror,
    const CookedPolygonAsset& asset,
    DashrAtlasUpdateStats& stats,
    rhi::FenceHandle* fence = nullptr,
    std::string* error = nullptr);

} // namespace dve::render
