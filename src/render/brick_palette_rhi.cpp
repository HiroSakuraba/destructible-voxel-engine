#include "dve/render/brick_palette_rhi.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace dve::render {
namespace {

constexpr std::uint64_t kHashOffset = 1469598103934665603ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kHashPrime;
}

template<class Integer>
void hash_integer(std::uint64_t& hash, Integer value) noexcept {
    static_assert(std::is_integral_v<Integer>);
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t shift = 0U; shift < sizeof(Unsigned) * 8U; shift += 8U)
        hash_byte(hash, static_cast<std::uint8_t>((bits >> shift) & static_cast<Unsigned>(0xFFU)));
}

template<class Value>
void hash_trivial(std::uint64_t& hash, const Value& value) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    const auto bytes = std::as_bytes(std::span<const Value>(&value, 1U));
    for (const std::byte byte : bytes) hash_byte(hash, std::to_integer<std::uint8_t>(byte));
}

void set_error(std::string* error, std::string_view message) {
    if (error != nullptr) error->assign(message.begin(), message.end());
}

[[nodiscard]] VoxelMaterialRuntimePath fallback_path(
    const VoxelMaterialRepresentationSet& representations,
    VoxelMaterialRuntimePath preferred,
    bool allowFallback) noexcept {
    if (voxel_material_representation_available(representations, preferred)) return preferred;
    if (!allowFallback) return VoxelMaterialRuntimePath::NoPath;
    constexpr std::array<VoxelMaterialRuntimePath, 4U> paths{
        VoxelMaterialRuntimePath::BakedProperties,
        VoxelMaterialRuntimePath::SingleMaterial,
        VoxelMaterialRuntimePath::DeferredPalette2,
        VoxelMaterialRuntimePath::DeferredPalette4};
    for (const auto path : paths)
        if (voxel_material_representation_available(representations, path)) return path;
    return VoxelMaterialRuntimePath::NoPath;
}

[[nodiscard]] const CookedBrickPalette* palette_for_path(
    const VoxelMaterialRepresentationSet& representations,
    VoxelMaterialRuntimePath path) noexcept {
    if (path == VoxelMaterialRuntimePath::DeferredPalette2 &&
        representations.deferredPalette2.has_value())
        return &*representations.deferredPalette2;
    if (path == VoxelMaterialRuntimePath::DeferredPalette4 &&
        representations.deferredPalette4.has_value())
        return &*representations.deferredPalette4;
    return nullptr;
}

