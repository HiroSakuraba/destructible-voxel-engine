#include "dve/region_game_damage_host.hpp"
#include <future>
#include <stdexcept>
using namespace dve;
static void require(bool x) {if(!x)throw std::runtime_error("R3.2 game host contract failed");}
static void make(GameWorld& w,GameObjectId id,int count) {
    auto& obj=w.vox[id];
    for(int i=0;i<count;++i)obj.map.contents[{i,0,0}]={static_cast<std::uint32_t>(i+1)};
}
int main() {
    GameWorld world;
    RegionDirtyDispatcher dispatcher;
    RegionGameDamageHostConfig conf;
    conf.maximumPendingIds=2;
    conf.maximumSnapshotsPerTick=1;
    conf.maximumOffersPerTick=2;
    make(world,10,5);
    {
        RegionGameDamageHost host(world,dispatcher,conf);
        world.damage({10,false,{}});
        WorkBudget budget;
        RegionDirtyTargetLookup lookup=[](std::uint64_t){return RegionDirtyTargets{};};
        const auto step=[&](){return host.tick({0,0,0,0,0},budget,lookup);};
        auto stats=step();
        require(stats.eventsObserved==1 && stats.editsOffered==2 && stats.pendingBricks==3);
        stats=step();
        require(stats.editsOffered==2 && stats.pendingBricks==1);
        // Re-damage during chunking invalidates the old incomplete snapshot.
        world.vox[10].map.contents[{5,0,0}]={6};world.damage({10,false,{}});
        stats=step();
        require(stats.editsOffered==2 && stats.pendingBricks==4);
        for(int i=0;i<3;++i){stats=step();}
        require(stats.pendingBricks==0 && stats.pendingObjects==0);
        world.vox.erase(10);world.damage({10,true,{}});
        stats=step();
        require(stats.forgotten>0);
        // Bounded listener inbox overflow falls back to authoritative rescan.
        make(world,21,2);make(world,22,2);make(world,23,2);
        world.damage({21,false,{}});world.damage({22,false,{}});world.damage({23,false,{}});
        stats=step();
        require(stats.overflowEvents>0 && stats.reconciliations==1);
        for(int i=0;i<9;++i){stats=step();}
        require(stats.pendingObjects==0);
        auto async=std::async(std::launch::async,[&](){return host.tick({0,0,0,0,0},budget,lookup);});
        require(async.get().wrongThread);
    }
    // Callback remains registered but weak inbox is gone: must be a safe no-op.
    world.damage({21,false,{}});
}
