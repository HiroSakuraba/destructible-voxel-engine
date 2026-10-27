// T1 Vulkan contract: native BC/ETC2/EAC/ASTC upload and byte-exact readback on a real device.
// Skips when no Vulkan device exists unless DVE_REQUIRE_VULKAN is set. A format is only
// exercised when the device reports it sampled; unsupported formats must report no capability
// and fail explicit upload instead of being replaced.
#include "dve_test_skip.hpp"
#include "dve/compressed_texture.hpp"
#include "dve/rhi/vulkan_device.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dve;
namespace {
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
}

int main() {
    try {
        rhi::VulkanDevice device;
        if (device.status() != rhi::DeviceStatus::Ready) {
            if (std::getenv("DVE_REQUIRE_VULKAN") != nullptr)
                throw std::runtime_error("required Vulkan device unavailable: " +
                                         std::string(device.device_loss_reason()));
            return dve::testing::skip(std::string("Vulkan T1 format test: ") +
                                      std::string(device.device_loss_reason()));
        }
        using F = rhi::TextureFormat;
        constexpr std::array formats{
            F::BC4RUnorm, F::BC6HRGBUfloat, F::BC6HRGBSfloat, F::BC7RGBAUnorm, F::BC7RGBASrgb,
            F::ETC2RGBUnorm, F::ETC2RGBSrgb, F::ETC2RGBAUnorm, F::ETC2RGBASrgb,
            F::EACR11Unorm, F::EACRG11Unorm,
            F::ASTC4x4RGBAUnorm, F::ASTC4x4RGBASrgb, F::ASTC6x6RGBAUnorm, F::ASTC6x6RGBASrgb,
            F::ASTC8x8RGBAUnorm, F::ASTC8x8RGBASrgb};
        std::size_t tested = 0;
        for (const F format : formats) {
            const auto caps = device.texture_format_capabilities(format);
            CompressedTextureAsset asset;
            asset.format = format;
            // Two mips with a non-multiple-of-block extent exercise edge blocks and mip offsets.
            std::uint32_t w = 19U, h = 11U;
            for (std::uint32_t mip = 0; mip < 2U; ++mip) {
                CompressedTextureMip data{w, h, {}};
                data.blocks.resize(rhi::texture_row_bytes(format, w) * rhi::texture_rows(format, h));
                for (std::size_t i = 0; i < data.blocks.size(); ++i)
                    data.blocks[i] = static_cast<std::byte>((i * 29U + mip * 7U +
                        static_cast<std::size_t>(format) * 13U) & 255U);
                asset.mips.push_back(std::move(data));
                w = std::max(1U, w / 2U); h = std::max(1U, h / 2U);
            }
            std::string error;
            CompressedTextureResource resource;
            if (!caps.sampled) {
                require(!upload_compressed_texture(device, asset, resource, &error),
                        "unsupported T1 format unexpectedly uploaded");
                require(!resource.texture && !resource.view, "failed T1 upload leaked a resource");
                continue;
            }
            require(upload_compressed_texture(device, asset, resource, &error),
                    "T1 Vulkan upload failed: " + error);
            destroy_compressed_texture(device, resource);

            rhi::TextureDesc desc;
            desc.format = format; desc.width = 19U; desc.height = 11U; desc.mipLevels = 2U;
            desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDestination |
                         rhi::TextureUsage::CopySource;
            const auto texture = device.create_texture(desc, &error);
            require(static_cast<bool>(texture), "T1 Vulkan texture create failed: " + error);
            for (std::uint32_t mip = 0; mip < 2U; ++mip) {
                const auto& source = asset.mips[mip];
                const auto pitch = rhi::texture_row_bytes(format, source.width);
                require(device.write_texture(texture, mip, 0U, source.blocks, pitch, &error),
                        "T1 Vulkan write failed: " + error);
                std::vector<std::byte> readback(source.blocks.size());
                require(device.read_texture(texture, mip, 0U, readback, pitch, &error),
                        "T1 Vulkan readback failed: " + error);
                require(readback == source.blocks, "T1 Vulkan compressed block bytes changed");
            }
            require(device.destroy_texture(texture, &error), "T1 Vulkan destroy failed");
            ++tested;
        }
        std::cout << "Vulkan T1 formats round-tripped: " << tested << '/' << formats.size() << '\n';
        if (std::getenv("DVE_REQUIRE_VULKAN") != nullptr)
            require(tested >= 1U, "required Vulkan device exposes no T1 formats");
        if (tested == 0U) dve::testing::note_skip("Vulkan T1 format test: the device exposes no T1 formats");
        return dve::testing::exit_code(false);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
