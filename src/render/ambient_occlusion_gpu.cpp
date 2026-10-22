#include "dve/render/ambient_occlusion_gpu.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

#include "dve/gpu_render_environment.hpp"
#include "dve/render/ambient_occlusion.hpp"
#include "dve/render/brickmap_rhi_mirror.hpp"
#include "dve/transform.hpp"

namespace dve::render {
namespace {
// cbuffer TraceConstants (brick_trace_common.hlsli).
struct TraceConstants {
    Int3 minBrick{};
    std::uint32_t rayCount{};
    Int3 brickExtent{};
    std::uint32_t invalidSlot{kInvalidGpuBrickSlot};
};
// cbuffer AoGenerationConstants (generate_ao_rays.hlsl).
struct AoGenerationConstants {
    std::uint32_t primaryRayCount{};
    float maxDistanceMeters{};
    std::uint32_t frameSeed{};
    std::uint32_t samplesPerHit{};
};
// cbuffer AoResolveConstants (resolve_ao.hlsl).
struct AoResolveConstants {
    std::uint32_t pixelCount{};
    float maxDistanceMeters{};
    std::uint32_t samplesPerHit{};
    std::uint32_t pad{};
};
static_assert(sizeof(TraceConstants) == 32U && sizeof(AoGenerationConstants) == 16U && sizeof(AoResolveConstants) == 16U);

constexpr std::uint32_t t(std::uint32_t r) noexcept { return kGpuShaderResourceShift + r; }
constexpr std::uint32_t u(std::uint32_t r) noexcept { return kGpuUnorderedAccessShift + r; }

std::uint32_t samples_per_hit(const RenderEnvironment& environment) noexcept {
    return std::clamp(environment.ambientOcclusionSamples, 1U, kMaximumAmbientOcclusionSamples);
}

void fail(std::string* error, const std::string& message) {
    if (error && error->empty()) *error = message;
}
}  // namespace

std::vector<GpuTraceRay> gpu_primary_rays(Float3 position, Float3 target, Float3 up, float verticalFieldOfViewRadians,
                                          std::uint32_t width, std::uint32_t height, float maximumDistance) {
    std::vector<GpuTraceRay> rays;
    if (width == 0U || height == 0U) return rays;
    const Float3 forward = normalize(subtract(target, position));
    Float3 right = normalize(Float3{forward.y * up.z - forward.z * up.y, forward.z * up.x - forward.x * up.z,
                                    forward.x * up.y - forward.y * up.x});
    if (!(length(right) > 0.5F)) right = {1.0F, 0.0F, 0.0F};
    const Float3 cameraUp{right.y * forward.z - right.z * forward.y, right.z * forward.x - right.x * forward.z,
                          right.x * forward.y - right.y * forward.x};
    const float tanHalf = std::tan(verticalFieldOfViewRadians * 0.5F);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    rays.reserve(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            const float ndcX = 2.0F * ((static_cast<float>(x) + 0.5F) / static_cast<float>(width)) - 1.0F;
            const float ndcY = 1.0F - 2.0F * ((static_cast<float>(y) + 0.5F) / static_cast<float>(height));
            const Float3 direction = normalize(add(add(forward, multiply(right, ndcX * tanHalf * aspect)),
                                                   multiply(cameraUp, ndcY * tanHalf)));
            rays.push_back({position, maximumDistance, direction, y * width + x});
        }
    return rays;
}

bool execute_gpu_ambient_occlusion(rhi::IDevice& device, const PackedBrickmapScene& scene,
                                   std::span<const GpuTraceRay> primaryRays, const RenderEnvironment& environment,
                                   float metersPerVoxel, std::uint32_t frameSeed, const GpuAmbientOcclusionShaders& shaders,
                                   GpuAmbientOcclusionResult& result, std::string* error) {
    result = {};
    if (primaryRays.empty()) { fail(error, "GPU AO: no primary rays"); return false; }
    if (shaders.trace.empty() || shaders.generate.empty() || shaders.resolve.empty()) {
        fail(error, "GPU AO: shader bytecode is missing");
        return false;
    }
    if (!scene.bounds().valid) { fail(error, "GPU AO: the brickmap is empty"); return false; }
    const auto pixels = static_cast<std::uint32_t>(primaryRays.size());
    const std::uint32_t samples = samples_per_hit(environment);
    const std::uint64_t aoRayCount = static_cast<std::uint64_t>(pixels) * samples;
    if (aoRayCount > 0xFFFFFFFFULL) { fail(error, "GPU AO: too many rays"); return false; }

    PackedBrickmapRhiMirror bricks(device);
    if (!bricks.upload(scene, error)) return false;

    // Everything this pass creates, destroyed on every path out.
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

    const BrickmapBounds& bounds = scene.bounds();
    const TraceConstants primaryTrace{{bounds.minKey.x, bounds.minKey.y, bounds.minKey.z}, pixels, bounds.extent,
                                      kInvalidGpuBrickSlot};
    const TraceConstants aoTrace{primaryTrace.minBrick, static_cast<std::uint32_t>(aoRayCount), bounds.extent,
                                 kInvalidGpuBrickSlot};
    const AoGenerationConstants generation{pixels, environment.ambientOcclusionMaxDistanceMeters, frameSeed, samples};
    const AoResolveConstants resolution{pixels, environment.ambientOcclusionMaxDistanceMeters, samples, 0U};
    const GpuRenderEnvironment gpuEnvironment = pack_gpu_render_environment(environment, pixels, 1U, metersPerVoxel);

    const auto primaryRayBuffer = buffer(primaryRays.size_bytes(), storage, rhi::MemoryDomain::Upload, "AO primary rays",
                                         rhi::ResourceState::ShaderRead);
    const auto primaryResults = buffer(pixels * sizeof(GpuTraceResult), storage, rhi::MemoryDomain::Readback,
                                       "AO primary hits", rhi::ResourceState::ShaderWrite);
    const auto aoRays = buffer(aoRayCount * sizeof(GpuTraceRay), storage, rhi::MemoryDomain::Readback, "AO rays",
                               rhi::ResourceState::ShaderWrite);
    const auto aoResults = buffer(aoRayCount * sizeof(GpuTraceResult), storage, rhi::MemoryDomain::Readback, "AO hits",
                                  rhi::ResourceState::ShaderWrite);
    const auto aoValues = buffer(pixels * sizeof(float), storage, rhi::MemoryDomain::Readback, "AO", rhi::ResourceState::ShaderWrite);
    const auto primaryTraceConstants = buffer(sizeof(TraceConstants), constant, rhi::MemoryDomain::Upload, "AO primary trace constants",
                                              rhi::ResourceState::ShaderRead);
    const auto aoTraceConstants = buffer(sizeof(TraceConstants), constant, rhi::MemoryDomain::Upload, "AO ray trace constants",
                                         rhi::ResourceState::ShaderRead);
    const auto generationConstants = buffer(sizeof(AoGenerationConstants), constant, rhi::MemoryDomain::Upload,
                                            "AO generation constants", rhi::ResourceState::ShaderRead);
    const auto resolveConstants = buffer(sizeof(AoResolveConstants), constant, rhi::MemoryDomain::Upload, "AO resolve constants",
                                         rhi::ResourceState::ShaderRead);
    const auto environmentConstants = buffer(sizeof(GpuRenderEnvironment), constant, rhi::MemoryDomain::Upload,
                                             "AO render environment", rhi::ResourceState::ShaderRead);
    if (std::any_of(buffers.begin(), buffers.end(), [](rhi::BufferHandle h) { return !h; }) ||
        !device.write_buffer(primaryRayBuffer, 0U, std::as_bytes(primaryRays), error) ||
        !device.write_buffer(primaryTraceConstants, 0U, std::as_bytes(std::span(&primaryTrace, 1)), error) ||
        !device.write_buffer(aoTraceConstants, 0U, std::as_bytes(std::span(&aoTrace, 1)), error) ||
        !device.write_buffer(generationConstants, 0U, std::as_bytes(std::span(&generation, 1)), error) ||
        !device.write_buffer(resolveConstants, 0U, std::as_bytes(std::span(&resolution, 1)), error) ||
        !device.write_buffer(environmentConstants, 0U, std::as_bytes(std::span(&gpuEnvironment, 1)), error)) {
        fail(error, "GPU AO: buffer setup failed");
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
    const std::size_t indexBytes = scene.index_grid().size_bytes(), recordBytes = scene.records().size_bytes(),
                      materialBytes = scene.material_arena().size();
    const auto traceLayout = layout_of({{0U, BindingType::UniformBuffer, {}, 0U},
                                        {t(0U), BindingType::StorageBufferReadOnly, {}, 0U},
                                        {t(1U), BindingType::StorageBufferReadOnly, {}, 0U},
                                        {t(2U), BindingType::StorageBufferReadOnly, {}, 0U},
                                        {t(3U), BindingType::StorageBufferReadOnly, {}, 0U},
                                        {u(0U), BindingType::StorageBufferReadWrite, {}, 0U}});
    const auto generateLayout = layout_of({{1U, BindingType::UniformBuffer, {}, 0U},
                                           {2U, BindingType::UniformBuffer, {}, 0U},
                                           {t(5U), BindingType::StorageBufferReadOnly, {}, 0U},
                                           {t(8U), BindingType::StorageBufferReadOnly, {}, 0U},
                                           {u(1U), BindingType::StorageBufferReadWrite, {}, 0U}});
    const auto resolveLayout = layout_of({{1U, BindingType::UniformBuffer, {}, 0U},
                                          {2U, BindingType::UniformBuffer, {}, 0U},
                                          {t(6U), BindingType::StorageBufferReadOnly, {}, 0U},
                                          {u(2U), BindingType::StorageBufferReadWrite, {}, 0U}});
    if (!traceLayout || !generateLayout || !resolveLayout) { fail(error, "GPU AO: bind group layouts"); cleanup(); return false; }
    const std::size_t pixelHits = pixels * sizeof(GpuTraceResult), aoRayBytes = aoRayCount * sizeof(GpuTraceRay),
                      aoHitBytes = aoRayCount * sizeof(GpuTraceResult);
    const auto primaryGroup = group_of(traceLayout, {{0U, {}, primaryTraceConstants, sizeof(TraceConstants)},
                                                     {t(0U), {}, bricks.index_buffer(), indexBytes},
                                                     {t(1U), {}, bricks.record_buffer(), recordBytes},
                                                     {t(2U), {}, bricks.material_buffer(), materialBytes},
                                                     {t(3U), {}, primaryRayBuffer, primaryRays.size_bytes()},
                                                     {u(0U), {}, primaryResults, pixelHits}});
    const auto aoTraceGroup = group_of(traceLayout, {{0U, {}, aoTraceConstants, sizeof(TraceConstants)},
                                                     {t(0U), {}, bricks.index_buffer(), indexBytes},
                                                     {t(1U), {}, bricks.record_buffer(), recordBytes},
                                                     {t(2U), {}, bricks.material_buffer(), materialBytes},
                                                     {t(3U), {}, aoRays, aoRayBytes},
                                                     {u(0U), {}, aoResults, aoHitBytes}});
    const auto generateGroup = group_of(generateLayout, {{1U, {}, generationConstants, sizeof(AoGenerationConstants)},
                                                         {2U, {}, environmentConstants, sizeof(GpuRenderEnvironment)},
                                                         {t(5U), {}, primaryResults, pixelHits},
                                                         {t(8U), {}, primaryRayBuffer, primaryRays.size_bytes()},
                                                         {u(1U), {}, aoRays, aoRayBytes}});
    const auto resolveGroup = group_of(resolveLayout, {{1U, {}, resolveConstants, sizeof(AoResolveConstants)},
                                                       {2U, {}, environmentConstants, sizeof(GpuRenderEnvironment)},
                                                       {t(6U), {}, aoResults, aoHitBytes},
                                                       {u(2U), {}, aoValues, pixels * sizeof(float)}});
    if (!primaryGroup || !aoTraceGroup || !generateGroup || !resolveGroup) { fail(error, "GPU AO: bind groups"); cleanup(); return false; }
    const auto tracePipeline = pipeline_of(shaders.trace, traceLayout, "TraceVoxelRays");
    const auto generatePipeline = pipeline_of(shaders.generate, generateLayout, "GenerateAmbientOcclusionRays");
    const auto resolvePipeline = pipeline_of(shaders.resolve, resolveLayout, "ResolveAmbientOcclusion");
    if (!tracePipeline || !generatePipeline || !resolvePipeline) { fail(error, "GPU AO: compute pipelines"); cleanup(); return false; }

    const auto groupsFor = [](std::uint64_t count) { return static_cast<std::uint32_t>((count + 63U) / 64U); };
    const auto commands = device.begin_commands(rhi::QueueKind::Compute, "AmbientOcclusion", error);
    const auto written = [&](rhi::BufferHandle b) {   // a pass's output becomes the next pass's input
        return device.transition_buffer(commands, b, rhi::ResourceState::ShaderWrite, rhi::ResourceState::ShaderRead, error);
    };
    const bool recorded = commands &&
        device.bind_compute_bind_group(commands, 0U, primaryGroup, error) &&
        device.dispatch(commands, tracePipeline, groupsFor(pixels), 1U, 1U, error) && written(primaryResults) &&
        device.bind_compute_bind_group(commands, 0U, generateGroup, error) &&
        device.dispatch(commands, generatePipeline, groupsFor(aoRayCount), 1U, 1U, error) && written(aoRays) &&
        device.bind_compute_bind_group(commands, 0U, aoTraceGroup, error) &&
        device.dispatch(commands, tracePipeline, groupsFor(aoRayCount), 1U, 1U, error) && written(aoResults) &&
        device.bind_compute_bind_group(commands, 0U, resolveGroup, error) &&
        device.dispatch(commands, resolvePipeline, groupsFor(pixels), 1U, 1U, error);
    if (!recorded) { fail(error, "GPU AO: recording failed"); cleanup(); return false; }
    const auto fence = device.submit(commands, error);
    if (!fence || !device.wait(fence, error)) { fail(error, "GPU AO: submission failed"); cleanup(); return false; }
    result.primary.resize(pixels);
    result.ambientOcclusion.resize(pixels);
    result.samplesPerHit = samples;
    const bool read = device.read_buffer(primaryResults, 0U, std::as_writable_bytes(std::span(result.primary)), error) &&
                      device.read_buffer(aoValues, 0U, std::as_writable_bytes(std::span(result.ambientOcclusion)), error);
    cleanup();
    if (!read) { result = {}; fail(error, "GPU AO: readback failed"); return false; }
    // Pixels whose primary ray missed get zero AO rays, which hit nothing: AO 1, as on the CPU.
    return true;
}

GpuAmbientOcclusionResult reference_gpu_ambient_occlusion(const PackedBrickmapScene& scene,
                                                          std::span<const GpuTraceRay> primaryRays,
                                                          const RenderEnvironment& environment, float metersPerVoxel,
                                                          std::uint32_t frameSeed) {
    GpuAmbientOcclusionResult result;
    result.samplesPerHit = samples_per_hit(environment);
    RenderEnvironment shared = environment;
    shared.ambientOcclusionSamples = result.samplesPerHit;
    result.primary.resize(primaryRays.size());
    result.ambientOcclusion.assign(primaryRays.size(), 1.0F);
    for (std::size_t p = 0; p < primaryRays.size(); ++p) {
        const GpuTraceRay& ray = primaryRays[p];
        const auto hit = raycast_packed_brickmap_hdda(scene, ray.origin, ray.direction, ray.maxDistance);
        GpuTraceResult& out = result.primary[p];
        if (!hit) continue;
        out.voxel = hit->voxel;
        out.hit = 1U;
        out.normal = hit->normal;
        out.material = hit->material;
        out.distance = hit->distance;
        const float magnitude = length(ray.direction);
        const Float3 position = add(ray.origin, multiply(ray.direction, hit->distance / magnitude));
        const Float3 normal = normalize(Float3{static_cast<float>(hit->normal.x), static_cast<float>(hit->normal.y),
                                               static_cast<float>(hit->normal.z)});
        result.ambientOcclusion[p] = reference_ambient_occlusion(
            position, normal, static_cast<std::uint32_t>(p), frameSeed, shared, metersPerVoxel,
            [&](Float3 origin, Float3 direction, float maximum) -> std::optional<float> {
                if (const auto aoHit = raycast_packed_brickmap_hdda(scene, origin, direction, maximum)) return aoHit->distance;
                return std::nullopt;
            });
    }
    return result;
}

} // namespace dve::render
