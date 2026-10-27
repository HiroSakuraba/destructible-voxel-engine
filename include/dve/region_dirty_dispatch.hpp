#pragma once

// R3.1: optional authority-thread dispatcher for committed voxel edits.
// Ingestion is separate from publication. Collision and persistence tickets
// are deliberately not acknowledged here: their publication is not yet wired.
#include "dve/region_dirty_index.hpp"
#include "dve/work_debt.hpp"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace dve {
class GpuBrickMirror;
class PackedBrickmapScene;
class DynamicNavigationWorld;

struct RegionDirtyTargets {
    const VoxelObject* authority{};
    GpuBrickMirror* mirror{};
    PackedBrickmapScene* packed{};
    DynamicNavigationWorld* navigation{};
    float metresPerVoxel{0.1F};
};

using RegionDirtyTargetLookup = std::function<RegionDirtyTargets(std::uint64_t objectId)>;

enum class RegionDirtyOfferResult : std::uint8_t { Accepted, Backpressure, Invalid };

struct RegionDirtyDispatchConfig {
    std::size_t maximumBufferedEdits{262144U};
    std::size_t maximumTicketsPerTick{192U};
    std::uint32_t maximumDispatcherMicroseconds{1600U};
    // Dispatches work in a Chebyshev cube around the camera; no spatial-data
    // transform is performed here. Callers pass RegionKey in region cells.
    std::int32_t interestRadius{4};
};

struct RegionDirtyDispatchStats {
    std::size_t offered{};
    std::size_t forwarded{};
    std::size_t buffered{};
    std::size_t coalesced{};
    std::size_t gpuTickets{};
    std::size_t gpuBatches{};
    std::size_t navigationInvalidations{};
    std::size_t superseded{};
    std::size_t retried{};
    std::size_t failed{};
    std::size_t pending{};
    bool queryRejected{};
    bool hitTicketBudget{};
    bool hitTimeBudget{};
    PressureLevel pressure{PressureLevel::Normal};
};

class RegionDirtyDispatcher {
public:
    explicit RegionDirtyDispatcher(RegionDirtyIndex& queue, RegionDirtyDispatchConfig config = {});

    // Copies edit data before returning Accepted. If Backpressure is returned,
    // ownership stays with caller: retain/retry the original authoritative event.
    // This API is intentionally not an implicit callback that may drop edits.
    [[nodiscard]] RegionDirtyOfferResult offer(std::uint32_t worldId, std::uint8_t lod,
        std::uint64_t objectId, std::span<const AppliedBrickEdit> edits, RegionDirtyChannels channels);

    // Authority-thread-only. The lookup must return object-identity-stable
    // references for this call. The queue is also authority-thread-only.
    [[nodiscard]] RegionDirtyDispatchStats tick(RegionKey camera, const WorkBudget& budget,
        const RegionDirtyTargetLookup& lookup);

    // Explicit lifecycle invalidation: remove outstanding edits on object
    // teardown, restore or generation reset before the ID can be reused.
    void forget_object(std::uint64_t objectId);
    [[nodiscard]] std::size_t buffered_edit_count() const noexcept { return bufferedEdits_; }
    [[nodiscard]] std::size_t buffer_capacity() const noexcept { return config_.maximumBufferedEdits; }

private:
    struct Buffered {
        std::uint32_t worldId{};
        std::uint8_t lod{};
        std::uint64_t objectId{};
        RegionDirtyChannels channels{};
        std::vector<AppliedBrickEdit> edits{};
        std::size_t cursor{};
    };
    void forward_ingress(RegionDirtyDispatchStats& stats);
    RegionDirtyIndex& queue_;
    RegionDirtyDispatchConfig config_;
    std::deque<Buffered> ingress_{};
    std::size_t bufferedEdits_{};
    std::size_t totalOffered_{};
    bool favorNavNext_{};
};

} // namespace dve
