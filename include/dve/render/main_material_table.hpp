#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "dve/gpu_material_buffer.hpp"
#include "dve/gpu_material_mapping.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/render/material_resource_residency.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

inline constexpr std::uint32_t kMainMaterialTextureChannelCount = 9U;

enum class MainMaterialTextureChannel : std::uint32_t {
    BaseColor = 0U,
    MetallicRoughness = 1U,
    Normal = 2U,
    Emissive = 3U,
    Opacity = 4U,
    Height = 5U,
    DetailBaseColor = 6U,
    DetailNormal = 7U,
    DetailRoughness = 8U,
};

struct MainMaterialDescriptor {
    rhi::BindGroupHandle bindGroup;
    rhi::BufferHandle materialRecordBuffer;
    rhi::BufferHandle mappingRecordBuffer;
    std::size_t materialRecordOffset{};
    std::size_t mappingRecordOffset{};
    std::uint32_t texturePresenceMask{};
    std::uint64_t assetContentHash{};
    std::uint32_t materialIndex{};
};

struct MainMaterialDescriptorStats {
    std::uint64_t assetsPublished{};
    std::uint64_t assetRecordBuffersCreated{};
    std::uint64_t materialRecordsUploaded{};
    std::uint64_t mappingRecordsUploaded{};
    std::uint64_t materialDescriptorsCreated{};
    std::uint64_t descriptorCacheHits{};
    // Shared-residency counters. When two tables use the same residency these values describe the
    // common pool, not additional resources owned by this table.
    std::uint64_t imagesUploaded{};
    std::uint64_t imageCacheHits{};
    std::uint64_t samplersCreated{};
    std::uint64_t samplerCacheHits{};
    std::uint64_t invalidations{};
    std::size_t assetResourceCount{};
    std::size_t materialDescriptorCount{};
    std::size_t retainedAssetCount{};
    std::size_t sharedAssetReferenceCount{};
    std::size_t imageResourceCount{};
    std::size_t samplerResourceCount{};
};

// Persistent descriptor table for the live polygon material pass. Material and mapping records,
// bind groups, and channel-specific fallback resources are table-local. Authored image/sampler
// residency may be shared with the shadow table through MaterialResourceResidency.
class MainMaterialDescriptorTable {
public:
    explicit MainMaterialDescriptorTable(rhi::IDevice& device);
    MainMaterialDescriptorTable(rhi::IDevice& device, MaterialResourceResidency& residency);
    ~MainMaterialDescriptorTable();
    MainMaterialDescriptorTable(const MainMaterialDescriptorTable&) = delete;
    MainMaterialDescriptorTable& operator=(const MainMaterialDescriptorTable&) = delete;

    bool initialize(rhi::TextureViewHandle staticShadowDepth,
                    rhi::TextureViewHandle dynamicShadowDepth,
                    rhi::SamplerHandle shadowComparisonSampler,
                    std::string* error = nullptr);
    bool ensure_material(const CookedPolygonAsset& asset, std::uint32_t materialIndex,
                         std::string* error = nullptr);
    [[nodiscard]] const MainMaterialDescriptor* find(
        std::uint64_t assetContentHash, std::uint32_t materialIndex) const noexcept;
    [[nodiscard]] rhi::BindGroupLayoutHandle layout() const noexcept { return layout_; }
    [[nodiscard]] rhi::BindGroupHandle fallback_group() const noexcept { return fallbackGroup_; }
    [[nodiscard]] MaterialResourceResidency& residency() noexcept { return *residency_; }
    [[nodiscard]] const MaterialResourceResidency& residency() const noexcept { return *residency_; }
    [[nodiscard]] bool valid() const noexcept;
    bool invalidate_asset(std::uint64_t assetContentHash, std::string* error = nullptr);
    void clear() noexcept;
    [[nodiscard]] MainMaterialDescriptorStats stats() const noexcept;

private:
    struct MaterialKey {
        std::uint64_t assetHash{};
        std::uint32_t materialIndex{};
        friend bool operator==(const MaterialKey&, const MaterialKey&) = default;
    };
    struct KeyHash {
        std::size_t operator()(const MaterialKey& key) const noexcept {
            std::uint64_t value = key.assetHash ^
                (static_cast<std::uint64_t>(key.materialIndex) << 32U);
            value ^= value >> 33U; value *= 0xff51afd7ed558ccdULL;
            value ^= value >> 33U; value *= 0xc4ceb9fe1a85ec53ULL;
            value ^= value >> 33U;
            return static_cast<std::size_t>(value);
        }
    };
    struct AssetResource {
        rhi::BufferHandle materialRecords;
        rhi::BufferHandle mappingRecords;
        std::size_t materialStride{};
        std::size_t mappingStride{};
        std::uint32_t materialCount{};
    };
    struct MaterialResource { MainMaterialDescriptor descriptor; };

    bool ensure_asset_records(const CookedPolygonAsset& asset, AssetResource*& resource,
                              std::string* error);
    bool resolve_binding(const CookedPolygonAsset& asset, const PolygonTextureBinding& binding,
                         MainMaterialTextureChannel channel, rhi::TextureViewHandle& view,
                         rhi::SamplerHandle& sampler, bool& present, std::string* error);
    bool retain_asset_once(std::uint64_t assetContentHash, bool& retainedNow,
                           std::string* error);
    void rollback_asset_retain(std::uint64_t assetContentHash, bool retainedNow) noexcept;

    rhi::IDevice& device_;
    std::unique_ptr<MaterialResourceResidency> ownedResidency_;
    MaterialResourceResidency* residency_{};
    rhi::BindGroupLayoutHandle layout_;
    rhi::BufferHandle fallbackMaterialBuffer_;
    rhi::BufferHandle fallbackMappingBuffer_;
    std::array<rhi::TextureHandle, kMainMaterialTextureChannelCount> fallbackTextures_{};
    std::array<rhi::TextureViewHandle, kMainMaterialTextureChannelCount> fallbackViews_{};
    rhi::SamplerHandle fallbackSampler_;
    // Borrowed from CascadedShadowAtlasResources for the renderer lifetime.
    rhi::TextureViewHandle staticShadowDepthView_;
    rhi::TextureViewHandle dynamicShadowDepthView_;
    rhi::SamplerHandle shadowComparisonSampler_;
    rhi::BindGroupHandle fallbackGroup_;
    std::unordered_map<std::uint64_t, AssetResource> assets_;
    std::unordered_map<MaterialKey, MaterialResource, KeyHash> materials_;
    std::unordered_set<std::uint64_t> retainedAssets_;
    MainMaterialDescriptorStats stats_{};
};

} // namespace dve::render
