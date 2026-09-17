#include "dve/runtime_brickmap_world.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace dve {
namespace {

[[nodiscard]] bool finite_transform(const RigidTransform& transform) noexcept {
    return std::isfinite(transform.position.x) && std::isfinite(transform.position.y) &&
           std::isfinite(transform.position.z) && std::isfinite(transform.rotation.x) &&
           std::isfinite(transform.rotation.y) && std::isfinite(transform.rotation.z) &&
           std::isfinite(transform.rotation.w);
}

[[nodiscard]] bool valid_desc(const RuntimeBrickmapCreateDesc& desc) noexcept {
    return desc.objectId != 0U && desc.packedScene != nullptr && finite_transform(desc.worldTransform);
}

[[nodiscard]] bool same_transform(const RigidTransform& a, const RigidTransform& b) noexcept {
    return a.position.x == b.position.x && a.position.y == b.position.y &&
           a.position.z == b.position.z && a.rotation.x == b.rotation.x &&
           a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z &&
           a.rotation.w == b.rotation.w;
}

[[nodiscard]] std::span<const std::byte> index_bytes(const PackedBrickmapScene& scene) noexcept {
    return std::as_bytes(scene.index_grid());
}

[[nodiscard]] std::span<const std::byte> record_bytes(const PackedBrickmapScene& scene) noexcept {
    return std::as_bytes(scene.records());
}

[[nodiscard]] std::span<const std::byte> material_bytes(const PackedBrickmapScene& scene) noexcept {
    return std::as_bytes(scene.material_arena());
}

[[nodiscard]] std::size_t copy_dirty_ranges(
    std::span<std::byte> destination,
    std::span<const std::byte> previous,
    std::span<const std::byte> next,
    PersistentBrickmapHeapKind heapKind,
    std::size_t destinationBase,
    std::vector<PersistentRuntimeBrickmapUploadRange>& ranges,
    std::size_t& uploadedBytes) {
    if (destination.size() < next.size()) return 0U;
    const std::size_t common = std::min(previous.size(), next.size());
    std::size_t rangeCount = 0U;
    std::size_t i = 0U;
    while (i < common) {
        if (previous[i] == next[i]) {
            ++i;
            continue;
        }
        const std::size_t begin = i;
        while (i < common && previous[i] != next[i]) ++i;
        std::memcpy(destination.data() + begin, next.data() + begin, i - begin);
        uploadedBytes += i - begin;
        ranges.push_back({heapKind, destinationBase + begin, i - begin});
        ++rangeCount;
    }
    if (next.size() > common) {
        std::memcpy(destination.data() + common, next.data() + common, next.size() - common);
        uploadedBytes += next.size() - common;
        ++rangeCount;
    }
    return rangeCount;
}

[[nodiscard]] std::size_t reservation_bytes(
    std::size_t liveBytes,
    std::size_t alignment,
    std::size_t heapCapacity) noexcept {
    const std::size_t requested = std::max(liveBytes, alignment);
    if (requested >= heapCapacity) return requested;
    const std::size_t rounded = std::bit_ceil(requested);
    return rounded == 0U || rounded > heapCapacity ? requested : rounded;
}

[[nodiscard]] bool bytes_equal(
    const std::vector<std::byte>& storage,
    const PersistentAllocation& allocation,
    std::size_t byteCount,
    std::span<const std::byte> expected) noexcept {
    if (byteCount != expected.size() || allocation.offset > storage.size() ||
        byteCount > storage.size() - allocation.offset) {
        return false;
    }
    return byteCount == 0U ||
        std::memcmp(storage.data() + allocation.offset, expected.data(), byteCount) == 0;
}

} // namespace

bool IRuntimeBrickmapWorld::update_object(
    RuntimeBrickmapHandle,
    const RuntimeBrickmapUpdateDesc&) {
    return false;
}

