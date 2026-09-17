#include "dve/render/texture_residency.hpp"
#include "dve/render/texture_preview.hpp"
#include "dve/rhi/null_device.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
#define CHECK(c) do { if(!(c)) throw std::runtime_error(std::string("CHECK failed: ")+#c); } while(false)

dve::PolygonImage image(std::string name, std::uint32_t size, std::uint8_t seed) {
    dve::PolygonImage result; result.name=std::move(name); result.mimeType="image/raw";
    result.width=size; result.height=size; result.rgba8.resize(static_cast<std::size_t>(size)*size*4U);
    for(std::size_t i=0;i<result.rgba8.size();++i) result.rgba8[i]=static_cast<std::uint8_t>(seed+i*13U);
    return result;
}
}


void test_texture_preview_defaults_and_fallbacks() {
    using namespace dve::render;
    CHECK(default_texture_preview_settings(TextureSemantic::BaseColor).mode == TexturePreviewMode::ColorSrgb);
    CHECK(default_texture_preview_settings(TextureSemantic::Normal).mode == TexturePreviewMode::TangentNormal);
    CHECK(default_texture_preview_settings(TextureSemantic::MetallicRoughness).mode == TexturePreviewMode::LinearData);
    CHECK(default_texture_preview_settings(TextureSemantic::Opacity).mode == TexturePreviewMode::Alpha);

    const auto color = make_missing_texture_fallback(TextureSemantic::BaseColor, 8U);
    CHECK(color.width == 8U && color.height == 8U);
    CHECK(color.rgba8[0] == 255U && color.rgba8[2] == 255U);
    const std::size_t black = (static_cast<std::size_t>(0U) * color.width + 2U) * 4U;
    CHECK(color.rgba8[black] == 0U && color.rgba8[black + 1U] == 0U && color.rgba8[black + 2U] == 0U);

    const auto normal = make_missing_texture_fallback(TextureSemantic::Normal, 4U);
    CHECK(normal.rgba8[0] == 128U && normal.rgba8[1] == 128U && normal.rgba8[2] == 255U);
    const auto packed = make_missing_texture_fallback(TextureSemantic::MetallicRoughness, 4U);
    CHECK(packed.rgba8[1] == 128U && packed.rgba8[2] == 0U);
    const auto ao = make_missing_texture_fallback(TextureSemantic::AmbientOcclusion, 4U);
    CHECK(ao.rgba8[0] == 255U && ao.rgba8[1] == 255U && ao.rgba8[2] == 255U);
    const auto emissive = make_missing_texture_fallback(TextureSemantic::Emissive, 4U);
    CHECK(emissive.rgba8[0] == 0U && emissive.rgba8[1] == 0U && emissive.rgba8[2] == 0U);
}

void test_semantic_mips() {
    dve::PolygonImage color;
    color.name = "gamma"; color.mimeType = "image/raw"; color.width = 2U; color.height = 2U;
    color.rgba8 = {0,0,0,255, 255,255,255,255, 0,0,0,255, 255,255,255,255};
    const auto colorMips = dve::render::generate_semantic_texture_mips(
        color, dve::render::TextureSemantic::BaseColor);
    CHECK(colorMips.levels.size() == 2U);
    // Linear-light averaging of black and white encodes near sRGB 188, not byte-average 128.
    CHECK(colorMips.levels[1].rgba8[0] >= 186U && colorMips.levels[1].rgba8[0] <= 189U);

    dve::PolygonImage normal;
    normal.name = "normal"; normal.mimeType = "image/raw"; normal.width = 2U; normal.height = 2U;
    normal.rgba8 = {255,128,128,255, 128,255,128,255, 128,128,255,255, 128,128,255,255};
    const auto normalMips = dve::render::generate_semantic_texture_mips(
        normal, dve::render::TextureSemantic::Normal);
    CHECK(normalMips.levels.size() == 2U);
    const auto& pixel = normalMips.levels[1].rgba8;
    const float x = static_cast<float>(pixel[0]) / 127.5F - 1.0F;
    const float y = static_cast<float>(pixel[1]) / 127.5F - 1.0F;
    const float z = static_cast<float>(pixel[2]) / 127.5F - 1.0F;
    CHECK(std::abs(std::sqrt(x*x + y*y + z*z) - 1.0F) < 0.02F);
}

