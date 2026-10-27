#include "dve/region_game_damage.hpp"
#include <algorithm>
namespace dve {
RegionGameDamageSnapshot snapshot_region_game_damage(const GameWorld& world, const GameDamageEvent& event) {
    RegionGameDamageSnapshot snapshot;
    if (event.destroyed) snapshot.destroyed.push_back(event.objectId);
    std::vector<GameObjectId> ids=event.newFragmentIds;
    if (!event.destroyed) ids.push_back(event.objectId);
    std::sort(ids.begin(),ids.end());
    ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
    // Build a lookup from the single read-only render snapshot. Do not save its pointers
    // beyond this function: subsequent gameplay mutations may invalidate them.
    const auto views=world.render_objects();
    for (auto id:ids) {
        const auto it=std::find_if(views.begin(),views.end(),[&](const auto& obj){return obj.id==id;});
        if (it==views.end() || !it->voxels) { snapshot.unavailable.push_back(id); continue; }
        RegionGameDamageObject item;
        item.id=id;
        const auto keys=it->voxels->bricks().sorted_keys();
        item.fullEdits.reserve(keys.size());
        for (const auto key:keys) {
            const auto* brick=it->voxels->find_brick(key);
            if (brick) item.fullEdits.push_back({key,Bitset512::full(),brick->generation()});
        }
        snapshot.refreshed.push_back(std::move(item));
    }
    return snapshot;
}
} // namespace dve
