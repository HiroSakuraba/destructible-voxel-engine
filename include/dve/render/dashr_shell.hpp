#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/dashr_surface.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/render/dashr_atlas.hpp"
#include "dve/render/cascaded_shadow_atlas.hpp"
#include "dve/render/environment_lighting_gpu.hpp"
#include "dve/render/main_material_table.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

struct GpuDashrShellVertex {
    float positionX{}, positionY{}, positionZ{};
    float uvX{}, uvY{};
};
static_assert(sizeof(GpuDashrShellVertex) == 5U * sizeof(float));

struct DashrShellSubmeshRange {
    std::uint32_t materialIndex{};
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
};

struct DashrShellBuildStats {
    std::uint64_t sourceTriangles{};
    std::uint64_t emittedTriangles{};
    std::uint64_t degenerateTrianglesSkipped{};
    float minimumExtrusion{};
    float maximumExtrusion{};
};

struct DashrShellMesh {
    std::vector<GpuDashrShellVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<DashrShellSubmeshRange> submeshes;
    DashrShellBuildStats stats{};
};

[[nodiscard]] std::optional<DashrShellMesh> build_dashr_shell_mesh(
    const CookedPolygonAsset& asset,
    const DashrSurfaceSettings& settings,
    std::span<const Float3> deformedPositions = {},
    std::string* error = nullptr);

class DashrShellMeshMirror {
public:
    explicit DashrShellMeshMirror(rhi::IDevice& device) : device_(device) {}
    ~DashrShellMeshMirror();
    DashrShellMeshMirror(const DashrShellMeshMirror&) = delete;
    DashrShellMeshMirror& operator=(const DashrShellMeshMirror&) = delete;

    [[nodiscard]] bool upload(
        const DashrShellMesh& shell,
        std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] rhi::BufferHandle vertex_buffer() const noexcept { return vertexBuffer_; }
    [[nodiscard]] rhi::BufferHandle index_buffer() const noexcept { return indexBuffer_; }
    [[nodiscard]] std::uint32_t index_count() const noexcept { return indexCount_; }
    [[nodiscard]] std::span<const DashrShellSubmeshRange> submeshes() const noexcept {
        return submeshes_;
    }

private:
    [[nodiscard]] bool ensure_buffer(
        rhi::BufferHandle& handle,
        std::size_t& capacity,
        std::size_t required,
        rhi::BufferUsage usage,
        std::string_view name,
        std::string* error);

    rhi::IDevice& device_;
    rhi::BufferHandle vertexBuffer_{};
    rhi::BufferHandle indexBuffer_{};
    std::size_t vertexCapacity_{};
    std::size_t indexCapacity_{};
    std::uint32_t indexCount_{};
    std::vector<DashrShellSubmeshRange> submeshes_;
};

struct DashrShellShaderBytecode {
    std::vector<std::byte> vertex;
    std::vector<std::byte> fragment;
    // Optional production material fragment. The diagnostic fragment remains
    // useful for UV/step/seam visualization even when PBR is available.
    std::vector<std::byte> pbrFragment;
    [[nodiscard]] bool valid() const noexcept {
        return !vertex.empty() && !fragment.empty();
    }
};

struct alignas(16) GpuDashrShellConstants {
    std::array<float, 16> objectToClip{
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::array<float, 16> objectToWorld{
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::array<float, 4> cameraObjectAndHeightScale{0,0,0,0.04F};
    std::array<float, 4> cameraWorldAndDebug{0,0,0,0};
    std::array<float, 4> environmentParameters{0,0,0,0};
    std::array<float, 4> heightAndStep{0.5F,0.0F,0.02F,0.005F};
    std::array<float, 4> distortion{16.0F,0.05F,1.5F,1.0F};
    std::array<float, 4> minimumStepAndReserved{0.01F,0,0,0};
    std::array<float, 4> heightUvScaleOffset{1,1,0,0};
    std::array<float, 4> heightUvRotation{};
    std::array<std::uint32_t, 4> limits{192U,4U,8U,0U};
};
static_assert(sizeof(GpuDashrShellConstants) == 272U);

struct DashrShellRetiredBindGroups {
    rhi::FenceHandle fence{};
    std::vector<rhi::BindGroupHandle> groups;
};

struct DashrShellRendererResources {
    rhi::BufferHandle constants;
    rhi::BindGroupLayoutHandle constantsLayout;
    rhi::BindGroupLayoutHandle surfaceLayout;
    rhi::BindGroupLayoutHandle materialLayout;
    rhi::BindGroupLayoutHandle environmentLayout;
    rhi::BindGroupLayoutHandle shadowLayout;
    rhi::GraphicsPipelineHandle pipeline;
    rhi::GraphicsPipelineHandle pbrPipeline;
    std::size_t constantCapacity{};
    std::size_t constantStride{};
    std::vector<DashrShellRetiredBindGroups> retiredBindGroups;

    [[nodiscard]] bool valid() const noexcept {
        return constants && constantsLayout && surfaceLayout && pipeline &&
               constantCapacity >= sizeof(GpuDashrShellConstants) &&
               constantStride >= sizeof(GpuDashrShellConstants);
    }
    [[nodiscard]] bool pbr_valid() const noexcept {
        return valid() && materialLayout && environmentLayout && shadowLayout && pbrPipeline;
    }
};

struct DashrShellDraw {
    const DashrShellMeshMirror* shell{};
    const DashrAtlasResources* atlas{};
    rhi::TextureViewHandle heightView{};
    rhi::SamplerHandle heightSampler{};
    std::uint32_t shellSubmeshIndex{};
    std::array<float, 16> objectToClip{
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::array<float, 16> objectToWorld{
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    Float3 cameraObjectPosition{};
    Float3 cameraWorldPosition{};
    std::array<float, 4> environmentParameters{};
    Float2 heightUvScale{1.0F,1.0F};
    Float2 heightUvOffset{};
    float heightUvRotationRadians{};
    DashrSurfaceSettings settings{};
    std::uint32_t debugMode{};

    // Optional production shading inputs. When usePbr is false the diagnostic
    // shell shader is used and these can be null.
    bool usePbr{};
    const MainMaterialDescriptor* material{};
    const EnvironmentLightingGpuResources* lighting{};
    const CascadedShadowAtlasResources* shadows{};
};

struct DashrShellFrameDesc {
    rhi::TextureHandle colorTarget{};
    rhi::TextureHandle depthTarget{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::span<const DashrShellDraw> draws;
};

struct DashrShellFrameStats {
    std::uint64_t draws{};
    std::uint64_t pbrDraws{};
    std::uint64_t diagnosticDraws{};
    std::uint64_t shellTriangles{};
    std::uint64_t transientBindGroups{};
    std::uint64_t constantRanges{};
};

[[nodiscard]] bool create_dashr_shell_renderer(
    rhi::IDevice& device,
    const DashrShellShaderBytecode& bytecode,
    rhi::TextureFormat colorFormat,
    std::size_t maximumConstantBytes,
    DashrShellRendererResources& resources,
    std::string* error = nullptr);

[[nodiscard]] bool destroy_dashr_shell_renderer(
    rhi::IDevice& device,
    DashrShellRendererResources& resources,
    std::string* error = nullptr);

[[nodiscard]] bool record_dashr_shell_frame(
    rhi::IDevice& device,
    DashrShellRendererResources& renderer,
    const DashrShellFrameDesc& frame,
    DashrShellFrameStats& stats,
    rhi::FenceHandle* fence = nullptr,
    std::string* error = nullptr);

} // namespace dve::render