[[nodiscard]] std::uint32_t checked_offset(std::size_t value) {
    if (value > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        throw std::length_error("brick-palette upload packet exceeds 32-bit offsets");
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] BrickPaletteGpuSampleEncoding pack_sample(
    const BrickPaletteSampleEncoding& sample) noexcept {
    BrickPaletteGpuSampleEncoding packed;
    for (std::size_t index = 0U; index < kBrickPaletteMaximumSlots; ++index) {
        packed.packedSlotIndices |= static_cast<std::uint32_t>(sample.slotIndices[index]) << (index * 8U);
        packed.packedWeights |= static_cast<std::uint32_t>(sample.quantizedWeights[index]) << (index * 8U);
    }
    return packed;
}

[[nodiscard]] BrickPaletteGpuBakedSample pack_baked(
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

[[nodiscard]] std::size_t grown_capacity(std::size_t current, std::size_t required) noexcept {
    std::size_t capacity = std::max<std::size_t>(current, 256U);
    while (capacity < required) capacity = capacity + capacity / 2U;
    return capacity;
}

template<class Value>
[[nodiscard]] std::span<const std::byte> bytes_of(std::span<const Value> values) noexcept {
    return std::as_bytes(values);
}

template<class Value>
[[nodiscard]] std::span<const std::byte> bytes_of(const std::vector<Value>& values) noexcept {
    return std::as_bytes(std::span<const Value>(values));
}

} // namespace

BrickPalettePipelineContract brick_palette_pipeline_contract(
    VoxelMaterialRuntimePath path, BrickPaletteMappingMode mapping) noexcept {
    BrickPalettePipelineContract contract;
    contract.runtimePath = path;
    contract.mapping = mapping;
    contract.maximumSlots = path == VoxelMaterialRuntimePath::DeferredPalette4 ? 4U : 2U;
    if (path == VoxelMaterialRuntimePath::DeferredPalette4) {
        contract.variant = mapping == BrickPaletteMappingMode::WorldTriplanar
            ? BrickPalettePipelineVariant::Palette4WorldTriplanar
            : BrickPalettePipelineVariant::Palette4AssetUv;
    } else {
        contract.variant = mapping == BrickPaletteMappingMode::WorldTriplanar
            ? BrickPalettePipelineVariant::Palette2WorldTriplanar
            : BrickPalettePipelineVariant::Palette2AssetUv;
    }
    return contract;
}

std::array<BrickPalettePipelineContract, 4U> brick_palette_pipeline_contracts() noexcept {
    return {
        brick_palette_pipeline_contract(VoxelMaterialRuntimePath::DeferredPalette2,
                                        BrickPaletteMappingMode::AssetUv),
        brick_palette_pipeline_contract(VoxelMaterialRuntimePath::DeferredPalette2,
                                        BrickPaletteMappingMode::WorldTriplanar),
        brick_palette_pipeline_contract(VoxelMaterialRuntimePath::DeferredPalette4,
                                        BrickPaletteMappingMode::AssetUv),
        brick_palette_pipeline_contract(VoxelMaterialRuntimePath::DeferredPalette4,
                                        BrickPaletteMappingMode::WorldTriplanar)};
}

rhi::BindGroupLayoutDesc brick_palette_bind_group_layout_desc() {
    rhi::BindGroupLayoutDesc desc;
    desc.debugName = "DVE brick palette renderer resources";
    const auto visibility = rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment;
    for (std::uint32_t binding = 0U; binding < kBrickPaletteBindingCount; ++binding)
        desc.bindings.push_back({binding, rhi::BindingType::StorageBufferReadOnly, visibility});
    return desc;
}

BrickPaletteUploadPacket build_brick_palette_upload_packet(
    const CookedBrickPaletteAsset& asset,
    const BrickPaletteSubmissionSettings& settings) {
    BrickPaletteUploadPacket packet;
    packet.sourceGeneration = asset.generation;
    packet.dependencyKey = asset.dependencyKey;
    packet.remapGlobalMaterialIds = asset.remap.globalMaterialIds;
    packet.records.reserve(asset.bricks.size());

    for (const auto& brick : asset.bricks) {
        BrickPaletteGpuBrickRecord record;
        record.key = brick.key;
        record.sourceGeneration = brick.sourceGeneration;
        const auto selected = fallback_path(
            brick.representations, settings.preferredPath, settings.allowFallback);
        record.runtimePath = static_cast<std::uint32_t>(selected);
        record.mappingMode = static_cast<std::uint32_t>(settings.mapping);
        if (selected == VoxelMaterialRuntimePath::NoPath) {
            ++packet.rejectedBrickCount;
            packet.records.push_back(record);
            continue;
        }
        if (selected != settings.preferredPath) ++packet.fallbackBrickCount;

        if (const auto* palette = palette_for_path(brick.representations, selected)) {
            record.paletteSlotOffset = checked_offset(packet.paletteSlots.size());
            const auto remapped = remap_brick_palette_slots(*palette, asset.remap);
            record.paletteSlotCount = checked_offset(remapped.size());
            packet.paletteSlots.insert(packet.paletteSlots.end(), remapped.begin(), remapped.end());
            record.sampleOffset = checked_offset(packet.paletteSamples.size());
            record.sampleCount = checked_offset(palette->samples.size());
            for (const auto& sample : palette->samples)
                packet.paletteSamples.push_back(pack_sample(sample));
        } else if (selected == VoxelMaterialRuntimePath::BakedProperties &&
                   brick.representations.bakedProperties.has_value()) {
            record.bakedOffset = checked_offset(packet.bakedSamples.size());
            record.sampleCount = checked_offset(brick.representations.bakedProperties->size());
            for (const auto& sample : *brick.representations.bakedProperties)
                packet.bakedSamples.push_back(pack_baked(sample));
        } else if (selected == VoxelMaterialRuntimePath::SingleMaterial &&
                   brick.representations.singleMaterialIds.has_value()) {
            record.singleOffset = checked_offset(packet.singleMaterialIds.size());
            record.sampleCount = checked_offset(brick.representations.singleMaterialIds->size());
            packet.singleMaterialIds.insert(packet.singleMaterialIds.end(),
                brick.representations.singleMaterialIds->begin(),
                brick.representations.singleMaterialIds->end());
        }
        packet.records.push_back(record);
    }

    std::uint64_t hash = kHashOffset;
    hash_integer(hash, packet.sourceGeneration);
    hash_integer(hash, packet.dependencyKey);
    for (const auto& value : packet.records) hash_trivial(hash, value);
    for (const auto value : packet.remapGlobalMaterialIds) hash_integer(hash, value);
    for (const auto value : packet.paletteSlots) hash_integer(hash, value);
    for (const auto& value : packet.paletteSamples) hash_trivial(hash, value);
    for (const auto& value : packet.bakedSamples) hash_trivial(hash, value);
    for (const auto value : packet.singleMaterialIds) hash_integer(hash, value);
    hash_integer(hash, packet.fallbackBrickCount);
    hash_integer(hash, packet.rejectedBrickCount);
    packet.contentHash = hash;
    return packet;
}

bool validate_brick_palette_upload_packet(
    const BrickPaletteUploadPacket& packet, std::string* error) {
    for (std::size_t index = 0U; index < packet.records.size(); ++index) {
        const auto& record = packet.records[index];
        if (index != 0U && !(packet.records[index - 1U].key < record.key)) {
            set_error(error, "brick-palette GPU records are not strictly sorted");
            return false;
        }
        const auto path = static_cast<VoxelMaterialRuntimePath>(record.runtimePath);
        if (path == VoxelMaterialRuntimePath::DeferredPalette2 ||
            path == VoxelMaterialRuntimePath::DeferredPalette4) {
            if (record.paletteSlotOffset == kInvalidBrickPaletteOffset ||
                record.sampleOffset == kInvalidBrickPaletteOffset ||
                static_cast<std::size_t>(record.paletteSlotOffset) + record.paletteSlotCount >
                    packet.paletteSlots.size() ||
                static_cast<std::size_t>(record.sampleOffset) + record.sampleCount >
                    packet.paletteSamples.size()) {
                set_error(error, "deferred palette record has an invalid buffer range");
                return false;
            }
            const std::uint32_t maximum = path == VoxelMaterialRuntimePath::DeferredPalette2 ? 2U : 4U;
            if (record.paletteSlotCount > maximum) {
                set_error(error, "deferred palette record exceeds its pipeline slot count");
                return false;
            }
            for (std::uint32_t slot = 0U; slot < record.paletteSlotCount; ++slot) {
                const auto remapIndex = packet.paletteSlots[record.paletteSlotOffset + slot];
                if (remapIndex >= packet.remapGlobalMaterialIds.size()) {
                    set_error(error, "palette slot references an invalid remap index");
                    return false;
                }
            }
        } else if (path == VoxelMaterialRuntimePath::BakedProperties) {
            if (record.bakedOffset == kInvalidBrickPaletteOffset ||
                static_cast<std::size_t>(record.bakedOffset) + record.sampleCount >
                    packet.bakedSamples.size()) {
                set_error(error, "baked record has an invalid buffer range");
                return false;
            }
        } else if (path == VoxelMaterialRuntimePath::SingleMaterial) {
            if (record.singleOffset == kInvalidBrickPaletteOffset ||
                static_cast<std::size_t>(record.singleOffset) + record.sampleCount >
                    packet.singleMaterialIds.size()) {
                set_error(error, "single-material record has an invalid buffer range");
                return false;
            }
        }
    }
    return true;
}

BrickPaletteRhiMirror::~BrickPaletteRhiMirror() {
    reset();
}

bool BrickPaletteRhiMirror::ensure_buffer(
    std::size_t index, std::size_t requiredBytes, std::string_view debugName,
    std::string* error) {
    const std::size_t nonzeroRequired = std::max<std::size_t>(requiredBytes, 4U);
    if (buffers_[index] && stats_.capacityBytes[index] >= nonzeroRequired) return true;
    if (buffers_[index] && !device_.destroy_buffer(buffers_[index], error)) return false;
    stats_.capacityBytes[index] = grown_capacity(0U, nonzeroRequired);
    rhi::BufferDesc desc;
    desc.bytes = stats_.capacityBytes[index];
    desc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopySource |
                 rhi::BufferUsage::CopyDestination;
    desc.memory = rhi::MemoryDomain::DeviceLocal;
    desc.debugName.assign(debugName.begin(), debugName.end());
    buffers_[index] = device_.create_buffer(desc, error);
    if (!buffers_[index]) {
        stats_.capacityBytes[index] = 0U;
        return false;
    }
    ++stats_.reallocations;
    return true;
}

bool BrickPaletteRhiMirror::upload(
    const BrickPaletteUploadPacket& packet, std::string* error) {
    if (!validate_brick_palette_upload_packet(packet, error)) return false;
    const std::array<std::span<const std::byte>, kBrickPaletteBindingCount> data{
        bytes_of(packet.records),
        bytes_of(packet.remapGlobalMaterialIds),
        bytes_of(packet.paletteSlots),
        bytes_of(packet.paletteSamples),
        bytes_of(packet.bakedSamples),
        bytes_of(packet.singleMaterialIds)};
    constexpr std::array<std::string_view, kBrickPaletteBindingCount> names{
        "DVE brick palette records",
        "DVE brick palette remap",
        "DVE brick palette slots",
        "DVE brick palette samples",
        "DVE brick baked samples",
        "DVE brick single material IDs"};
    for (std::size_t index = 0U; index < data.size(); ++index) {
        if (!ensure_buffer(index, data[index].size(), names[index], error)) return false;
        if (!data[index].empty() &&
            !device_.write_buffer(buffers_[index], 0U, data[index], error)) return false;
        uploadedBytes_[index] = data[index].size();
        stats_.uploadedBytes += data[index].size();
    }
    ++stats_.publications;
    return true;
}

bool BrickPaletteRhiMirror::readback_matches(
    const BrickPaletteUploadPacket& packet, std::string* error) {
    const std::array<std::span<const std::byte>, kBrickPaletteBindingCount> expected{
        bytes_of(packet.records),
        bytes_of(packet.remapGlobalMaterialIds),
        bytes_of(packet.paletteSlots),
        bytes_of(packet.paletteSamples),
        bytes_of(packet.bakedSamples),
        bytes_of(packet.singleMaterialIds)};
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        if (uploadedBytes_[index] != expected[index].size()) {
            set_error(error, "brick-palette RHI byte counts differ from the packet");
            return false;
        }
        if (expected[index].empty()) continue;
        std::vector<std::byte> actual(expected[index].size());
        if (!device_.read_buffer(buffers_[index], 0U, actual, error)) return false;
        if (!std::equal(actual.begin(), actual.end(), expected[index].begin(), expected[index].end())) {
            set_error(error, "brick-palette RHI readback differs from the CPU packet");
            return false;
        }
    }
    return true;
}

