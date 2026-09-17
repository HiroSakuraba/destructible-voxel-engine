#include "dve/render/brick_palette_gpu_resolve.hpp"
#include "dve/rhi/null_device.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void require_near(float left, float right, const char* message) {
    if (std::fabs(left - right) > 2.0e-5F) throw std::runtime_error(message);
}

void require_sample_near(const dve::render::BrickPaletteShadedSample& left,
                         const dve::render::BrickPaletteShadedSample& right,
                         const char* message) {
    require(left.valid == right.valid, message);
    require(left.slotsEvaluated == right.slotsEvaluated, message);
    require(left.textureSamples == right.textureSamples, message);
    require_near(left.baseColor.x, right.baseColor.x, message);
    require_near(left.baseColor.y, right.baseColor.y, message);
    require_near(left.baseColor.z, right.baseColor.z, message);
    require_near(left.roughness, right.roughness, message);
    require_near(left.metallic, right.metallic, message);
    require_near(left.emissive.x, right.emissive.x, message);
    require_near(left.emissive.y, right.emissive.y, message);
    require_near(left.emissive.z, right.emissive.z, message);
    require_near(left.opacity, right.opacity, message);
    require_near(left.worldNormal.x, right.worldNormal.x, message);
    require_near(left.worldNormal.y, right.worldNormal.y, message);
    require_near(left.worldNormal.z, right.worldNormal.z, message);
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("could not read shader source");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

dve::VoxelMaterialDefinition material(const char* name, float r, float g, float b,
                                      float roughness, float metallic) {
    dve::VoxelMaterialDefinition value;
    value.name = name;
    value.baseColor = {r, g, b, 1.0F};
    value.roughness = roughness;
    value.metallic = metallic;
    value.emissive = {r * 0.03F, g * 0.02F, b * 0.01F};
    return value;
}

const dve::CookedBrickPalette* palette_for_record(
    const dve::CookedBrickMaterialRepresentations& brick,
    dve::VoxelMaterialRuntimePath path) {
    if (path == dve::VoxelMaterialRuntimePath::DeferredPalette2 &&
        brick.representations.deferredPalette2) {
        return &*brick.representations.deferredPalette2;
    }
    if (path == dve::VoxelMaterialRuntimePath::DeferredPalette4 &&
        brick.representations.deferredPalette4) {
        return &*brick.representations.deferredPalette4;
    }
    return nullptr;
}

void compare_deferred_packet(
    const dve::CookedBrickPaletteAsset& asset,
    const dve::render::BrickPaletteUploadPacket& packet,
    std::span<const dve::render::BrickPaletteMaterialRecord> materials,
    std::size_t recordIndex,
    std::span<const std::uint32_t> sampleIndices) {
    using namespace dve;
    using namespace dve::render;
    require(recordIndex < packet.records.size(), "record index is invalid");
    const auto& gpuRecord = packet.records[recordIndex];
    const auto path = static_cast<VoxelMaterialRuntimePath>(gpuRecord.runtimePath);
    require(path == VoxelMaterialRuntimePath::DeferredPalette2 ||
            path == VoxelMaterialRuntimePath::DeferredPalette4,
            "expected a deferred packet record");
    const auto* cookedBrick = find_cooked_brick_materials(asset, gpuRecord.key);
    require(cookedBrick != nullptr, "packet brick is absent from cooked asset");
    const auto* palette = palette_for_record(*cookedBrick, path);
    require(palette != nullptr, "packet path is absent from cooked brick");

    BrickPaletteShadingConfig config;
    config.mapping = static_cast<BrickPaletteMappingMode>(gpuRecord.mappingMode);
    for (const auto sampleIndex : sampleIndices) {
        BrickPaletteGpuResolveRequest request;
        request.brickRecordIndex = static_cast<std::uint32_t>(recordIndex);
        request.sampleIndex = sampleIndex;
        request.worldPositionX = 1.25F + static_cast<float>(sampleIndex) * 0.03F;
        request.worldPositionY = -0.75F;
        request.worldPositionZ = 2.5F;
        request.worldNormalX = 0.35F;
        request.worldNormalY = 0.80F;
        request.worldNormalZ = -0.22F;
        request.assetU = 0.2F + static_cast<float>(sampleIndex) * 0.01F;
        request.assetV = 0.65F;
        const BrickPaletteSurfacePoint point{
            {request.worldPositionX, request.worldPositionY, request.worldPositionZ},
            {request.worldNormalX, request.worldNormalY, request.worldNormalZ},
            request.assetU, request.assetV};
        const auto expected = shade_brick_palette_sample(
            *palette, materials, sampleIndex, point, config, nullptr);
        std::string error;
        const auto actual = resolve_brick_palette_upload_request(
            packet, materials, request, &error);
        require(error.empty(), error.c_str());
        require_sample_near(actual, expected, "packed resolve differs from CPU palette reference");
    }
}

} // namespace

