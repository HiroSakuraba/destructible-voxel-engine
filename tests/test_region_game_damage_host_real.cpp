#include "dve/region_game_damage_host.hpp"
#include "dve/gpu_brick_mirror.hpp"
#include "dve/packed_brickmap.hpp"
#include "dve/physics3d_backend.hpp"
#include <algorithm>
#include <memory>
#include <source_location>
#include <string>
#include <stdexcept>
#include <utility>
using namespace dve;
static void require(bool ok,std::source_location loc=std::source_location::current()) {
    if(!ok)throw std::runtime_error("R3.2 real GameWorld host test failed at line "+std::to_string(loc.line()));
}
int main() {
    auto physics=create_physics3d_world(Physics3DBackend::Reference);
    require(physics!=nullptr);
    GameWorld world(std::move(physics));
    GameObjectDesc desc;
    desc.name="r32_game_host_fixture";
    desc.voxels=std::make_unique<VoxelObject>(80);
    desc.voxels->fill_brick({0,0,0},1);
    desc.dynamic=false;
    const auto id=world.create_object(std::move(desc));
    require(id!=kInvalidGameObjectId);
    const auto original=world.render_objects();
    require(original.size()==1 && original.front().voxels);
    GpuBrickMirror mirror;
    PackedBrickmapScene packed;
    mirror.rebuild(*original.front().voxels);
    packed.rebuild(*original.front().voxels);
    RegionCatalog catalog;
    RegionDirtyIndex queue(catalog);
    RegionDirtyDispatcher dispatcher(queue,{.maximumBufferedEdits=1,
        .maximumTicketsPerTick=1,.maximumDispatcherMicroseconds=500000,.interestRadius=2});
    RegionGameDamageHostConfig config;
    config.channels=region_dirty_channel(RegionDirtyConsumer::Gpu);
    config.maximumPendingIds=1;
    config.maximumOffersPerTick=1;
    config.maximumSnapshotsPerTick=1;
    config.maximumSnapshotBricks=16;
    WorkBudget budget;budget.patchBrickLimit=1;
    const auto lookup=[&](std::uint64_t request)->RegionDirtyTargets {
        if(request!=id)return {};
        const auto views=world.render_objects();
        const auto it=std::find_if(views.begin(),views.end(),
            [&](const auto& view){return view.id==request;});
        return it==views.end() || !it->voxels?RegionDirtyTargets{}:
            RegionDirtyTargets{it->voxels,&mirror,&packed,nullptr,0.1F};
    };
    {
        RegionGameDamageHost host(world,dispatcher,config);
        const auto removed=world.damage_sphere(id,{0.35F,0.35F,0.35F},0.18F);
        require(removed && *removed>0);
        auto stats=host.tick({0,0,0,0,0},budget,lookup);
        require(stats.eventsObserved==1 && stats.objectsSnapshotted==1 && stats.editsOffered==1);
        for(int i=0;i<5 && queue.task_count()!=0;++i)
            stats=host.tick({0,0,0,0,0},budget,lookup);
        require(queue.task_count()==0);
        const auto after=world.render_objects();
        require(after.size()==1 && after.front().voxels);
        require(mirror.validate_against(*after.front().voxels));
        require(packed.validate_against(*after.front().voxels));
        const auto destroyed=world.damage_sphere(id,{0.35F,0.35F,0.35F},2.0F);
        require(destroyed && *destroyed>0);
        stats=host.tick({0,0,0,0,0},budget,lookup);
        require(stats.forgotten>0 && queue.task_count()==0);
        host.request_full_reconcile();
        stats=host.tick({0,0,0,0,0},budget,lookup);
        require(stats.reconciliations==1);
    }
    require(world.render_objects().empty());
}
