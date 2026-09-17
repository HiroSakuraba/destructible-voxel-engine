#include "dve/render/main_material_table.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include "dve/master_material.hpp"

namespace dve::render {
namespace {
void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    if (alignment <= 1U) return value;
    const std::size_t remainder = value % alignment;
    return remainder == 0U ? value : value + alignment - remainder;
}

constexpr std::array<std::array<std::byte, 4>, kMainMaterialTextureChannelCount>
kFallbackPixels{{
    {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}}, // base color
    {std::byte{0xFF}, std::byte{0xFF}, std::byte{0x00}, std::byte{0xFF}}, // roughness 1, metallic 0
    {std::byte{0x80}, std::byte{0x80}, std::byte{0xFF}, std::byte{0xFF}}, // flat normal
    {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xFF}}, // emissive black
    {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}}, // opacity one
    {std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0xFF}}, // neutral height
    {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}}, // detail color neutral
    {std::byte{0x80}, std::byte{0x80}, std::byte{0xFF}, std::byte{0xFF}}, // detail flat normal
    {std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0xFF}}, // detail roughness neutral
}};

const PolygonTextureBinding& channel_binding(const PolygonMaterialBinding& material,
                                              MainMaterialTextureChannel channel) noexcept {
    switch (channel) {
    case MainMaterialTextureChannel::BaseColor: return material.baseColor;
    case MainMaterialTextureChannel::MetallicRoughness: return material.metallicRoughness;
    case MainMaterialTextureChannel::Normal: return material.normal;
    case MainMaterialTextureChannel::Emissive: return material.emissive;
    case MainMaterialTextureChannel::Opacity: return material.opacity;
    case MainMaterialTextureChannel::Height: return material.height;
    case MainMaterialTextureChannel::DetailBaseColor: return material.detailBaseColor;
    case MainMaterialTextureChannel::DetailNormal: return material.detailNormal;
    case MainMaterialTextureChannel::DetailRoughness: return material.detailRoughness;
    }
    return material.baseColor;
}
} // namespace

MainMaterialDescriptorTable::MainMaterialDescriptorTable(rhi::IDevice& device)
    : device_(device),
      ownedResidency_(std::make_unique<MaterialResourceResidency>(device)),
      residency_(ownedResidency_.get()) {}

MainMaterialDescriptorTable::MainMaterialDescriptorTable(
    rhi::IDevice& device, MaterialResourceResidency& residency)
    : device_(device), residency_(&residency) {}

MainMaterialDescriptorTable::~MainMaterialDescriptorTable() { clear(); }

bool MainMaterialDescriptorTable::valid() const noexcept {
    if (!residency_ || !layout_ || !fallbackMaterialBuffer_ || !fallbackMappingBuffer_ ||
        !fallbackSampler_ || !fallbackGroup_ || !staticShadowDepthView_ ||
        !dynamicShadowDepthView_ || !shadowComparisonSampler_) return false;
    return std::ranges::all_of(fallbackTextures_, [](auto handle) { return static_cast<bool>(handle); }) &&
           std::ranges::all_of(fallbackViews_, [](auto handle) { return static_cast<bool>(handle); });
}

