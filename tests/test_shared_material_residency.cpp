#include "dve/render/main_material_table.hpp"
#include "dve/render/material_resource_residency.hpp"
#include "dve/render/shadow_material_table.hpp"
#include "dve/rhi/null_device.hpp"

#include <iostream>
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
}

int main() {
    try {
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
