#include "dve/render/atmosphere_gpu.hpp"

#include <algorithm>
#include <cmath>

#include "dve/gpu_render_environment.hpp"
#include "dve/transform.hpp"

namespace dve::render {
namespace {
constexpr std::uint32_t t(std::uint32_t r) noexcept { return kGpuShaderResourceShift + r; }
constexpr std::uint32_t u(std::uint32_t r) noexcept { return kGpuUnorderedAccessShift + r; }
void fail(std::string* error, const std::string& message) {
    if (error && error->empty()) *error = message;
}
} // namespace

GpuAtmosphereConstants pack_gpu_atmosphere(const RenderEnvironment& environment) noexcept {
    const AtmosphereParameters p = atmosphere_parameters(environment);
    constexpr float km = 1000.0F;
    GpuAtmosphereConstants c;
    c.radii = {p.bottomRadiusMeters / km, p.topRadiusMeters / km, p.rayleighScaleHeightMeters / km, p.mieScaleHeightMeters / km};
    c.rayleigh = {p.rayleighScattering.x * km, p.rayleighScattering.y * km, p.rayleighScattering.z * km, p.mieScattering * km};
    c.ozone = {p.ozoneAbsorption.x * km, p.ozoneAbsorption.y * km, p.ozoneAbsorption.z * km, p.mieAbsorption * km};
    c.shape = {p.ozoneCenterAltitudeMeters / km, p.ozoneHalfWidthMeters / km, p.mieAnisotropy, p.groundAlbedo};
    // The CPU keeps the observer at least 1 m up (AtmosphereModel).
    const float altitude = std::max(environment.observerAltitudeMeters, 1.0F);
    c.view = {altitude / km, normalize(environment.sunDirection).y, 0.0F, 0.0F};
    return c;
}

bool execute_gpu_atmosphere(rhi::IDevice& device, const RenderEnvironment& environment, const GpuAtmosphereShaders& shaders,
                            GpuAtmosphereTables& tables, std::string* error) {
    tables = {};
    if (shaders.transmittance.empty() || shaders.multipleScattering.empty() || shaders.skyView.empty()) {
        fail(error, "GPU atmosphere: shader bytecode is missing");
        return false;
    }
    constexpr std::uint32_t transmittanceCount = AtmosphereTables::kTransmittanceWidth * AtmosphereTables::kTransmittanceHeight;
    constexpr std::uint32_t msCount = AtmosphereTables::kMultipleScatteringSize * AtmosphereTables::kMultipleScatteringSize;
    constexpr std::uint32_t skyCount = AtmosphereModel::kSkyViewWidth * AtmosphereModel::kSkyViewHeight;
    const GpuAtmosphereConstants constants = pack_gpu_atmosphere(environment);

    std::vector<rhi::BufferHandle> buffers;
    std::vector<rhi::BindGroupLayoutHandle> layouts;
    std::vector<rhi::BindGroupHandle> groups;
    std::vector<rhi::ComputePipelineHandle> pipelines;
    const auto cleanup = [&] {
        device.wait_idle();
        for (auto h : pipelines) if (h) device.destroy_compute_pipeline(h);
        for (auto h : groups) if (h) device.destroy_bind_group(h);
        for (auto h : layouts) if (h) device.destroy_bind_group_layout(h);
        for (auto h : buffers) if (h) device.destroy_buffer(h);
    };
    const auto buffer = [&](std::size_t bytes, rhi::BufferUsage usage, rhi::MemoryDomain memory, const char* name,
                            rhi::ResourceState state) {
        const auto handle = device.create_buffer({std::max<std::size_t>(bytes, 4U), usage, memory, name, state}, error);
        buffers.push_back(handle);
        return handle;
    };
    const auto storage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopySource | rhi::BufferUsage::CopyDestination;
    const auto constant = rhi::BufferUsage::Constant | rhi::BufferUsage::CopyDestination;
    const std::size_t tBytes = transmittanceCount * sizeof(Float4), mBytes = msCount * sizeof(Float4), sBytes = skyCount * sizeof(Float4);
    const auto constantBuffer = buffer(sizeof(GpuAtmosphereConstants), constant, rhi::MemoryDomain::Upload, "Atmosphere constants",
                                       rhi::ResourceState::ShaderRead);
    const auto transmittance = buffer(tBytes, storage, rhi::MemoryDomain::Readback, "Atmosphere transmittance", rhi::ResourceState::ShaderWrite);
    const auto multiple = buffer(mBytes, storage, rhi::MemoryDomain::Readback, "Atmosphere multiple scattering", rhi::ResourceState::ShaderWrite);
    const auto sky = buffer(sBytes, storage, rhi::MemoryDomain::Readback, "Atmosphere sky view", rhi::ResourceState::ShaderWrite);
    if (std::any_of(buffers.begin(), buffers.end(), [](rhi::BufferHandle h) { return !h; }) ||
        !device.write_buffer(constantBuffer, 0U, std::as_bytes(std::span(&constants, 1)), error)) {
        fail(error, "GPU atmosphere: buffer setup failed");
        cleanup();
        return false;
    }

    using rhi::BindingType;
    struct Slot { std::uint32_t binding; BindingType type; rhi::BufferHandle buffer; std::size_t bytes; };
    const auto layout_of = [&](std::initializer_list<Slot> slots) {
        rhi::BindGroupLayoutDesc desc;
        for (const Slot& s : slots) desc.bindings.push_back({s.binding, s.type, rhi::ShaderStage::Compute});
        const auto handle = device.create_bind_group_layout(desc, error);
        layouts.push_back(handle);
        return handle;
    };
    const auto group_of = [&](rhi::BindGroupLayoutHandle layout, std::initializer_list<Slot> slots) {
        rhi::BindGroupDesc desc;
        desc.layout = layout;
        for (const Slot& s : slots) desc.entries.push_back({s.binding, s.buffer, {}, 0U, std::max<std::size_t>(s.bytes, 4U), {}});
        const auto handle = device.create_bind_group(desc, error);
        groups.push_back(handle);
        return handle;
    };
    const auto pipeline_of = [&](std::span<const std::byte> bytecode, rhi::BindGroupLayoutHandle layout, const char* name) {
        rhi::ComputePipelineDesc desc;
        desc.debugName = name;
        desc.bytecode.assign(bytecode.begin(), bytecode.end());
        desc.bindGroupLayouts.push_back(layout);
        desc.threadsX = 64U;
        const auto handle = device.create_compute_pipeline(desc, error);
        pipelines.push_back(handle);
        return handle;
    };
    const auto transmittanceLayout = layout_of({{0U, BindingType::UniformBuffer, {}, 0U}, {u(0U), BindingType::StorageBufferReadWrite, {}, 0U}});
    const auto multipleLayout = layout_of({{0U, BindingType::UniformBuffer, {}, 0U},
                                           {t(0U), BindingType::StorageBufferReadOnly, {}, 0U},
                                           {u(0U), BindingType::StorageBufferReadWrite, {}, 0U}});
    const auto skyLayout = layout_of({{0U, BindingType::UniformBuffer, {}, 0U},
                                      {t(0U), BindingType::StorageBufferReadOnly, {}, 0U},
                                      {t(1U), BindingType::StorageBufferReadOnly, {}, 0U},
                                      {u(0U), BindingType::StorageBufferReadWrite, {}, 0U}});
    if (!transmittanceLayout || !multipleLayout || !skyLayout) { fail(error, "GPU atmosphere: bind group layouts"); cleanup(); return false; }
    const auto transmittanceGroup = group_of(transmittanceLayout, {{0U, {}, constantBuffer, sizeof(GpuAtmosphereConstants)},
                                                                   {u(0U), {}, transmittance, tBytes}});
    const auto multipleGroup = group_of(multipleLayout, {{0U, {}, constantBuffer, sizeof(GpuAtmosphereConstants)},
                                                         {t(0U), {}, transmittance, tBytes},
                                                         {u(0U), {}, multiple, mBytes}});
    const auto skyGroup = group_of(skyLayout, {{0U, {}, constantBuffer, sizeof(GpuAtmosphereConstants)},
                                               {t(0U), {}, transmittance, tBytes},
                                               {t(1U), {}, multiple, mBytes},
                                               {u(0U), {}, sky, sBytes}});
    if (!transmittanceGroup || !multipleGroup || !skyGroup) { fail(error, "GPU atmosphere: bind groups"); cleanup(); return false; }
    const auto transmittancePipeline = pipeline_of(shaders.transmittance, transmittanceLayout, "AtmosphereTransmittance");
    const auto multiplePipeline = pipeline_of(shaders.multipleScattering, multipleLayout, "AtmosphereMultipleScattering");
    const auto skyPipeline = pipeline_of(shaders.skyView, skyLayout, "AtmosphereSkyView");
    if (!transmittancePipeline || !multiplePipeline || !skyPipeline) { fail(error, "GPU atmosphere: compute pipelines"); cleanup(); return false; }

    const auto groupsFor = [](std::uint32_t count) { return (count + 63U) / 64U; };
    const auto commands = device.begin_commands(rhi::QueueKind::Compute, "Atmosphere", error);
    const auto written = [&](rhi::BufferHandle b) {
        return device.transition_buffer(commands, b, rhi::ResourceState::ShaderWrite, rhi::ResourceState::ShaderRead, error);
    };
    const bool recorded = commands &&
        device.bind_compute_bind_group(commands, 0U, transmittanceGroup, error) &&
        device.dispatch(commands, transmittancePipeline, groupsFor(transmittanceCount), 1U, 1U, error) && written(transmittance) &&
        device.bind_compute_bind_group(commands, 0U, multipleGroup, error) &&
        device.dispatch(commands, multiplePipeline, groupsFor(msCount), 1U, 1U, error) && written(multiple) &&
        device.bind_compute_bind_group(commands, 0U, skyGroup, error) &&
        device.dispatch(commands, skyPipeline, groupsFor(skyCount), 1U, 1U, error);
    if (!recorded) { fail(error, "GPU atmosphere: recording failed"); cleanup(); return false; }
    const auto fence = device.submit(commands, error);
    if (!fence || !device.wait(fence, error)) { fail(error, "GPU atmosphere: submission failed"); cleanup(); return false; }
    tables.transmittance.resize(transmittanceCount);
    tables.multipleScattering.resize(msCount);
    tables.skyView.resize(skyCount);
    const bool read = device.read_buffer(transmittance, 0U, std::as_writable_bytes(std::span(tables.transmittance)), error) &&
                      device.read_buffer(multiple, 0U, std::as_writable_bytes(std::span(tables.multipleScattering)), error) &&
                      device.read_buffer(sky, 0U, std::as_writable_bytes(std::span(tables.skyView)), error);
    cleanup();
    if (!read) { tables = {}; fail(error, "GPU atmosphere: readback failed"); return false; }
    return true;
}

} // namespace dve::render