bool MainMaterialDescriptorTable::initialize(rhi::TextureViewHandle staticShadowDepth,
                                             rhi::TextureViewHandle dynamicShadowDepth,
                                             rhi::SamplerHandle shadowComparisonSampler,
                                             std::string* error) {
    if (!staticShadowDepth || !dynamicShadowDepth || !shadowComparisonSampler) {
        set_error(error, "main material table requires both shadow atlas views and comparison sampler");
        return false;
    }
    if (valid() && staticShadowDepthView_ == staticShadowDepth &&
        dynamicShadowDepthView_ == dynamicShadowDepth &&
        shadowComparisonSampler_ == shadowComparisonSampler) return true;
    clear();
    staticShadowDepthView_ = staticShadowDepth;
    dynamicShadowDepthView_ = dynamicShadowDepth;
    shadowComparisonSampler_ = shadowComparisonSampler;
    std::string local;
    rhi::BindGroupLayoutDesc layout;
    layout.debugName = "Persistent live material record and texture layout";
    layout.bindings = {
        {0U, rhi::BindingType::StorageBufferReadOnly, rhi::ShaderStage::Fragment},
        {1U, rhi::BindingType::StorageBufferReadOnly, rhi::ShaderStage::Fragment},
    };
    for (std::uint32_t binding = 0U; binding < kMainMaterialTextureChannelCount; ++binding) {
        layout.bindings.push_back({binding + 2U, rhi::BindingType::SampledTexture,
                                   rhi::ShaderStage::Fragment});
    }
    layout.bindings.push_back({11U, rhi::BindingType::SampledTexture, rhi::ShaderStage::Fragment});
    layout.bindings.push_back({12U, rhi::BindingType::SampledTexture, rhi::ShaderStage::Fragment});
    layout_ = device_.create_bind_group_layout(layout, &local);
    if (!layout_) { set_error(error, local); clear(); return false; }

    auto make_buffer = [&](std::size_t bytes, std::string name) {
        rhi::BufferDesc desc;
        desc.bytes = bytes;
        desc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDestination;
        desc.memory = rhi::MemoryDomain::Upload;
        desc.initialState = rhi::ResourceState::ShaderRead;
        desc.debugName = std::move(name);
        return device_.create_buffer(desc, &local);
    };
    fallbackMaterialBuffer_ = make_buffer(sizeof(GpuMaterialRecord),
                                          "Fallback live GPU material record");
    if (!fallbackMaterialBuffer_) { set_error(error, local); clear(); return false; }
    fallbackMappingBuffer_ = make_buffer(sizeof(GpuPolygonMaterialMappingRecord),
                                         "Fallback live GPU mapping record");
    if (!fallbackMappingBuffer_) { set_error(error, local); clear(); return false; }
    const VoxelMaterialDefinition fallbackMaterial{};
    const PolygonMaterialBinding fallbackMapping{};
    const auto materialRecord = pack_gpu_material_record(fallbackMaterial);
    const auto mappingRecord = pack_gpu_polygon_material_mapping(fallbackMapping);
    if (!device_.write_buffer(fallbackMaterialBuffer_, 0U,
                              std::as_bytes(std::span(&materialRecord, 1U)), &local) ||
        !device_.write_buffer(fallbackMappingBuffer_, 0U,
                              std::as_bytes(std::span(&mappingRecord, 1U)), &local)) {
        set_error(error, local); clear(); return false;
    }

    rhi::SamplerDesc sampler;
    sampler.minFilter = rhi::FilterMode::Linear;
    sampler.magFilter = rhi::FilterMode::Linear;
    sampler.mipmapFilter = rhi::MipmapFilterMode::Linear;
    sampler.addressU = sampler.addressV = sampler.addressW = rhi::AddressMode::Repeat;
    sampler.debugName = "Persistent live material fallback sampler";
    fallbackSampler_ = device_.create_sampler(sampler, &local);
    if (!fallbackSampler_) { set_error(error, local); clear(); return false; }

    for (std::uint32_t channel = 0U; channel < kMainMaterialTextureChannelCount; ++channel) {
        rhi::TextureDesc texture;
        texture.format = rhi::TextureFormat::RGBA8Unorm;
        texture.width = 1U; texture.height = 1U;
        texture.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDestination;
        texture.initialState = rhi::ResourceState::ShaderRead;
        texture.debugName = "Persistent live material fallback channel " + std::to_string(channel);
        fallbackTextures_[channel] = device_.create_texture(texture, &local);
        if (!fallbackTextures_[channel]) { set_error(error, local); clear(); return false; }
        if (!device_.write_texture(fallbackTextures_[channel], 0U, 0U,
                                   kFallbackPixels[channel], 4U, &local)) {
            set_error(error, local); clear(); return false;
        }
        rhi::TextureViewDesc view;
        view.texture = fallbackTextures_[channel];
        view.debugName = "Persistent live material fallback view " + std::to_string(channel);
        fallbackViews_[channel] = device_.create_texture_view(view, &local);
        if (!fallbackViews_[channel]) { set_error(error, local); clear(); return false; }
    }

    rhi::BindGroupDesc group;
    group.layout = layout_;
    group.debugName = "Persistent live material fallback descriptor";
    group.entries = {
        {0U, fallbackMaterialBuffer_, {}, 0U, sizeof(GpuMaterialRecord), {}},
        {1U, fallbackMappingBuffer_, {}, 0U, sizeof(GpuPolygonMaterialMappingRecord), {}},
    };
    for (std::uint32_t channel = 0U; channel < kMainMaterialTextureChannelCount; ++channel) {
        group.entries.push_back({channel + 2U, {}, fallbackViews_[channel], 0U, 0U,
                                 fallbackSampler_});
    }
    group.entries.push_back({11U, {}, staticShadowDepthView_, 0U, 0U, shadowComparisonSampler_});
    group.entries.push_back({12U, {}, dynamicShadowDepthView_, 0U, 0U, shadowComparisonSampler_});
    fallbackGroup_ = device_.create_bind_group(group, &local);
    if (!fallbackGroup_) { set_error(error, local); clear(); return false; }
    return true;
}

