#include "dve/render/radiance_cascade_gpu_probes.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace dve::render {
namespace {
void validate(const spwi::VoxelGBuffer& gbuffer, const spwi::CascadeLevelInfo* level) {
    if (gbuffer.width == 0 || gbuffer.height == 0 ||
        static_cast<std::uint64_t>(gbuffer.width) * gbuffer.height != gbuffer.texels.size())
        throw std::invalid_argument("radiance cascade G-buffer dimensions do not match texels");
    if (!level) return;
    const auto spacing = level->probeSpacingPixels;
    if (spacing == 0 || level->probesX != 1U + (gbuffer.width - 1U) / spacing ||
        level->probesY != 1U + (gbuffer.height - 1U) / spacing ||
        static_cast<std::uint64_t>(level->probesX) * level->probesY >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max() - 63U))
        throw std::invalid_argument("radiance cascade probe grid does not match G-buffer");
}

RadianceCascadeSurfaceSample pack(const spwi::VoxelGBufferTexel& texel) {
    if (!texel.valid) return {};
    return {texel.position, texel.viewDistance, texel.normal, 1U};
}
} // namespace

std::vector<RadianceCascadeSurfaceSample> pack_radiance_cascade_primary(
    const spwi::VoxelGBuffer& gbuffer) {
    validate(gbuffer, nullptr);
    std::vector<RadianceCascadeSurfaceSample> samples;
    samples.reserve(gbuffer.texels.size());
    for (const auto& texel : gbuffer.texels) samples.push_back(pack(texel));
    return samples;
}

std::vector<RadianceCascadeSurfaceSample> reference_radiance_cascade_probes(
    const spwi::VoxelGBuffer& gbuffer, const spwi::CascadeLevelInfo& level) {
    validate(gbuffer, &level);
    std::vector<RadianceCascadeSurfaceSample> probes;
    probes.reserve(static_cast<std::size_t>(level.probesX) * level.probesY);
    for (std::uint32_t y = 0; y < level.probesY; ++y)
        for (std::uint32_t x = 0; x < level.probesX; ++x) {
            const auto px = std::min(static_cast<std::uint64_t>(x) * level.probeSpacingPixels +
                                         level.probeSpacingPixels / 2U,
                                     static_cast<std::uint64_t>(gbuffer.width - 1U));
            const auto py = std::min(static_cast<std::uint64_t>(y) * level.probeSpacingPixels +
                                         level.probeSpacingPixels / 2U,
                                     static_cast<std::uint64_t>(gbuffer.height - 1U));
            probes.push_back(pack(gbuffer.texels[py * gbuffer.width + px]));
        }
    return probes;
}

bool execute_radiance_cascade_probe_pass(rhi::IDevice& device,
    const spwi::VoxelGBuffer& gbuffer, const spwi::CascadeLevelInfo& level,
    std::span<const std::byte> bytecode, std::vector<RadianceCascadeSurfaceSample>& probes,
    std::string* error) {
    probes.clear();
    try { validate(gbuffer, &level); }
    catch (const std::invalid_argument& exception) {
        if (error) *error = exception.what();
        return false;
    }
    if (bytecode.empty()) {
        if (error) *error = "radiance cascade probe shader bytecode is empty";
        return false;
    }
    const auto primary = pack_radiance_cascade_primary(gbuffer);
    const auto count = static_cast<std::size_t>(level.probesX) * level.probesY;
    const RadianceCascadeProbeConstants constants{gbuffer.width, gbuffer.height,
        level.probeSpacingPixels, level.probesX, level.probesY, {0, 0, 0}};
    rhi::BufferHandle input{}, output{}, uniform{};
    rhi::BindGroupLayoutHandle layout{};
    rhi::BindGroupHandle group{};
    rhi::ComputePipelineHandle pipeline{};
    auto cleanup = [&] {
        device.wait_idle();
        if (pipeline) device.destroy_compute_pipeline(pipeline);
        if (group) device.destroy_bind_group(group);
        if (layout) device.destroy_bind_group_layout(layout);
        if (uniform) device.destroy_buffer(uniform);
        if (output) device.destroy_buffer(output);
        if (input) device.destroy_buffer(input);
    };
    input = device.create_buffer({primary.size() * sizeof(primary[0]),
        rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDestination,
        rhi::MemoryDomain::Upload, "RC primary samples", rhi::ResourceState::ShaderRead}, error);
    output = device.create_buffer({count * sizeof(primary[0]),
        rhi::BufferUsage::Storage | rhi::BufferUsage::CopySource,
        rhi::MemoryDomain::Readback, "RC probes", rhi::ResourceState::ShaderWrite}, error);
    uniform = device.create_buffer({sizeof(constants), rhi::BufferUsage::Constant |
        rhi::BufferUsage::CopyDestination, rhi::MemoryDomain::Upload,
        "RC probe constants", rhi::ResourceState::ShaderRead}, error);
    if (!input || !output || !uniform ||
        !device.write_buffer(input, 0, std::as_bytes(std::span(primary)), error) ||
        !device.write_buffer(uniform, 0, std::as_bytes(std::span(&constants, 1)), error)) {
        cleanup(); return false;
    }
    rhi::BindGroupLayoutDesc layoutDesc;
    layoutDesc.bindings = {{0U, rhi::BindingType::StorageBufferReadOnly, rhi::ShaderStage::Compute},
                           {1U, rhi::BindingType::StorageBufferReadWrite, rhi::ShaderStage::Compute},
                           {2U, rhi::BindingType::UniformBuffer, rhi::ShaderStage::Compute}};
    layout = device.create_bind_group_layout(layoutDesc, error);
    if (!layout) { cleanup(); return false; }
    rhi::BindGroupDesc groupDesc;
    groupDesc.layout = layout;
    groupDesc.entries = {{0U, input, {}, 0U, primary.size() * sizeof(primary[0]), {}},
                         {1U, output, {}, 0U, count * sizeof(primary[0]), {}},
                         {2U, uniform, {}, 0U, sizeof(constants), {}}};
    group = device.create_bind_group(groupDesc, error);
    if (!group) { cleanup(); return false; }
    rhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.debugName = "BuildRadianceCascadeProbes";
    pipelineDesc.bytecode.assign(bytecode.begin(), bytecode.end());
    pipelineDesc.bindGroupLayouts.push_back(layout);
    pipelineDesc.threadsX = 64U;
    pipeline = device.create_compute_pipeline(pipelineDesc, error);
    if (!pipeline) { cleanup(); return false; }
    const auto commands = device.begin_commands(rhi::QueueKind::Compute,
                                                 "BuildRadianceCascadeProbes", error);
    if (!commands || !device.bind_compute_bind_group(commands, 0U, group, error) ||
        !device.dispatch(commands, pipeline, static_cast<std::uint32_t>((count + 63U) / 64U),
                         1U, 1U, error)) {
        cleanup(); return false;
    }
    const auto fence = device.submit(commands, error);
    if (!fence || !device.wait(fence, error)) { cleanup(); return false; }
    probes.resize(count);
    const bool success = device.read_buffer(output, 0, std::as_writable_bytes(std::span(probes)), error);
    if (!success) probes.clear();
    cleanup();
    return success;
}

} // namespace dve::render
