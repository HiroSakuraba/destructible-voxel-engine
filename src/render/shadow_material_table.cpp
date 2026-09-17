#include "dve/render/shadow_material_table.hpp"

#include <array>
#include <utility>

namespace dve::render {
namespace {
void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}
} // namespace

ShadowMaterialDescriptorTable::ShadowMaterialDescriptorTable(rhi::IDevice& device)
    : device_(device),
      ownedResidency_(std::make_unique<MaterialResourceResidency>(device)),
      residency_(ownedResidency_.get()) {}

ShadowMaterialDescriptorTable::ShadowMaterialDescriptorTable(
    rhi::IDevice& device, MaterialResourceResidency& residency)
    : device_(device), residency_(&residency) {}

ShadowMaterialDescriptorTable::~ShadowMaterialDescriptorTable() { clear(); }

bool ShadowMaterialDescriptorTable::valid() const noexcept {
    return residency_ && layout_ && fallbackTexture_ && fallbackView_ &&
           fallbackSampler_ && fallbackGroup_;
}

bool ShadowMaterialDescriptorTable::initialize(std::string* error) {
    if (valid()) return true;
    clear();
    std::string local;
    rhi::BindGroupLayoutDesc layout;
    layout.debugName = "Shadow caster material texture layout";
    layout.bindings = {
        {0U, rhi::BindingType::SampledTexture, rhi::ShaderStage::Fragment},
        {1U, rhi::BindingType::SampledTexture, rhi::ShaderStage::Fragment},
    };
    layout_ = device_.create_bind_group_layout(layout, &local);
    if (!layout_) { set_error(error, local); clear(); return false; }

    rhi::TextureDesc texture;
    texture.format = rhi::TextureFormat::RGBA8Unorm;
    texture.width = 1U; texture.height = 1U;
    texture.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDestination;
    texture.initialState = rhi::ResourceState::ShaderRead;
    texture.debugName = "Shadow caster opaque fallback texture";
    fallbackTexture_ = device_.create_texture(texture, &local);
    if (!fallbackTexture_) { set_error(error, local); clear(); return false; }
    const std::array<std::byte, 4> white{
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
    if (!device_.write_texture(fallbackTexture_, 0U, 0U, white, 4U, &local)) {
        set_error(error, local); clear(); return false;
    }
    rhi::TextureViewDesc view;
    view.texture = fallbackTexture_;
    view.debugName = "Shadow caster opaque fallback view";
    fallbackView_ = device_.create_texture_view(view, &local);
    if (!fallbackView_) { set_error(error, local); clear(); return false; }

    rhi::SamplerDesc sampler;
    sampler.minFilter = rhi::FilterMode::Linear;
    sampler.magFilter = rhi::FilterMode::Linear;
    sampler.mipmapFilter = rhi::MipmapFilterMode::Linear;
    sampler.addressU = sampler.addressV = sampler.addressW = rhi::AddressMode::Repeat;
    sampler.debugName = "Shadow caster fallback sampler";
    fallbackSampler_ = device_.create_sampler(sampler, &local);
    if (!fallbackSampler_) { set_error(error, local); clear(); return false; }

    rhi::BindGroupDesc group;
    group.layout = layout_;
    group.debugName = "Shadow caster fallback material";
    group.entries = {
        {0U, {}, fallbackView_, 0U, 0U, fallbackSampler_},
        {1U, {}, fallbackView_, 0U, 0U, fallbackSampler_},
    };
    fallbackGroup_ = device_.create_bind_group(group, &local);
    if (!fallbackGroup_) { set_error(error, local); clear(); return false; }
    return true;
}

bool ShadowMaterialDescriptorTable::retain_asset_once(std::uint64_t assetContentHash,
                                                       bool& retainedNow,
                                                       std::string* error) {
    retainedNow = false;
    if (retainedAssets_.contains(assetContentHash)) return true;
    if (!residency_->retain_asset(assetContentHash, error)) return false;
    retainedAssets_.insert(assetContentHash);
    retainedNow = true;
    return true;
}

void ShadowMaterialDescriptorTable::rollback_asset_retain(std::uint64_t assetContentHash,
                                                          bool retainedNow) noexcept {
    if (!retainedNow) return;
    retainedAssets_.erase(assetContentHash);
    (void)residency_->release_asset(assetContentHash, nullptr);
}

bool ShadowMaterialDescriptorTable::resolve_binding(
    const CookedPolygonAsset& asset, const PolygonTextureBinding& binding,
    rhi::TextureViewHandle& view, rhi::SamplerHandle& sampler, bool& present,
    std::string* error) {
    present = binding.texture.has_value();
    if (!binding.texture) {
        view = fallbackView_; sampler = fallbackSampler_; return true;
    }
    if (*binding.texture >= asset.textures.size()) {
        set_error(error, "shadow material texture index is out of range"); return false;
    }
    const auto& texture = asset.textures[*binding.texture];
    return residency_->ensure_image(asset, texture.imageIndex, view, error) &&
           residency_->ensure_sampler(asset, texture.samplerIndex, sampler, error);
}

bool ShadowMaterialDescriptorTable::ensure_material(const CookedPolygonAsset& asset,
                                                     std::uint32_t materialIndex,
                                                     std::string* error) {
    if (!valid() && !initialize(error)) return false;
    if (materialIndex >= asset.materialBindings.size() || materialIndex >= asset.materials.size()) {
        set_error(error, "shadow material index is out of range"); return false;
    }
    const MaterialKey key{asset.contentHash, materialIndex};
    if (materials_.contains(key)) { ++stats_.descriptorCacheHits; return true; }

    bool retainedNow{};
    if (!retain_asset_once(asset.contentHash, retainedNow, error)) return false;
    const auto& binding = asset.materialBindings[materialIndex];
    rhi::TextureViewHandle baseView, opacityView;
    rhi::SamplerHandle baseSampler, opacitySampler;
    bool basePresent{}, opacityPresent{};
    if (!resolve_binding(asset, binding.baseColor, baseView, baseSampler, basePresent, error) ||
        !resolve_binding(asset, binding.opacity, opacityView, opacitySampler, opacityPresent, error)) {
        rollback_asset_retain(asset.contentHash, retainedNow);
        return false;
    }
    rhi::BindGroupDesc group;
    group.layout = layout_;
    group.debugName = "Shadow caster material descriptor";
    group.entries = {
        {0U, {}, baseView, 0U, 0U, baseSampler},
        {1U, {}, opacityView, 0U, 0U, opacitySampler},
    };
    std::string local;
    const auto handle = device_.create_bind_group(group, &local);
    if (!handle) {
        set_error(error, local);
        rollback_asset_retain(asset.contentHash, retainedNow);
        return false;
    }
    MaterialResource resource;
    resource.descriptor = {handle, basePresent, opacityPresent, asset.contentHash, materialIndex};
    const auto [it, inserted] = materials_.emplace(key, resource);
    (void)it;
    if (!inserted) {
        (void)device_.destroy_bind_group(handle, nullptr);
        rollback_asset_retain(asset.contentHash, retainedNow);
        set_error(error, "shadow material descriptor publication collided");
        return false;
    }
    ++stats_.materialDescriptorsCreated;
    if (retainedNow) ++stats_.assetsPublished;
    return true;
}

const ShadowMaterialDescriptor* ShadowMaterialDescriptorTable::find(
    std::uint64_t assetContentHash, std::uint32_t materialIndex) const noexcept {
    const auto it = materials_.find({assetContentHash, materialIndex});
    return it == materials_.end() ? nullptr : &it->second.descriptor;
}

bool ShadowMaterialDescriptorTable::invalidate_asset(std::uint64_t assetContentHash,
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
    if (retainedAssets_.erase(assetContentHash) != 0U &&
        !residency_->release_asset(assetContentHash, &local)) {
        ok = false;
        if (error && error->empty()) *error = local;
    }
    ++stats_.invalidations;
    return ok;
}

void ShadowMaterialDescriptorTable::clear() noexcept {
    for (auto& [key, resource] : materials_) {
        (void)key;
        if (resource.descriptor.bindGroup)
            (void)device_.destroy_bind_group(resource.descriptor.bindGroup, nullptr);
    }
    materials_.clear();
    for (const auto assetContentHash : retainedAssets_)
        (void)residency_->release_asset(assetContentHash, nullptr);
    retainedAssets_.clear();
    if (fallbackGroup_) (void)device_.destroy_bind_group(fallbackGroup_, nullptr);
    if (fallbackView_) (void)device_.destroy_texture_view(fallbackView_, nullptr);
    if (fallbackSampler_) (void)device_.destroy_sampler(fallbackSampler_, nullptr);
    if (fallbackTexture_) (void)device_.destroy_texture(fallbackTexture_, nullptr);
    if (layout_) (void)device_.destroy_bind_group_layout(layout_, nullptr);
    layout_ = {}; fallbackTexture_ = {}; fallbackView_ = {}; fallbackSampler_ = {};
    fallbackGroup_ = {};
}

ShadowMaterialDescriptorStats ShadowMaterialDescriptorTable::stats() const noexcept {
    auto result = stats_;
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
