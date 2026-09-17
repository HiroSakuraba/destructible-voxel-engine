#include "dve/render/shadow_material_table.hpp"
#include "dve/rhi/null_device.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x + " at " + std::to_string(__LINE__)); } while(false)

dve::CookedPolygonAsset make_asset() {
    dve::CookedPolygonAsset asset;
    asset.contentHash = 0x168ULL;
    asset.materials.resize(2U);
    asset.materialBindings.resize(2U);
    asset.images.push_back({"shared alpha", "image/raw", 2U, 1U,
                            {255U,255U,255U,64U, 255U,255U,255U,192U}});
    asset.images.push_back({"opacity", "image/raw", 1U, 1U, {128U,0U,0U,255U}});
    asset.samplers.push_back({});
    asset.textures.push_back({"base", 0U, 0U});
    asset.textures.push_back({"opacity", 1U, 0U});
    asset.materialBindings[0].baseColor.texture = 0U;
    asset.materialBindings[0].opacity.texture = 1U;
    asset.materialBindings[1].baseColor.texture = 0U;
    return asset;
}
}

int main() {
    try {
        dve::rhi::NullDevice device;
        dve::render::ShadowMaterialDescriptorTable table(device);
        std::string error;
        CHECK(table.initialize(&error));
        const auto asset = make_asset();
        CHECK(table.ensure_material(asset, 0U, &error));
        CHECK(table.ensure_material(asset, 1U, &error));
        CHECK(table.ensure_material(asset, 0U, &error));
        const auto* first = table.find(asset.contentHash, 0U);
        const auto* second = table.find(asset.contentHash, 1U);
        CHECK(first && second && first->bindGroup && second->bindGroup);
        CHECK(first->hasBaseColorTexture && first->hasOpacityTexture);
        CHECK(second->hasBaseColorTexture && !second->hasOpacityTexture);
        auto stats = table.stats();
        CHECK(stats.materialDescriptorCount == 2U);
        CHECK(stats.imageResourceCount == 2U);
        CHECK(stats.samplerResourceCount == 1U);
        CHECK(stats.imageCacheHits >= 1U);
        CHECK(stats.samplerCacheHits >= 2U);
        CHECK(stats.descriptorCacheHits == 1U);
        CHECK(table.invalidate_asset(asset.contentHash, &error));
        stats = table.stats();
        CHECK(stats.materialDescriptorCount == 0U);
        CHECK(stats.imageResourceCount == 0U);
        CHECK(stats.samplerResourceCount == 0U);
        CHECK(table.ensure_material(asset, 0U, &error));
        table.clear();
        CHECK(!table.valid());
        CHECK(device.statistics().texturesCreated == device.statistics().texturesDestroyed);
        CHECK(device.statistics().textureViewsCreated == device.statistics().textureViewsDestroyed);
        CHECK(device.statistics().samplersCreated == device.statistics().samplersDestroyed);
        CHECK(device.statistics().bindGroupsCreated == device.statistics().bindGroupsDestroyed);
        std::cout << "dve_shadow_material_table_tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dve_shadow_material_table_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