void BrickPaletteRhiMirror::reset() noexcept {
    std::string ignored;
    for (std::size_t index = 0U; index < buffers_.size(); ++index) {
        if (buffers_[index]) device_.destroy_buffer(buffers_[index], &ignored);
        buffers_[index] = {};
        uploadedBytes_[index] = 0U;
        stats_.capacityBytes[index] = 0U;
    }
}

bool create_brick_palette_rhi_bindings(
    rhi::IDevice& device,
    const BrickPaletteRhiMirror& mirror,
    BrickPaletteRhiBindings& output,
    std::string* error) {
    if (output.layout || output.group) {
        set_error(error, "brick-palette bindings are already initialized");
        return false;
    }
    output.layout = device.create_bind_group_layout(brick_palette_bind_group_layout_desc(), error);
    if (!output.layout) return false;
    rhi::BindGroupDesc desc;
    desc.layout = output.layout;
    desc.debugName = "DVE brick palette renderer bind group";
    const std::array<rhi::BufferHandle, kBrickPaletteBindingCount> buffers{
        mirror.record_buffer(), mirror.remap_buffer(), mirror.slot_buffer(),
        mirror.sample_buffer(), mirror.baked_buffer(), mirror.single_buffer()};
    const auto& capacities = mirror.stats().capacityBytes;
    for (std::uint32_t binding = 0U; binding < kBrickPaletteBindingCount; ++binding)
        desc.entries.push_back({binding, buffers[binding], {}, 0U, capacities[binding], {}});
    output.group = device.create_bind_group(desc, error);
    if (!output.group) {
        device.destroy_bind_group_layout(output.layout, nullptr);
        output.layout = {};
        return false;
    }
    return true;
}

bool destroy_brick_palette_rhi_bindings(
    rhi::IDevice& device,
    BrickPaletteRhiBindings& bindings,
    std::string* error) {
    if (bindings.group && !device.destroy_bind_group(bindings.group, error)) return false;
    bindings.group = {};
    if (bindings.layout && !device.destroy_bind_group_layout(bindings.layout, error)) return false;
    bindings.layout = {};
    return true;
}

const char* to_string(BrickPalettePipelineVariant value) noexcept {
    switch (value) {
        case BrickPalettePipelineVariant::Palette2AssetUv: return "Palette2AssetUv";
        case BrickPalettePipelineVariant::Palette2WorldTriplanar: return "Palette2WorldTriplanar";
        case BrickPalettePipelineVariant::Palette4AssetUv: return "Palette4AssetUv";
        case BrickPalettePipelineVariant::Palette4WorldTriplanar: return "Palette4WorldTriplanar";
    }
    return "Unknown";
}

} // namespace dve::render