std::vector<RuntimeBrickmapHandle> IRuntimeBrickmapWorld::create_objects(
    std::span<const RuntimeBrickmapCreateDesc> descs) {
    std::vector<RuntimeBrickmapHandle> handles;
    handles.reserve(descs.size());
    for (const RuntimeBrickmapCreateDesc& desc : descs) {
        const RuntimeBrickmapHandle handle = create_object(desc);
        if (handle == kInvalidRuntimeBrickmapHandle) {
            for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
                (void)destroy_object(*it);
            }
            return {};
        }
        handles.push_back(handle);
    }
    return handles;
}

bool IRuntimeBrickmapWorld::destroy_objects(
    std::span<const RuntimeBrickmapHandle> handles) {
    bool success = true;
    for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
        success = destroy_object(*it) && success;
    }
    return success;
}

RuntimeBrickmapHandle ReferenceRuntimeBrickmapWorld::create_object(
    const RuntimeBrickmapCreateDesc& desc) {
    if (!valid_desc(desc)) return kInvalidRuntimeBrickmapHandle;
    for (const Slot& slot : slots_) {
        if (slot.alive && slot.objectId == desc.objectId) return kInvalidRuntimeBrickmapHandle;
    }

    Slot staged;
    staged.alive = true;
    staged.objectId = desc.objectId;
    staged.worldTransform = make_rigid_transform(
        desc.worldTransform.position, desc.worldTransform.rotation);
    staged.scene = *desc.packedScene;

    if (!freeHandles_.empty()) {
        const RuntimeBrickmapHandle handle = freeHandles_.back();
        freeHandles_.pop_back();
        slots_[handle] = std::move(staged);
        return handle;
    }
    if (slots_.size() >= static_cast<std::size_t>(kInvalidRuntimeBrickmapHandle)) {
        return kInvalidRuntimeBrickmapHandle;
    }
    slots_.push_back(std::move(staged));
    return static_cast<RuntimeBrickmapHandle>(slots_.size() - 1U);
}

bool ReferenceRuntimeBrickmapWorld::destroy_object(RuntimeBrickmapHandle handle) {
    if (handle >= slots_.size() || !slots_[handle].alive) return false;
    slots_[handle] = {};
    freeHandles_.push_back(handle);
    return true;
}

std::optional<std::uint64_t> ReferenceRuntimeBrickmapWorld::readback_hash(
    RuntimeBrickmapHandle handle) const {
    if (handle >= slots_.size() || !slots_[handle].alive) return std::nullopt;
    return slots_[handle].scene.readback_hash();
}

bool ReferenceRuntimeBrickmapWorld::update_object(
    RuntimeBrickmapHandle handle,
    const RuntimeBrickmapUpdateDesc& desc) {
    if (handle >= slots_.size() || !slots_[handle].alive || !valid_desc(desc) ||
        slots_[handle].objectId != desc.objectId) {
        return false;
    }
    slots_[handle].worldTransform = make_rigid_transform(
        desc.worldTransform.position, desc.worldTransform.rotation);
    slots_[handle].scene = *desc.packedScene;
    return true;
}

RuntimeBrickmapCounts ReferenceRuntimeBrickmapWorld::counts() const noexcept {
    RuntimeBrickmapCounts result;
    for (const Slot& slot : slots_) {
        if (!slot.alive) continue;
        ++result.objects;
        const PackedBrickmapStorageStats stats = slot.scene.storage_stats();
        result.brickRecords += stats.slots;
        result.indexCells += stats.indexCells;
        result.recordBytes += stats.recordBytes;
        result.materialArenaBytes += stats.materialArenaBytes;
    }
    return result;
}

std::optional<std::uint64_t> ReferenceRuntimeBrickmapWorld::object_id(
    RuntimeBrickmapHandle handle) const {
    if (handle >= slots_.size() || !slots_[handle].alive) return std::nullopt;
    return slots_[handle].objectId;
}

