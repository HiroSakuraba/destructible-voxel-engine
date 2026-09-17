#include "dve/runtime_text3d_world.hpp"

#include <algorithm>
#include <limits>

namespace dve {
namespace {

CookedText3DAsset normalized_asset(const CookedText3DAsset& input, std::uint64_t objectId) {
    CookedText3DAsset asset = input;
    asset.objectId = objectId;
    asset.sideMesh.objectId = objectId;
    asset.sideMesh.contentHash = polygon_asset_content_hash(asset.sideMesh);
    asset.contentHash = text3d_content_hash(asset);
    return asset;
}

std::size_t asset_bytes(const CookedText3DAsset& asset) noexcept {
    return asset.textUtf8.size() + asset.glyphGeometry.size() * sizeof(Text3DGlyphGeometry) +
           asset.glyphInstances.size() * sizeof(SlugGlyphInstance) +
           asset.atlas.curveTexels.size() * sizeof(Float4) +
           asset.atlas.bandTexels.size() * sizeof(SlugBandTexel) +
           asset.sideMesh.vertices.size() * sizeof(PolygonVertex) +
           asset.sideMesh.indices.size() * sizeof(std::uint32_t);
}

} // namespace

std::vector<RuntimeText3DHandle> IRuntimeText3DWorld::create_objects(
    std::span<const RuntimeText3DCreateDesc> descs) {
    std::vector<RuntimeText3DHandle> result;
    result.reserve(descs.size());
    for (const auto& desc : descs) {
        const RuntimeText3DHandle handle = create_object(desc);
        if (handle == kInvalidRuntimeText3DHandle) {
            (void)destroy_objects(result);
            return {};
        }
        result.push_back(handle);
    }
    return result;
}

bool IRuntimeText3DWorld::destroy_objects(std::span<const RuntimeText3DHandle> handles) {
    bool success = true;
    for (const RuntimeText3DHandle handle : handles) success = destroy_object(handle) && success;
    return success;
}

RuntimeText3DHandle ReferenceRuntimeText3DWorld::create_object(const RuntimeText3DCreateDesc& desc) {
    if (desc.objectId == 0U || desc.asset == nullptr) return kInvalidRuntimeText3DHandle;
    for (const Slot& slot : slots_) {
        if (slot.alive && slot.instance.objectId == desc.objectId)
            return kInvalidRuntimeText3DHandle;
    }
    CookedText3DAsset asset = normalized_asset(*desc.asset, desc.objectId);
    if (!validate_text3d_asset(asset)) return kInvalidRuntimeText3DHandle;
    RuntimeText3DHandle handle{};
    if (!freeHandles_.empty()) {
        handle = freeHandles_.back();
        freeHandles_.pop_back();
    } else {
        if (slots_.size() >= static_cast<std::size_t>(kInvalidRuntimeText3DHandle))
            return kInvalidRuntimeText3DHandle;
        handle = static_cast<RuntimeText3DHandle>(slots_.size());
        slots_.push_back({});
    }
    Slot& slot = slots_[handle];
    slot.alive = true;
    slot.asset = std::move(asset);
    slot.instance = {desc.objectId, desc.objectId, desc.worldTransform, desc.visible, desc.selected,
                     desc.castShadows, desc.receiveGlobalIllumination};
    return handle;
}

bool ReferenceRuntimeText3DWorld::destroy_object(RuntimeText3DHandle handle) {
    if (handle >= slots_.size() || !slots_[handle].alive) return false;
    slots_[handle] = {};
    freeHandles_.push_back(handle);
    return true;
}

bool ReferenceRuntimeText3DWorld::update_object(RuntimeText3DHandle handle,
                                                 const RuntimeText3DUpdateDesc& desc) {
    if (handle >= slots_.size() || !slots_[handle].alive || desc.objectId == 0U || desc.asset == nullptr)
        return false;
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (index != handle && slots_[index].alive && slots_[index].instance.objectId == desc.objectId)
            return false;
    }
    CookedText3DAsset asset = normalized_asset(*desc.asset, desc.objectId);
    if (!validate_text3d_asset(asset)) return false;
    Slot replacement;
    replacement.alive = true;
    replacement.asset = std::move(asset);
    replacement.instance = {desc.objectId, desc.objectId, desc.worldTransform, desc.visible, desc.selected,
                            desc.castShadows, desc.receiveGlobalIllumination};
    slots_[handle] = std::move(replacement);
    return true;
}

std::optional<std::uint64_t> ReferenceRuntimeText3DWorld::readback_hash(
    RuntimeText3DHandle handle) const {
    if (handle >= slots_.size() || !slots_[handle].alive) return std::nullopt;
    return slots_[handle].asset.contentHash;
}

RuntimeText3DSceneSnapshot ReferenceRuntimeText3DWorld::snapshot() const {
    RuntimeText3DSceneSnapshot result;
    for (const Slot& slot : slots_) {
        if (!slot.alive) continue;
        result.assets.push_back(slot.asset);
        result.instances.push_back(slot.instance);
    }
    return result;
}

RuntimeText3DCounts ReferenceRuntimeText3DWorld::counts() const noexcept {
    RuntimeText3DCounts result;
    for (const Slot& slot : slots_) {
        if (!slot.alive) continue;
        ++result.objects;
        result.glyphs += slot.asset.glyphInstances.size();
        result.curves += slot.asset.atlas.curveTexels.size() / 2U;
        result.sideTriangles += slot.asset.sideMesh.indices.size() / 3U;
        result.residentBytes += asset_bytes(slot.asset);
    }
    return result;
}

std::optional<std::uint64_t> ReferenceRuntimeText3DWorld::object_id(
    RuntimeText3DHandle handle) const noexcept {
    if (handle >= slots_.size() || !slots_[handle].alive) return std::nullopt;
    return slots_[handle].instance.objectId;
}

std::optional<RigidTransform> ReferenceRuntimeText3DWorld::world_transform(
    RuntimeText3DHandle handle) const noexcept {
    if (handle >= slots_.size() || !slots_[handle].alive) return std::nullopt;
    return slots_[handle].instance.worldTransform;
}

const CookedText3DAsset* ReferenceRuntimeText3DWorld::asset(RuntimeText3DHandle handle) const noexcept {
    if (handle >= slots_.size() || !slots_[handle].alive) return nullptr;
    return &slots_[handle].asset;
}

} // namespace dve
