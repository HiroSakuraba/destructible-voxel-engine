#include "dve/render/main_material_table.hpp"
#include "dve/render/material_resource_residency.hpp"
#include "dve/render/shadow_material_table.hpp"
#include "dve/rhi/null_device.hpp"

#include <iostream>
#include <array>
#include <span>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x + " at " + std::to_string(__LINE__)); } while(false)

dve::CookedPolygonAsset make_asset() {
    dve::CookedPolygonAsset asset;
    asset.contentHash = 0x172172ULL;
    asset.materials.resize(1U);
    asset.materialBindings.resize(1U);
    asset.images.push_back({"base", "image/raw", 1U, 1U, {255U,128U,64U,192U}});
    asset.images.push_back({"opacity", "image/raw", 1U, 1U, {128U,0U,0U,255U}});
    asset.samplers.push_back({});
    asset.textures.push_back({"base", 0U, 0U});
    asset.textures.push_back({"opacity", 1U, 0U});
    asset.materialBindings[0].baseColor.texture = 0U;
    asset.materialBindings[0].opacity.texture = 1U;
    return asset;
}
void test_typed_mip_residency() {
    using namespace dve;using namespace dve::render;
    rhi::NullDevice device;MaterialResourceResidency residency(device);std::string error;
    auto asset=make_asset();asset.images.clear();
    PolygonImage checker{"checker","image/raw",4,4,{}};
    PolygonImage normals{"normals","image/raw",4,4,{}};
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){
        const auto v=static_cast<std::uint8_t>((x+y)%2?255:0);
        checker.rgba8.insert(checker.rgba8.end(),{v,v,v,v});
        normals.rgba8.insert(normals.rgba8.end(),{static_cast<std::uint8_t>(x%2?218:37),128,218,255});
    }
    PolygonImage odd{"odd","image/raw",5,3,{}};
    for(unsigned i=0;i<15;++i){auto v=static_cast<std::uint8_t>(i%5==4?255:0);odd.rgba8.insert(odd.rgba8.end(),{v,v,v,255});}
    asset.images={checker,normals,odd};asset.contentHash=polygon_asset_content_hash(asset);
    CHECK(residency.retain_asset(asset.contentHash,&error));
    rhi::TextureViewHandle linear,srgb,normal,normalMap,npot;
    CHECK(residency.ensure_image(asset,0,linear,&error,MaterialImageInterpretation::LinearData));
    CHECK(residency.ensure_image(asset,0,srgb,&error,MaterialImageInterpretation::SrgbColor));
    CHECK(residency.ensure_image(asset,0,normal,&error,MaterialImageInterpretation::Normal));
    CHECK(linear!=srgb&&linear!=normal&&srgb!=normal);
    CHECK(residency.ensure_image(asset,1,normalMap,&error,MaterialImageInterpretation::Normal));
    CHECK(residency.ensure_image(asset,2,npot,&error));
    const auto read=[&](unsigned image,MaterialImageInterpretation interpretation){
        const auto* info=residency.find_image(asset.contentHash,image,interpretation);
        CHECK(info&&info->mipLevels==3);std::array<std::byte,4> value{};
        CHECK(device.read_texture(info->texture,2,0,value,4,&error));return value;
    };
    const auto color=read(0,MaterialImageInterpretation::SrgbColor);
    const auto data=read(0,MaterialImageInterpretation::LinearData);
    CHECK(std::to_integer<unsigned>(color[0])==188U);
    CHECK(std::to_integer<unsigned>(color[3])==128U);
    CHECK(std::to_integer<unsigned>(data[0])==128U);
    const auto n=read(1,MaterialImageInterpretation::Normal);
    CHECK(std::to_integer<unsigned>(n[0])>=127&&std::to_integer<unsigned>(n[0])<=129);
    CHECK(std::to_integer<unsigned>(n[2])>=254);
    CHECK(std::to_integer<unsigned>(read(2,MaterialImageInterpretation::LinearData)[0])==51U);
    CHECK(residency.find_image(asset.contentHash,0,MaterialImageInterpretation::SrgbColor)->format==rhi::TextureFormat::RGBA8Srgb);
    rhi::TextureViewHandle cached;CHECK(residency.ensure_image(asset,0,cached,&error,MaterialImageInterpretation::SrgbColor));CHECK(cached==srgb);
    CHECK(residency.stats().imagesUploaded==5&&residency.stats().mipLevelsUploaded==15);
    CHECK(residency.stats().residentImageBytes==408&&residency.stats().imageBytesUploaded==408);
    rhi::SamplerHandle sampler;CHECK(residency.ensure_sampler(asset,{},sampler,&error));
    CHECK(!residency.ensure_sampler(asset,std::numeric_limits<std::uint32_t>::max(),sampler,&error));error.clear();
    auto edited=asset;edited.images[0].rgba8.assign(64,255);edited.contentHash=polygon_asset_content_hash(edited);
    CHECK(edited.contentHash!=asset.contentHash&&residency.retain_asset(edited.contentHash,&error));
    CHECK(residency.ensure_image(edited,0,cached,&error,MaterialImageInterpretation::SrgbColor));CHECK(cached!=srgb);
    std::array<std::byte,4> fresh{};const auto* info=residency.find_image(edited.contentHash,0,MaterialImageInterpretation::SrgbColor);
    CHECK(device.read_texture(info->texture,2,0,fresh,4,&error)&&fresh[0]==std::byte{255});
    CHECK(residency.release_asset(edited.contentHash,&error));
    CHECK(residency.release_asset(asset.contentHash,&error));
    CHECK(residency.stats().residentImageBytes==0&&residency.stats().imageResourceCount==0);
    CHECK(device.statistics().texturesCreated==device.statistics().texturesDestroyed);
    CHECK(device.statistics().textureViewsCreated==device.statistics().textureViewsDestroyed);
}

}

