#include "dve/region_dirty_index.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dve {
namespace {
[[nodiscard]] bool valid_channel_mask(RegionDirtyChannels mask) noexcept {
    return mask != 0U && (mask & static_cast<RegionDirtyChannels>(~kAllRegionDirtyChannels)) == 0U;
}
[[nodiscard]] bool in_bounds(RegionKey key, BrickKey low, BrickKey high) noexcept {
    return key.x >= low.x && key.x <= high.x && key.y >= low.y && key.y <= high.y &&
           key.z >= low.z && key.z <= high.z;
}
} // namespace

RegionDirtyIndex::RegionDirtyIndex(const RegionCatalog& catalog, RegionDirtyIndexConfig config)
    : catalog_(catalog), config_(config) {
    if (config_.maximumTasks == 0 || config_.maximumExamined == 0)
        throw std::invalid_argument("region dirty index requires positive admission limits");
}

RegionDirtySubmitResult RegionDirtyIndex::submit(
    std::uint32_t worldId, std::uint8_t lod, std::uint64_t objectId,
    const AppliedBrickEdit& edit, RegionDirtyChannels channels) {
    if (objectId == 0 || !valid_channel_mask(channels)) return RegionDirtySubmitResult::Invalid;
    if (edit.changedMask.none()) return RegionDirtySubmitResult::Ignored;
    const RegionKey region = catalog_.region_of_brick(worldId, lod, edit.key);
    std::array<RegionDirtyTaskKey, 4> keys{};
    std::size_t count = 0;
    std::size_t newTasks = 0;
    std::size_t staleTasks = 0;
    for (unsigned n = 0; n < 4; ++n) {
        if ((channels & (1U << n)) == 0U) continue;
        const auto key = RegionDirtyTaskKey{region, objectId, edit.key, static_cast<RegionDirtyConsumer>(n)};
        keys[count++] = key;
        const auto found = tasks_.find(key);
        if (found == tasks_.end()) ++newTasks;
        else if (edit.generation < found->second.generation) ++staleTasks;
    }
    if (newTasks > config_.maximumTasks - tasks_.size()) return RegionDirtySubmitResult::CapacityExceeded;
    if (staleTasks == count) return RegionDirtySubmitResult::Stale;
    bool changed = false;
    for (std::size_t n = 0; n < count; ++n) {
        const auto& key = keys[n];
        auto [it, inserted] = tasks_.try_emplace(key);
        auto& value = it->second;
        if (!inserted && edit.generation < value.generation) continue;
        const auto before = value.mask;
        value.mask |= edit.changedMask;
        if (inserted || edit.generation != value.generation || !(value.mask == before)) {
            value.generation = edit.generation;
            value.revision = ++revision_;
            changed = true;
        }
    }
    return changed ? (newTasks != 0 ? RegionDirtySubmitResult::Inserted : RegionDirtySubmitResult::Coalesced)
                   : RegionDirtySubmitResult::Ignored;
}

RegionDirtySubmitResult RegionDirtyIndex::submit_full(
    std::uint32_t worldId, std::uint8_t lod, std::uint64_t objectId,
    BrickKey brick, std::uint32_t generation, RegionDirtyChannels channels) {
    return submit(worldId, lod, objectId, {brick, Bitset512::full(), generation}, channels);
}

std::optional<std::vector<RegionDirtyTicket>> RegionDirtyIndex::lease_box(
    std::uint32_t worldId, std::uint8_t lod, BrickKey low, BrickKey high,
    std::size_t maximumTickets) {
    if (low.x > high.x || low.y > high.y || low.z > high.z) return std::nullopt;
    const RegionDirtyTaskKey from{{worldId, lod, low.x,
        std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::min()},
        0, {}, RegionDirtyConsumer::Gpu};
    std::vector<decltype(tasks_)::iterator> selected;
    selected.reserve(std::min(maximumTickets, tasks_.size()));
    std::size_t examined = 0;
    for (auto it = tasks_.lower_bound(from); it != tasks_.end(); ++it) {
        const auto& key = it->first;
        if (key.region.worldId != worldId || key.region.lod != lod || key.region.x > high.x) break;
        if (++examined > config_.maximumExamined) return std::nullopt;
        if (!in_bounds(key.region, low, high) || it->second.activeLease != 0) continue;
        if (selected.size() < maximumTickets) selected.push_back(it);
    }
    // Only now mutate leases; a rejected region query never strands tickets.
    std::vector<RegionDirtyTicket> tickets;
    tickets.reserve(selected.size());
    for (auto it : selected) {
        auto& value = it->second;
        if (nextLeaseId_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("region dirty lease id exhausted");
        const auto leaseId = ++nextLeaseId_;
        tickets.push_back({it->first, value.generation, value.mask, value.revision, leaseId});
        value.activeLease = leaseId;
        ++inFlight_;
    }
    return tickets;
}

RegionDirtyCompletion RegionDirtyIndex::complete(const RegionDirtyTicket& ticket, bool success) {
    const auto it = tasks_.find(ticket.key);
    if (it == tasks_.end() || ticket.leaseId == 0 || it->second.activeLease != ticket.leaseId)
        return RegionDirtyCompletion::UnknownTicket;
    auto& value = it->second;
    value.activeLease = 0;
    --inFlight_;
    if (!success) return RegionDirtyCompletion::Retry;
    if (value.revision != ticket.revision) return RegionDirtyCompletion::Superseded;
    tasks_.erase(it);
    ++revision_;
    return RegionDirtyCompletion::Retired;
}

std::size_t RegionDirtyIndex::forget_object(std::uint64_t objectId) {
    std::size_t removed = 0;
    for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (it->first.objectId != objectId) { ++it; continue; }
        if (it->second.activeLease != 0) --inFlight_;
        it = tasks_.erase(it);
        ++removed;
    }
    if (removed != 0) ++revision_;
    return removed;
}

std::optional<RegionDirtyVoxelBounds> RegionDirtyIndex::voxel_bounds(BrickKey key) noexcept {
    const auto convert = [](std::int32_t coordinate) -> std::optional<std::pair<std::int32_t, std::int32_t>> {
        const auto first = static_cast<std::int64_t>(coordinate) * static_cast<std::int64_t>(kBrickDim);
        const auto last = first + (kBrickDim - 1);
        if (first < std::numeric_limits<std::int32_t>::min() ||
            last > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
        return {{static_cast<std::int32_t>(first), static_cast<std::int32_t>(last)}};
    };
    const auto x = convert(key.x), y = convert(key.y), z = convert(key.z);
    if (!x || !y || !z) return std::nullopt;
    return RegionDirtyVoxelBounds{{x->first,y->first,z->first},{x->second,y->second,z->second}};
}
} // namespace dve
