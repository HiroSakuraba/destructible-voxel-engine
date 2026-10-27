// T4 Vulkan contract: (1) the production HLSL BC5 normal decoder matches the CPU mirror on real
// BC5_UNORM texture sampling; (2) MainMaterialDescriptorTable::bind_resident_normal publishes a
// BC5 view + flag (and undoes it) on a real device. Skips without Vulkan unless DVE_REQUIRE_VULKAN
// is set; the GPU decode half also needs the dxc-compiled probe (DVE_BC5_SHADER_DIR).
#include "dve/render/main_material_table.hpp"
#include "dve/render/normal_texture_binding.hpp"
#include "dve/render/texture_residency.hpp"
#include "dve/render/normal_bc5_codec.hpp"
#include "dve/rhi/vulkan_device.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dve;
using namespace dve::render;
namespace {
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }

// Constant BC4 block: both endpoints equal, every index selects endpoint 0 -> exactly `value`/255.
void append_constant_bc5_block(std::vector<std::byte>& out, std::uint8_t r, std::uint8_t g) {
    for (const std::uint8_t v : {r, g}) {
        out.push_back(static_cast<std::byte>(v));
        out.push_back(static_cast<std::byte>(v));
        for (int i = 0; i < 6; ++i) out.push_back(std::byte{0});
    }
}