bool MainMaterialDescriptorTable::ensure_asset_records(const CookedPolygonAsset& asset,
                                                        AssetResource*& resource,
                                                        std::string* error) {
    if (auto it = assets_.find(asset.contentHash); it != assets_.end()) {
        resource = &it->second;
        return true;
    }
    if (asset.materials.empty() || asset.materials.size() != asset.materialBindings.size()) {
        set_error(error, "main material asset record counts are invalid");
        return false;
    }
    std::string local;
    const auto resolved = resolve_voxel_material_layers(asset.materials, &local);
    if (!resolved) { set_error(error, local); return false; }
    const std::size_t alignment = std::max<std::size_t>(
        4U, device_.capabilities().minStorageBufferOffsetAlignment);
    AssetResource created;
    created.materialStride = align_up(sizeof(GpuMaterialRecord), alignment);
    created.mappingStride = align_up(sizeof(GpuPolygonMaterialMappingRecord), alignment);
    created.materialCount = static_cast<std::uint32_t>(asset.materials.size());

    auto make_buffer = [&](std::size_t bytes, std::string name) {
        rhi::BufferDesc desc;
        desc.bytes = bytes;
        desc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDestination;
        desc.memory = rhi::MemoryDomain::Upload;
        desc.initialState = rhi::ResourceState::ShaderRead;
        desc.debugName = std::move(name);
        return device_.create_buffer(desc, &local);
    };
    created.materialRecords = make_buffer(created.materialStride * asset.materials.size(),
                                          "Persistent live material records");
    if (!created.materialRecords) { set_error(error, local); return false; }
    created.mappingRecords = make_buffer(created.mappingStride * asset.materialBindings.size(),
                                         "Persistent live material mapping records");
    if (!created.mappingRecords) {
        (void)device_.destroy_buffer(created.materialRecords, nullptr);
        set_error(error, local); return false;
    }
    for (std::size_t index = 0U; index < asset.materials.size(); ++index) {
        const auto materialRecord = pack_gpu_material_record((*resolved)[index]);
        const auto mappingRecord = pack_gpu_polygon_material_mapping(asset.materialBindings[index]);
        if (!device_.write_buffer(created.materialRecords, index * created.materialStride,
                                  std::as_bytes(std::span(&materialRecord, 1U)), &local) ||
            !device_.write_buffer(created.mappingRecords, index * created.mappingStride,
                                  std::as_bytes(std::span(&mappingRecord, 1U)), &local)) {
            (void)device_.destroy_buffer(created.mappingRecords, nullptr);
            (void)device_.destroy_buffer(created.materialRecords, nullptr);
            set_error(error, local); return false;
        }
        ++stats_.materialRecordsUploaded;
        ++stats_.mappingRecordsUploaded;
    }
    auto [it, inserted] = assets_.emplace(asset.contentHash, created);
    if (!inserted) {
        (void)device_.destroy_buffer(created.mappingRecords, nullptr);
        (void)device_.destroy_buffer(created.materialRecords, nullptr);
        set_error(error, "main material asset record publication collided");
        return false;
    }
    ++stats_.assetsPublished;
    stats_.assetRecordBuffersCreated += 2U;
    resource = &it->second;
    return true;
}

bool MainMaterialDescriptorTable::retain_asset_once(std::uint64_t assetContentHash,
                                                     bool& retainedNow,
                                                     std::string* error) {
    retainedNow = false;
    if (retainedAssets_.contains(assetContentHash)) return true;
    if (!residency_->retain_asset(assetContentHash, error)) return false;
    retainedAssets_.insert(assetContentHash);
    retainedNow = true;
    return true;
}

void MainMaterialDescriptorTable::rollback_asset_retain(std::uint64_t assetContentHash,
                                                        bool retainedNow) noexcept {
    if (!retainedNow) return;
    retainedAssets_.erase(assetContentHash);
    (void)residency_->release_asset(assetContentHash, nullptr);
}