std::optional<RigidTransform> ReferenceRuntimeBrickmapWorld::world_transform(
    RuntimeBrickmapHandle handle) const {
    if (handle >= slots_.size() || !slots_[handle].alive) return std::nullopt;
    return slots_[handle].worldTransform;
}

PersistentRuntimeBrickmapWorld::PersistentRuntimeBrickmapWorld(
    const PersistentRuntimeBrickmapWorldConfig& config)
    : config_(config),
      indexHeap_(config.indexHeapBytes, config.allocationAlignment),
      recordHeap_(config.recordHeapBytes, config.allocationAlignment),
      materialHeap_(config.materialHeapBytes, config.allocationAlignment),
      indexStorage_(config.indexHeapBytes),
      recordStorage_(config.recordHeapBytes),
      materialStorage_(config.materialHeapBytes) {}

bool PersistentRuntimeBrickmapWorld::object_id_available(
    std::uint64_t objectId,
    RuntimeBrickmapHandle except) const noexcept {
    if (objectId == 0U) return false;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (i == except) continue;
        if (slots_[i].alive && slots_[i].objectId == objectId) return false;
    }
    return true;
}

RuntimeBrickmapHandle PersistentRuntimeBrickmapWorld::create_object(
    const RuntimeBrickmapCreateDesc& desc) {
    ++totals_.createCalls;
    if (!valid_desc(desc) || !object_id_available(desc.objectId)) {
        ++totals_.failedPublications;
        return kInvalidRuntimeBrickmapHandle;
    }

    const auto index = index_bytes(*desc.packedScene);
    const auto records = record_bytes(*desc.packedScene);
    const auto materials = material_bytes(*desc.packedScene);
    auto indexAllocation = indexHeap_.allocate(
        reservation_bytes(index.size(), config_.allocationAlignment, indexHeap_.capacity()),
        config_.allocationAlignment);
    auto recordAllocation = recordHeap_.allocate(
        reservation_bytes(records.size(), config_.allocationAlignment, recordHeap_.capacity()),
        config_.allocationAlignment);
    auto materialAllocation = materialHeap_.allocate(
        reservation_bytes(materials.size(), config_.allocationAlignment, materialHeap_.capacity()),
        config_.allocationAlignment);
    if (!indexAllocation || !recordAllocation || !materialAllocation) {
        if (indexAllocation) (void)indexHeap_.release(indexAllocation->handle);
        if (recordAllocation) (void)recordHeap_.release(recordAllocation->handle);
        if (materialAllocation) (void)materialHeap_.release(materialAllocation->handle);
        ++totals_.failedPublications;
        return kInvalidRuntimeBrickmapHandle;
    }

    if (!index.empty()) std::memcpy(indexStorage_.data() + indexAllocation->offset, index.data(), index.size());
    if (!records.empty()) std::memcpy(recordStorage_.data() + recordAllocation->offset, records.data(), records.size());
    if (!materials.empty()) std::memcpy(materialStorage_.data() + materialAllocation->offset, materials.data(), materials.size());

    Slot staged;
    staged.alive = true;
    staged.objectId = desc.objectId;
    staged.worldTransform = make_rigid_transform(desc.worldTransform.position, desc.worldTransform.rotation);
    staged.indexAllocation = *indexAllocation;
    staged.recordAllocation = *recordAllocation;
    staged.materialAllocation = *materialAllocation;
    staged.indexBytes = index.size();
    staged.recordBytes = records.size();
    staged.materialBytes = materials.size();
    staged.shadow = *desc.packedScene;
    staged.lastUpload.publicationGeneration = 1U;
    staged.lastUpload.indexBytesUploaded = index.size();
    staged.lastUpload.recordBytesUploaded = records.size();
    staged.lastUpload.materialBytesUploaded = materials.size();
    staged.lastUpload.dirtyRangeCount =
        static_cast<std::size_t>(!index.empty()) + static_cast<std::size_t>(!records.empty()) +
        static_cast<std::size_t>(!materials.empty());
    if (!index.empty()) staged.pendingUploads.push_back(
        {PersistentBrickmapHeapKind::Index, staged.indexAllocation.offset, index.size()});
    if (!records.empty()) staged.pendingUploads.push_back(
        {PersistentBrickmapHeapKind::Record, staged.recordAllocation.offset, records.size()});
    if (!materials.empty()) staged.pendingUploads.push_back(
        {PersistentBrickmapHeapKind::Material, staged.materialAllocation.offset, materials.size()});

    totals_.totalUploadedBytes += staged.lastUpload.total_bytes_uploaded();
    totals_.totalDirtyRanges += staged.lastUpload.dirtyRangeCount;

    RuntimeBrickmapHandle handle{};
    if (!freeHandles_.empty()) {
        handle = freeHandles_.back();
        freeHandles_.pop_back();
        slots_[handle] = std::move(staged);
    } else {
        if (slots_.size() >= static_cast<std::size_t>(kInvalidRuntimeBrickmapHandle)) {
            (void)indexHeap_.release(indexAllocation->handle);
            (void)recordHeap_.release(recordAllocation->handle);
            (void)materialHeap_.release(materialAllocation->handle);
            ++totals_.failedPublications;
            return kInvalidRuntimeBrickmapHandle;
        }
        slots_.push_back(std::move(staged));
        handle = static_cast<RuntimeBrickmapHandle>(slots_.size() - 1U);
    }
    return handle;
}

