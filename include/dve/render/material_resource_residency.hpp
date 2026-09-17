#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "dve/polygon_asset.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

struct MaterialResourceResidencyStats {
    std::uint64_t assetRetainCalls{};
    std::uint64_t assetReleaseCalls{};
    std::uint64_t assetEvictions{};
    std::uint64_t imagesUploaded{};
    std::uint64_t imageCacheHits{};
    std::uint64_t samplersCreated{};
    std::uint64_t samplerCacheHits{};
    std::size_t retainedAssetCount{};
    std::size_t assetReferenceCount{};
    std::size_t imageResourceCount{};
    std::size_t samplerResourceCount{};
};

// Shared owner for authored polygon-material images and samplers. Descriptor tables retain an
// asset once while any of their bind groups reference that asset. GPU resources are destroyed only
// after the final table releases the asset, preventing main/shadow duplication and premature frees.
class MaterialResourceResidency {
public:
    explicit MaterialResourceResidency(rhi::IDevice& device) : device_(device) {}
    ~MaterialResourceResidency();
    MaterialResourceResidency(const MaterialResourceResidency&) = delete;
    MaterialResourceResidency& operator=(const MaterialResourceResidency&) = delete;

    bool retain_asset(std::uint64_t assetContentHash, std::string* error = nullptr);
    bool release_asset(std::uint64_t assetContentHash, std::string* error = nullptr);
    bool ensure_image(const CookedPolygonAsset& asset, std::uint32_t imageIndex,
                      rhi::TextureViewHandle& view, std::string* error = nullptr);
    bool ensure_sampler(const CookedPolygonAsset& asset,
                        std::optional<std::uint32_t> samplerIndex,
                        rhi::SamplerHandle& sampler, std::string* error = nullptr);
    [[nodiscard]] bool is_retained(std::uint64_t assetContentHash) const noexcept;
    [[nodiscard]] std::uint32_t reference_count(std::uint64_t assetContentHash) const noexcept;
    [[nodiscard]] MaterialResourceResidencyStats stats() const noexcept;
    void clear() noexcept;

private:
    struct ImageKey {
        std::uint64_t assetHash{};
        std::uint32_t imageIndex{};
        friend bool operator==(const ImageKey&, const ImageKey&) = default;
    };
    struct SamplerKey {
        std::uint64_t assetHash{};
        std::uint32_t encodedIndex{};
        friend bool operator==(const SamplerKey&, const SamplerKey&) = default;
    };
    struct KeyHash {
        template <class Key>
        std::size_t operator()(const Key& key) const noexcept {
            std::uint32_t index{};
            if constexpr (requires { key.imageIndex; }) index = key.imageIndex;
            else index = key.encodedIndex;
            std::uint64_t value = key.assetHash ^ (static_cast<std::uint64_t>(index) << 32U);
            value ^= value >> 33U; value *= 0xff51afd7ed558ccdULL;
            value ^= value >> 33U; value *= 0xc4ceb9fe1a85ec53ULL;
            value ^= value >> 33U;
            return static_cast<std::size_t>(value);
        }
    };
    struct ImageResource {
        rhi::TextureHandle texture;
        rhi::TextureViewHandle view;
    };

    bool destroy_asset_resources(std::uint64_t assetContentHash,
                                 std::string* error) noexcept;

    rhi::IDevice& device_;
    std::unordered_map<std::uint64_t, std::uint32_t> assetReferences_;
    std::unordered_map<ImageKey, ImageResource, KeyHash> images_;
    std::unordered_map<SamplerKey, rhi::SamplerHandle, KeyHash> samplers_;
    MaterialResourceResidencyStats stats_{};
};

} // namespace dve::render
