#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dve/polygon_asset.hpp"
#include "dve/render/cascaded_shadow_atlas.hpp"
#include "dve/render/dashr_live_instance.hpp"
#include "dve/render/dashr_shell.hpp"
#include "dve/render/environment_lighting_gpu.hpp"
#include "dve/render/mesh_rhi_mirror.hpp"
#include "dve/render/main_material_table.hpp"
#include "dve/render/material_resource_residency.hpp"
#include "dve/render/shadow_material_table.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

struct LiveEnvironmentShaderBytecode {
    std::vector<std::byte> skyboxVertex;
    std::vector<std::byte> skyboxFragment;
    std::vector<std::byte> materialVertex;
    std::vector<std::byte> materialFragment;
    std::vector<std::byte> shadowVertex;
    // Optional for opaque depth-only casters, required by the alpha-masked caster pipeline.
    std::vector<std::byte> shadowFragment;

    // Optional DASHR production path. Existing live rendering remains valid when
    // this packet is empty; a LivePolygonDraw that opts into DASHR requires it.
    DashrShellShaderBytecode dashrShell;

    [[nodiscard]] bool valid() const noexcept;
};

struct alignas(16) GpuLiveObjectConstants {
    std::array<float, 16> objectToWorld{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::uint32_t objectIdLow{};
    std::uint32_t objectIdHigh{};
    std::uint32_t materialIndex{};
    std::uint32_t flags{};
    float alphaCutoff{0.5F};
    float baseAlpha{1.0F};
    float normalScale{1.0F};
    float reserved{};
};
static_assert(sizeof(GpuLiveObjectConstants) == 96U);

struct alignas(16) GpuLiveCascadeConstants {
    std::array<float, 4> centerRadius{};
    std::array<float, 4> lightRight{};
    std::array<float, 4> lightUp{};
    std::array<float, 4> lightForward{};
    std::array<float, 4> depthAndAtlas{};
    std::array<float, 4> atlasScaleOffset{};
};
static_assert(sizeof(GpuLiveCascadeConstants) == 96U);

inline constexpr std::uint32_t kLiveObjectFlagAlphaMasked = 1U << 0U;
inline constexpr std::uint32_t kLiveObjectFlagAlphaTexturePresent = 1U << 1U;
inline constexpr std::uint32_t kLiveObjectFlagDoubleSided = 1U << 2U;
inline constexpr std::uint32_t kLiveObjectFlagBaseColorTexturePresent = 1U << 3U;
inline constexpr std::uint32_t kLiveObjectFlagOpacityTexturePresent = 1U << 4U;

struct LiveEnvironmentRetiredBindGroups {
    rhi::FenceHandle fence{};
    std::vector<rhi::BindGroupHandle> groups;
};

struct LiveEnvironmentRendererResources {
    rhi::BufferHandle frameConstants;
    rhi::BufferHandle objectConstants;
    rhi::BufferHandle cascadeConstants;
    rhi::BindGroupLayoutHandle frameLayout;
    rhi::BindGroupLayoutHandle objectLayout;
    rhi::BindGroupLayoutHandle cascadeLayout;
    rhi::BindGroupHandle frameGroup;
    rhi::GraphicsPipelineHandle skyboxPipeline;
    rhi::GraphicsPipelineHandle materialPipeline;
    rhi::GraphicsPipelineHandle shadowOpaquePipeline;
    rhi::GraphicsPipelineHandle shadowMaskedPipeline;
    rhi::BufferHandle skyboxVertices;
    rhi::BufferHandle skyboxIndices;
    std::size_t frameConstantCapacity{};
    std::size_t objectConstantCapacity{};
    std::size_t cascadeConstantCapacity{};
    std::size_t objectConstantStride{};
    std::size_t cascadeConstantStride{};
    std::unique_ptr<MaterialResourceResidency> materialResidency;
    std::unique_ptr<ShadowMaterialDescriptorTable> shadowMaterials;
    std::unique_ptr<MainMaterialDescriptorTable> mainMaterials;

    // One renderer/pipeline set is shared by all DASHR instances. Pose-dependent
    // surface/shell/atlas resources live on DashrLiveSurfaceInstance instead.
    std::unique_ptr<DashrShellRendererResources> dashrShellRenderer;

