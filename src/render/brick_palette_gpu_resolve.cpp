#include "dve/render/brick_palette_gpu_resolve.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <utility>

namespace dve::render {
namespace {

void set_error(std::string* error, std::string_view message) {
    if (error != nullptr) error->assign(message.begin(), message.end());
}

[[nodiscard]] std::size_t grown_capacity(std::size_t current, std::size_t required) noexcept {
    std::size_t capacity = std::max<std::size_t>(current, 256U);
    while (capacity < required) capacity = capacity + capacity / 2U;
    return capacity;
}

template<class Value>
[[nodiscard]] std::span<const std::byte> bytes_of(std::span<const Value> values) noexcept {
    return std::as_bytes(values);
}

[[nodiscard]] BrickPaletteSurfacePoint surface_point(
    const BrickPaletteGpuResolveRequest& request) noexcept {
    return {
        {request.worldPositionX, request.worldPositionY, request.worldPositionZ},
        {request.worldNormalX, request.worldNormalY, request.worldNormalZ},
        request.assetU,
        request.assetV};
}

[[nodiscard]] BrickPaletteShadingConfig shading_config(
    const BrickPaletteGpuBrickRecord& record, bool& valid) noexcept {
    BrickPaletteShadingConfig config;
    if (record.mappingMode == static_cast<std::uint32_t>(BrickPaletteMappingMode::WorldTriplanar)) {
        config.mapping = BrickPaletteMappingMode::WorldTriplanar;
    } else if (record.mappingMode == static_cast<std::uint32_t>(BrickPaletteMappingMode::AssetUv)) {
        config.mapping = BrickPaletteMappingMode::AssetUv;
    } else {
        valid = false;
    }
    return config;
}

[[nodiscard]] BrickPaletteGpuBakedSample pack_resolved_sample(
    const BrickPaletteShadedSample& sample) noexcept {
    BrickPaletteGpuBakedSample packed;
    packed.baseColorOpacity = {
        sample.baseColor.x, sample.baseColor.y, sample.baseColor.z, sample.opacity};
    packed.normalRoughness = {
        sample.worldNormal.x, sample.worldNormal.y, sample.worldNormal.z, sample.roughness};
    packed.emissiveMetallic = {
        sample.emissive.x, sample.emissive.y, sample.emissive.z, sample.metallic};
    packed.valid = sample.valid ? 1U : 0U;
    packed.textureSamples = sample.textureSamples;
    packed.slotsEvaluated = sample.slotsEvaluated;
    return packed;
}

[[nodiscard]] BrickPaletteShadedSample unpack_baked_sample(
    const BrickPaletteGpuBakedSample& sample) noexcept {
    BrickPaletteShadedSample result;
    result.baseColor = {
        sample.baseColorOpacity[0U], sample.baseColorOpacity[1U], sample.baseColorOpacity[2U]};
    result.opacity = sample.baseColorOpacity[3U];
    result.worldNormal = {
        sample.normalRoughness[0U], sample.normalRoughness[1U], sample.normalRoughness[2U]};
    result.roughness = sample.normalRoughness[3U];
    result.emissive = {
        sample.emissiveMetallic[0U], sample.emissiveMetallic[1U], sample.emissiveMetallic[2U]};
    result.metallic = sample.emissiveMetallic[3U];
    result.valid = sample.valid != 0U;
    result.textureSamples = sample.textureSamples;
    result.slotsEvaluated = static_cast<std::uint8_t>(
        std::min<std::uint32_t>(sample.slotsEvaluated, std::numeric_limits<std::uint8_t>::max()));
    return result;
}

[[nodiscard]] BrickPaletteSampleEncoding unpack_palette_sample(
    const BrickPaletteGpuSampleEncoding& packed, std::uint32_t slotCount) noexcept {
    BrickPaletteSampleEncoding encoding;
    encoding.usedSlots = static_cast<std::uint8_t>(
        std::min<std::uint32_t>(slotCount, kBrickPaletteMaximumSlots));
    for (std::size_t index = 0U; index < kBrickPaletteMaximumSlots; ++index) {
        const auto shift = static_cast<std::uint32_t>(index * 8U);
        encoding.slotIndices[index] = static_cast<std::uint8_t>(
            (packed.packedSlotIndices >> shift) & 0xFFU);
        encoding.quantizedWeights[index] = static_cast<std::uint8_t>(
            (packed.packedWeights >> shift) & 0xFFU);
    }
    return encoding;
}

} // namespace

BrickPaletteGpuMaterialRecord pack_brick_palette_gpu_material(
    const BrickPaletteMaterialRecord& material) noexcept {
    BrickPaletteGpuMaterialRecord packed;
    packed.globalMaterialId = material.globalMaterialId;
    packed.baseColorX = material.baseColor.x;
    packed.baseColorY = material.baseColor.y;
    packed.baseColorZ = material.baseColor.z;
    packed.roughness = material.roughness;
    packed.metallic = material.metallic;
    packed.emissiveX = material.emissive.x;
    packed.emissiveY = material.emissive.y;
    packed.emissiveZ = material.emissive.z;
    packed.opacity = material.opacity;
    packed.normalX = material.normal.x;
    packed.normalY = material.normal.y;
    packed.normalZ = material.normal.z;
    packed.textureScale = material.textureScale;
    packed.detailContrast = material.detailContrast;
    return packed;
}

std::vector<BrickPaletteGpuMaterialRecord> pack_brick_palette_gpu_materials(
    std::span<const BrickPaletteMaterialRecord> materials) {
    std::vector<BrickPaletteGpuMaterialRecord> packed;
    packed.reserve(materials.size());
    for (const auto& material : materials) packed.push_back(pack_brick_palette_gpu_material(material));
    return packed;
}

BrickPaletteShadedSample resolve_brick_palette_upload_request(
    const BrickPaletteUploadPacket& packet,
    std::span<const BrickPaletteMaterialRecord> materials,
    const BrickPaletteGpuResolveRequest& request,
    std::string* error) {
    BrickPaletteShadedSample invalid;
    invalid.worldNormal = {
        request.worldNormalX, request.worldNormalY, request.worldNormalZ};
    if (!validate_brick_palette_upload_packet(packet, error)) return invalid;
    if (request.brickRecordIndex >= packet.records.size()) {
        set_error(error, "brick-palette resolve request references an invalid brick record");
        return invalid;
    }
    const auto& record = packet.records[request.brickRecordIndex];
    if (request.sampleIndex >= record.sampleCount) {
        set_error(error, "brick-palette resolve request references an invalid sample");
        return invalid;
    }
    bool validMapping = true;
    const auto config = shading_config(record, validMapping);
    if (!validMapping) {
        set_error(error, "brick-palette resolve request uses an invalid mapping mode");
        return invalid;
    }
    const auto point = surface_point(request);
    const auto path = static_cast<VoxelMaterialRuntimePath>(record.runtimePath);

    if (path == VoxelMaterialRuntimePath::BakedProperties) {
        const std::size_t index = static_cast<std::size_t>(record.bakedOffset) + request.sampleIndex;
        if (record.bakedOffset == kInvalidBrickPaletteOffset || index >= packet.bakedSamples.size()) {
            set_error(error, "brick-palette baked resolve range is invalid");
            return invalid;
        }
        return unpack_baked_sample(packet.bakedSamples[index]);
    }

    CookedBrickPalette palette;
    palette.valid = true;
    BrickPaletteSampleEncoding encoding;
    if (path == VoxelMaterialRuntimePath::SingleMaterial) {
        const std::size_t index = static_cast<std::size_t>(record.singleOffset) + request.sampleIndex;
        if (record.singleOffset == kInvalidBrickPaletteOffset || index >= packet.singleMaterialIds.size()) {
            set_error(error, "brick-palette single-material resolve range is invalid");
            return invalid;
        }
        palette.runtimePath = path;
        palette.encoding = BrickPaletteEncoding::Single;
        palette.slots.push_back(packet.singleMaterialIds[index]);
        encoding.usedSlots = 1U;
        encoding.slotIndices[0U] = 0U;
        encoding.quantizedWeights[0U] = kBrickPaletteWeightDenominator;
    } else if (path == VoxelMaterialRuntimePath::DeferredPalette2 ||
               path == VoxelMaterialRuntimePath::DeferredPalette4) {
        palette.runtimePath = path;
        palette.encoding = path == VoxelMaterialRuntimePath::DeferredPalette4
            ? BrickPaletteEncoding::Palette4 : BrickPaletteEncoding::Palette2;
        palette.slots.reserve(record.paletteSlotCount);
        for (std::uint32_t slot = 0U; slot < record.paletteSlotCount; ++slot) {
            const std::size_t slotIndex = static_cast<std::size_t>(record.paletteSlotOffset) + slot;
            if (record.paletteSlotOffset == kInvalidBrickPaletteOffset ||
                slotIndex >= packet.paletteSlots.size()) {
                set_error(error, "brick-palette slot resolve range is invalid");
                return invalid;
            }
            const std::uint32_t remapIndex = packet.paletteSlots[slotIndex];
            if (remapIndex >= packet.remapGlobalMaterialIds.size()) {
                set_error(error, "brick-palette slot resolve references an invalid remap index");
                return invalid;
            }
            palette.slots.push_back(packet.remapGlobalMaterialIds[remapIndex]);
        }
        const std::size_t sampleIndex = static_cast<std::size_t>(record.sampleOffset) + request.sampleIndex;
        if (record.sampleOffset == kInvalidBrickPaletteOffset || sampleIndex >= packet.paletteSamples.size()) {
            set_error(error, "brick-palette sample resolve range is invalid");
            return invalid;
        }
        encoding = unpack_palette_sample(packet.paletteSamples[sampleIndex], record.paletteSlotCount);
    } else {
        set_error(error, "brick-palette resolve request has no executable runtime path");
        return invalid;
    }

    return shade_brick_palette_encoding(palette, materials, encoding, point, config, nullptr);
}

bool resolve_brick_palette_upload_requests(
    const BrickPaletteUploadPacket& packet,
    std::span<const BrickPaletteMaterialRecord> materials,
    std::span<const BrickPaletteGpuResolveRequest> requests,
    std::span<BrickPaletteGpuBakedSample> outputs,
    std::string* error) {
    if (outputs.size() < requests.size()) {
        set_error(error, "brick-palette resolve output span is smaller than the request span");
        return false;
    }
    if (!validate_brick_palette_upload_packet(packet, error)) return false;
    for (std::size_t index = 0U; index < requests.size(); ++index) {
        std::string local;
        const auto resolved = resolve_brick_palette_upload_request(
            packet, materials, requests[index], &local);
        if (!local.empty()) {
            set_error(error, local);
            return false;
        }
        outputs[index] = pack_resolved_sample(resolved);
    }
    return true;
}

rhi::BindGroupLayoutDesc brick_palette_resolve_bind_group_layout_desc() {
    rhi::BindGroupLayoutDesc desc;
    desc.debugName = "DVE brick palette packed resolve resources";
    for (std::uint32_t binding = 0U; binding < kBrickPaletteResolveBindingCount; ++binding) {
        const auto type = binding == kBrickPaletteResolveOutputBinding
            ? rhi::BindingType::StorageBufferReadWrite
            : rhi::BindingType::StorageBufferReadOnly;
        desc.bindings.push_back({binding, type, rhi::ShaderStage::Compute});
    }
    return desc;
}

BrickPaletteResolveRhiHarness::~BrickPaletteResolveRhiHarness() {
    reset();
}

bool BrickPaletteResolveRhiHarness::ensure_buffer(
    rhi::BufferHandle& handle, std::size_t& capacity, std::size_t requiredBytes,
    rhi::BufferUsage usage, std::string_view debugName, std::string* error) {
    const std::size_t required = std::max<std::size_t>(requiredBytes, 4U);
    if (handle && capacity >= required) return true;
    if (group_ || layout_ || pipeline_) {
        set_error(error, "brick-palette resolve buffers cannot be reallocated while bindings or pipeline are live");
        return false;
    }
    if (handle && !device_.destroy_buffer(handle, error)) return false;
    capacity = grown_capacity(0U, required);
    rhi::BufferDesc desc;
    desc.bytes = capacity;
    desc.usage = usage | rhi::BufferUsage::CopyDestination | rhi::BufferUsage::CopySource;
    desc.memory = rhi::MemoryDomain::DeviceLocal;
    desc.initialState = rhi::ResourceState::ShaderRead;
    desc.debugName.assign(debugName.begin(), debugName.end());
    handle = device_.create_buffer(desc, error);
    if (!handle) {
        capacity = 0U;
        return false;
    }
    return true;
}

bool BrickPaletteResolveRhiHarness::destroy_bindings(std::string* error) noexcept {
    bool ok = true;
    std::string local;
    if (pipeline_ && !device_.destroy_compute_pipeline(pipeline_, &local)) ok = false;
    if (!ok && error != nullptr && error->empty()) *error = local;
    local.clear();
    pipeline_ = {};
    if (group_ && !device_.destroy_bind_group(group_, &local)) ok = false;
    if (!ok && error != nullptr && error->empty()) *error = local;
    local.clear();
    group_ = {};
    if (layout_ && !device_.destroy_bind_group_layout(layout_, &local)) ok = false;
    if (!ok && error != nullptr && error->empty()) *error = local;
    layout_ = {};
    return ok;
}

bool BrickPaletteResolveRhiHarness::publish(
    const BrickPaletteRhiMirror& paletteMirror,
    std::span<const BrickPaletteGpuMaterialRecord> materials,
    std::span<const BrickPaletteGpuResolveRequest> requests,
    std::string* error) {
    if (layout_ || group_ || pipeline_) {
        set_error(error, "brick-palette resolve harness is already published");
        return false;
    }
    const auto materialBytes = bytes_of(materials);
    const auto requestBytes = bytes_of(requests);
    const std::size_t outputBytes = requests.size() * sizeof(BrickPaletteGpuBakedSample);
    if (requests.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        set_error(error, "brick-palette resolve request count exceeds 32-bit dispatch limits");
        return false;
    }
    if (!ensure_buffer(materialBuffer_, stats_.materialCapacityBytes, materialBytes.size(),
                       rhi::BufferUsage::Storage, "DVE brick palette resolve materials", error) ||
        !ensure_buffer(requestBuffer_, stats_.requestCapacityBytes, requestBytes.size(),
                       rhi::BufferUsage::Storage, "DVE brick palette resolve requests", error) ||
        !ensure_buffer(outputBuffer_, stats_.outputCapacityBytes, outputBytes,
                       rhi::BufferUsage::Storage, "DVE brick palette resolve outputs", error)) {
        return false;
    }
    if (!materialBytes.empty() && !device_.write_buffer(materialBuffer_, 0U, materialBytes, error)) return false;
    if (!requestBytes.empty() && !device_.write_buffer(requestBuffer_, 0U, requestBytes, error)) return false;
    if (outputBytes != 0U) {
        std::vector<std::byte> zero(outputBytes);
        if (!device_.write_buffer(outputBuffer_, 0U, zero, error)) return false;
    }

    layout_ = device_.create_bind_group_layout(brick_palette_resolve_bind_group_layout_desc(), error);
    if (!layout_) return false;
    rhi::BindGroupDesc desc;
    desc.layout = layout_;
    desc.debugName = "DVE brick palette packed resolve bind group";
    const std::array<rhi::BufferHandle, kBrickPaletteBindingCount> paletteBuffers{
        paletteMirror.record_buffer(), paletteMirror.remap_buffer(), paletteMirror.slot_buffer(),
        paletteMirror.sample_buffer(), paletteMirror.baked_buffer(), paletteMirror.single_buffer()};
    const auto& paletteCapacities = paletteMirror.stats().capacityBytes;
    for (std::uint32_t binding = 0U; binding < kBrickPaletteBindingCount; ++binding) {
        if (!paletteBuffers[binding] || paletteCapacities[binding] == 0U) {
            set_error(error, "brick-palette resolve harness requires a published palette mirror");
            (void)destroy_bindings(nullptr);
            return false;
        }
        desc.entries.push_back({binding, paletteBuffers[binding], {}, 0U,
                                paletteCapacities[binding], {}});
    }
    desc.entries.push_back({kBrickPaletteResolveMaterialBinding, materialBuffer_, {}, 0U,
                            stats_.materialCapacityBytes, {}});
    desc.entries.push_back({kBrickPaletteResolveRequestBinding, requestBuffer_, {}, 0U,
                            stats_.requestCapacityBytes, {}});
    desc.entries.push_back({kBrickPaletteResolveOutputBinding, outputBuffer_, {}, 0U,
                            stats_.outputCapacityBytes, {}});
    group_ = device_.create_bind_group(desc, error);
    if (!group_) {
        (void)destroy_bindings(nullptr);
        return false;
    }
    publishedRequestCount_ = static_cast<std::uint32_t>(requests.size());
    stats_.uploadedBytes += materialBytes.size() + requestBytes.size() + outputBytes;
    ++stats_.publications;
    return true;
}

bool BrickPaletteResolveRhiHarness::create_pipeline(
    std::span<const std::byte> bytecode, std::string_view entryPoint, std::string* error) {
    if (!layout_ || !group_) {
        set_error(error, "brick-palette resolve resources must be published before pipeline creation");
        return false;
    }
    if (pipeline_) {
        set_error(error, "brick-palette resolve pipeline is already initialized");
        return false;
    }
    if (bytecode.empty() || entryPoint.empty()) {
        set_error(error, "brick-palette resolve pipeline requires shader bytecode and an entry point");
        return false;
    }
    rhi::ComputePipelineDesc desc;
    desc.debugName = "DVE brick palette packed resolve";
    desc.entryPoint.assign(entryPoint.begin(), entryPoint.end());
    desc.bytecode.assign(bytecode.begin(), bytecode.end());
    desc.bindGroupLayouts = {layout_};
    desc.threadsX = kBrickPaletteResolveThreads;
    pipeline_ = device_.create_compute_pipeline(desc, error);
    return static_cast<bool>(pipeline_);
}

bool BrickPaletteResolveRhiHarness::dispatch_and_wait(
    std::uint32_t requestCount, std::string* error) {
    if (!pipeline_ || !group_) {
        set_error(error, "brick-palette resolve pipeline and resources are not initialized");
        return false;
    }
    if (requestCount == 0U || requestCount > publishedRequestCount_) {
        set_error(error, "brick-palette resolve dispatch count is outside the published request range");
        return false;
    }
    const auto commands = device_.begin_commands(
        rhi::QueueKind::Compute, "DVE brick palette packed resolve", error);
    if (!commands) return false;
    if (!device_.bind_compute_bind_group(commands, 0U, group_, error)) return false;
    const std::uint32_t groups = (requestCount + kBrickPaletteResolveThreads - 1U) /
                                 kBrickPaletteResolveThreads;
    if (!device_.dispatch(commands, pipeline_, groups, 1U, 1U, error)) return false;
    const auto fence = device_.submit(commands, error);
    if (!fence || !device_.wait(fence, error)) return false;
    ++stats_.dispatches;
    return true;
}

bool BrickPaletteResolveRhiHarness::readback(
    std::span<BrickPaletteGpuBakedSample> outputs, std::string* error) {
    if (outputs.size() > publishedRequestCount_) {
        set_error(error, "brick-palette resolve readback exceeds the published output range");
        return false;
    }
    if (outputs.empty()) return true;
    return device_.read_buffer(outputBuffer_, 0U, std::as_writable_bytes(outputs), error);
}

void BrickPaletteResolveRhiHarness::reset() noexcept {
    (void)destroy_bindings(nullptr);
    if (outputBuffer_) (void)device_.destroy_buffer(outputBuffer_, nullptr);
    if (requestBuffer_) (void)device_.destroy_buffer(requestBuffer_, nullptr);
    if (materialBuffer_) (void)device_.destroy_buffer(materialBuffer_, nullptr);
    outputBuffer_ = {};
    requestBuffer_ = {};
    materialBuffer_ = {};
    publishedRequestCount_ = 0U;
    stats_.materialCapacityBytes = 0U;
    stats_.requestCapacityBytes = 0U;
    stats_.outputCapacityBytes = 0U;
}

bool load_brick_palette_resolve_spirv(
    const std::filesystem::path& path, std::vector<std::byte>& bytecode,
    std::string* error) {
    bytecode.clear();
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        set_error(error, "brick-palette resolve SPIR-V file could not be opened");
        return false;
    }
    const auto end = stream.tellg();
    if (end <= 0 || static_cast<std::uint64_t>(end) % sizeof(std::uint32_t) != 0U) {
        set_error(error, "brick-palette resolve SPIR-V byte count is invalid");
        return false;
    }
    bytecode.resize(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(bytecode.data()),
                static_cast<std::streamsize>(bytecode.size()));
    if (!stream) {
        bytecode.clear();
        set_error(error, "brick-palette resolve SPIR-V file could not be read");
        return false;
    }
    std::uint32_t magic{};
    std::memcpy(&magic, bytecode.data(), sizeof(magic));
    if (magic != 0x07230203U) {
        bytecode.clear();
        set_error(error, "brick-palette resolve shader is not SPIR-V");
        return false;
    }
    return true;
}

} // namespace dve::render