void test_hot_reload() {
    dve::rhi::NullDevice device;
    dve::render::TextureResidencyManager manager(device, 4096U);
    std::string error;
    auto first = image("hot", 4U, 10U);
    CHECK(manager.enqueue({11U, first, dve::render::TextureSemantic::BaseColor,
                           dve::render::TextureCompressionPreference::Uncompressed, true,
                           dve::render::TextureUploadMode::ReplaceIfChanged}, &error));
    CHECK(manager.pump(4096U, &error) > 0U);
    const auto originalHash = manager.find(11U)->sourceHash;
    const auto originalReference = manager.reference(11U);
    CHECK(originalReference && manager.reference_valid(*originalReference));
    CHECK(manager.enqueue({11U, first, dve::render::TextureSemantic::BaseColor,
                           dve::render::TextureCompressionPreference::Uncompressed, true,
                           dve::render::TextureUploadMode::ReplaceIfChanged}, &error));
    CHECK(manager.stats().unchangedRequests == 1U);
    auto changed = image("hot", 4U, 11U);
    CHECK(manager.enqueue({11U, changed, dve::render::TextureSemantic::BaseColor,
                           dve::render::TextureCompressionPreference::Uncompressed, true,
                           dve::render::TextureUploadMode::ReplaceIfChanged}, &error));
    CHECK(manager.pump(4096U, &error) > 0U);
    CHECK(manager.find(11U) && manager.find(11U)->sourceHash != originalHash);
    CHECK(!manager.reference_valid(*originalReference));
    const auto replacementReference = manager.reference(11U);
    CHECK(replacementReference && replacementReference->generation == originalReference->generation + 1U);
    CHECK(manager.reference_valid(*replacementReference));
    CHECK(manager.stats().residentTexturesReplaced == 1U);
    CHECK(manager.stats().residentCount == 1U);

    auto queuedA = image("queued", 4U, 20U);
    auto queuedB = image("queued", 4U, 21U);
    CHECK(manager.enqueue({12U, queuedA, dve::render::TextureSemantic::Emissive,
                           dve::render::TextureCompressionPreference::Uncompressed, true,
                           dve::render::TextureUploadMode::ReplaceIfChanged}, &error));
    CHECK(manager.enqueue({12U, queuedB, dve::render::TextureSemantic::Emissive,
                           dve::render::TextureCompressionPreference::Uncompressed, true,
                           dve::render::TextureUploadMode::ReplaceIfChanged}, &error));
    CHECK(manager.stats().pendingRequestsReplaced == 1U);
    CHECK(manager.pump(4096U, &error) > 0U);
    CHECK(manager.stats().residentCount == 2U);
}

int main(){try{
    test_texture_preview_defaults_and_fallbacks();
    test_semantic_mips();
    test_hot_reload();
    dve::rhi::NullDevice device;
    dve::render::TextureResidencyManager manager(device, 400U);
    std::string error;
    CHECK(manager.enqueue({1U,image("base",4U,3U),dve::render::TextureSemantic::BaseColor,
                           dve::render::TextureCompressionPreference::PreferBc7,true},&error));
    CHECK(manager.enqueue({2U,image("normal",4U,17U),dve::render::TextureSemantic::Normal,
                           dve::render::TextureCompressionPreference::PreferBc5ForNormals,true},&error));
    CHECK(!manager.enqueue({2U,image("duplicate",2U,1U)},&error));
    CHECK(manager.pump(128U,&error)>0U);
    CHECK(manager.find(1U)!=nullptr);
    CHECK(manager.pump(1024U,&error)>0U);
    CHECK(manager.find(2U)!=nullptr);
    CHECK(manager.touch(1U));
    manager.set_budget(100U);
    CHECK(manager.enqueue({3U,image("new",4U,31U)},&error));
    CHECK(manager.pump(1024U,&error)>0U);
    CHECK(manager.find(3U)!=nullptr);
    const auto stats=manager.stats();
    CHECK(stats.uploadsCompleted==3U);
    CHECK(stats.compressionFallbacks==2U);
    CHECK(stats.evictions>=1U);
    CHECK(stats.duplicateRequests==1U);
    manager.clear();
    CHECK(device.statistics().texturesCreated==device.statistics().texturesDestroyed);
    std::cout<<"texture residency tests: PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
