#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/rhi/device.hpp"
#include "dve/sprite2d.hpp"

namespace dve::render {

struct SpriteRhiShaderBytecode {
    std::vector<std::byte> vertex;
    std::vector<std::byte> fragment;
    // Optional indexed-palette fragment module. When present, indexed sprite batches bind a
    // second group containing a 256-entry packed RGBA8 palette buffer.
    std::vector<std::byte> indexedFragment;

    [[nodiscard]] bool valid() const noexcept {
        return !vertex.empty() && !fragment.empty();
    }
    [[nodiscard]] bool indexed_valid() const noexcept {
        return !vertex.empty() && !indexedFragment.empty();
    }
};

// Loads the canonical Vulkan sprite shader modules from a directory containing
// sprite_2d.vert.spv and sprite_2d.frag.spv. Files are bounded and validated
// before they reach the RHI.
[[nodiscard]] bool load_sprite_spirv_shaders(
    const std::filesystem::path& directory,
    SpriteRhiShaderBytecode& bytecode,
    std::string* error = nullptr);

// Loads sprite_2d_indexed.frag.spv into an existing shader bundle. This is separate from the
// legacy loader so RGBA-only deployments remain source and binary compatible.
[[nodiscard]] bool load_sprite_indexed_spirv_shader(
    const std::filesystem::path& directory,
    SpriteRhiShaderBytecode& bytecode,
    std::string* error = nullptr);

// The sprite vertex shader consumes NDC position, atlas UV, and linear RGBA color in this order.
struct GpuSpriteVertex {
    std::array<float, 3> positionNdc{};
    std::array<float, 2> uv{};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
};
static_assert(sizeof(GpuSpriteVertex) == 36U);

[[nodiscard]] GpuSpriteVertex project_sprite_vertex(
    const SpriteVertex& source,
    GameplayPlane2D plane,
    const PixelPresentationConfig& presentation,
    Float3 cameraOrigin,
    SpriteVec2 logicalCameraCenter) noexcept;

struct SpriteRhiFrameDesc {
    rhi::TextureHandle colorTarget;
    PixelPresentationConfig presentation{};
    std::uint32_t outputWidth{};
    std::uint32_t outputHeight{};
    std::span<const SpriteDrawItem> items;
    std::span<const SpriteBatch> batches;
    std::span<const SpritePalettePacket> palettePackets;
    Float3 cameraOrigin{};
    SpriteVec2 logicalCameraCenter{}; // {0,0} selects the center of the logical resolution.
    rhi::ResourceState colorBefore{rhi::ResourceState::Undefined};
    rhi::ResourceState colorAfter{rhi::ResourceState::ShaderRead};
    std::array<float, 4> letterboxColor{0.0F, 0.0F, 0.0F, 1.0F};
    bool clear{true};
};

struct SpriteRhiFrameStats {
    PixelPresentationLayout layout{};
    std::uint64_t items{};
    std::uint64_t batches{};
    std::uint64_t drawCalls{};
    std::uint64_t triangles{};
    std::uint64_t fallbackTextureBatches{};
    std::uint64_t indexedPaletteBatches{};
    std::uint64_t palettePacketUploads{};
    std::uint64_t uploadedVertexBytes{};
    std::uint64_t uploadedIndexBytes{};
};

class SpriteRhiRenderer {
public:
    explicit SpriteRhiRenderer(rhi::IDevice& device) noexcept : device_(device) {}
    ~SpriteRhiRenderer();

    SpriteRhiRenderer(const SpriteRhiRenderer&) = delete;
    SpriteRhiRenderer& operator=(const SpriteRhiRenderer&) = delete;

    [[nodiscard]] bool initialize(
        const SpriteRhiShaderBytecode& bytecode,
        rhi::TextureFormat colorFormat = rhi::TextureFormat::RGBA8Unorm,
        std::string* error = nullptr);
    [[nodiscard]] bool shutdown(std::string* error = nullptr) noexcept;
    [[nodiscard]] bool initialized() const noexcept { return static_cast<bool>(textureLayout_); }
    [[nodiscard]] bool indexed_palette_supported() const noexcept {
        return static_cast<bool>(paletteLayout_) && static_cast<bool>(indexedPipelines_[0]);
    }

    // The renderer borrows the texture view. Call unregister_texture before destroying that view.
    [[nodiscard]] bool register_texture(
        std::string textureAsset, rhi::TextureViewHandle view,
        std::string* error = nullptr);
    [[nodiscard]] bool unregister_texture(
        std::string_view textureAsset, std::string* error = nullptr) noexcept;
    [[nodiscard]] bool has_texture(std::string_view textureAsset) const noexcept;

    [[nodiscard]] bool record_frame(
        const SpriteRhiFrameDesc& frame,
        SpriteRhiFrameStats& stats,
        rhi::FenceHandle* fence = nullptr,
        std::string* error = nullptr);

private:
    struct TextureGroups {
        rhi::TextureViewHandle view;
        rhi::BindGroupHandle nearest;
        rhi::BindGroupHandle linear;
    };

    struct PaletteResource {
        rhi::BufferHandle buffer;
        rhi::BindGroupHandle group;
        std::uint64_t paletteContentHash{};
        std::uint64_t resolvedColorHash{};
        std::size_t colorCount{};
    };

    [[nodiscard]] bool create_groups(
        rhi::TextureViewHandle view, TextureGroups& groups, std::string* error);
    [[nodiscard]] bool ensure_buffers(
        std::size_t vertexBytes, std::size_t indexBytes, std::string* error);
    [[nodiscard]] bool ensure_palette_resource(
        const SpritePalettePacket& packet, bool& uploaded, std::string* error);
    [[nodiscard]] rhi::GraphicsPipelineHandle pipeline(
        SpriteBlendMode mode, bool indexed) const noexcept;

    rhi::IDevice& device_;
    rhi::TextureFormat colorFormat_{rhi::TextureFormat::RGBA8Unorm};
    rhi::BindGroupLayoutHandle textureLayout_;
    rhi::BindGroupLayoutHandle paletteLayout_;
    rhi::SamplerHandle nearestSampler_;
    rhi::SamplerHandle linearSampler_;
    rhi::TextureHandle fallbackTexture_;
    rhi::TextureViewHandle fallbackView_;
    TextureGroups fallbackGroups_;
    std::array<rhi::GraphicsPipelineHandle, 4> pipelines_{};
    std::array<rhi::GraphicsPipelineHandle, 4> indexedPipelines_{};
    rhi::BufferHandle vertexBuffer_;
    rhi::BufferHandle indexBuffer_;
    std::size_t vertexCapacity_{};
    std::size_t indexCapacity_{};
    std::map<std::string, TextureGroups, std::less<>> textures_;
    std::map<std::pair<std::string, std::uint64_t>, PaletteResource> paletteResources_;
};

} // namespace dve::render
