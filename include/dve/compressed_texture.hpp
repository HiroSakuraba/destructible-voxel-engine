#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "dve/rhi/device.hpp"

namespace dve {

struct CompressedTextureMip {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::byte> blocks;
};

struct CompressedTextureAsset {
    std::string name;
    rhi::TextureFormat format{rhi::TextureFormat::BC1RGBAUnorm};
    std::vector<CompressedTextureMip> mips;
};

struct CompressedTextureResource {
    rhi::TextureHandle texture{};
    rhi::TextureViewHandle view{};
};

// DDS and KTX2 BC1/BC3/BC5, single 2D image only. Payload bytes remain compressed.
[[nodiscard]] bool parse_compressed_texture(std::span<const std::byte> bytes,
                                             std::string name,
                                             CompressedTextureAsset& output,
                                             std::string* error = nullptr);
[[nodiscard]] bool load_compressed_texture(const std::filesystem::path& path,
                                            CompressedTextureAsset& output,
                                            std::string* error = nullptr);
[[nodiscard]] bool upload_compressed_texture(rhi::IDevice& device,
                                              const CompressedTextureAsset& asset,
                                              CompressedTextureResource& output,
                                              std::string* error = nullptr);
void destroy_compressed_texture(rhi::IDevice& device, CompressedTextureResource& resource) noexcept;

} // namespace dve
