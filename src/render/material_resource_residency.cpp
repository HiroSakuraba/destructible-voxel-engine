#include "dve/render/material_resource_residency.hpp"

#include <limits>
#include <span>
#include <utility>

namespace dve::render {
namespace {
void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

rhi::AddressMode to_address_mode(ImportedWrapMode mode) noexcept {
    switch (mode) {
    case ImportedWrapMode::ClampToEdge: return rhi::AddressMode::ClampToEdge;
    case ImportedWrapMode::MirroredRepeat: return rhi::AddressMode::MirroredRepeat;
    case ImportedWrapMode::Repeat: return rhi::AddressMode::Repeat;
    }
    return rhi::AddressMode::Repeat;
}

rhi::FilterMode to_filter(ImportedTextureFilter filter) noexcept {
    return filter == ImportedTextureFilter::Nearest ? rhi::FilterMode::Nearest
                                                     : rhi::FilterMode::Linear;
}
} // namespace

MaterialResourceResidency::~MaterialResourceResidency() { clear(); }

bool MaterialResourceResidency::retain_asset(std::uint64_t assetContentHash,
                                             std::string* error) {
    auto [it, inserted] = assetReferences_.try_emplace(assetContentHash, 0U);
    (void)inserted;
    if (it->second == std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "material residency asset reference count overflow");
        return false;
    }
    ++it->second;
    ++stats_.assetRetainCalls;
    return true;
}

bool MaterialResourceResidency::destroy_asset_resources(std::uint64_t assetContentHash,
                                                         std::string* error) noexcept {
    bool ok = true;
    std::string local;
    for (auto it = images_.begin(); it != images_.end();) {
        if (it->first.assetHash != assetContentHash) {
            ++it;
            continue;
        }
        if (it->second.view && !device_.destroy_texture_view(it->second.view, &local)) {
            ok = false;
            if (error && error->empty()) *error = local;
            local.clear();
        }
        if (it->second.texture && !device_.destroy_texture(it->second.texture, &local)) {
            ok = false;
            if (error && error->empty()) *error = local;
            local.clear();
        }
        it = images_.erase(it);
    }
    for (auto it = samplers_.begin(); it != samplers_.end();) {
        if (it->first.assetHash != assetContentHash) {
            ++it;
            continue;
        }
        if (it->second && !device_.destroy_sampler(it->second, &local)) {
            ok = false;
            if (error && error->empty()) *error = local;
            local.clear();
        }
        it = samplers_.erase(it);
    }
    ++stats_.assetEvictions;
    return ok;
}

bool MaterialResourceResidency::release_asset(std::uint64_t assetContentHash,
                                              std::string* error) {
    const auto it = assetReferences_.find(assetContentHash);
    if (it == assetReferences_.end() || it->second == 0U) {
        set_error(error, "material residency asset was released without a matching retain");
        return false;
    }
    ++stats_.assetReleaseCalls;
    --it->second;
    if (it->second != 0U) return true;
    assetReferences_.erase(it);
    return destroy_asset_resources(assetContentHash, error);
}

bool MaterialResourceResidency::ensure_image(const CookedPolygonAsset& asset,
                                              std::uint32_t imageIndex,
                                              rhi::TextureViewHandle& view,
                                              std::string* error) {
    if (!is_retained(asset.contentHash)) {
        set_error(error, "material image requested before retaining its asset");
        return false;
    }
    const ImageKey key{asset.contentHash, imageIndex};
    if (const auto it = images_.find(key); it != images_.end()) {
        view = it->second.view;
        ++stats_.imageCacheHits;
        return true;
    }
    if (imageIndex >= asset.images.size()) {
        set_error(error, "material image index is out of range");
        return false;
    }
    const auto& image = asset.images[imageIndex];
    const std::size_t required = static_cast<std::size_t>(image.width) * image.height * 4U;
    if (image.width == 0U || image.height == 0U || image.rgba8.size() != required) {
        set_error(error, "material image payload is invalid");
        return false;
    }

    std::string local;
    rhi::TextureDesc desc;
    desc.format = rhi::TextureFormat::RGBA8Unorm;
    desc.width = image.width;
    desc.height = image.height;
    desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDestination;
    desc.initialState = rhi::ResourceState::ShaderRead;
    desc.debugName = "Shared polygon material image " + image.name;
    ImageResource created;
    created.texture = device_.create_texture(desc, &local);
    if (!created.texture) {
        set_error(error, local);
        return false;
    }
    if (!device_.write_texture(created.texture, 0U, 0U,
                               std::as_bytes(std::span(image.rgba8)),
                               static_cast<std::size_t>(image.width) * 4U, &local)) {
        (void)device_.destroy_texture(created.texture, nullptr);
        set_error(error, local);
        return false;
    }
    rhi::TextureViewDesc viewDesc;
    viewDesc.texture = created.texture;
    viewDesc.debugName = "Shared polygon material image view " + image.name;
    created.view = device_.create_texture_view(viewDesc, &local);
    if (!created.view) {
        (void)device_.destroy_texture(created.texture, nullptr);
        set_error(error, local);
        return false;
    }
    view = created.view;
    images_.emplace(key, created);
    ++stats_.imagesUploaded;
    return true;
}

