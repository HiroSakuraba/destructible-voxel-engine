#include "dve/region_game_damage.hpp"
#include "dve/region_dirty_dispatch.hpp"
#include "dve/gpu_brick_mirror.hpp"
#include "dve/packed_brickmap.hpp"
#include "dve/physics3d_backend.hpp"
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace dve;
static void require(bool valid) {if (!valid) throw std::runtime_error("R3.1 GameWorld producer integration failed");}

int main() {
    auto physics=create_physics3d_world(Physics3DBackend::Reference);
    require(physics != nullptr);
    GameWorld world(std::move(physics));
    GameObjectDesc desc;
    desc.name="region_damage_ingestion_fixture";
    desc.voxels=std::make_unique<VoxelObject>(8);
    desc.voxels->fill_brick({0,0,0},1);
    desc.dynamic=false;
    const GameObjectId id=world.create_object(std::move(desc));
    require(id!=kInvalidGameObjectId);

    const auto before=world.render_objects();
    require(before.size()==1 && before.front().voxels);
    GpuBrickMirror mirror;
    PackedBrickmapScene packed;
    mirror.rebuild(*before.front().voxels);
    packed.rebuild(*before.front().voxels);

    std::vector<RegionGameDamageSnapshot> events;
    world.on_damage([&](const GameDamageEvent& event) {
        events.push_back(snapshot_region_game_damage(world,event));
    });
    const auto removed=world.damage_sphere(id,{0.35F,0.35F,0.35F},0.18F);
    require(removed && *removed>0 && events.size()==1);
    require(events.front().destroyed.empty());
    require(events.front().unavailable.empty());
    require(events.front().refreshed.size()==1);
    require(events.front().refreshed.front().id==id);
    const auto& edits=events.front().refreshed.front().fullEdits;
    require(!edits.empty());
    require(events.front().refreshed.front().rebuildRequired);

    RegionCatalog catalog;
    RegionDirtyIndex queue(catalog);
    RegionDirtyDispatcher dispatcher(queue,{.maximumBufferedEdits=1024,.maximumTicketsPerTick=16,
        .maximumDispatcherMicroseconds=500000,.interestRadius=2});
    require(dispatcher.offer(0,0,id,edits,region_dirty_channel(RegionDirtyConsumer::Gpu))
        == RegionDirtyOfferResult::Accepted);
    dispatcher.mark_geometry_replaced(id);
    WorkBudget budget;
    auto stats=dispatcher.tick({0,0,0,0,0},budget,[&](std::uint64_t requestedId) {
        if (requestedId!=id) return RegionDirtyTargets{};
        const auto current=world.render_objects();
        return current.empty()?RegionDirtyTargets{}:
            RegionDirtyTargets{current.front().voxels,&mirror,&packed,nullptr,.1F};
    });
    require(stats.gpuRebuilds==1 && stats.gpuTickets>0 && queue.task_count()==0);
    const auto after=world.render_objects();
    require(after.size()==1 && after.front().voxels);
    require(mirror.validate_against(*after.front().voxels));
    require(packed.validate_against(*after.front().voxels));

    // A second blast that removes the object emits a destruction marker,
    // so the host can forget any residual work rather than publish stale data.
    const auto destroyed=world.damage_sphere(id,{0.35F,0.35F,0.35F},2.0F);
    require(destroyed && *destroyed>0 && events.size()==2);
    require(events.back().destroyed.size()==1 && events.back().destroyed.front()==id);
    require(events.back().refreshed.empty());
    dispatcher.forget_object(id);
}
