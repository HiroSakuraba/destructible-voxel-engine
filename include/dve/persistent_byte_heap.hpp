#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dve {

struct PersistentAllocationHandle {
    std::uint32_t slot{0xFFFFFFFFU};
    std::uint32_t generation{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot != 0xFFFFFFFFU;
    }
    friend bool operator==(const PersistentAllocationHandle&, const PersistentAllocationHandle&) = default;
};

struct PersistentAllocation {
    PersistentAllocationHandle handle{};
    std::size_t offset{};
    std::size_t size{};
    std::size_t capacity{};
};

struct PersistentByteHeapStats {
    std::size_t capacityBytes{};
    std::size_t liveBytes{};
    std::size_t reservedBytes{};
    std::size_t freeBytes{};
    std::size_t largestFreeSpan{};
    std::size_t liveAllocations{};
    std::size_t freeSpanCount{};
    std::size_t allocationFailures{};
    std::size_t relocationCount{};
    double externalFragmentation{};
};

// Deterministic best-fit allocator used by persistent renderer heaps. The allocator never moves
// an allocation implicitly. Callers can allocate a replacement, copy, then release the old range
// when growth is required, preserving the stage-publish-commit ownership contract.
class PersistentByteHeap {
public:
    PersistentByteHeap() = default;
    explicit PersistentByteHeap(std::size_t capacityBytes, std::size_t defaultAlignment = 16U);

    void reset(std::size_t capacityBytes, std::size_t defaultAlignment = 16U);

    [[nodiscard]] std::optional<PersistentAllocation> allocate(
        std::size_t sizeBytes,
        std::size_t alignment = 0U);
    [[nodiscard]] bool release(PersistentAllocationHandle handle) noexcept;
    [[nodiscard]] std::optional<PersistentAllocation> allocation(
        PersistentAllocationHandle handle) const noexcept;
    [[nodiscard]] bool contains(PersistentAllocationHandle handle) const noexcept {
        return allocation(handle).has_value();
    }

    [[nodiscard]] PersistentByteHeapStats stats() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacityBytes_; }
    [[nodiscard]] std::size_t default_alignment() const noexcept { return defaultAlignment_; }

    // Records a caller-performed allocate/copy/release relocation for telemetry.
    void record_relocation() noexcept { ++relocationCount_; }

private:
    struct Slot {
        bool alive{};
        std::uint32_t generation{1U};
        std::size_t offset{};
        std::size_t size{};
        std::size_t capacity{};
    };
    struct FreeSpan {
        std::size_t offset{};
        std::size_t size{};
    };

    std::size_t capacityBytes_{};
    std::size_t defaultAlignment_{16U};
    std::vector<Slot> slots_{};
    std::vector<std::uint32_t> freeSlots_{};
    std::vector<FreeSpan> freeSpans_{};
    std::size_t allocationFailures_{};
    std::size_t relocationCount_{};

    void insert_free_span(FreeSpan span) noexcept;
};

} // namespace dve