int main() {
    try {
        using namespace dve;
        using namespace dve::render;
        using namespace dve::rhi;

        static_assert(sizeof(BrickPaletteGpuBrickRecord) == 48U);
        static_assert(sizeof(BrickPaletteGpuSampleEncoding) == 8U);
        static_assert(sizeof(BrickPaletteGpuBakedSample) == 64U);
        static_assert(sizeof(BrickPaletteGpuMaterialRecord) == 64U);
        static_assert(sizeof(BrickPaletteGpuResolveRequest) == 48U);

        VoxelObject object(227U);
        object.fill_brick({0, 0, 0}, 1U);
        object.set_voxel({0, 0, 0}, 2U); // exactly two materials in brick 0
        object.fill_brick({1, 0, 0}, 1U);
        object.set_voxel({8, 0, 0}, 2U);
        object.set_voxel({9, 0, 0}, 3U);
        object.set_voxel({10, 0, 0}, 4U); // four materials in brick 1

        std::vector<VoxelMaterialDefinition> sourceMaterials;
        sourceMaterials.push_back(material("Air", 0.0F, 0.0F, 0.0F, 1.0F, 0.0F));
        sourceMaterials.push_back(material("Stone", 0.45F, 0.50F, 0.55F, 0.70F, 0.0F));
        sourceMaterials.push_back(material("Gold", 1.0F, 0.65F, 0.08F, 0.22F, 0.9F));
        sourceMaterials.push_back(material("Copper", 0.75F, 0.20F, 0.08F, 0.30F, 0.75F));
        sourceMaterials.push_back(material("Paint", 0.08F, 0.18F, 0.95F, 0.42F, 0.05F));

        BrickPaletteAssetCookSettings cookSettings;
        cookSettings.palette2Overflow = VoxelMaterialPaletteOverflowPolicy::DominantMaterial;
        cookSettings.palette4Overflow = VoxelMaterialPaletteOverflowPolicy::DominantMaterial;
        const auto asset = cook_voxel_object_brick_palettes(object, sourceMaterials, cookSettings);
        require(asset.bricks.size() == 2U, "expected two brick-palette assets");
        auto materials = make_brick_palette_material_records(sourceMaterials);
        require(materials.size() == sourceMaterials.size(), "material record conversion failed");
        for (std::size_t index = 1U; index < materials.size(); ++index) {
            materials[index].normal = {0.15F * static_cast<float>(index), -0.08F, 1.0F};
            materials[index].textureScale = 0.75F + 0.2F * static_cast<float>(index);
            materials[index].detailContrast = 0.07F * static_cast<float>(index);
        }

        BrickPaletteSubmissionSettings palette2Settings;
        palette2Settings.preferredPath = VoxelMaterialRuntimePath::DeferredPalette2;
        palette2Settings.mapping = BrickPaletteMappingMode::AssetUv;
        const auto palette2Packet = build_brick_palette_upload_packet(asset, palette2Settings);
        require(static_cast<VoxelMaterialRuntimePath>(palette2Packet.records[0U].runtimePath) ==
                    VoxelMaterialRuntimePath::DeferredPalette2,
                "two-material brick did not select palette2");
        std::vector<std::uint32_t> samples(palette2Packet.records[0U].sampleCount);
        for (std::size_t index = 0U; index < samples.size(); ++index)
            samples[index] = static_cast<std::uint32_t>(index);
        compare_deferred_packet(asset, palette2Packet, materials, 0U, samples);

        BrickPaletteSubmissionSettings palette4Settings;
        palette4Settings.preferredPath = VoxelMaterialRuntimePath::DeferredPalette4;
        palette4Settings.mapping = BrickPaletteMappingMode::WorldTriplanar;
        const auto palette4Packet = build_brick_palette_upload_packet(asset, palette4Settings);
        require(static_cast<VoxelMaterialRuntimePath>(palette4Packet.records[1U].runtimePath) ==
                    VoxelMaterialRuntimePath::DeferredPalette4,
                "four-material brick did not select palette4");
        compare_deferred_packet(asset, palette4Packet, materials, 1U, samples);

        BrickPaletteSubmissionSettings bakedSettings;
        bakedSettings.preferredPath = VoxelMaterialRuntimePath::BakedProperties;
        bakedSettings.mapping = BrickPaletteMappingMode::AssetUv;
        const auto bakedPacket = build_brick_palette_upload_packet(asset, bakedSettings);
        BrickPaletteGpuResolveRequest bakedRequest;
        bakedRequest.brickRecordIndex = 0U;
        bakedRequest.sampleIndex = 0U;
        bakedRequest.worldNormalY = 1.0F;
        std::string error;
        const auto baked = resolve_brick_palette_upload_request(
            bakedPacket, materials, bakedRequest, &error);
        require(error.empty() && baked.valid, "packed baked resolve failed");
        require(baked.textureSamples == 1U && baked.slotsEvaluated == 1U,
                "packed baked metadata is wrong");

        BrickPaletteSubmissionSettings singleSettings;
        singleSettings.preferredPath = VoxelMaterialRuntimePath::SingleMaterial;
        singleSettings.mapping = BrickPaletteMappingMode::AssetUv;
        const auto singlePacket = build_brick_palette_upload_packet(asset, singleSettings);
        BrickPaletteGpuResolveRequest singleRequest = bakedRequest;
        singleRequest.assetU = 0.3F;
        singleRequest.assetV = 0.7F;
        const auto single = resolve_brick_palette_upload_request(
            singlePacket, materials, singleRequest, &error);
        require(error.empty() && single.valid && single.slotsEvaluated == 1U,
                "packed single-material resolve failed");

        std::vector<BrickPaletteGpuResolveRequest> batchRequests{singleRequest, singleRequest};
        batchRequests[1U].sampleIndex = 1U;
        std::vector<BrickPaletteGpuBakedSample> batchOutputs(batchRequests.size());
        require(resolve_brick_palette_upload_requests(
                    singlePacket, materials, batchRequests, batchOutputs, &error),
                error.c_str());
        require(batchOutputs[0U].valid == 1U && batchOutputs[1U].valid == 1U,
                "packed batch resolve produced invalid outputs");
        BrickPaletteGpuResolveRequest invalidRequest = singleRequest;
        invalidRequest.brickRecordIndex = std::numeric_limits<std::uint32_t>::max();
        error.clear();
        const auto invalidResolve = resolve_brick_palette_upload_request(
            singlePacket, materials, invalidRequest, &error);
        require(!invalidResolve.valid && error.find("invalid brick record") != std::string::npos,
                "packed resolver accepted an invalid brick record");
        error.clear();
        std::array<BrickPaletteGpuBakedSample, 1U> undersizedOutputs{};
        require(!resolve_brick_palette_upload_requests(
                    singlePacket, materials, batchRequests, undersizedOutputs, &error) &&
                    error.find("smaller") != std::string::npos,
                "packed batch resolver accepted an undersized output range");

        const auto gpuMaterials = pack_brick_palette_gpu_materials(materials);
        require(gpuMaterials.size() == materials.size(), "GPU material packing lost records");
        require(gpuMaterials[2U].globalMaterialId == materials[2U].globalMaterialId,
                "GPU material identity changed during packing");
        require_near(gpuMaterials[2U].detailContrast, materials[2U].detailContrast,
                     "GPU material detail contrast changed during packing");

        const auto layout = brick_palette_resolve_bind_group_layout_desc();
        require(layout.bindings.size() == kBrickPaletteResolveBindingCount,
                "resolve layout binding count is wrong");
        require(layout.bindings[kBrickPaletteResolveOutputBinding].type ==
                    BindingType::StorageBufferReadWrite,
                "resolve output binding is not writable");

        const std::filesystem::path sourceRoot = DVE_SOURCE_DIR;
        const auto shader = read_text(sourceRoot / "shaders/brick_palette_resolve.hlsl");
        const auto include = read_text(sourceRoot / "shaders/common/brick_palette_resolve.hlsli");
        require(shader.find("register(t0)") != std::string::npos &&
                shader.find("register(u8)") != std::string::npos,
                "resolve shader register contract is incomplete");
        require(include.find("struct BrickPaletteGpuBrickRecord") != std::string::npos &&
                include.find("struct BrickPaletteGpuResolveRequest") != std::string::npos,
                "resolve shader ABI structs are missing");

        const auto temp = std::filesystem::temp_directory_path() /
                          "dve_brick_palette_resolve_v227.spv";
        {
            const std::array<std::uint32_t, 2U> words{0x07230203U, 0x00010000U};
            std::ofstream stream(temp, std::ios::binary);
            stream.write(reinterpret_cast<const char*>(words.data()),
                         static_cast<std::streamsize>(sizeof(words)));
        }
        std::vector<std::byte> loaded;
        require(load_brick_palette_resolve_spirv(temp, loaded, &error), error.c_str());
        require(loaded.size() == 8U, "SPIR-V loader returned the wrong byte count");
        {
            const std::array<std::uint32_t, 1U> words{0xDEADBEEFU};
            std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(words.data()),
                         static_cast<std::streamsize>(sizeof(words)));
        }
        error.clear();
        require(!load_brick_palette_resolve_spirv(temp, loaded, &error) &&
                    error.find("not SPIR-V") != std::string::npos,
                "SPIR-V loader accepted an invalid magic number");
        std::filesystem::remove(temp);

        NullDevice device;
        BrickPaletteRhiMirror mirror(device);
        require(mirror.upload(palette4Packet, &error), error.c_str());
        std::vector<BrickPaletteGpuResolveRequest> gpuRequests(2U);
        gpuRequests[0U].brickRecordIndex = 1U;
        gpuRequests[0U].sampleIndex = 0U;
        gpuRequests[0U].worldNormalY = 1.0F;
        gpuRequests[1U] = gpuRequests[0U];
        gpuRequests[1U].sampleIndex = 1U;

        BrickPaletteResolveRhiHarness harness(device);
        require(harness.publish(mirror, gpuMaterials, gpuRequests, &error), error.c_str());
        const std::array<std::byte, 4U> nullBytecode{
            std::byte{0x03}, std::byte{0x02}, std::byte{0x23}, std::byte{0x07}};
        require(harness.create_pipeline(nullBytecode, "main", &error), error.c_str());
        require(harness.dispatch_and_wait(2U, &error), error.c_str());
        require(harness.stats().publications == 1U && harness.stats().dispatches == 1U,
                "resolve RHI statistics are wrong");
        require(!device.destroy_buffer(mirror.record_buffer(), &error),
                "resolve bind group did not retain the palette mirror");
        std::vector<BrickPaletteGpuBakedSample> nullOutputs(2U);
        require(harness.readback(nullOutputs, &error), error.c_str());
        harness.reset();
        mirror.reset();
        require(device.statistics().dispatchesExecuted == 1U,
                "Null-RHI did not record the resolve dispatch");

        std::cout << "dve_v227_brick_palette_gpu_resolve_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dve_v227_brick_palette_gpu_resolve_tests: FAIL: "
                  << error.what() << '\n';
        return 1;
    }
}
