#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "dve/rhi/device.hpp"
#include "dve/runtime_text3d_world.hpp"
#include "dve/text3d.hpp"
#include "dve/transform.hpp"

namespace dve::render {

enum class Text3DPassKind : std::uint8_t {
    BackFaces,
    ExtrusionSides,
    FrontFaces,
    SelectionOverlay,
};

struct Text3DGpuAsset {
    std::uint64_t assetId{};
    std::uint64_t contentHash{};
    rhi::TextureHandle curveTexture;
    rhi::TextureViewHandle curveView;
    rhi::TextureHandle bandTexture;
    rhi::TextureViewHandle bandView;
    rhi::SamplerHandle atlasSampler;
    rhi::BufferHandle faceVertexBuffer;
    rhi::BufferHandle faceIndexBuffer;
    rhi::BufferHandle sideVertexBuffer;
    rhi::BufferHandle sideIndexBuffer;
    rhi::BindGroupLayoutHandle atlasLayout;
    rhi::BindGroupHandle atlasGroup;
    std::uint32_t curveWidth{};
    std::uint32_t curveHeight{};
    std::uint32_t bandWidth{};
    std::uint32_t bandHeight{};
    std::uint32_t glyphCount{};
    std::uint32_t faceIndexCount{};
    std::uint32_t sideIndexCount{};
    std::size_t residentBytes{};
};

struct Text3DGpuCacheStats {
    std::uint64_t uploads{};
    std::uint64_t replacements{};
    std::uint64_t unchanged{};
    std::uint64_t failures{};
    std::uint64_t removals{};
    std::size_t residentAssets{};
    std::size_t residentBytes{};
};

// Owns the immutable curve/band atlases and face/side geometry for cooked .dtext assets.
// Upload is transactional: an existing resident asset remains usable until every replacement
// resource and bind group has been created successfully.
class Text3DGpuCache {
public:
    explicit Text3DGpuCache(rhi::IDevice& device) : device_(device) {}
    ~Text3DGpuCache();
    Text3DGpuCache(const Text3DGpuCache&) = delete;
    Text3DGpuCache& operator=(const Text3DGpuCache&) = delete;

    bool upload(const CookedText3DAsset& asset, std::string* error = nullptr);
    [[nodiscard]] const Text3DGpuAsset* find(std::uint64_t assetId) const noexcept;
    bool remove(std::uint64_t assetId, std::string* error = nullptr);
    void clear() noexcept;
    [[nodiscard]] Text3DGpuCacheStats stats() const noexcept;

private:
    bool destroy(Text3DGpuAsset& asset, std::string* error) noexcept;
    rhi::IDevice& device_;
    std::unordered_map<std::uint64_t, Text3DGpuAsset> assets_;
    Text3DGpuCacheStats stats_{};
};

struct Text3DRenderInstance {
    std::uint64_t objectId{};
    std::uint64_t assetId{};
    RigidTransform transform{};
    bool visible{true};
    bool selected{};
    bool castShadows{true};
    bool receiveGlobalIllumination{true};
};

struct Text3DDrawPacket {
    Text3DPassKind pass{Text3DPassKind::FrontFaces};
    std::uint64_t objectId{};
    std::uint64_t assetId{};
    RigidTransform transform{};
    const Text3DGpuAsset* gpu{};
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
    std::int32_t vertexOffset{};
    std::uint32_t materialId{};
    bool selected{};
};

struct Text3DFramePlan {
    std::vector<Text3DDrawPacket> packets;
    std::uint64_t submittedInstances{};
    std::uint64_t missingAssets{};
    std::uint64_t faceDraws{};
    std::uint64_t sideDraws{};
    std::uint64_t selectionDraws{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

[[nodiscard]] Text3DFramePlan make_text3d_frame_plan(
    std::span<const Text3DRenderInstance> instances,
    const Text3DGpuCache& cache,
    std::span<const CookedText3DAsset> assets);

// Converts a live runtime text snapshot into the same immutable frame plan used by editor and
// direct callers. Assets must already be resident in the cache; missing residents are reported in
// the plan rather than uploaded implicitly on the render thread.
[[nodiscard]] Text3DFramePlan make_text3d_frame_plan(
    const RuntimeText3DSceneSnapshot& snapshot,
    const Text3DGpuCache& cache);

struct Text3DObjectBinding {
    std::uint64_t objectId{};
    rhi::BindGroupHandle constantsGroup;
};

struct Text3DRenderPipelines {
    rhi::GraphicsPipelineHandle backFaces;
    rhi::GraphicsPipelineHandle extrusionSides;
    rhi::GraphicsPipelineHandle frontFaces;
    rhi::GraphicsPipelineHandle selectionOverlay;
};

// Records the analytic Slug face draws for one pass. A caller-owned constants bind group may be
// supplied at group 0. The resident curve/band atlas is bound at group 1. Front and back glyph
// quads are deliberately emitted as separate six-index draws because they are interleaved in the
// immutable face index buffer.
bool record_text3d_face_pass(
    rhi::IDevice& device,
    rhi::CommandListHandle commands,
    rhi::GraphicsPipelineHandle pipeline,
    const Text3DFramePlan& plan,
    Text3DPassKind pass,
    rhi::BindGroupHandle constantsGroup = {},
    std::string* error = nullptr);

// Multi-object overload. Each packet binds the constants group associated with its object before
// drawing, allowing the group to carry the object transform, camera MVP, object/material IDs,
// motion-vector history, and any backend-specific lighting data.
bool record_text3d_bound_face_pass(
    rhi::IDevice& device,
    rhi::CommandListHandle commands,
    rhi::GraphicsPipelineHandle pipeline,
    const Text3DFramePlan& plan,
    Text3DPassKind pass,
    std::span<const Text3DObjectBinding> objectBindings,
    std::string* error = nullptr);

bool record_text3d_side_pass(
    rhi::IDevice& device,
    rhi::CommandListHandle commands,
    rhi::GraphicsPipelineHandle pipeline,
    const Text3DFramePlan& plan,
    std::span<const Text3DObjectBinding> objectBindings = {},
    std::string* error = nullptr);

// Records the complete production ordering into an already-open render pass: back faces, PBR
// extrusion sides, front faces, then the optional selection overlay.
bool record_text3d_frame(
    rhi::IDevice& device,
    rhi::CommandListHandle commands,
    const Text3DRenderPipelines& pipelines,
    const Text3DFramePlan& plan,
    std::span<const Text3DObjectBinding> objectBindings = {},
    std::string* error = nullptr);

} // namespace dve::render