bool PersistentRuntimeBrickmapWorld::destroy_object(RuntimeBrickmapHandle handle) {
    ++totals_.destroyCalls;
    if (handle >= slots_.size() || !slots_[handle].alive) return false;
    Slot& slot = slots_[handle];
    const bool released = indexHeap_.release(slot.indexAllocation.handle) &&
        recordHeap_.release(slot.recordAllocation.handle) &&
        materialHeap_.release(slot.materialAllocation.handle);
    slot = {};
    freeHandles_.push_back(handle);
    return released;
}

std::optional<std::uint64_t> PersistentRuntimeBrickmapWorld::readback_hash(
    RuntimeBrickmapHandle handle) const {
    if (handle >= slots_.size() || !slots_[handle].alive) return std::nullopt;
    const Slot& slot = slots_[handle];
    if (!bytes_equal(indexStorage_, slot.indexAllocation, slot.indexBytes, index_bytes(slot.shadow)) ||
        !bytes_equal(recordStorage_, slot.recordAllocation, slot.recordBytes, record_bytes(slot.shadow)) ||
        !bytes_equal(materialStorage_, slot.materialAllocation, slot.materialBytes, material_bytes(slot.shadow))) {
        const_cast<PersistentRuntimeBrickmapWorld*>(this)->totals_.readbackFailures++;
        return std::nullopt;
    }
    return slot.shadow.readback_hash();
}