bool MainMaterialDescriptorTable::resolve_binding(
    const CookedPolygonAsset& asset, const PolygonTextureBinding& binding,
    MainMaterialTextureChannel channel, rhi::TextureViewHandle& view,
    rhi::SamplerHandle& sampler, bool& present, std::string* error) {
    present = binding.texture.has_value();
    const auto channelIndex = static_cast<std::uint32_t>(channel);
    if (!binding.texture) {
        view = fallbackViews_[channelIndex]; sampler = fallbackSampler_; return true;
    }
    if (*binding.texture >= asset.textures.size()) {
        set_error(error, "main material texture index is out of range"); return false;
    }
    const auto& texture = asset.textures[*binding.texture];
    return residency_->ensure_image(asset, texture.imageIndex, view, error) &&
           residency_->ensure_sampler(asset, texture.samplerIndex, sampler, error);
}

bool MainMaterialDescriptorTable::ensure_material(const CookedPolygonAsset& asset,
                                                   std::uint32_t materialIndex,
                                                   std::string* error) {
    if (!valid()) { set_error(error, "main material table is not initialized"); return false; }
    if (materialIndex >= asset.materialBindings.size() || materialIndex >= asset.materials.size()) {
        set_error(error, "main material index is out of range"); return false;
    }
    const MaterialKey key{asset.contentHash, materialIndex};
    if (materials_.contains(key)) { ++stats_.descriptorCacheHits; return true; }

    bool retainedNow{};
    if (!retain_asset_once(asset.contentHash, retainedNow, error)) return false;
    AssetResource* assetResource{};
    if (!ensure_asset_records(asset, assetResource, error) || !assetResource) {
        rollback_asset_retain(asset.contentHash, retainedNow);
        return false;
    }

    const auto& binding = asset.materialBindings[materialIndex];
    std::array<rhi::TextureViewHandle, kMainMaterialTextureChannelCount> views{};
    std::array<rhi::SamplerHandle, kMainMaterialTextureChannelCount> samplers{};
    std::uint32_t textureMask{};
    for (std::uint32_t channel = 0U; channel < kMainMaterialTextureChannelCount; ++channel) {
        bool present{};
        const auto typedChannel = static_cast<MainMaterialTextureChannel>(channel);
        if (!resolve_binding(asset, channel_binding(binding, typedChannel), typedChannel,
                             views[channel], samplers[channel], present, error)) {
            rollback_asset_retain(asset.contentHash, retainedNow);
            return false;
        }
        if (present) textureMask |= 1U << channel;
    }

    const std::size_t materialOffset = static_cast<std::size_t>(materialIndex) *
                                       assetResource->materialStride;
    const std::size_t mappingOffset = static_cast<std::size_t>(materialIndex) *
                                      assetResource->mappingStride;
    rhi::BindGroupDesc group;
    group.layout = layout_;
    group.debugName = "Persistent live polygon material descriptor";
    group.entries = {
        {0U, assetResource->materialRecords, {}, materialOffset, sizeof(GpuMaterialRecord), {}},
        {1U, assetResource->mappingRecords, {}, mappingOffset,
         sizeof(GpuPolygonMaterialMappingRecord), {}},
    };
    for (std::uint32_t channel = 0U; channel < kMainMaterialTextureChannelCount; ++channel) {
        group.entries.push_back({channel + 2U, {}, views[channel], 0U, 0U, samplers[channel]});
    }
    group.entries.push_back({11U, {}, staticShadowDepthView_, 0U, 0U, shadowComparisonSampler_});
    group.entries.push_back({12U, {}, dynamicShadowDepthView_, 0U, 0U, shadowComparisonSampler_});
    std::string local;
    const auto bindGroup = device_.create_bind_group(group, &local);
    if (!bindGroup) {
        set_error(error, local);
        rollback_asset_retain(asset.contentHash, retainedNow);
        return false;
    }
    MaterialResource created;
    created.descriptor = {bindGroup, assetResource->materialRecords, assetResource->mappingRecords,
                          materialOffset, mappingOffset, textureMask, asset.contentHash, materialIndex};
    const auto [it, inserted] = materials_.emplace(key, created);
    (void)it;
    if (!inserted) {
        (void)device_.destroy_bind_group(bindGroup, nullptr);
        rollback_asset_retain(asset.contentHash, retainedNow);
        set_error(error, "main material descriptor publication collided");
        return false;
    }
    ++stats_.materialDescriptorsCreated;
    return true;
}