bool MaterialResourceResidency::ensure_sampler(
    const CookedPolygonAsset& asset, std::optional<std::uint32_t> samplerIndex,
    rhi::SamplerHandle& sampler, std::string* error) {
    if (!is_retained(asset.contentHash)) {
        set_error(error, "material sampler requested before retaining its asset");
        return false;
    }
    const std::uint32_t encoded = samplerIndex ? *samplerIndex + 1U : 0U;
    const SamplerKey key{asset.contentHash, encoded};
    if (const auto it = samplers_.find(key); it != samplers_.end()) {
        sampler = it->second;
        ++stats_.samplerCacheHits;
        return true;
    }

    PolygonSampler source;
    if (samplerIndex) {
        if (*samplerIndex >= asset.samplers.size()) {
            set_error(error, "material sampler index is out of range");
            return false;
        }
        source = asset.samplers[*samplerIndex];
    }
    rhi::SamplerDesc desc;
    desc.minFilter = to_filter(source.minFilter);
    desc.magFilter = to_filter(source.magFilter);
    desc.mipmapFilter = desc.minFilter == rhi::FilterMode::Nearest
        ? rhi::MipmapFilterMode::Nearest : rhi::MipmapFilterMode::Linear;
    desc.addressU = to_address_mode(source.wrapS);
    desc.addressV = to_address_mode(source.wrapT);
    desc.addressW = rhi::AddressMode::Repeat;
    desc.debugName = "Shared polygon material sampler";
    std::string local;
    sampler = device_.create_sampler(desc, &local);
    if (!sampler) {
        set_error(error, local);
        return false;
    }
    samplers_.emplace(key, sampler);
    ++stats_.samplersCreated;
    return true;
}

bool MaterialResourceResidency::is_retained(std::uint64_t assetContentHash) const noexcept {
    const auto it = assetReferences_.find(assetContentHash);
    return it != assetReferences_.end() && it->second > 0U;
}

std::uint32_t MaterialResourceResidency::reference_count(
    std::uint64_t assetContentHash) const noexcept {
    const auto it = assetReferences_.find(assetContentHash);
    return it == assetReferences_.end() ? 0U : it->second;
}

MaterialResourceResidencyStats MaterialResourceResidency::stats() const noexcept {
    auto result = stats_;
    result.retainedAssetCount = assetReferences_.size();
    result.imageResourceCount = images_.size();
    result.samplerResourceCount = samplers_.size();
    for (const auto& [asset, references] : assetReferences_) {
        (void)asset;
        result.assetReferenceCount += references;
    }
    return result;
}

void MaterialResourceResidency::clear() noexcept {
    for (auto& [key, resource] : images_) {
        (void)key;
        if (resource.view) (void)device_.destroy_texture_view(resource.view, nullptr);
        if (resource.texture) (void)device_.destroy_texture(resource.texture, nullptr);
    }
    images_.clear();
    for (auto& [key, sampler] : samplers_) {
        (void)key;
        if (sampler) (void)device_.destroy_sampler(sampler, nullptr);
    }
    samplers_.clear();
    assetReferences_.clear();
}

} // namespace dve::render