bool PersistentRuntimeBrickmapWorld::update_object(
    RuntimeBrickmapHandle handle,
    const RuntimeBrickmapUpdateDesc& desc) {
    ++totals_.updateCalls;
    if (handle >= slots_.size() || !slots_[handle].alive || !valid_desc(desc) ||
        slots_[handle].objectId != desc.objectId || !object_id_available(desc.objectId, handle)) {
        ++totals_.failedPublications;
        return false;
    }

    Slot& slot = slots_[handle];
    const auto newIndex = index_bytes(*desc.packedScene);
    const auto newRecords = record_bytes(*desc.packedScene);
    const auto newMaterials = material_bytes(*desc.packedScene);

    struct Replacement {
        bool needed{};
        std::optional<PersistentAllocation> allocation{};
    } indexReplacement, recordReplacement, materialReplacement;
    indexReplacement.needed = newIndex.size() > slot.indexAllocation.capacity;
    recordReplacement.needed = newRecords.size() > slot.recordAllocation.capacity;
    materialReplacement.needed = newMaterials.size() > slot.materialAllocation.capacity;

    if (indexReplacement.needed) indexReplacement.allocation = indexHeap_.allocate(
        reservation_bytes(newIndex.size(), config_.allocationAlignment, indexHeap_.capacity()),
        config_.allocationAlignment);
    if (recordReplacement.needed) recordReplacement.allocation = recordHeap_.allocate(
        reservation_bytes(newRecords.size(), config_.allocationAlignment, recordHeap_.capacity()),
        config_.allocationAlignment);
    if (materialReplacement.needed) materialReplacement.allocation = materialHeap_.allocate(
        reservation_bytes(newMaterials.size(), config_.allocationAlignment, materialHeap_.capacity()),
        config_.allocationAlignment);
    if ((indexReplacement.needed && !indexReplacement.allocation) ||
        (recordReplacement.needed && !recordReplacement.allocation) ||
        (materialReplacement.needed && !materialReplacement.allocation)) {
        if (indexReplacement.allocation) (void)indexHeap_.release(indexReplacement.allocation->handle);
        if (recordReplacement.allocation) (void)recordHeap_.release(recordReplacement.allocation->handle);
        if (materialReplacement.allocation) (void)materialHeap_.release(materialReplacement.allocation->handle);
        ++totals_.failedPublications;
        return false;
    }

    RuntimeBrickmapUploadStats upload;
    slot.pendingUploads.clear();
    upload.publicationGeneration = slot.lastUpload.publicationGeneration + 1U;
    upload.transformChanged = !same_transform(slot.worldTransform, make_rigid_transform(
        desc.worldTransform.position, desc.worldTransform.rotation));

    const auto publish_section = [&](PersistentBrickmapHeapKind heapKind,
                                     PersistentByteHeap& heap,
                                     std::vector<std::byte>& storage,
                                     PersistentAllocation& liveAllocation,
                                     std::size_t& liveBytes,
                                     std::span<const std::byte> previous,
                                     std::span<const std::byte> next,
                                     Replacement& replacement,
                                     std::size_t& bytesUploaded) {
        if (replacement.needed) {
            PersistentAllocation fresh = *replacement.allocation;
            if (!next.empty()) std::memcpy(storage.data() + fresh.offset, next.data(), next.size());
            bytesUploaded += next.size();
            if (!next.empty()) {
                ++upload.dirtyRangeCount;
                slot.pendingUploads.push_back({heapKind, fresh.offset, next.size()});
            }
            (void)heap.release(liveAllocation.handle);
            heap.record_relocation();
            liveAllocation = fresh;
            liveBytes = next.size();
            ++upload.relocatedAllocationCount;
            return;
        }
        std::span<std::byte> destination(
            storage.data() + liveAllocation.offset, liveAllocation.capacity);
        upload.dirtyRangeCount += copy_dirty_ranges(
            destination, previous, next, heapKind, liveAllocation.offset,
            slot.pendingUploads, bytesUploaded);
        liveBytes = next.size();
    };

    publish_section(PersistentBrickmapHeapKind::Index,
                    indexHeap_, indexStorage_, slot.indexAllocation, slot.indexBytes,
                    index_bytes(slot.shadow), newIndex, indexReplacement, upload.indexBytesUploaded);
    publish_section(PersistentBrickmapHeapKind::Record,
                    recordHeap_, recordStorage_, slot.recordAllocation, slot.recordBytes,
                    record_bytes(slot.shadow), newRecords, recordReplacement, upload.recordBytesUploaded);
    publish_section(PersistentBrickmapHeapKind::Material,
                    materialHeap_, materialStorage_, slot.materialAllocation, slot.materialBytes,
                    material_bytes(slot.shadow), newMaterials, materialReplacement, upload.materialBytesUploaded);

    slot.worldTransform = make_rigid_transform(desc.worldTransform.position, desc.worldTransform.rotation);
    slot.shadow = *desc.packedScene;
    slot.lastUpload = upload;
    totals_.totalUploadedBytes += upload.total_bytes_uploaded();
    totals_.totalDirtyRanges += upload.dirtyRangeCount;
    totals_.totalRelocations += upload.relocatedAllocationCount;
    return true;
}