int main() {
    try {
        test_typed_mip_residency();
        dve::rhi::NullDevice device;
        dve::render::MaterialResourceResidency residency(device);
        dve::render::MainMaterialDescriptorTable mainTable(device, residency);
        dve::render::ShadowMaterialDescriptorTable shadowTable(device, residency);
        std::string error;

        dve::rhi::TextureDesc shadowDesc;
        shadowDesc.format = dve::rhi::TextureFormat::D32Float;
        shadowDesc.width = 4U;
        shadowDesc.height = 4U;
        shadowDesc.usage = dve::rhi::TextureUsage::Sampled | dve::rhi::TextureUsage::DepthStencil;
        shadowDesc.initialState = dve::rhi::ResourceState::ShaderRead;
        const auto staticShadow = device.create_texture(shadowDesc, &error);
        const auto dynamicShadow = device.create_texture(shadowDesc, &error);
        dve::rhi::TextureViewDesc viewDesc;
        viewDesc.texture = staticShadow;
        const auto staticView = device.create_texture_view(viewDesc, &error);
        viewDesc.texture = dynamicShadow;
        const auto dynamicView = device.create_texture_view(viewDesc, &error);
        dve::rhi::SamplerDesc comparisonDesc;
        comparisonDesc.comparison = true;
        const auto comparisonSampler = device.create_sampler(comparisonDesc, &error);
        CHECK(staticShadow && dynamicShadow && staticView && dynamicView && comparisonSampler);
        CHECK(mainTable.initialize(staticView, dynamicView, comparisonSampler, &error));
        CHECK(shadowTable.initialize(&error));

        const auto asset = make_asset();
        CHECK(mainTable.ensure_material(asset, 0U, &error));
        auto shared = residency.stats();
        CHECK(shared.imagesUploaded == 2U);
        CHECK(shared.samplersCreated == 1U);
        CHECK(shared.assetReferenceCount == 1U);
        CHECK(shared.imageResourceCount == 2U);
        CHECK(shared.samplerResourceCount == 1U);

        const auto afterMain = device.statistics();
        CHECK(shadowTable.ensure_material(asset, 0U, &error));
        shared = residency.stats();
        CHECK(shared.imagesUploaded == 2U);
        CHECK(shared.samplersCreated == 1U);
        CHECK(shared.imageCacheHits >= 2U);
        CHECK(shared.samplerCacheHits >= 2U);
        CHECK(shared.assetReferenceCount == 2U);
        const auto afterShadow = device.statistics();
        CHECK(afterShadow.texturesCreated == afterMain.texturesCreated);
        CHECK(afterShadow.textureViewsCreated == afterMain.textureViewsCreated);
        CHECK(afterShadow.samplersCreated == afterMain.samplersCreated);

        CHECK(mainTable.invalidate_asset(asset.contentHash, &error));
        shared = residency.stats();
        CHECK(shared.assetReferenceCount == 1U);
        CHECK(shared.imageResourceCount == 2U);
        CHECK(shared.samplerResourceCount == 1U);
        CHECK(shadowTable.find(asset.contentHash, 0U) != nullptr);

        CHECK(shadowTable.invalidate_asset(asset.contentHash, &error));
        shared = residency.stats();
        CHECK(shared.assetReferenceCount == 0U);
        CHECK(shared.retainedAssetCount == 0U);
        CHECK(shared.imageResourceCount == 0U);
        CHECK(shared.samplerResourceCount == 0U);
        CHECK(shared.assetEvictions == 1U);

        shadowTable.clear();
        mainTable.clear();
        CHECK(device.destroy_sampler(comparisonSampler, &error));
        CHECK(device.destroy_texture_view(dynamicView, &error));
        CHECK(device.destroy_texture_view(staticView, &error));
        CHECK(device.destroy_texture(dynamicShadow, &error));
        CHECK(device.destroy_texture(staticShadow, &error));
        const auto lifetime = device.statistics();
        CHECK(lifetime.buffersCreated == lifetime.buffersDestroyed);
        CHECK(lifetime.texturesCreated == lifetime.texturesDestroyed);
        CHECK(lifetime.textureViewsCreated == lifetime.textureViewsDestroyed);
        CHECK(lifetime.samplersCreated == lifetime.samplersDestroyed);
        CHECK(lifetime.bindGroupsCreated == lifetime.bindGroupsDestroyed);
        std::cout << "dve_shared_material_residency_tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dve_shared_material_residency_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
