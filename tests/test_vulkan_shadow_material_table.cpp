#include "dve/render/shadow_material_table.hpp"
#include "dve/rhi/vulkan_device.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dve;
using namespace dve::render;
using namespace dve::rhi;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
CookedPolygonAsset make_asset() {
    CookedPolygonAsset asset;
    asset.contentHash = 0x168168ULL;
    asset.materials.resize(1U);
    asset.materialBindings.resize(1U);
    asset.images.push_back({"alpha", "image/raw", 2U, 2U,
        {255U,255U,255U,0U, 255U,255U,255U,255U,
         255U,255U,255U,128U, 255U,255U,255U,192U}});
    asset.images.push_back({"opacity", "image/raw", 1U, 1U, {64U,0U,0U,255U}});
    PolygonSampler sampler;
    sampler.wrapS = ImportedWrapMode::ClampToEdge;
    sampler.wrapT = ImportedWrapMode::MirroredRepeat;
    sampler.minFilter = ImportedTextureFilter::Nearest;
    sampler.magFilter = ImportedTextureFilter::Linear;
    asset.samplers.push_back(sampler);
    asset.textures.push_back({"alpha", 0U, 0U});
    asset.textures.push_back({"opacity", 1U, 0U});
    asset.materialBindings[0].baseColor.texture = 0U;
    asset.materialBindings[0].opacity.texture = 1U;
    return asset;
}
}

int main() {
    try {
        VulkanDevice device;
        if (device.status() != DeviceStatus::Ready) {
            if (std::getenv("DVE_REQUIRE_VULKAN"))
                throw std::runtime_error(std::string(device.device_loss_reason()));
            std::cout << "Vulkan shadow material table test skipped\n";
            return 0;
        }
        std::string error;
        ShadowMaterialDescriptorTable table(device);
        require(table.initialize(&error), error.c_str());
        const auto asset = make_asset();
        require(table.ensure_material(asset, 0U, &error), error.c_str());
        const auto* descriptor = table.find(asset.contentHash, 0U);
        require(descriptor && descriptor->bindGroup, "Vulkan shadow material descriptor missing");
        require(descriptor->hasBaseColorTexture && descriptor->hasOpacityTexture,
                "Vulkan shadow material texture flags differ");
        const auto stats = table.stats();
        require(stats.materialDescriptorCount == 1U, "Vulkan descriptor count differs");
        require(stats.imageResourceCount == 2U, "Vulkan image count differs");
        require(stats.samplerResourceCount == 1U, "Vulkan sampler reuse differs");
        table.clear();
        const auto lifetime = device.statistics();
        require(lifetime.texturesCreated == lifetime.texturesDestroyed,
                "Vulkan shadow texture lifetime differs");
        require(lifetime.textureViewsCreated == lifetime.textureViewsDestroyed,
                "Vulkan shadow view lifetime differs");
        require(lifetime.samplersCreated == lifetime.samplersDestroyed,
                "Vulkan shadow sampler lifetime differs");
        require(lifetime.bindGroupsCreated == lifetime.bindGroupsDestroyed,
                "Vulkan shadow bind-group lifetime differs");
        std::cout << "dve_vulkan_shadow_material_table_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dve_vulkan_shadow_material_table_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}
