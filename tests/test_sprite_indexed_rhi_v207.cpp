#include "dve/render/sprite_renderer.hpp"
#include "dve/rhi/null_device.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

SpriteDrawItem indexed_item() {
    SpriteDrawItem item;
    item.owner = 1U;
    item.asset = 1U;
    item.frame = 0U;
    item.textureAsset = "hero_indices.png";
    item.materialId = 7U;
    item.paletteBank = 0U;
    item.paletteAsset = "hero.dvepalette";
    item.paletteStateHash = 12345U;
    item.palettePacket = 0U;
    item.sampling = SpriteSampling::Nearest;
    item.blendMode = SpriteBlendMode::Alpha;
    item.vertices[0].position = {-0.5F, -0.5F, 0.0F};
    item.vertices[1].position = {0.5F, -0.5F, 0.0F};
    item.vertices[2].position = {0.5F, 0.5F, 0.0F};
    item.vertices[3].position = {-0.5F, 0.5F, 0.0F};
    item.vertices[0].uv = {0.0F, 1.0F};
    item.vertices[1].uv = {1.0F, 1.0F};
    item.vertices[2].uv = {1.0F, 0.0F};
    item.vertices[3].uv = {0.0F, 0.0F};
    return item;
}

SpriteBatch indexed_batch(const SpriteDrawItem& item) {
    SpriteBatch batch;
    batch.textureAsset = item.textureAsset;
    batch.materialId = item.materialId;
    batch.paletteBank = item.paletteBank;
    batch.sampling = item.sampling;
    batch.blendMode = item.blendMode;
    batch.firstItem = 0U;
    batch.itemCount = 1U;
    batch.paletteAsset = item.paletteAsset;
    batch.paletteStateHash = item.paletteStateHash;
    batch.palettePacket = item.palettePacket;
    return batch;
}

void test_indexed_null_rhi_pipeline() {
    rhi::NullDevice device;
    render::SpriteRhiRenderer renderer(device);
    render::SpriteRhiShaderBytecode shaders;
    shaders.vertex = {std::byte{1}};
    shaders.fragment = {std::byte{2}};
    shaders.indexedFragment = {std::byte{3}};
    std::string error;
    require(renderer.initialize(shaders, rhi::TextureFormat::RGBA8Unorm, &error), error);
    require(renderer.indexed_palette_supported(), "indexed sprite pipeline was not created");

    rhi::TextureDesc atlasDesc;
    atlasDesc.width = 4U;
    atlasDesc.height = 4U;
    atlasDesc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDestination;
    atlasDesc.initialState = rhi::ResourceState::ShaderRead;
    const auto atlas = device.create_texture(atlasDesc, &error);
    require(static_cast<bool>(atlas), error);
    rhi::TextureViewDesc viewDesc;
    viewDesc.texture = atlas;
    const auto view = device.create_texture_view(viewDesc, &error);
    require(static_cast<bool>(view), error);
    require(renderer.register_texture("hero_indices.png", view, &error), error);

    rhi::TextureDesc targetDesc;
    targetDesc.width = 320U;
    targetDesc.height = 180U;
    targetDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySource;
    const auto target = device.create_texture(targetDesc, &error);
    require(static_cast<bool>(target), error);

    const SpriteDrawItem item = indexed_item();
    const SpriteBatch batch = indexed_batch(item);
    SpritePalettePacket packet;
    packet.paletteAsset = item.paletteAsset;
    packet.bank = 0U;
    packet.paletteContentHash = 99U;
    packet.stateHash = item.paletteStateHash;
    packet.colors = {{0U, 0U, 0U, 0U}, {255U, 0U, 0U, 255U},
                     {0U, 255U, 0U, 255U}, {0U, 0U, 255U, 255U}};

    render::SpriteRhiFrameDesc frame;
    frame.colorTarget = target;
    frame.presentation = {320U, 180U, PixelScaleMode::IntegerFit, true, 16.0F};
    frame.outputWidth = 320U;
    frame.outputHeight = 180U;
    frame.items = std::span<const SpriteDrawItem>(&item, 1U);
    frame.batches = std::span<const SpriteBatch>(&batch, 1U);
    frame.palettePackets = std::span<const SpritePalettePacket>(&packet, 1U);
    render::SpriteRhiFrameStats stats;
    require(renderer.record_frame(frame, stats, nullptr, &error), error);
    require(stats.indexedPaletteBatches == 1U && stats.palettePacketUploads == 1U &&
            stats.drawCalls == 1U,
            "indexed sprite frame did not bind and upload its palette packet");
    frame.colorBefore = rhi::ResourceState::ShaderRead;
    require(renderer.record_frame(frame, stats, nullptr, &error), error);
    require(stats.palettePacketUploads == 0U && stats.indexedPaletteBatches == 1U,
            "unchanged indexed palette packet was uploaded again");

    SpritePalettePacket collision = packet;
    collision.colors[1U] = {12U, 34U, 56U, 255U};
    frame.palettePackets = std::span<const SpritePalettePacket>(&collision, 1U);
    error.clear();
    require(!renderer.record_frame(frame, stats, nullptr, &error) &&
            error.find("state hash collision") != std::string::npos,
            "indexed palette cache accepted different colors under the same state hash");

    frame.palettePackets = {};
    error.clear();
    require(!renderer.record_frame(frame, stats, nullptr, &error) &&
            error.find("missing palette packet") != std::string::npos,
            "indexed batch accepted a missing palette packet");

    require(renderer.shutdown(&error), error);
    require(device.destroy_texture_view(view, &error), error);
    require(device.destroy_texture(atlas, &error), error);
    require(device.destroy_texture(target, &error), error);
}

} // namespace

int main() {
    try {
        test_indexed_null_rhi_pipeline();
        std::cout << "dve_v207_sprite_indexed_rhi_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v207_sprite_indexed_rhi_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
