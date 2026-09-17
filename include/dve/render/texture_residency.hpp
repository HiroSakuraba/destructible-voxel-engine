#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "dve/render/polygon_renderer.hpp"
#include "dve/rhi/device.hpp"

namespace dve::render {

enum class TextureSemantic : std::uint8_t {
    BaseColor,
    MetallicRoughness,
    Normal,
    Emissive,
    Opacity,
    AmbientOcclusion,
};

enum class TextureCompressionPreference : std::uint8_t {
    Uncompressed,
    PreferBc7,
    PreferBc5ForNormals,
};

enum class TextureUploadMode : std::uint8_t {
    RejectDuplicate,
    // Used by editor hot reload. Identical content becomes a no-op; changed content is uploaded
    // first and replaces the resident texture only after the new view is fully usable.
    ReplaceIfChanged,
};

struct TextureUploadRequest {
    std::uint64_t assetId{};
    PolygonImage image;
    TextureSemantic semantic{TextureSemantic::BaseColor};
    TextureCompressionPreference compression{TextureCompressionPreference::Uncompressed};
    bool generateMips{true};
    TextureUploadMode mode{TextureUploadMode::RejectDuplicate};
};

struct ResidentTexture {
    rhi::TextureHandle texture;
    rhi::TextureViewHandle view;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t mipLevels{};
    std::size_t residentBytes{};
    std::uint64_t sourceHash{};
    TextureSemantic semantic{TextureSemantic::BaseColor};
    std::uint64_t lastUseSerial{};
    // Monotonic per asset id. Hot reload increments this only after the replacement view is
    // usable, allowing material descriptors to detect stale bindings without observing a
    // partially uploaded texture.
    std::uint64_t generation{1U};
};

struct TextureResidencyReference {
    std::uint64_t assetId{};
    std::uint64_t generation{};
    std::uint64_t sourceHash{};
};

struct TextureResidencyStats {
    std::uint64_t requestsQueued{};
    std::uint64_t uploadsCompleted{};
    std::uint64_t uploadsFailed{};
    std::uint64_t evictions{};
    std::uint64_t compressionFallbacks{};
    std::uint64_t duplicateRequests{};
    std::uint64_t unchangedRequests{};
    std::uint64_t pendingRequestsReplaced{};
    std::uint64_t residentTexturesReplaced{};
    std::uint64_t failedReplacementsPreserved{};
    std::size_t pendingBytes{};
    std::size_t residentBytes{};
    std::size_t budgetBytes{};
    std::size_t residentCount{};
    std::size_t pendingCount{};
};

// Generates semantically correct mip chains: color textures are filtered in linear light and
// normal maps are decoded, averaged, and renormalized rather than averaged as arbitrary bytes.
[[nodiscard]] PolygonTextureMipChain generate_semantic_texture_mips(
    const PolygonImage& image,
    TextureSemantic semantic);

// Control/upload-thread texture residency. enqueue() is thread-safe and performs no RHI work;
// pump() owns all device interaction and can be bounded per frame by upload bytes.
class TextureResidencyManager {
public:
    explicit TextureResidencyManager(rhi::IDevice& device,
                                     std::size_t budgetBytes = 256U * 1024U * 1024U);
    ~TextureResidencyManager();
    TextureResidencyManager(const TextureResidencyManager&) = delete;
    TextureResidencyManager& operator=(const TextureResidencyManager&) = delete;

    bool enqueue(TextureUploadRequest request, std::string* error = nullptr);
    std::size_t pump(std::size_t maximumUploadBytes, std::string* error = nullptr);
    [[nodiscard]] const ResidentTexture* find(std::uint64_t assetId) const noexcept;
    [[nodiscard]] std::optional<TextureResidencyReference> reference(std::uint64_t assetId) const noexcept;
    [[nodiscard]] bool reference_valid(const TextureResidencyReference& reference) const noexcept;
    bool touch(std::uint64_t assetId) noexcept;
    bool remove(std::uint64_t assetId, std::string* error = nullptr);
    void clear() noexcept;
    void set_budget(std::size_t bytes) noexcept;
    [[nodiscard]] TextureResidencyStats stats() const noexcept;

private:
    struct Pending;
    bool evict_until_fits(std::size_t incomingBytes, std::uint64_t replacingAssetId,
                          std::string* error);
    static std::uint64_t image_hash(const PolygonImage& image) noexcept;

    rhi::IDevice& device_;
    mutable std::mutex mutex_;
    std::deque<Pending> pending_;
    std::unordered_map<std::uint64_t, ResidentTexture> resident_;
    TextureResidencyStats stats_;
    std::uint64_t serial_{1};
};

} // namespace dve::render
