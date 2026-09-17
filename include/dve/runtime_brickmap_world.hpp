#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "dve/packed_brickmap.hpp"
#include "dve/persistent_byte_heap.hpp"
#include "dve/transform.hpp"

namespace dve {

using RuntimeBrickmapHandle = std::uint32_t;
constexpr RuntimeBrickmapHandle kInvalidRuntimeBrickmapHandle = 0xFFFFFFFFU;

struct RuntimeBrickmapCreateDesc {
    std::uint64_t objectId{};
    RigidTransform worldTransform{};
    const PackedBrickmapScene* packedScene{};
};

using RuntimeBrickmapUpdateDesc = RuntimeBrickmapCreateDesc;

struct RuntimeBrickmapCounts {
    std::size_t objects{};
    std::size_t brickRecords{};
    std::size_t indexCells{};
    std::size_t recordBytes{};
    std::size_t materialArenaBytes{};
};

struct RuntimeBrickmapUploadStats {
    std::uint64_t publicationGeneration{};
    std::size_t indexBytesUploaded{};
    std::size_t recordBytesUploaded{};
    std::size_t materialBytesUploaded{};
    std::size_t dirtyRangeCount{};
    std::size_t relocatedAllocationCount{};
    bool transformChanged{};

    [[nodiscard]] std::size_t total_bytes_uploaded() const noexcept {
        return indexBytesUploaded + recordBytesUploaded + materialBytesUploaded;
    }
};

// Narrow publication capability used by the runtime scene transaction. Implementations own
// stable backend slots and must not retain pointers from RuntimeBrickmapCreateDesc after a call
// returns. Batch creation is all-or-nothing: the default implementation rolls back earlier
// objects if any later object fails.
class IRuntimeBrickmapWorld {
public:
    virtual ~IRuntimeBrickmapWorld() = default;

    [[nodiscard]] virtual RuntimeBrickmapHandle create_object(
        const RuntimeBrickmapCreateDesc& desc) = 0;
    virtual bool destroy_object(RuntimeBrickmapHandle handle) = 0;
    [[nodiscard]] virtual std::optional<std::uint64_t> readback_hash(
        RuntimeBrickmapHandle handle) const = 0;

    // Optional incremental publication path. Backends that do not implement it return false;
    // the runtime may then replace the object transactionally through create/destroy.
    virtual bool update_object(
        RuntimeBrickmapHandle handle,
        const RuntimeBrickmapUpdateDesc& desc);
    [[nodiscard]] virtual bool supports_incremental_update() const noexcept { return false; }

    [[nodiscard]] virtual std::vector<RuntimeBrickmapHandle> create_objects(
        std::span<const RuntimeBrickmapCreateDesc> descs);
    virtual bool destroy_objects(std::span<const RuntimeBrickmapHandle> handles);
};

// CPU reference implementation for publication, rollback, stable-slot reuse, and readback
// equality tests. It deep-copies packed data and deliberately never rebuilds unrelated slots
// when one deferred object is published or removed.
class ReferenceRuntimeBrickmapWorld final : public IRuntimeBrickmapWorld {
public:
    [[nodiscard]] RuntimeBrickmapHandle create_object(
        const RuntimeBrickmapCreateDesc& desc) override;
    bool destroy_object(RuntimeBrickmapHandle handle) override;
    [[nodiscard]] std::optional<std::uint64_t> readback_hash(
        RuntimeBrickmapHandle handle) const override;
    bool update_object(
        RuntimeBrickmapHandle handle,
        const RuntimeBrickmapUpdateDesc& desc) override;
    [[nodiscard]] bool supports_incremental_update() const noexcept override { return true; }

    [[nodiscard]] RuntimeBrickmapCounts counts() const noexcept;
    [[nodiscard]] std::size_t object_count() const noexcept { return counts().objects; }
    [[nodiscard]] std::size_t full_rebuild_count() const noexcept { return fullRebuildCount_; }
    [[nodiscard]] std::optional<std::uint64_t> object_id(RuntimeBrickmapHandle handle) const;
    [[nodiscard]] std::optional<RigidTransform> world_transform(RuntimeBrickmapHandle handle) const;

private:
    struct Slot {
        bool alive{};
        std::uint64_t objectId{};
        RigidTransform worldTransform{};
        PackedBrickmapScene scene{};
    };

