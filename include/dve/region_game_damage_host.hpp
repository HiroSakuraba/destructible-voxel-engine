#pragma once
// R3.2: opt-in GameWorld derived-state host. Listener holds a weak inbox:
// GameWorld may outlive this host without calling a dangling callback target.
// Use a dedicated GPU/navigation RegionDirtyIndex, never a persistence queue.
#include "dve/region_dirty_dispatch.hpp"
#include "dve/game_world.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace dve {
struct RegionGameDamageHostConfig {
    std::uint32_t worldId{};
    std::uint8_t lod{};
    RegionDirtyChannels channels{static_cast<RegionDirtyChannels>(
        region_dirty_channel(RegionDirtyConsumer::Gpu) |
        region_dirty_channel(RegionDirtyConsumer::Navigation))};
    std::size_t maximumPendingIds{4096};
    std::size_t maximumSnapshotsPerTick{8};
    std::size_t maximumOffersPerTick{1024};
    std::size_t maximumSnapshotBricks{262144};
};
struct RegionGameDamageHostStats {
    std::size_t eventsObserved{}; // cumulative
    std::size_t overflowEvents{}; // cumulative
    std::size_t reconciliations{};
    std::size_t objectsSnapshotted{};
    std::size_t forgotten{};
    std::size_t editsOffered{};
    std::size_t pendingObjects{};
    std::size_t pendingBricks{};
    bool backpressure{};
    bool snapshotLimit{};
    bool wrongThread{};
    RegionDirtyDispatchStats delivery{};
};

class RegionGameDamageHost {
public:
    // GameWorld and dispatcher must outlive this host; tick on the authority thread.
    RegionGameDamageHost(GameWorld& world, RegionDirtyDispatcher& dispatcher,
                         RegionGameDamageHostConfig config = {});
    ~RegionGameDamageHost();
    RegionGameDamageHost(const RegionGameDamageHost&) = delete;
    RegionGameDamageHost& operator=(const RegionGameDamageHost&) = delete;

    [[nodiscard]] RegionGameDamageHostStats tick(RegionKey camera, const WorkBudget& budget,
                                                   const RegionDirtyTargetLookup& lookup);
    // Also use after checkpoint restore or external geometry mutations that bypass on_damage.
    void request_full_reconcile();
private:
    struct Inbox;
    struct Snapshot {
        GameObjectId id{};
        std::vector<AppliedBrickEdit> edits;
        std::size_t offset{};
    };
    void reconcile(RegionGameDamageHostStats& stats);
    bool begin_snapshot(GameObjectId id, RegionGameDamageHostStats& stats);

    GameWorld& world_;
    RegionDirtyDispatcher& dispatcher_;
    RegionGameDamageHostConfig config_;
    std::thread::id ownerThread_;
    std::shared_ptr<Inbox> inbox_;
    std::set<GameObjectId> indexedObjects_;
    std::optional<Snapshot> current_;
};
} // namespace dve