const MainMaterialDescriptor* MainMaterialDescriptorTable::find(
    std::uint64_t assetContentHash, std::uint32_t materialIndex) const noexcept {
    const auto it = materials_.find({assetContentHash, materialIndex});
    return it == materials_.end() ? nullptr : &it->second.descriptor;
}

bool MainMaterialDescriptorTable::invalidate_asset(std::uint64_t assetContentHash,
                                                   std::string* error) {
    bool ok = true;
    std::string local;
    for (auto it = materials_.begin(); it != materials_.end();) {
        if (it->first.assetHash == assetContentHash) {
            if (it->second.descriptor.bindGroup &&
                !device_.destroy_bind_group(it->second.descriptor.bindGroup, &local)) {
                ok = false; if (error && error->empty()) *error = local;
                local.clear();
            }
            it = materials_.erase(it);
        } else ++it;
    }
    if (auto it = assets_.find(assetContentHash); it != assets_.end()) {
        if (it->second.mappingRecords) (void)device_.destroy_buffer(it->second.mappingRecords, nullptr);
        if (it->second.materialRecords) (void)device_.destroy_buffer(it->second.materialRecords, nullptr);
        assets_.erase(it);
    }
    if (retainedAssets_.erase(assetContentHash) != 0U &&
        !residency_->release_asset(assetContentHash, &local)) {
        ok = false;
        if (error && error->empty()) *error = local;
    }
    ++stats_.invalidations;
    return ok;
}

void MainMaterialDescriptorTable::clear() noexcept {
    for (auto& [key, resource] : materials_) {
        (void)key;
        if (resource.descriptor.bindGroup)
            (void)device_.destroy_bind_group(resource.descriptor.bindGroup, nullptr);
    }
    materials_.clear();
    for (auto& [key, resource] : assets_) {
        (void)key;
        if (resource.mappingRecords) (void)device_.destroy_buffer(resource.mappingRecords, nullptr);
        if (resource.materialRecords) (void)device_.destroy_buffer(resource.materialRecords, nullptr);
    }
    assets_.clear();
    for (const auto assetContentHash : retainedAssets_)
        (void)residency_->release_asset(assetContentHash, nullptr);
    retainedAssets_.clear();
    if (fallbackGroup_) (void)device_.destroy_bind_group(fallbackGroup_, nullptr);
    for (auto view : fallbackViews_) if (view) (void)device_.destroy_texture_view(view, nullptr);
    for (auto texture : fallbackTextures_) if (texture) (void)device_.destroy_texture(texture, nullptr);
    if (fallbackSampler_) (void)device_.destroy_sampler(fallbackSampler_, nullptr);
    if (fallbackMappingBuffer_) (void)device_.destroy_buffer(fallbackMappingBuffer_, nullptr);
    if (fallbackMaterialBuffer_) (void)device_.destroy_buffer(fallbackMaterialBuffer_, nullptr);
    if (layout_) (void)device_.destroy_bind_group_layout(layout_, nullptr);
    layout_ = {}; fallbackMaterialBuffer_ = {}; fallbackMappingBuffer_ = {};
    fallbackTextures_.fill({}); fallbackViews_.fill({}); fallbackSampler_ = {}; fallbackGroup_ = {};
    staticShadowDepthView_ = {}; dynamicShadowDepthView_ = {}; shadowComparisonSampler_ = {};
}

MainMaterialDescriptorStats MainMaterialDescriptorTable::stats() const noexcept {
    auto result = stats_;
    result.assetResourceCount = assets_.size();
    result.materialDescriptorCount = materials_.size();
    result.retainedAssetCount = retainedAssets_.size();
    if (residency_) {
        const auto shared = residency_->stats();
        result.imagesUploaded = shared.imagesUploaded;
        result.imageCacheHits = shared.imageCacheHits;
        result.samplersCreated = shared.samplersCreated;
        result.samplerCacheHits = shared.samplerCacheHits;
        result.sharedAssetReferenceCount = shared.assetReferenceCount;
        result.imageResourceCount = shared.imageResourceCount;
        result.samplerResourceCount = shared.samplerResourceCount;
    }
    return result;
}

} // namespace dve::render
