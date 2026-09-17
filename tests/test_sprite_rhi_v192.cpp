#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dve/render/sprite_renderer.hpp"
#include "dve/rhi/null_device.hpp"

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(float left, float right, float epsilon = 0.0001F) {
    return std::abs(left - right) <= epsilon;
}

SpriteDrawItem item(
    SpriteOwnerId owner, std::string texture, SpriteSampling sampling,
    SpriteBlendMode blend, GameplayPlane2D plane = GameplayPlane2D::XY) {
    SpriteDrawItem result;
    result.owner = owner;
    result.asset = 1U;
    result.frame = 0U;
    result.textureAsset = std::move(texture);
    result.materialId = 7U;
    result.paletteBank = 2U;
    result.sampling = sampling;
    result.blendMode = blend;
    result.plane = plane;
    const std::array<Float3, 4> positions = plane == GameplayPlane2D::XY
        ? std::array<Float3, 4>{{{-0.5F, -0.5F, 0.0F}, {0.5F, -0.5F, 0.0F},
                                 {0.5F, 0.5F, 0.0F}, {-0.5F, 0.5F, 0.0F}}}
        : std::array<Float3, 4>{{{-0.5F, 3.0F, -0.5F}, {0.5F, 3.0F, -0.5F},
                                 {0.5F, 3.0F, 0.5F}, {-0.5F, 3.0F, 0.5F}}};
    const std::array<SpriteVec2, 4> uvs{{{0.0F, 1.0F}, {1.0F, 1.0F},
                                         {1.0F, 0.0F}, {0.0F, 0.0F}}};
    for (std::size_t index = 0U; index < 4U; ++index) {
        result.vertices[index].position = positions[index];
        result.vertices[index].uv = uvs[index];
        result.vertices[index].color = {1.0F, 0.5F, 0.25F, 1.0F};
    }
    return result;
}

SpriteBatch batch(const SpriteDrawItem& source, std::size_t first, std::size_t count) {
    SpriteBatch result;
    result.textureAsset = source.textureAsset;
    result.materialId = source.materialId;
    result.paletteBank = source.paletteBank;
    result.sampling = source.sampling;
    result.blendMode = source.blendMode;
    result.firstItem = first;
    result.itemCount = count;
    result.paletteAsset = source.paletteAsset;
    result.paletteStateHash = source.paletteStateHash;
    result.palettePacket = source.palettePacket;
    return result;
}

void test_projection_contract() {
    PixelPresentationConfig presentation;
    presentation.logicalWidth = 320U;
    presentation.logicalHeight = 180U;
    presentation.pixelsPerWorldUnit = 16.0F;
    SpriteVertex source;
    source.position = {1.0F, 1.0F, 8.0F};
    source.uv = {0.25F, 0.75F};
    source.color = {1.0F, 0.5F, 0.25F, 1.0F};
    const auto xy = render::project_sprite_vertex(
        source, GameplayPlane2D::XY, presentation, {}, {160.0F, 90.0F});
    require(close(xy.positionNdc[0], 0.1F) && close(xy.positionNdc[1], 16.0F / 90.0F),
            "XY sprite projection did not preserve the logical pixel scale");
    const auto xz = render::project_sprite_vertex(
        source, GameplayPlane2D::XZ, presentation, {}, {160.0F, 90.0F});
    require(close(xz.positionNdc[0], 0.1F) && close(xz.positionNdc[1], 128.0F / 90.0F),
            "XZ sprite projection did not use world Z as screen vertical");
    require(close(xy.uv[0], 0.25F) && close(xy.color[2], 0.25F),
            "sprite GPU projection changed UV or color attributes");
}

void test_vertex_layout_validation() {
    rhi::GraphicsPipelineDesc desc;
    std::string error;
    desc.vertexBuffer = rhi::VertexBufferLayoutDesc{8U, false};
    desc.vertexAttributes = {{0U, rhi::VertexFormat::Float3, 0U}};
    require(!rhi::validate_vertex_input_layout(desc, &error),
            "vertex attribute outside its stride was accepted");
    desc.vertexBuffer = rhi::VertexBufferLayoutDesc{36U, false};
    desc.vertexAttributes = {
        {0U, rhi::VertexFormat::Float3, 0U},
        {1U, rhi::VertexFormat::Float2, 8U},
    };
    require(!rhi::validate_vertex_input_layout(desc, &error),
            "overlapping vertex attributes were accepted");
    desc.vertexAttributes = {
        {0U, rhi::VertexFormat::Float3, 0U},
        {1U, rhi::VertexFormat::Float2, 12U},
        {2U, rhi::VertexFormat::Float4, 20U},
    };
    require(rhi::validate_vertex_input_layout(desc, &error), error);
}

void test_production_shader_loading() {
    render::SpriteRhiShaderBytecode shaders;
    std::string error;
    require(render::load_sprite_spirv_shaders(
                std::filesystem::path(DVE_SOURCE_DIR) / "shaders/compiled", shaders, &error),
            error);
    require(shaders.vertex.size() >= 20U && shaders.fragment.size() >= 20U,
            "production sprite shader modules are unexpectedly small");
}