PersistentRuntimeBrickmapWorldStats PersistentRuntimeBrickmapWorld::stats() const noexcept {
    PersistentRuntimeBrickmapWorldStats result = totals_;
    result.indexHeap = indexHeap_.stats();
    result.recordHeap = recordHeap_.stats();
    result.materialHeap = materialHeap_.stats();
    for (const Slot& slot : slots_) {
        if (!slot.alive) continue;
        ++result.live.objects;
        const PackedBrickmapStorageStats sceneStats = slot.shadow.storage_stats();
        result.live.brickRecords += sceneStats.slots;
        result.live.indexCells += sceneStats.indexCells;
        result.live.recordBytes += sceneStats.recordBytes;
        result.live.materialArenaBytes += sceneStats.materialArenaBytes;
    }
    return result;
}

std::optional<RuntimeBrickmapUploadStats> PersistentRuntimeBrickmapWorld::last_upload_stats(
    RuntimeBrickmapHandle handle) const noexcept {
    if (handle >= slots_.size() || !slots_[handle].alive) return std::nullopt;
    return slots_[handle].lastUpload;
}

std::optional<std::uint64_t> PersistentRuntimeBrickmapWorld::object_id(
    RuntimeBrickmapHandle handle) const noexcept {
    if (handle >= slots_.size() || !slots_[handle].alive) return std::nullopt;
    return slots_[handle].objectId;
}

std::optional<RigidTransform> PersistentRuntimeBrickmapWorld::world_transform(
    RuntimeBrickmapHandle handle) const noexcept {
    if (handle >= slots_.size() || !slots_[handle].alive) return std::nullopt;
    return slots_[handle].worldTransform;
}

std::optional<PersistentRuntimeBrickmapObjectView> PersistentRuntimeBrickmapWorld::object_view(
    RuntimeBrickmapHandle handle) const noexcept {
    if (handle >= slots_.size() || !slots_[handle].alive) return std::nullopt;
    const Slot& slot = slots_[handle];
    return PersistentRuntimeBrickmapObjectView{
        slot.objectId, slot.worldTransform, slot.indexAllocation, slot.recordAllocation,
        slot.materialAllocation, slot.indexBytes, slot.recordBytes, slot.materialBytes,
        slot.shadow.readback_hash()};
}

std::span<const PersistentRuntimeBrickmapUploadRange>
PersistentRuntimeBrickmapWorld::pending_upload_ranges(
    RuntimeBrickmapHandle handle) const noexcept {
    if (handle >= slots_.size() || !slots_[handle].alive) return {};
    return slots_[handle].pendingUploads;
}

std::span<const std::byte> PersistentRuntimeBrickmapWorld::heap_bytes(
    PersistentBrickmapHeapKind heap,
    std::size_t offset,
    std::size_t bytes) const noexcept {
    const std::vector<std::byte>* storage = nullptr;
    switch (heap) {
    case PersistentBrickmapHeapKind::Index: storage = &indexStorage_; break;
    case PersistentBrickmapHeapKind::Record: storage = &recordStorage_; break;
    case PersistentBrickmapHeapKind::Material: storage = &materialStorage_; break;
    }
    if (storage == nullptr || offset > storage->size() || bytes > storage->size() - offset) return {};
    return std::span<const std::byte>(storage->data() + offset, bytes);
}

bool PersistentRuntimeBrickmapWorld::clear_pending_upload_ranges(
    RuntimeBrickmapHandle handle) noexcept {
    if (handle >= slots_.size() || !slots_[handle].alive) return false;
    slots_[handle].pendingUploads.clear();
    return true;
}

} // namespace dve
