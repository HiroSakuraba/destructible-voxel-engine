#pragma once

// R3: experimental authority-thread-only destruction work index.
// Spatial catalog and residency remain independent of consumer delivery.
#include "dve/region_catalog.hpp"
#include "dve/voxel_object.hpp"
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace dve {

enum class RegionDirtyConsumer : std::uint8_t {
    Gpu = 0, Navigation = 1, Collision = 2, Persistence = 3,
};
using RegionDirtyChannels = std::uint8_t;
[[nodiscard]] constexpr RegionDirtyChannels region_dirty_channel(RegionDirtyConsumer consumer) noexcept {
    return static_cast<RegionDirtyChannels>(1U << static_cast<unsigned>(consumer));
}
inline constexpr RegionDirtyChannels kAllRegionDirtyChannels = 0x0FU;

struct RegionDirtyTaskKey {
    RegionKey region{};
    std::uint64_t objectId{};
    BrickKey brick{};
    RegionDirtyConsumer consumer{RegionDirtyConsumer::Gpu};
    auto operator<=>(const RegionDirtyTaskKey&) const = default;
};

struct RegionDirtyTicket {
    RegionDirtyTaskKey key{};
    std::uint32_t generation{};
    Bitset512 changedMask{};
    std::uint64_t revision{};
    std::uint64_t leaseId{};
};

struct RegionDirtyVoxelBounds {
    Int3 minimum{};
    Int3 maximum{}; // inclusive
};

enum class RegionDirtySubmitResult : std::uint8_t {
    Inserted, Coalesced, Ignored, Stale, Invalid, CapacityExceeded,
};
enum class RegionDirtyCompletion : std::uint8_t {
    Retired, Superseded, Retry, UnknownTicket,
};

struct RegionDirtyIndexConfig {
    std::size_t maximumTasks{1'000'000U};
    std::size_t maximumExamined{200'000U};
};

class RegionDirtyIndex {
public:
    explicit RegionDirtyIndex(const RegionCatalog& catalog, RegionDirtyIndexConfig config = {});
    // Latest generation wins; changed masks are ORed across queued generations.
    // Multi-channel submissions are admission-checked as a unit.
    [[nodiscard]] RegionDirtySubmitResult submit(
        std::uint32_t worldId, std::uint8_t lod, std::uint64_t objectId,
        const AppliedBrickEdit& edit, RegionDirtyChannels channels);
    [[nodiscard]] RegionDirtySubmitResult submit_full(
        std::uint32_t worldId, std::uint8_t lod, std::uint64_t objectId,
        BrickKey brick, std::uint32_t generation, RegionDirtyChannels channels);

    // Query budget is checked before issuing any tickets; no partial batch.
    [[nodiscard]] std::optional<std::vector<RegionDirtyTicket>> lease_box(
        std::uint32_t worldId, std::uint8_t lod, BrickKey minimumRegion,
        BrickKey maximumRegion, std::size_t maximumTickets,
        RegionDirtyChannels channels = kAllRegionDirtyChannels);
    [[nodiscard]] RegionDirtyCompletion complete(const RegionDirtyTicket& ticket, bool success);

    // Required before replacing/restoring an object when generations can reset.
    [[nodiscard]] std::size_t forget_object(std::uint64_t objectId);
    [[nodiscard]] std::size_t task_capacity() const noexcept { return config_.maximumTasks; }
    [[nodiscard]] std::size_t task_count() const noexcept { return tasks_.size(); }
    [[nodiscard]] std::size_t inflight_count() const noexcept { return inFlight_; }
    [[nodiscard]] std::size_t pending_count() const noexcept { return tasks_.size() - inFlight_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    // Safe inclusive bounds for DynamicNavigationWorld::mark_voxel_edit.
    [[nodiscard]] static std::optional<RegionDirtyVoxelBounds> voxel_bounds(BrickKey key) noexcept;
    [[nodiscard]] static AppliedBrickEdit gpu_edit(const RegionDirtyTicket& ticket) noexcept {
        return {ticket.key.brick, ticket.changedMask, ticket.generation};
    }

private:
    struct Value {
        std::uint32_t generation{};
        Bitset512 mask{};
        std::uint64_t revision{};
        std::uint64_t activeLease{}; // 0 = idle
    };
    const RegionCatalog& catalog_;
    RegionDirtyIndexConfig config_{};
    std::map<RegionDirtyTaskKey, Value> tasks_{};
    std::uint64_t revision_{};
    std::uint64_t nextLeaseId_{};
    std::size_t inFlight_{};
};

} // namespace dve
