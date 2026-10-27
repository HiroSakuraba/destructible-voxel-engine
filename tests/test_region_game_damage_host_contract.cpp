#include "dve/region_game_damage_host.hpp"
#include <future>
#include <set>
#include <source_location>
#include <string>
#include <stdexcept>
using namespace dve;
static void require(bool x,std::source_location loc=std::source_location::current()) {
    if(!x)throw std::runtime_error("R3.2 game host contract failed at line "+std::to_string(loc.line()));
}
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
    world.destroy(21);
    const WorkBudget budget;
    const RegionDirtyTargetLookup lookup=[](std::uint64_t){return RegionDirtyTargets{};};
    // #241 interplay: every snapshot must request a geometry rebuild after the forget.
    {
        GameWorld w;RegionDirtyDispatcher d;d.capacity=1000;
        make(w,1,3);
        RegionGameDamageHost host(w,d);
        w.damage({1,false,{}});
        auto stats=host.tick({0,0,0,0,0},budget,lookup);
        require(stats.objectsSnapshotted==1 && d.replaced.size()==1 && d.replaced[0]==1);
        require(!d.forgotten.empty() && d.forgotten.back()==1);
        // Retired debris (no damage event) must forget indexed state.
        d.forgotten.clear();
        w.destroy(1);
        stats=host.tick({0,0,0,0,0},budget,lookup);
        require(stats.forgotten==1 && d.forgotten.size()==1 && d.forgotten[0]==1);
        // Unrelated (never indexed) object churn must not touch the dispatcher.
        d.forgotten.clear();
        w.destroy(999);
        stats=host.tick({0,0,0,0,0},budget,lookup);
        require(stats.forgotten==0 && d.forgotten.empty());
    }
    // An oversized object keeps its dirty state but must not block later ids.
    {
        GameWorld w;RegionDirtyDispatcher d;d.capacity=1000;
        RegionGameDamageHostConfig c;c.maximumSnapshotBricks=4;
        make(w,1,10);make(w,2,2);make(w,3,3);
        RegionGameDamageHost host(w,d,c);
        w.damage({1,false,{}});w.damage({2,false,{}});w.damage({3,false,{}});
        auto stats=host.tick({0,0,0,0,0},budget,lookup);
        require(stats.snapshotLimit && stats.oversizedObjects==1);
        require(stats.objectsSnapshotted==2 && stats.editsOffered==5 && stats.pendingObjects==0);
        // Not retried (no re-enumeration) until damaged again or rescanned.
        stats=host.tick({0,0,0,0,0},budget,lookup);
        require(stats.oversizedObjects==1 && stats.objectsSnapshotted==0);
        // Shrinks below the limit, then re-damaged: now snapshotted.
        for(int i=4;i<10;++i)w.vox[1].map.contents.erase({i,0,0});
        w.damage({1,false,{}});
        stats=host.tick({0,0,0,0,0},budget,lookup);
        require(stats.oversizedObjects==0 && stats.objectsSnapshotted==1 && !stats.snapshotLimit);
    }
    // More live objects than maximumPendingIds: rescan sweep converges under sustained damage.
    {
        GameWorld w;RegionDirtyDispatcher d;d.capacity=1000000;
        RegionGameDamageHostConfig c;c.maximumPendingIds=2;c.maximumSnapshotsPerTick=2;
        for(GameObjectId id=1;id<=9;++id)make(w,id,1);
        RegionGameDamageHost host(w,d,c);
        host.request_full_reconcile();
        std::set<GameObjectId> seen;
        for(int i=0;i<12;++i){
            w.damage({1,false,{}});w.damage({2,false,{}});w.damage({3,false,{}});
            (void)host.tick({0,0,0,0,0},budget,lookup);
            for(const auto& o:d.offered)seen.insert(o.id);
        }
        require(seen.size()==9);
    }
    // Listener invoked off the authority thread is not recorded unsafely; it forces a rescan.
    {
        GameWorld w;RegionDirtyDispatcher d;d.capacity=1000;
        make(w,1,2);
        RegionGameDamageHost host(w,d);
        std::async(std::launch::async,[&](){w.damage({1,false,{}});}).get();
        const auto stats=host.tick({0,0,0,0,0},budget,lookup);
        require(stats.foreignThreadEvents==1 && stats.reconciliations==1 && stats.objectsSnapshotted==1);
    }
}
