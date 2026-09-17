#include "dve/runtime_mesh_world.hpp"

#include <algorithm>

namespace dve {

bool IRuntimeMeshWorld::update_object(RuntimeMeshHandle, const RuntimeMeshUpdateDesc&) { return false; }

std::vector<RuntimeMeshHandle> IRuntimeMeshWorld::create_objects(
    std::span<const RuntimeMeshCreateDesc> descs) {
    std::vector<RuntimeMeshHandle> handles;
    handles.reserve(descs.size());
    for (const RuntimeMeshCreateDesc& desc : descs) {
        const RuntimeMeshHandle handle = create_object(desc);
        if (handle == kInvalidRuntimeMeshHandle) {
            destroy_objects(handles);
            return {};
        }
        handles.push_back(handle);
    }
    return handles;
}

bool IRuntimeMeshWorld::destroy_objects(std::span<const RuntimeMeshHandle> handles) {
    bool ok = true;
    for (const RuntimeMeshHandle handle : handles) ok = destroy_object(handle) && ok;
    return ok;
}

RuntimeMeshHandle ReferenceRuntimeMeshWorld::create_object(const RuntimeMeshCreateDesc& desc) {
    if (desc.asset == nullptr || !validate_polygon_asset(*desc.asset)) return kInvalidRuntimeMeshHandle;
    RuntimeMeshHandle handle;
    if (!freeHandles_.empty()) {
        handle = freeHandles_.back();
        freeHandles_.pop_back();
    } else {
        if (slots_.size() >= kInvalidRuntimeMeshHandle) return kInvalidRuntimeMeshHandle;
        handle = static_cast<RuntimeMeshHandle>(slots_.size());
        slots_.push_back({});
    }
    Slot& slot = slots_[handle];
    slot.alive = true;
    slot.objectId = desc.objectId;
    slot.worldTransform = desc.worldTransform;
    slot.asset = *desc.asset;
    slot.asset.contentHash = polygon_asset_content_hash(slot.asset);
    return handle;
}

bool ReferenceRuntimeMeshWorld::destroy_object(RuntimeMeshHandle handle) {
    if (handle >= slots_.size() || !slots_[handle].alive) return false;
    slots_[handle] = {};
    freeHandles_.push_back(handle);
    return true;
}

std::optional<std::uint64_t> ReferenceRuntimeMeshWorld::readback_hash(RuntimeMeshHandle handle) const {
    if (handle >= slots_.size() || !slots_[handle].alive) return std::nullopt;
    return polygon_asset_content_hash(slots_[handle].asset);
}

bool ReferenceRuntimeMeshWorld::update_object(RuntimeMeshHandle handle, const RuntimeMeshUpdateDesc& desc) {
    if (handle >= slots_.size() || !slots_[handle].alive || desc.asset == nullptr ||
        !validate_polygon_asset(*desc.asset)) return false;
    Slot replacement;
    replacement.alive = true;
    replacement.objectId = desc.objectId;
    replacement.worldTransform = desc.worldTransform;
    replacement.asset = *desc.asset;
    replacement.asset.contentHash = polygon_asset_content_hash(replacement.asset);
    slots_[handle] = std::move(replacement);
    return true;
}

RuntimeMeshCounts ReferenceRuntimeMeshWorld::counts() const noexcept {
    RuntimeMeshCounts counts;
    for (const Slot& slot : slots_) {
        if (!slot.alive) continue;
        ++counts.objects;
        counts.vertices += slot.asset.vertices.size();
        counts.indices += slot.asset.indices.size();
        counts.submeshes += slot.asset.submeshes.size();
        counts.materials += slot.asset.materials.size();
    }
    return counts;
}

std::optional<std::uint64_t> ReferenceRuntimeMeshWorld::object_id(RuntimeMeshHandle handle) const noexcept {
    if (handle >= slots_.size() || !slots_[handle].alive) return std::nullopt;
    return slots_[handle].objectId;
}

std::optional<RigidTransform> ReferenceRuntimeMeshWorld::world_transform(RuntimeMeshHandle handle) const noexcept {
    if (handle >= slots_.size() || !slots_[handle].alive) return std::nullopt;
    return slots_[handle].worldTransform;
}

} // namespace dve