void gpu_decode_parity(rhi::VulkanDevice& device) {
#ifndef DVE_BC5_SHADER_DIR
    std::cout << "BC5 GPU decode parity skipped: no dxc SPIR-V probe was built\n";
    (void)device;
#else
    const auto spirvPath = std::filesystem::path(DVE_BC5_SHADER_DIR) / "bc5_normal_decode_reference.spv";
    if (!std::filesystem::exists(spirvPath)) {
        std::cout << "BC5 GPU decode parity skipped: missing " << spirvPath.string() << '\n';
        return;
    }
    if (!device.texture_format_capabilities(rhi::TextureFormat::BC5RGUnorm).sampled) {
        std::cout << "BC5 GPU decode parity skipped: device cannot sample BC5\n";
        return;
    }
    constexpr std::array<std::array<std::uint8_t, 2>, 8> texels{{
        {{128, 128}}, {{255, 128}}, {{0, 128}}, {{164, 202}},
        {{255, 255}}, {{0, 0}}, {{192, 192}}, {{128, 255}}}};
    std::vector<std::byte> blocks;
    for (const auto& t : texels) append_constant_bc5_block(blocks, t[0], t[1]);
    std::string error;
    rhi::TextureDesc desc;
    desc.format = rhi::TextureFormat::BC5RGUnorm; desc.width = 32U; desc.height = 4U;
    desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDestination;
    desc.initialState = rhi::ResourceState::ShaderRead;
    const auto texture = device.create_texture(desc, &error);
    require(static_cast<bool>(texture), "BC5 probe texture: " + error);
    require(device.write_texture(texture, 0U, 0U, blocks,
                rhi::texture_row_bytes(rhi::TextureFormat::BC5RGUnorm, 32U), &error),
            "BC5 probe upload: " + error);
    rhi::TextureViewDesc viewDesc; viewDesc.texture = texture;
    const auto view = device.create_texture_view(viewDesc, &error);
    rhi::SamplerDesc samplerDesc;
    samplerDesc.minFilter = rhi::FilterMode::Linear; samplerDesc.magFilter = rhi::FilterMode::Linear;
    const auto sampler = device.create_sampler(samplerDesc, &error);
    require(view && sampler, "BC5 probe view/sampler: " + error);
    rhi::BufferDesc bufferDesc;
    bufferDesc.bytes = texels.size() * 2U * 16U;
    bufferDesc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopySource | rhi::BufferUsage::CopyDestination;
    bufferDesc.memory = rhi::MemoryDomain::Upload;
    bufferDesc.initialState = rhi::ResourceState::ShaderRead;
    const auto output = device.create_buffer(bufferDesc, &error);
    require(static_cast<bool>(output), "BC5 probe output: " + error);
    rhi::BindGroupLayoutDesc layoutDesc;
    layoutDesc.bindings = {{0U, rhi::BindingType::SampledTexture, rhi::ShaderStage::Compute},
                           {1U, rhi::BindingType::StorageBufferReadWrite, rhi::ShaderStage::Compute}};
    const auto layout = device.create_bind_group_layout(layoutDesc, &error);
    rhi::BindGroupDesc groupDesc; groupDesc.layout = layout;
    groupDesc.entries = {{0U, {}, view, 0U, 0U, sampler}, {1U, output, {}, 0U, bufferDesc.bytes, {}}};
    const auto group = device.create_bind_group(groupDesc, &error);
    require(layout && group, "BC5 probe bind group: " + error);
    std::ifstream in(spirvPath, std::ios::binary | std::ios::ate);
    require(static_cast<bool>(in), "BC5 probe SPIR-V unreadable");
    std::vector<std::byte> spirv(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(spirv.data()), static_cast<std::streamsize>(spirv.size()));
    rhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bytecode = spirv; pipelineDesc.bindGroupLayouts = {layout};
    const auto pipeline = device.create_compute_pipeline(pipelineDesc, &error);
    require(static_cast<bool>(pipeline), "BC5 probe pipeline: " + error);
    auto commands = device.begin_commands(rhi::QueueKind::Compute, "BC5 normal decode parity", &error);
    require(commands && device.bind_compute_bind_group(commands, 0U, group, &error) &&
            device.dispatch(commands, pipeline, 1U, 1U, 1U, &error), "BC5 probe dispatch: " + error);
    const auto fence = device.submit(commands, &error);
    require(fence && device.wait(fence, &error), "BC5 probe submit: " + error);
    struct Float4 { float x, y, z, w; };
    std::vector<Float4> actual(texels.size() * 2U);
    require(device.read_buffer(output, 0U, std::as_writable_bytes(std::span(actual)), &error),
            "BC5 probe readback: " + error);
    for (std::size_t i = 0; i < texels.size(); ++i) {
        const float r = static_cast<float>(texels[i][0]) / 255.0F;
        const float g = static_cast<float>(texels[i][1]) / 255.0F;
        const auto& raw = actual[i * 2U + 1U];
        // The GPU must have sampled exactly the BC5 payload (constant blocks decode exactly).
        require(std::abs(raw.x - r) < 1.0e-3F && std::abs(raw.y - g) < 1.0e-3F,
                "GPU BC5 RG sample differs from the encoded block, texel " + std::to_string(i));
        require(raw.z == 1.0F && raw.w == 1.0F, "flag-clear decode must pass the sample through");
        const auto expected = decode_bc5_normal_sample(r, g);
        const auto& got = actual[i * 2U];
        require(std::abs(got.x - expected[0]) < 2.0e-3F && std::abs(got.y - expected[1]) < 2.0e-3F &&
                std::abs(got.z - expected[2]) < 2.0e-3F,
                "HLSL BC5 normal decode differs from the CPU mirror, texel " + std::to_string(i));
        require(got.z >= 0.5F - 1.0e-4F, "GPU BC5 decode left the +Z hemisphere, texel " + std::to_string(i));
    }
    require(device.destroy_compute_pipeline(pipeline, &error) && device.destroy_bind_group(group, &error) &&
            device.destroy_bind_group_layout(layout, &error) && device.destroy_buffer(output, &error) &&
            device.destroy_sampler(sampler, &error) && device.destroy_texture_view(view, &error) &&
            device.destroy_texture(texture, &error), "BC5 probe cleanup: " + error);
    std::cout << "BC5 GPU decode parity: PASS (" << texels.size() << " texels)\n";
#endif
}

CookedPolygonAsset make_asset() {
    CookedPolygonAsset asset;
    asset.contentHash = 0x4BC501ULL;
    asset.materials.resize(1U);
    asset.materialBindings.resize(1U);
    PolygonImage image;
    image.name = "normal"; image.mimeType = "image/raw"; image.width = 8U; image.height = 8U;
    image.rgba8.resize(8U * 8U * 4U);
    for (std::size_t i = 0; i < image.rgba8.size(); i += 4U) {
        image.rgba8[i] = 160U; image.rgba8[i + 1U] = 100U; image.rgba8[i + 2U] = 230U; image.rgba8[i + 3U] = 255U;
    }
    asset.images.push_back(std::move(image));
    asset.samplers.push_back({});
    asset.textures.push_back({"normal", 0U, 0U});
    asset.materialBindings[0].normal.texture = 0U;
    asset.materialBindings[0].doubleSided = true;
    return asset;
}