void test_null_rhi_sprite_frame() {
    rhi::NullDevice device;
    render::SpriteRhiRenderer renderer(device);
    render::SpriteRhiShaderBytecode shaders;
    shaders.vertex = {std::byte{1}};
    shaders.fragment = {std::byte{2}};
    std::string error;
    require(renderer.initialize(shaders, rhi::TextureFormat::RGBA8Unorm, &error), error);

    rhi::TextureDesc atlasDesc;
    atlasDesc.width = 16U;
    atlasDesc.height = 16U;
    atlasDesc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDestination;
    atlasDesc.initialState = rhi::ResourceState::ShaderRead;
    const rhi::TextureHandle atlas = device.create_texture(atlasDesc, &error);
    require(static_cast<bool>(atlas), error);
    rhi::TextureViewDesc atlasViewDesc;
    atlasViewDesc.texture = atlas;
    const rhi::TextureViewHandle atlasView = device.create_texture_view(atlasViewDesc, &error);
    require(static_cast<bool>(atlasView), error);
    require(renderer.register_texture("hero.png", atlasView, &error), error);
    require(!renderer.register_texture("hero.png", atlasView, &error),
            "duplicate sprite texture registration was accepted");

    rhi::TextureDesc targetDesc;
    targetDesc.width = 1366U;
    targetDesc.height = 768U;
    targetDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySource;
    targetDesc.initialState = rhi::ResourceState::Undefined;
    const rhi::TextureHandle target = device.create_texture(targetDesc, &error);
    require(static_cast<bool>(target), error);

    std::vector<SpriteDrawItem> items;
    items.push_back(item(1U, "hero.png", SpriteSampling::Nearest, SpriteBlendMode::Alpha));
    items.push_back(item(2U, "hero.png", SpriteSampling::Nearest, SpriteBlendMode::Alpha));
    items.push_back(item(3U, "missing.png", SpriteSampling::Linear, SpriteBlendMode::Additive,
                         GameplayPlane2D::XZ));
    items.push_back(item(4U, "hero.png", SpriteSampling::Nearest, SpriteBlendMode::Multiply));
    const std::vector<SpriteBatch> batches{
        batch(items[0], 0U, 2U), batch(items[2], 2U, 1U), batch(items[3], 3U, 1U)};

    render::SpriteRhiFrameDesc frame;
    frame.colorTarget = target;
    frame.presentation = {320U, 180U, PixelScaleMode::IntegerFit, true, 16.0F};
    frame.outputWidth = 1366U;
    frame.outputHeight = 768U;
    frame.items = items;
    frame.batches = batches;
    frame.letterboxColor = {0.02F, 0.04F, 0.06F, 1.0F};
    render::SpriteRhiFrameStats frameStats;
    rhi::FenceHandle fence;
    require(renderer.record_frame(frame, frameStats, &fence, &error), error);
    require(fence && device.fence_complete(fence), "sprite frame fence did not complete");
    require(frameStats.layout.viewportX == 43 && frameStats.layout.viewportY == 24 &&
            frameStats.layout.viewportWidth == 1280U && frameStats.layout.viewportHeight == 720U,
            "sprite RHI frame ignored logical integer presentation");
    require(frameStats.items == 4U && frameStats.batches == 3U &&
            frameStats.drawCalls == 3U && frameStats.triangles == 8U &&
            frameStats.fallbackTextureBatches == 1U,
            "sprite RHI frame statistics are incorrect");
    const rhi::DeviceStatistics deviceStats = device.statistics();
    require(deviceStats.renderPassesExecuted == 1U && deviceStats.indexedDrawsExecuted == 3U &&
            deviceStats.indexedTrianglesSubmitted == 8U && deviceStats.barriersExecuted == 2U,
            "Null RHI did not execute the sprite render contract");

    SpriteDrawItem indexed = item(
        5U, "hero.png", SpriteSampling::Nearest, SpriteBlendMode::Alpha);
    indexed.paletteAsset = "hero.dvepalette";
    indexed.paletteStateHash = 123U;
    indexed.palettePacket = 0U;
    const SpriteBatch indexedBatch = batch(indexed, 0U, 1U);
    frame.items = std::span<const SpriteDrawItem>(&indexed, 1U);
    frame.batches = std::span<const SpriteBatch>(&indexedBatch, 1U);
    error.clear();
    require(!renderer.record_frame(frame, frameStats, nullptr, &error) &&
                error.find("indexed sprite palette shader resources are unavailable") != std::string::npos,
            "RGBA-only sprite RHI did not reject indexed palette packets explicitly");

    SpriteBatch invalid = batches[0];
    invalid.itemCount = 1U;
    frame.items = items;
    frame.batches = std::span<const SpriteBatch>(&invalid, 1U);
    require(!renderer.record_frame(frame, frameStats, nullptr, &error),
            "incomplete sprite batch coverage was accepted");

    require(renderer.unregister_texture("hero.png", &error), error);
    require(!renderer.has_texture("hero.png"), "sprite texture unregister left a live entry");
    require(renderer.shutdown(&error), error);
    require(device.destroy_texture_view(atlasView, &error), error);
    require(device.destroy_texture(atlas, &error), error);
    require(device.destroy_texture(target, &error), error);
}

} // namespace

int main() {
    try {
        test_projection_contract();
        test_vertex_layout_validation();
        test_production_shader_loading();
        test_null_rhi_sprite_frame();
        std::cout << "dve_v192_sprite_rhi_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v192_sprite_rhi_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
