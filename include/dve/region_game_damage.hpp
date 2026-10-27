#pragma once
// R3.1 game producer adapter. Copies committed GameWorld voxel generations
// after its existing damage callback (which may fragment or replace geometry).
// Caller retains the returned owned records until an R3.1 offer is Accepted.
#include "dve/game_world.hpp"
#include <vector>
namespace dve {
struct RegionGameDamageObject {
    GameObjectId id{};
    std::vector<AppliedBrickEdit> fullEdits{};
    // fullEdits only name bricks that exist now. GPU mirrors never drop slots on an
    // incremental update, so bricks removed by a queued-destruction commit or a fragment
    // split would stay visible. The host must call
    // RegionDirtyDispatcher::mark_geometry_replaced(id) when this is true (it always is:
    // the adapter has no previous snapshot to prove that no brick disappeared).
    bool rebuildRequired{true};
};
struct RegionGameDamageSnapshot {
    std::vector<RegionGameDamageObject> refreshed{};
    std::vector<GameObjectId> destroyed{};
    std::vector<GameObjectId> unavailable{}; // caller must not silently ignore
};
// Can be invoked synchronously from GameWorld::on_damage. For queued damage,
// GameWorld may swap the primary voxel object and reset brick generations.
// Before offering the refreshed entries, caller must invalidate old tickets
// for that primary object (and preserve any unsaved persistence separately).
[[nodiscard]] RegionGameDamageSnapshot snapshot_region_game_damage(
    const GameWorld& world, const GameDamageEvent& event);
} // namespace dve
