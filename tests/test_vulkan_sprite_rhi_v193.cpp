#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/render/sprite_renderer.hpp"
#include "dve/rhi/vulkan_device.hpp"

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

SpriteDrawItem centered_sprite() {
    SpriteDrawItem item;
    item.owner = 1U;
    item.asset = 1U;
    item.textureAsset = "validation-atlas";
    item.sampling = SpriteSampling::Nearest;
    item.blendMode = SpriteBlendMode::Alpha;
    item.plane = GameplayPlane2D::XY;
    constexpr std::array<Float3, 4> positions{{
        {-2.0F, -2.0F, 0.0F}, {2.0F, -2.0F, 0.0F},
        {2.0F, 2.0F, 0.0F}, {-2.0F, 2.0F, 0.0F},
    }};
    constexpr std::array<SpriteVec2, 4> uvs{{
        {0.0F, 1.0F}, {1.0F, 1.0F}, {1.0F, 0.0F}, {0.0F, 0.0F},
    }};
    for (std::size_t index = 0U; index < item.vertices.size(); ++index) {
        item.vertices[index].position = positions[index];
        item.vertices[index].uv = uvs[index];
        item.vertices[index].color = {1.0F, 0.5F, 1.0F, 1.0F};
    }
    return item;
}

} // namespace

int main() {
    try {
        rhi::VulkanDevice device;
        if (device.status() != rhi::DeviceStatus::Ready) {
            if (std::getenv("DVE_REQUIRE_VULKAN") != nullptr)
                throw std::runtime_error(std::string("required Vulkan device unavailable: ") +
                    std::string(device.device_loss_reason()));
            std::cout << "dve_v193_vulkan_sprite_tests: SKIP: "
                      << device.device_loss_reason() << '\n';
            return 0;
        }

        std::string error;
        render::SpriteRhiShaderBytecode bytecode;
        require(render::load_sprite_spirv_shaders(
                    std::filesystem::path(DVE_SOURCE_DIR) / "shaders/compiled", bytecode, &error),
                error);
        render::SpriteRhiRenderer renderer(device);
        require(renderer.initialize(bytecode, rhi::TextureFormat::RGBA8Unorm, &error), error);

        rhi::TextureDesc atlasDesc;
        atlasDesc.width = 2U;
        atlasDesc.height = 2U;
        atlasDesc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDestination;
        atlasDesc.initialState = rhi::ResourceState::ShaderRead;
        atlasDesc.debugName = "v1.93 solid sprite atlas";
        const rhi::TextureHandle atlas = device.create_texture(atlasDesc, &error);
        require(static_cast<bool>(atlas), error);
        constexpr std::array<std::byte, 16> texels{
            std::byte{64}, std::byte{128}, std::byte{192}, std::byte{255},
            std::byte{64}, std::byte{128}, std::byte{192}, std::byte{255},
            std::byte{64}, std::byte{128}, std::byte{192}, std::byte{255},
            std::byte{64}, std::byte{128}, std::byte{192}, std::byte{255},
        };
        require(device.write_texture(atlas, 0U, 0U, texels, 8U, &error), error);
        rhi::TextureViewDesc atlasViewDesc;
        atlasViewDesc.texture = atlas;
        atlasViewDesc.debugName = "v1.93 sprite atlas view";
        const rhi::TextureViewHandle atlasView = device.create_texture_view(atlasViewDesc, &error);
        require(static_cast<bool>(atlasView), error);
        require(renderer.register_texture("validation-atlas", atlasView, &error), error);

        constexpr std::uint32_t width = 320U;
        constexpr std::uint32_t height = 240U;
        rhi::TextureDesc targetDesc;
        targetDesc.width = width;
        targetDesc.height = height;
        targetDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySource;
        targetDesc.initialState = rhi::ResourceState::Undefined;
        targetDesc.debugName = "v1.93 sprite reference target";
        const rhi::TextureHandle target = device.create_texture(targetDesc, &error);
        require(static_cast<bool>(target), error);

        const SpriteDrawItem sprite = centered_sprite();
        const SpriteBatch batch{sprite.textureAsset, sprite.materialId, sprite.paletteBank,
            sprite.sampling, sprite.blendMode, 0U, 1U};
        render::SpriteRhiFrameDesc frame;
        frame.colorTarget = target;
        frame.presentation = {160U, 90U, PixelScaleMode::IntegerFit, true, 16.0F};
        frame.outputWidth = width;
        frame.outputHeight = height;
        frame.items = std::span<const SpriteDrawItem>(&sprite, 1U);
        frame.batches = std::span<const SpriteBatch>(&batch, 1U);
        frame.colorAfter = rhi::ResourceState::CopySource;
        render::SpriteRhiFrameStats stats;
        rhi::FenceHandle fence;
        require(renderer.record_frame(frame, stats, &fence, &error), error);
        require(fence && device.fence_complete(fence), "sprite validation fence did not complete");
        require(stats.layout.viewportX == 0 && stats.layout.viewportY == 30 &&
                stats.layout.viewportWidth == 320U && stats.layout.viewportHeight == 180U,
                "sprite validation viewport is not the expected integer-fit letterbox");

        std::vector<std::byte> pixels(static_cast<std::size_t>(width) * height * 4U);
        require(device.read_texture(target, 0U, 0U, pixels,
                    static_cast<std::size_t>(width) * 4U, &error), error);
        const auto channel = [&](std::uint32_t x, std::uint32_t y, std::uint32_t component) {
            return std::to_integer<std::uint8_t>(
                pixels[(static_cast<std::size_t>(y) * width + x) * 4U + component]);
        };
        require(channel(4U, 4U, 0U) <= 2U && channel(4U, 4U, 1U) <= 2U &&
                channel(4U, 4U, 2U) <= 2U,
                "sprite renderer did not preserve the black letterbox clear");
        require(channel(width / 2U, height / 2U, 0U) >= 62U &&
                channel(width / 2U, height / 2U, 0U) <= 66U &&
                channel(width / 2U, height / 2U, 1U) >= 62U &&
                channel(width / 2U, height / 2U, 1U) <= 66U &&
                channel(width / 2U, height / 2U, 2U) >= 190U,
                "sprite texture, vertex tint, or vertex-input contract differs from the CPU oracle");

        if (const char* output = std::getenv("DVE_VULKAN_SPRITE_OUTPUT")) {
            std::ofstream ppm(output, std::ios::binary | std::ios::trunc);
            require(ppm.good(), "could not open sprite reference image output");
            ppm << "P6\n" << width << ' ' << height << "\n255\n";
            for (std::uint32_t y = 0U; y < height; ++y)
                for (std::uint32_t x = 0U; x < width; ++x) {
                    const std::array<char, 3> rgb{
                        static_cast<char>(channel(x, y, 0U)),
                        static_cast<char>(channel(x, y, 1U)),
                        static_cast<char>(channel(x, y, 2U)),
                    };
                    ppm.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
                }
            require(ppm.good(), "could not write sprite reference image output");
        }

        require(renderer.unregister_texture("validation-atlas", &error), error);
        require(renderer.shutdown(&error), error);
        require(device.destroy_texture_view(atlasView, &error), error);
        require(device.destroy_texture(atlas, &error), error);
        require(device.destroy_texture(target, &error), error);
        std::cout << "dve_v193_vulkan_sprite_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v193_vulkan_sprite_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
