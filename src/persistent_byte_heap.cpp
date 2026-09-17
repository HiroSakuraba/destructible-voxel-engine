#include "dve/persistent_byte_heap.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace dve {
namespace {

[[nodiscard]] bool power_of_two(std::size_t value) noexcept {
    return value != 0U && (value & (value - 1U)) == 0U;
}

[[nodiscard]] std::optional<std::size_t> align_up_checked(
    std::size_t value,
    std::size_t alignment) noexcept {
    if (!power_of_two(alignment)) return std::nullopt;
    const std::size_t mask = alignment - 1U;
    if (value > std::numeric_limits<std::size_t>::max() - mask) return std::nullopt;
    return (value + mask) & ~mask;
}

} // namespace

PersistentByteHeap::PersistentByteHeap(
    std::size_t capacityBytes,
    std::size_t defaultAlignment) {
    reset(capacityBytes, defaultAlignment);
}

void PersistentByteHeap::reset(
    std::size_t capacityBytes,
    std::size_t defaultAlignment) {
    if (!power_of_two(defaultAlignment)) {
        throw std::invalid_argument("PersistentByteHeap alignment must be a power of two");
    }
    capacityBytes_ = capacityBytes;
    defaultAlignment_ = defaultAlignment;
    slots_.clear();
    freeSlots_.clear();
    freeSpans_.clear();
    allocationFailures_ = 0U;
    relocationCount_ = 0U;
    if (capacityBytes_ != 0U) freeSpans_.push_back({0U, capacityBytes_});
}

std::optional<PersistentAllocation> PersistentByteHeap::allocate(
    std::size_t sizeBytes,
    std::size_t alignment) {
    if (sizeBytes == 0U) {
        sizeBytes = 1U;
    }
    if (alignment == 0U) alignment = defaultAlignment_;
    if (!power_of_two(alignment)) {
        ++allocationFailures_;
        return std::nullopt;
    }

    std::size_t bestIndex = freeSpans_.size();
    std::size_t bestOffset{};
    std::size_t bestWaste = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = 0; i < freeSpans_.size(); ++i) {
        const FreeSpan span = freeSpans_[i];
        const auto aligned = align_up_checked(span.offset, alignment);
        if (!aligned || *aligned < span.offset) continue;
        const std::size_t prefix = *aligned - span.offset;
        if (prefix > span.size || sizeBytes > span.size - prefix) continue;
        const std::size_t waste = span.size - prefix - sizeBytes;
        if (waste < bestWaste ||
            (waste == bestWaste && (bestIndex == freeSpans_.size() || *aligned < bestOffset))) {
            bestIndex = i;
            bestOffset = *aligned;
            bestWaste = waste;
        }
    }
    if (bestIndex == freeSpans_.size()) {
        ++allocationFailures_;
        return std::nullopt;
    }

    const FreeSpan selected = freeSpans_[bestIndex];
    freeSpans_.erase(freeSpans_.begin() + static_cast<std::ptrdiff_t>(bestIndex));
    if (bestOffset > selected.offset) {
        insert_free_span({selected.offset, bestOffset - selected.offset});
    }
    const std::size_t allocationEnd = bestOffset + sizeBytes;
    const std::size_t selectedEnd = selected.offset + selected.size;
    if (allocationEnd < selectedEnd) {
        insert_free_span({allocationEnd, selectedEnd - allocationEnd});
    }

    std::uint32_t slotIndex{};
    if (!freeSlots_.empty()) {
        slotIndex = freeSlots_.back();
        freeSlots_.pop_back();
    } else {
        if (slots_.size() >= static_cast<std::size_t>(0xFFFFFFFFU)) {
            insert_free_span({bestOffset, sizeBytes});
            ++allocationFailures_;
            return std::nullopt;
        }
        slotIndex = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back({});
    }

    Slot& slot = slots_[slotIndex];
    slot.alive = true;
    slot.offset = bestOffset;
    slot.size = sizeBytes;
    slot.capacity = sizeBytes;
    return PersistentAllocation{{slotIndex, slot.generation}, slot.offset, slot.size, slot.capacity};
}

bool PersistentByteHeap::release(PersistentAllocationHandle handle) noexcept {
    if (handle.slot >= slots_.size()) return false;
    Slot& slot = slots_[handle.slot];
    if (!slot.alive || slot.generation != handle.generation) return false;
    insert_free_span({slot.offset, slot.capacity});
    slot.alive = false;
    slot.offset = 0U;
    slot.size = 0U;
    slot.capacity = 0U;
    ++slot.generation;
    if (slot.generation == 0U) slot.generation = 1U;
    freeSlots_.push_back(handle.slot);
    return true;
}

std::optional<PersistentAllocation> PersistentByteHeap::allocation(
    PersistentAllocationHandle handle) const noexcept {
    if (handle.slot >= slots_.size()) return std::nullopt;
    const Slot& slot = slots_[handle.slot];
    if (!slot.alive || slot.generation != handle.generation) return std::nullopt;
    return PersistentAllocation{handle, slot.offset, slot.size, slot.capacity};
}

void PersistentByteHeap::insert_free_span(FreeSpan span) noexcept {
    if (span.size == 0U) return;
    const auto it = std::lower_bound(
        freeSpans_.begin(), freeSpans_.end(), span.offset,
        [](const FreeSpan& value, std::size_t offset) { return value.offset < offset; });
    freeSpans_.insert(it, span);

    std::vector<FreeSpan> merged;
    merged.reserve(freeSpans_.size());
    for (const FreeSpan current : freeSpans_) {
        if (!merged.empty() && merged.back().offset + merged.back().size >= current.offset) {
            const std::size_t end = std::max(
                merged.back().offset + merged.back().size,
                current.offset + current.size);
            merged.back().size = end - merged.back().offset;
        } else {
            merged.push_back(current);
        }
    }
    freeSpans_.swap(merged);
}

PersistentByteHeapStats PersistentByteHeap::stats() const noexcept {
    PersistentByteHeapStats result;
    result.capacityBytes = capacityBytes_;
    result.allocationFailures = allocationFailures_;
    result.relocationCount = relocationCount_;
    result.freeSpanCount = freeSpans_.size();
    for (const Slot& slot : slots_) {
        if (!slot.alive) continue;
        ++result.liveAllocations;
        result.liveBytes += slot.size;
        result.reservedBytes += slot.capacity;
    }
    for (const FreeSpan span : freeSpans_) {
        result.freeBytes += span.size;
        result.largestFreeSpan = std::max(result.largestFreeSpan, span.size);
    }
    if (result.freeBytes != 0U) {
        result.externalFragmentation = 1.0 -
            static_cast<double>(result.largestFreeSpan) / static_cast<double>(result.freeBytes);
    }
    return result;
}

} // namespace dve
