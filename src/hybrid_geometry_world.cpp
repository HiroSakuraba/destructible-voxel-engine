#include "dve/hybrid_geometry_world.hpp"

namespace dve {
namespace {
void set_error(std::string* error, std::string_view value) {
    if (error != nullptr) error->assign(value.begin(), value.end());
}
}

RuntimeGeometryHandle HybridGeometryWorld::allocate_slot() {
    if (!freeHandles_.empty()) {
        const RuntimeGeometryHandle handle = freeHandles_.back();
        freeHandles_.pop_back();
        return handle;
    }
    if (slots_.size() >= kInvalidRuntimeGeometryHandle) return kInvalidRuntimeGeometryHandle;
    const auto handle = static_cast<RuntimeGeometryHandle>(slots_.size());
    slots_.push_back({});
    return handle;
}

void HybridGeometryWorld::reject_profile(std::string* error, GeometryKind kind) {
    ++rejectedByBuildProfile_;
    if (error != nullptr) {
        *error = "the " + std::string(to_string(compiled_geometry_build_mode())) +
                 " build profile does not enable " + std::string(to_string(kind)) + " geometry";
    }
}

RuntimeGeometryHandle HybridGeometryWorld::create_voxel_object(
    const RuntimeBrickmapCreateDesc& desc, std::string* error) {
    if (!geometry_kind_supported(GeometryKind::Voxel)) {
        reject_profile(error, GeometryKind::Voxel);
        return kInvalidRuntimeGeometryHandle;
    }
    if (voxelWorld_ == nullptr || desc.packedScene == nullptr) {
        set_error(error, "voxel renderer world or packed scene is unavailable");
        return kInvalidRuntimeGeometryHandle;
    }
    const RuntimeBrickmapHandle backend = voxelWorld_->create_object(desc);
    if (backend == kInvalidRuntimeBrickmapHandle) {
        set_error(error, "voxel renderer rejected object publication");
        return kInvalidRuntimeGeometryHandle;
    }
    const RuntimeGeometryHandle handle = allocate_slot();
    if (handle == kInvalidRuntimeGeometryHandle) {
        voxelWorld_->destroy_object(backend);
        set_error(error, "hybrid geometry handle space is exhausted");
        return handle;
    }
    Slot& slot = slots_[handle];
    slot.alive = true;
    slot.info = {desc.objectId, GeometryKind::Voxel, desc.worldTransform, backend,
                 kInvalidRuntimeMeshHandle};
    slot.voxelScene = desc.packedScene;
    return handle;
}

RuntimeGeometryHandle HybridGeometryWorld::create_polygon_object(
    const RuntimeMeshCreateDesc& desc, std::string* error) {
    if (!geometry_kind_supported(GeometryKind::Polygon)) {
        reject_profile(error, GeometryKind::Polygon);
        return kInvalidRuntimeGeometryHandle;
    }
    if (meshWorld_ == nullptr || desc.asset == nullptr) {
        set_error(error, "polygon renderer world or mesh asset is unavailable");
        return kInvalidRuntimeGeometryHandle;
    }
    const RuntimeMeshHandle backend = meshWorld_->create_object(desc);
    if (backend == kInvalidRuntimeMeshHandle) {
        set_error(error, "polygon renderer rejected object publication");
        return kInvalidRuntimeGeometryHandle;
    }
    const RuntimeGeometryHandle handle = allocate_slot();
    if (handle == kInvalidRuntimeGeometryHandle) {
        meshWorld_->destroy_object(backend);
        set_error(error, "hybrid geometry handle space is exhausted");
        return handle;
    }
    Slot& slot = slots_[handle];
    slot.alive = true;
    slot.info = {desc.objectId, GeometryKind::Polygon, desc.worldTransform,
                 kInvalidRuntimeBrickmapHandle, backend};
    slot.polygonAsset = desc.asset;
    return handle;
}

bool HybridGeometryWorld::update_transform(RuntimeGeometryHandle handle,
                                            const RigidTransform& transform,
                                            std::string* error) {
    if (handle >= slots_.size() || !slots_[handle].alive) {
        set_error(error, "unknown hybrid geometry handle");
        return false;
    }
    Slot& slot = slots_[handle];
    bool ok = false;
    if (slot.info.kind == GeometryKind::Voxel) {
        if (voxelWorld_ != nullptr && slot.voxelScene != nullptr &&
            voxelWorld_->supports_incremental_update()) {
            ok = voxelWorld_->update_object(slot.info.brickmapHandle,
                {slot.info.objectId, transform, slot.voxelScene});
        }
    } else if (meshWorld_ != nullptr && slot.polygonAsset != nullptr &&
               meshWorld_->supports_incremental_update()) {
        ok = meshWorld_->update_object(slot.info.meshHandle,
            {slot.info.objectId, transform, slot.polygonAsset});
    }
    if (!ok) {
        set_error(error, "geometry backend does not support the requested transform update");
        return false;
    }
    slot.info.worldTransform = transform;
    return true;
}

bool HybridGeometryWorld::destroy_object(RuntimeGeometryHandle handle) {
    if (handle >= slots_.size() || !slots_[handle].alive) return false;
    Slot& slot = slots_[handle];
    const bool destroyed = slot.info.kind == GeometryKind::Voxel
        ? voxelWorld_ != nullptr && voxelWorld_->destroy_object(slot.info.brickmapHandle)
        : meshWorld_ != nullptr && meshWorld_->destroy_object(slot.info.meshHandle);
    if (!destroyed) return false;
    slot = {};
    freeHandles_.push_back(handle);
    return true;
}

std::optional<RuntimeGeometryObjectInfo> HybridGeometryWorld::object(
    RuntimeGeometryHandle handle) const noexcept {
    if (handle >= slots_.size() || !slots_[handle].alive) return std::nullopt;
    return slots_[handle].info;
}

HybridGeometryWorldStats HybridGeometryWorld::stats() const noexcept {
    HybridGeometryWorldStats result;
    result.rejectedByBuildProfile = rejectedByBuildProfile_;
    for (const Slot& slot : slots_) {
        if (!slot.alive) continue;
        ++result.objects;
        if (slot.info.kind == GeometryKind::Voxel) ++result.voxelObjects;
        else ++result.polygonObjects;
    }
    return result;
}

} // namespace dve
