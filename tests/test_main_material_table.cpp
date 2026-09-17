#include "dve/render/main_material_table.hpp"
#include "dve/rhi/null_device.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x + " at " + std::to_string(__LINE__)); } while(false)

dve::CookedPolygonAsset make_asset() {
    dve::CookedPolygonAsset asset;
    asset.contentHash = 0x170ULL;
    dve::VoxelMaterialDefinition first;
    first.name = "painted metal";
    first.baseColor = {0.2F, 0.4F, 0.8F, 0.75F};
    first.metallic = 0.8F;
    first.roughness = 0.22F;
    first.emissive = {0.01F, 0.02F, 0.03F};
    dve::VoxelMaterialDefinition second;
    second.name = "rough stone";
    second.baseColor = {0.5F, 0.45F, 0.35F, 1.0F};
    second.metallic = 0.0F;
    second.roughness = 0.9F;
    asset.materials = {first, second};
    asset.materialBindings.resize(2U);
    asset.materialBindings[0].mapping.mappingMode = dve::MaterialMappingMode::WorldTriplanar;
    asset.materialBindings[0].mapping.triplanarScale = 2.5F;
    asset.materialBindings[0].mapping.detailColorStrength = 0.35F;
    asset.materialBindings[0].alphaCutoff = 0.33F;
    asset.materialBindings[0].doubleSided = true;
    asset.materialBindings[1].mapping.mappingMode = dve::MaterialMappingMode::UV1;
    asset.materialBindings[1].mapping.baseTransform.scale = {3.0F, 4.0F};
    asset.images.push_back({"base", "image/raw", 1U, 1U, {64U, 128U, 255U, 192U}});
    asset.images.push_back({"normal", "image/raw", 1U, 1U, {128U, 128U, 255U, 255U}});
    asset.images.push_back({"detail", "image/raw", 1U, 1U, {220U, 220U, 220U, 255U}});
    asset.samplers.push_back({});
    asset.textures.push_back({"base", 0U, 0U});
    asset.textures.push_back({"normal", 1U, 0U});
    asset.textures.push_back({"detail", 2U, 0U});
    asset.materialBindings[0].baseColor.texture = 0U;
    asset.materialBindings[0].normal.texture = 1U;
    asset.materialBindings[0].detailBaseColor.texture = 2U;
    asset.materialBindings[1].baseColor.texture = 0U;
    return asset;
}
}

int main() {
    try {
        dve::rhi::NullDevice device;
        dve::render::MainMaterialDescriptorTable table(device);
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
        CHECK(staticShadow && dynamicShadow && staticView && dynamicView && comparisonSampler);
        CHECK(table.initialize(staticView, dynamicView, comparisonSampler, &error));
        const auto asset = make_asset();
        CHECK(table.ensure_material(asset, 0U, &error));
        CHECK(table.ensure_material(asset, 1U, &error));
        CHECK(table.ensure_material(asset, 0U, &error));
        const auto* first = table.find(asset.contentHash, 0U);
        const auto* second = table.find(asset.contentHash, 1U);
        CHECK(first && second && first->bindGroup && second->bindGroup);
        CHECK(first->materialRecordBuffer == second->materialRecordBuffer);
        CHECK(first->mappingRecordBuffer == second->mappingRecordBuffer);
        CHECK(first->materialRecordOffset == 0U);
        CHECK(second->materialRecordOffset >= sizeof(dve::GpuMaterialRecord));
        CHECK(second->materialRecordOffset % device.capabilities().minStorageBufferOffsetAlignment == 0U);
        CHECK(second->mappingRecordOffset % device.capabilities().minStorageBufferOffsetAlignment == 0U);
        const auto expectedMask = (1U << static_cast<std::uint32_t>(dve::render::MainMaterialTextureChannel::BaseColor)) |
                                  (1U << static_cast<std::uint32_t>(dve::render::MainMaterialTextureChannel::Normal)) |
                                  (1U << static_cast<std::uint32_t>(dve::render::MainMaterialTextureChannel::DetailBaseColor));
        CHECK(first->texturePresenceMask == expectedMask);
        CHECK(second->texturePresenceMask == 1U);

        dve::GpuMaterialRecord materialRead{};
        dve::GpuPolygonMaterialMappingRecord mappingRead{};
        CHECK(device.read_buffer(first->materialRecordBuffer, first->materialRecordOffset,
                                 std::as_writable_bytes(std::span(&materialRead, 1U)), &error));
        CHECK(device.read_buffer(first->mappingRecordBuffer, first->mappingRecordOffset,
                                 std::as_writable_bytes(std::span(&mappingRead, 1U)), &error));
        const auto expectedMaterial = dve::pack_gpu_material_record(asset.materials[0]);
        const auto expectedMapping = dve::pack_gpu_polygon_material_mapping(asset.materialBindings[0]);
        CHECK(std::memcmp(&materialRead, &expectedMaterial, sizeof(materialRead)) == 0);
        CHECK(std::memcmp(&mappingRead, &expectedMapping, sizeof(mappingRead)) == 0);

        const auto stats = table.stats();
        CHECK(stats.assetsPublished == 1U);
        CHECK(stats.assetRecordBuffersCreated == 2U);
        CHECK(stats.materialRecordsUploaded == 2U);
        CHECK(stats.mappingRecordsUploaded == 2U);
        CHECK(stats.materialDescriptorCount == 2U);
        CHECK(stats.assetResourceCount == 1U);
        CHECK(stats.imageResourceCount == 3U);
        CHECK(stats.samplerResourceCount == 1U);
        CHECK(stats.descriptorCacheHits == 1U);
        CHECK(stats.imageCacheHits >= 1U);
        CHECK(stats.samplerCacheHits >= 3U);

        CHECK(table.invalidate_asset(asset.contentHash, &error));
        CHECK(table.stats().assetResourceCount == 0U);
        CHECK(table.stats().materialDescriptorCount == 0U);
        CHECK(table.ensure_material(asset, 0U, &error));
        table.clear();
        CHECK(!table.valid());
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
        std::cout << "dve_main_material_table_tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dve_main_material_table_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
