#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "dve/polygon_asset.hpp"
#include "dve/render/material_resource_residency.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

struct ShadowMaterialDescriptor {
    rhi::BindGroupHandle bindGroup;
    bool hasBaseColorTexture{};
    bool hasOpacityTexture{};
    std::uint64_t assetContentHash{};
    std::uint32_t materialIndex{};
};

struct ShadowMaterialDescriptorStats {
    std::uint64_t assetsPublished{};
    std::uint64_t materialDescriptorsCreated{};
    std::uint64_t descriptorCacheHits{};
    // Shared-residency counters. When paired with the main table these describe one common pool.
    std::uint64_t imagesUploaded{};
    std::uint64_t imageCacheHits{};
    std::uint64_t samplersCreated{};
    std::uint64_t samplerCacheHits{};
    std::uint64_t invalidations{};
    std::size_t materialDescriptorCount{};
    std::size_t retainedAssetCount{};
    std::size_t sharedAssetReferenceCount{};
    std::size_t imageResourceCount{};
    std::size_t samplerResourceCount{};
};

// Persistent descriptor table used by alpha-masked shadow casters. Table-local bind groups and the
// neutral fallback stay independent; authored images and samplers may share renderer-owned residency.
class ShadowMaterialDescriptorTable {
public:
    explicit ShadowMaterialDescriptorTable(rhi::IDevice& device);
    ShadowMaterialDescriptorTable(rhi::IDevice& device, MaterialResourceResidency& residency);
    ~ShadowMaterialDescriptorTable();
    ShadowMaterialDescriptorTable(const ShadowMaterialDescriptorTable&) = delete;
    ShadowMaterialDescriptorTable& operator=(const ShadowMaterialDescriptorTable&) = delete;

    bool initialize(std::string* error = nullptr);
    bool ensure_material(const CookedPolygonAsset& asset, std::uint32_t materialIndex,
                         std::string* error = nullptr);
    [[nodiscard]] const ShadowMaterialDescriptor* find(
        std::uint64_t assetContentHash, std::uint32_t materialIndex) const noexcept;
    [[nodiscard]] rhi::BindGroupLayoutHandle layout() const noexcept { return layout_; }
    [[nodiscard]] rhi::BindGroupHandle fallback_group() const noexcept { return fallbackGroup_; }
    [[nodiscard]] MaterialResourceResidency& residency() noexcept { return *residency_; }
    [[nodiscard]] const MaterialResourceResidency& residency() const noexcept { return *residency_; }
    [[nodiscard]] bool valid() const noexcept;
    bool invalidate_asset(std::uint64_t assetContentHash, std::string* error = nullptr);
    void clear() noexcept;
    [[nodiscard]] ShadowMaterialDescriptorStats stats() const noexcept;

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
    struct MaterialResource { ShadowMaterialDescriptor descriptor; };

    bool resolve_binding(const CookedPolygonAsset& asset, const PolygonTextureBinding& binding,
                         rhi::TextureViewHandle& view, rhi::SamplerHandle& sampler,
                         bool& present, std::string* error);
    bool retain_asset_once(std::uint64_t assetContentHash, bool& retainedNow,
                           std::string* error);
    void rollback_asset_retain(std::uint64_t assetContentHash, bool retainedNow) noexcept;

    rhi::IDevice& device_;
    std::unique_ptr<MaterialResourceResidency> ownedResidency_;
    MaterialResourceResidency* residency_{};
    rhi::BindGroupLayoutHandle layout_;
    rhi::TextureHandle fallbackTexture_;
    rhi::TextureViewHandle fallbackView_;
    rhi::SamplerHandle fallbackSampler_;
    rhi::BindGroupHandle fallbackGroup_;
    std::unordered_map<MaterialKey, MaterialResource, KeyHash> materials_;
    std::unordered_set<std::uint64_t> retainedAssets_;
    ShadowMaterialDescriptorStats stats_{};
};

} // namespace dve::render
