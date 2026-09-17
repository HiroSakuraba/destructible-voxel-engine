#include "dve/render/main_material_table.hpp"
#include "dve/rhi/vulkan_device.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
dve::CookedPolygonAsset make_asset() {
    dve::CookedPolygonAsset asset;
    asset.contentHash = 0x170170ULL;
    dve::VoxelMaterialDefinition material;
    material.baseColor = {0.2F, 0.4F, 0.8F, 0.75F};
    material.metallic = 0.7F;
    material.roughness = 0.3F;
    asset.materials.push_back(material);
    asset.materialBindings.resize(1U);
    asset.images.push_back({"base", "image/raw", 1U, 1U, {64U,128U,255U,192U}});
    asset.samplers.push_back({});
    asset.textures.push_back({"base", 0U, 0U});
    asset.materialBindings[0].baseColor.texture = 0U;
    asset.materialBindings[0].alphaCutoff = 0.42F;
    asset.materialBindings[0].doubleSided = true;
    return asset;
}
}
int main() {
    try {
        dve::rhi::VulkanDevice device;
        if (device.status() != dve::rhi::DeviceStatus::Ready) {
            if (std::getenv("DVE_REQUIRE_VULKAN")) throw std::runtime_error(std::string(device.device_loss_reason()));
            std::cout << "Vulkan main material table test skipped\n";
            return 0;
        }
        std::string error;
        dve::rhi::TextureDesc shadowDesc;
        shadowDesc.format = dve::rhi::TextureFormat::D32Float;
        shadowDesc.width = 4U; shadowDesc.height = 4U;
        shadowDesc.usage = dve::rhi::TextureUsage::Sampled | dve::rhi::TextureUsage::DepthStencil;
        shadowDesc.initialState = dve::rhi::ResourceState::ShaderRead;
        const auto staticShadow = device.create_texture(shadowDesc, &error);
        const auto dynamicShadow = device.create_texture(shadowDesc, &error);
        dve::rhi::TextureViewDesc shadowViewDesc; shadowViewDesc.texture = staticShadow;
        const auto staticView = device.create_texture_view(shadowViewDesc, &error);
        shadowViewDesc.texture = dynamicShadow;
        const auto dynamicView = device.create_texture_view(shadowViewDesc, &error);
        dve::rhi::SamplerDesc comparison; comparison.comparison = true;
        const auto comparisonSampler = device.create_sampler(comparison, &error);
        require(staticShadow && dynamicShadow && staticView && dynamicView && comparisonSampler, error);
        dve::render::MainMaterialDescriptorTable table(device);
        require(table.initialize(staticView, dynamicView, comparisonSampler, &error), error);
        const auto asset = make_asset();
        require(table.ensure_material(asset, 0U, &error), error);
        const auto* descriptor = table.find(asset.contentHash, 0U);
        require(descriptor && descriptor->bindGroup, "main material descriptor missing");
        dve::GpuMaterialRecord material{};
        dve::GpuPolygonMaterialMappingRecord mapping{};
        require(device.read_buffer(descriptor->materialRecordBuffer, descriptor->materialRecordOffset,
            std::as_writable_bytes(std::span(&material, 1U)), &error), error);
        require(device.read_buffer(descriptor->mappingRecordBuffer, descriptor->mappingRecordOffset,
            std::as_writable_bytes(std::span(&mapping, 1U)), &error), error);
        require(material.metallic == 0.7F && material.roughness == 0.3F, "material record mismatch");
        require(mapping.baseColorTexture == 0U, "mapping record mismatch");
        require(mapping.alphaCutoff == 0.42F, "mapping alpha cutoff mismatch");
        require((mapping.mappingFlags & dve::kGpuMaterialMappingDoubleSided) != 0U,
                "mapping flag mismatch");
        table.clear();
        require(device.destroy_sampler(comparisonSampler, &error), error);
        require(device.destroy_texture_view(dynamicView, &error), error);
        require(device.destroy_texture_view(staticView, &error), error);
        require(device.destroy_texture(dynamicShadow, &error), error);
        require(device.destroy_texture(staticShadow, &error), error);
        std::cout << "dve_vulkan_main_material_table_tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dve_vulkan_main_material_table_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