    // Ordinary live rendering previously destroyed transient range descriptors
    // immediately after submit. Retain them to the final submission fence just
    // like the DASHR path does.
    std::vector<LiveEnvironmentRetiredBindGroups> retiredBindGroups;
    rhi::FenceHandle lastSubmissionFence{};

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool dashr_valid() const noexcept {
        return dashrShellRenderer && dashrShellRenderer->pbr_valid() &&
               static_cast<bool>(dashrShellRenderer->shadowPipeline);
    }
};

struct LivePolygonDraw {
    const MeshRhiMirror* mirror{};
    const CookedPolygonAsset* asset{};
    std::uint32_t instanceCount{1U};
    std::array<float, 16> objectToWorld{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bool castsShadow{true};
    bool staticShadowCaster{false};

    // Optional pose-owned DASHR resources. Material selection is stored by the
    // instance, so ordinary submeshes in the same asset remain on the polygon path.
    DashrLiveSurfaceInstance* dashr{};
};

// Explicit camera packet used only by the DASHR shell path. The legacy frame
// constant blob remains opaque and unchanged for existing callers.
struct LiveDashrViewDesc {
    std::array<float, 16> worldToClip{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    Float3 cameraWorldPosition{};
    std::array<float, 4> environmentParameters{};
};

struct LiveEnvironmentFrameDesc {
    rhi::TextureHandle colorTarget;
    rhi::TextureHandle depthTarget;
    std::uint32_t width{};
    std::uint32_t height{};
    std::span<const std::byte> frameConstants;
    std::span<const LivePolygonDraw> polygonDraws;
    // Legacy shared dirty set. Used for a layer when its layer-specific set is empty.
    std::vector<std::uint32_t> dirtyCascades;
    std::vector<std::uint32_t> staticDirtyCascades;
    std::vector<std::uint32_t> dynamicDirtyCascades;
    // Static depth is retained in its own atlas when false. Dynamic depth is always cleared
    // in its selected regions before dynamic casters are redrawn.
    bool refreshStaticShadowCasters{true};
    bool drawSkybox{true};
    bool drawMaterials{true};
    bool drawShadowCasters{true};

    std::optional<LiveDashrViewDesc> dashrView;
};

struct LiveEnvironmentFrameStats {
    std::uint64_t skyboxDraws{};
    std::uint64_t materialDraws{};
    std::uint64_t shadowDraws{};
    std::uint64_t materialTriangles{};
    std::uint64_t shadowTriangles{};
    std::uint64_t alphaMaskedShadowDraws{};
    std::uint64_t alphaTextureFallbackDraws{};
    std::uint64_t texturedAlphaShadowDraws{};
    std::uint64_t baseColorAlphaShadowDraws{};
    std::uint64_t opacityTextureShadowDraws{};
    std::uint64_t persistentMaterialDescriptorHits{};
    std::uint64_t persistentMainMaterialDescriptorHits{};
    std::uint64_t mainMaterialDescriptorsBound{};
    std::uint64_t gpuMaterialRecordsBound{};
    std::uint64_t gpuMappingRecordsBound{};
    std::uint64_t staticShadowDraws{};
    std::uint64_t dynamicShadowDraws{};
    std::uint64_t staticShadowDrawsSkipped{};
    std::uint64_t shadowDepthRegionsCleared{};
    std::uint64_t staticAtlasPasses{};
    std::uint64_t dynamicAtlasPasses{};
    std::uint32_t staticCascadesRendered{};
    std::uint32_t dynamicCascadesRendered{};
    std::uint64_t objectConstantRanges{};
    std::uint64_t cascadeConstantRanges{};
    std::uint32_t cascadesRendered{};

    // DASHR-specific scheduling/recording diagnostics.
    std::uint64_t dashrSubmeshes{};
    std::uint64_t dashrMaterialDraws{};
    std::uint64_t dashrMaterialTriangles{};
    std::uint64_t dashrShadowDraws{};
    std::uint64_t dashrShadowTriangles{};
    std::uint64_t dashrShadowPasses{};
};

[[nodiscard]] bool create_live_environment_renderer(
    rhi::IDevice& device,
    const EnvironmentLightingGpuResources& lighting,
    const CascadedShadowAtlasResources& shadowAtlas,
    const LiveEnvironmentShaderBytecode& bytecode,
    std::size_t maximumFrameConstantBytes,
    rhi::TextureFormat colorFormat,
    LiveEnvironmentRendererResources& resources,
    std::string* error = nullptr);

[[nodiscard]] bool destroy_live_environment_renderer(
    rhi::IDevice& device,
    LiveEnvironmentRendererResources& resources,
    std::string* error = nullptr);

[[nodiscard]] bool record_live_environment_frame(
    rhi::IDevice& device,
    const EnvironmentLightingGpuResources& lighting,
    const CascadedShadowPlan& shadowPlan,
    const CascadedShadowAtlasResources& shadowAtlas,
    LiveEnvironmentRendererResources& renderer,
    const LiveEnvironmentFrameDesc& frame,
    LiveEnvironmentFrameStats& stats,
    rhi::FenceHandle* fence = nullptr,
    std::string* error = nullptr);

} // namespace dve::render