void vulkan_live_binding(rhi::VulkanDevice& device) {
    std::string error;
    rhi::TextureDesc shadowDesc;
    shadowDesc.format = rhi::TextureFormat::D32Float; shadowDesc.width = 4U; shadowDesc.height = 4U;
    shadowDesc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::DepthStencil;
    shadowDesc.initialState = rhi::ResourceState::ShaderRead;
    const auto shadowA = device.create_texture(shadowDesc, &error);
    const auto shadowB = device.create_texture(shadowDesc, &error);
    rhi::TextureViewDesc sv; sv.texture = shadowA;
    const auto viewA = device.create_texture_view(sv, &error);
    sv.texture = shadowB;
    const auto viewB = device.create_texture_view(sv, &error);
    rhi::SamplerDesc comparison; comparison.comparison = true;
    const auto compare = device.create_sampler(comparison, &error);
    require(shadowA && shadowB && viewA && viewB && compare, "shadow resources: " + error);
    MainMaterialDescriptorTable table(device);
    require(table.initialize(viewA, viewB, compare, &error), "table: " + error);
    const auto asset = make_asset();
    require(table.ensure_material(asset, 0U, &error), "material: " + error);
    const auto* authored = table.find(asset.contentHash, 0U);
    require(authored != nullptr, "descriptor missing");
    const auto authoredView = authored->textureViews[2U];
    const auto authoredGroup = authored->bindGroup;

    TextureResidencyManager residency(device, 1U << 20U, {}, make_bc5_normal_encoder());
    require(residency.enqueue({900U, asset.images[0], TextureSemantic::Normal,
                               TextureCompressionPreference::PreferBc5ForNormals, true}, &error),
            "resident enqueue: " + error);
    require(residency.pump(1U << 20U, &error) != 0U, "resident pump: " + error);
    const auto* resident = residency.find(900U);
    require(resident && resident->view, "resident normal missing");
    const bool expectBc5 = resident->format == rhi::TextureFormat::BC5RGUnorm;
    require(table.bind_resident_normal(asset.contentHash, 0U, *resident, &error), "bind: " + error);
    const auto* bound = table.find(asset.contentHash, 0U);
    require(bound->textureViews[2U] == resident->view && bound->bindGroup != authoredGroup,
            "Vulkan descriptor was not republished");
    GpuPolygonMaterialMappingRecord mapping{};
    require(device.read_buffer(bound->mappingRecordBuffer, bound->mappingRecordOffset,
                std::as_writable_bytes(std::span(&mapping, 1U)), &error), "mapping read: " + error);
    require(((mapping.mappingFlags & kGpuMaterialMappingNormalBc5) != 0U) == expectBc5,
            "BC5 flag does not match the resident GPU format");
    require((mapping.mappingFlags & kGpuMaterialMappingDoubleSided) != 0U, "unrelated flag dropped");
    require(table.restore_authored_normal(asset.contentHash, 0U, &error), "restore: " + error);
    const auto* restored = table.find(asset.contentHash, 0U);
    require(restored->textureViews[2U] == authoredView, "authored view not restored");
    require(device.read_buffer(restored->mappingRecordBuffer, restored->mappingRecordOffset,
                std::as_writable_bytes(std::span(&mapping, 1U)), &error), "mapping read 2: " + error);
    require((mapping.mappingFlags & kGpuMaterialMappingNormalBc5) == 0U, "BC5 flag survived restore");
    // The external view must be detached (restored) before residency tears it down.
    table.clear();
    residency.clear();
    require(device.destroy_sampler(compare, &error) && device.destroy_texture_view(viewB, &error) &&
            device.destroy_texture_view(viewA, &error) && device.destroy_texture(shadowB, &error) &&
            device.destroy_texture(shadowA, &error), "cleanup: " + error);
    std::cout << "Vulkan live BC5 binding (" << (expectBc5 ? "BC5" : "RGBA8 fallback") << "): PASS\n";
}
} // namespace

int main() {
    try {
        rhi::VulkanDevice device;
        if (device.status() != rhi::DeviceStatus::Ready) {
            if (std::getenv("DVE_REQUIRE_VULKAN") != nullptr)
                throw std::runtime_error("required Vulkan device unavailable: " +
                                         std::string(device.device_loss_reason()));
            std::cout << "Vulkan BC5 normal decode test skipped: " << device.device_loss_reason() << '\n';
            return 0;
        }
        vulkan_live_binding(device);
        gpu_decode_parity(device);
        std::cout << "dve_vulkan_bc5_normal_decode_tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dve_vulkan_bc5_normal_decode_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