    std::vector<Slot> slots_{};
    std::vector<RuntimeBrickmapHandle> freeHandles_{};
    std::size_t fullRebuildCount_{};
};

struct PersistentRuntimeBrickmapWorldConfig {
    std::size_t indexHeapBytes{16ULL * 1024ULL * 1024ULL};
    std::size_t recordHeapBytes{16ULL * 1024ULL * 1024ULL};
    std::size_t materialHeapBytes{16ULL * 1024ULL * 1024ULL};
    std::size_t allocationAlignment{256U};
};

enum class PersistentBrickmapHeapKind : std::uint8_t { Index, Record, Material };

struct PersistentRuntimeBrickmapUploadRange {
    PersistentBrickmapHeapKind heap{PersistentBrickmapHeapKind::Index};
    std::size_t destinationOffset{};
    std::size_t bytes{};
};

struct PersistentRuntimeBrickmapObjectView {
    std::uint64_t objectId{};
    RigidTransform worldTransform{};
    PersistentAllocation indexAllocation{};
    PersistentAllocation recordAllocation{};
    PersistentAllocation materialAllocation{};
    std::size_t indexBytes{};
    std::size_t recordBytes{};
    std::size_t materialBytes{};
    std::uint64_t expectedReadbackHash{};
};

struct PersistentRuntimeBrickmapWorldStats {
    RuntimeBrickmapCounts live{};
    PersistentByteHeapStats indexHeap{};
    PersistentByteHeapStats recordHeap{};
    PersistentByteHeapStats materialHeap{};
    std::uint64_t createCalls{};
    std::uint64_t updateCalls{};
    std::uint64_t destroyCalls{};
    std::uint64_t failedPublications{};
    std::uint64_t readbackFailures{};
    std::size_t totalUploadedBytes{};
    std::size_t totalDirtyRanges{};
    std::size_t totalRelocations{};
};

// Executable cross-platform model of the D3D12 persistent heap contract. It uses three shared
// byte heaps, stable object handles, allocate-copy-swap relocation, and byte-exact readback.
// The Windows backend uses the same allocation and dirty-range rules with DEFAULT/UPLOAD heaps.
class PersistentRuntimeBrickmapWorld final : public IRuntimeBrickmapWorld {
public:
    explicit PersistentRuntimeBrickmapWorld(
        const PersistentRuntimeBrickmapWorldConfig& config = {});

    [[nodiscard]] RuntimeBrickmapHandle create_object(
        const RuntimeBrickmapCreateDesc& desc) override;
    bool destroy_object(RuntimeBrickmapHandle handle) override;
    [[nodiscard]] std::optional<std::uint64_t> readback_hash(
        RuntimeBrickmapHandle handle) const override;
    bool update_object(
        RuntimeBrickmapHandle handle,
        const RuntimeBrickmapUpdateDesc& desc) override;
    [[nodiscard]] bool supports_incremental_update() const noexcept override { return true; }

    [[nodiscard]] PersistentRuntimeBrickmapWorldStats stats() const noexcept;
    [[nodiscard]] std::optional<RuntimeBrickmapUploadStats> last_upload_stats(
        RuntimeBrickmapHandle handle) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> object_id(
        RuntimeBrickmapHandle handle) const noexcept;
    [[nodiscard]] std::optional<RigidTransform> world_transform(
        RuntimeBrickmapHandle handle) const noexcept;
    [[nodiscard]] std::optional<PersistentRuntimeBrickmapObjectView> object_view(
        RuntimeBrickmapHandle handle) const noexcept;
    [[nodiscard]] std::span<const PersistentRuntimeBrickmapUploadRange> pending_upload_ranges(
        RuntimeBrickmapHandle handle) const noexcept;
    [[nodiscard]] std::span<const std::byte> heap_bytes(
        PersistentBrickmapHeapKind heap,
        std::size_t offset,
        std::size_t bytes) const noexcept;
    bool clear_pending_upload_ranges(RuntimeBrickmapHandle handle) noexcept;

private:
    struct Slot {
        bool alive{};
        std::uint64_t objectId{};
        RigidTransform worldTransform{};
        PersistentAllocation indexAllocation{};
        PersistentAllocation recordAllocation{};
        PersistentAllocation materialAllocation{};
        std::size_t indexBytes{};
        std::size_t recordBytes{};
        std::size_t materialBytes{};
        PackedBrickmapScene shadow{};
        RuntimeBrickmapUploadStats lastUpload{};
        std::vector<PersistentRuntimeBrickmapUploadRange> pendingUploads{};
    };

    PersistentRuntimeBrickmapWorldConfig config_{};
    PersistentByteHeap indexHeap_{};
    PersistentByteHeap recordHeap_{};
    PersistentByteHeap materialHeap_{};
    std::vector<std::byte> indexStorage_{};
    std::vector<std::byte> recordStorage_{};
    std::vector<std::byte> materialStorage_{};
    std::vector<Slot> slots_{};
    std::vector<RuntimeBrickmapHandle> freeHandles_{};
    PersistentRuntimeBrickmapWorldStats totals_{};

    [[nodiscard]] bool object_id_available(
        std::uint64_t objectId,
        RuntimeBrickmapHandle except = kInvalidRuntimeBrickmapHandle) const noexcept;
};

} // namespace dve
