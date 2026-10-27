#include "dve/region_dirty_dispatch.hpp"
#include "dve/gpu_brick_mirror.hpp"
#include "dve/packed_brickmap.hpp"
#include "dve/v235_foundations.hpp"
#include <stdexcept>
#include <string>
#include <vector>
using namespace dve;
#define require(condition) do { if(!(condition)) throw std::runtime_error(std::string("R3.1 failed line ")+std::to_string(__LINE__)); } while(false)
static AppliedBrickEdit e(BrickKey k, std::uint32_t generation) {Bitset512 b;b.set(1);return {k,b,generation};}
static constexpr RegionDirtyChannels gpuNav=region_dirty_channel(RegionDirtyConsumer::Gpu)|region_dirty_channel(RegionDirtyConsumer::Navigation);

static void budgeted_delivery() {
    RegionCatalog c;
    RegionDirtyIndex q(c);
    RegionDirtyDispatcher d(q, {.maximumBufferedEdits=5,.maximumTicketsPerTick=8,.maximumDispatcherMicroseconds=100000,.interestRadius=2});
    VoxelObject o;
    o.bricks.emplace(BrickKey{0,0,0},Brick{2});
    GpuBrickMirror m;
    PackedBrickmapScene p;
    DynamicNavigationWorld nav;
    const auto target=[&](std::uint64_t id){return id==42?RegionDirtyTargets{&o,&m,&p,&nav,.5F}:RegionDirtyTargets{};};
    auto a=e({0,0,0},1),b=e({0,0,0},2);
    const std::vector edits{a,b};
    require(d.offer(0,0,42,edits,gpuNav|region_dirty_channel(RegionDirtyConsumer::Persistence))==RegionDirtyOfferResult::Accepted);
    require(d.offer(0,0,42,edits,gpuNav)==RegionDirtyOfferResult::Accepted);
    require(d.offer(0,0,42,edits,gpuNav)==RegionDirtyOfferResult::Backpressure);
    WorkBudget budget;budget.patchBrickLimit=1;
    const RegionKey camera{0,0,0,0,0};
    auto s=d.tick(camera,budget,target);
    require(s.forwarded==4 && s.gpuTickets==1 && s.gpuBatches==1 && s.navigationInvalidations==0 && s.pending==2);
    require(m.calls==1 && p.calls==1 && nav.calls==0 && d.buffered_edit_count()==0);
    require(d.offer(0,0,42,edits,gpuNav)==RegionDirtyOfferResult::Accepted);
    budget.patchBrickLimit=3;
    s=d.tick(camera,budget,target);
    require(s.navigationInvalidations==1 && nav.calls==1 && s.pending==1);
    // Persistence not processed automatically: receipt requires durable save.
    require(q.task_count()==1);
    d.forget_object(42);require(q.task_count()==0 && d.buffered_edit_count()==0);
}
static void stale_and_failed() {
    RegionCatalog c;
    RegionDirtyIndex q(c);
    RegionDirtyDispatcher d(q,{.maximumBufferedEdits=8,.maximumTicketsPerTick=8,.maximumDispatcherMicroseconds=100000,.interestRadius=1});
    VoxelObject o; o.bricks.emplace(BrickKey{-1,0,0},Brick{3});
    GpuBrickMirror mirror;
    const auto lookup=[&](std::uint64_t){return RegionDirtyTargets{&o,&mirror};};
    auto old=e({-1,0,0},2);
    require(d.offer(0,0,15,std::span<const AppliedBrickEdit>(&old,1),region_dirty_channel(RegionDirtyConsumer::Gpu))==RegionDirtyOfferResult::Accepted);
    WorkBudget budget;budget.patchBrickLimit=8;
    const RegionKey camera{0,0,-1,0,0};
    auto s=d.tick(camera,budget,lookup);
    require(s.gpuTickets==0 && q.task_count()==1);
    // Next tick processes refreshed generation; no stale source is published.
    s=d.tick(camera,budget,lookup);
    require(s.gpuTickets==1 && mirror.calls==1 && q.task_count()==0);
    const auto latest=e({-1,0,0},3);
    require(d.offer(0,0,15,std::span<const AppliedBrickEdit>(&latest,1),region_dirty_channel(RegionDirtyConsumer::Gpu))==RegionDirtyOfferResult::Accepted);
    mirror.throwOnUpdate=true;
    s=d.tick(camera,budget,lookup);
    require(s.failed==1 && q.task_count()==1 && s.retried>=1);
    mirror.throwOnUpdate=false;
    s=d.tick(camera,budget,lookup);
    require(s.gpuTickets==1 && q.task_count()==0);
}
static void channels_and_limits() {
    RegionCatalog c;
    RegionDirtyIndex q(c,{.maximumTasks=1,.maximumExamined=100});
    RegionDirtyDispatcher d(q,{.maximumBufferedEdits=10,.maximumTicketsPerTick=5,.maximumDispatcherMicroseconds=100000,.interestRadius=1});
    VoxelObject o;o.bricks.emplace(BrickKey{0,0,0},Brick{1});o.bricks.emplace(BrickKey{1,0,0},Brick{1});GpuBrickMirror m;
    const auto lookup=[&](std::uint64_t){return RegionDirtyTargets{&o,&m};};
    const std::vector edits{e({0,0,0},1),e({1,0,0},1)};
    require(d.offer(0,0,1,edits,region_dirty_channel(RegionDirtyConsumer::Gpu))==RegionDirtyOfferResult::Accepted);
    WorkBudget budget;budget.patchBrickLimit=1;
    auto s=d.tick({0,0,0,0,0},budget,lookup);
    require(s.gpuTickets==1 && d.buffered_edit_count()==1);
    s=d.tick({0,0,0,0,0},budget,lookup);
    require(d.buffered_edit_count()==0 && q.task_count()==0 && s.gpuTickets==1);
    d.forget_object(1);require(q.task_count()==0);
    require(d.offer(0,0,0,std::span<const AppliedBrickEdit>(&edits[0],1),gpuNav)==RegionDirtyOfferResult::Invalid);
}

static void no_navigation_starvation() {
    RegionCatalog c;RegionDirtyIndex q(c);
    RegionDirtyDispatcher d(q,{.maximumBufferedEdits=32,.maximumTicketsPerTick=1,.maximumDispatcherMicroseconds=100000,.interestRadius=1});
    VoxelObject o;for(int x=0;x<5;++x)o.bricks.emplace(BrickKey{x,0,0},Brick{1});
    GpuBrickMirror m;DynamicNavigationWorld nav;
    const auto lookup=[&](std::uint64_t){return RegionDirtyTargets{&o,&m,nullptr,&nav,.1F};};
    std::vector<AppliedBrickEdit> edits;
    for(int x=0;x<5;++x)edits.push_back(e({x,0,0},1));
    require(d.offer(0,0,77,edits,gpuNav)==RegionDirtyOfferResult::Accepted);
    WorkBudget budget;budget.patchBrickLimit=1;
    (void)d.tick({0,0,0,0,0},budget,lookup); // GPU first
    (void)d.tick({0,0,0,0,0},budget,lookup); // NAV next
    require(m.calls>=1 && nav.calls>=1); // cannot starve navigation behind continuous GPU edits
}

// A ticket whose brick vanished from the authority (storage replaced) or whose generation
// went backwards cannot be published incrementally; it must reconcile by rebuilding the
// GPU state, never sit at the head of the queue retrying forever.
static void removed_or_regressed_bricks_reconcile() {
    RegionCatalog c;
    RegionDirtyIndex q(c);
    RegionDirtyDispatcher d(q,{.maximumBufferedEdits=16,.maximumTicketsPerTick=1,.maximumDispatcherMicroseconds=100000,.interestRadius=1});
    VoxelObject gone; // brick {0,0,0} no longer exists
    VoxelObject healthy; healthy.bricks.emplace(BrickKey{0,0,0},Brick{1});
    GpuBrickMirror gm, hm; PackedBrickmapScene gp;
    const auto lookup=[&](std::uint64_t id){return id==1?RegionDirtyTargets{&gone,&gm,&gp}:RegionDirtyTargets{&healthy,&hm};};
    const auto gpu=region_dirty_channel(RegionDirtyConsumer::Gpu);
    auto removed=e({0,0,0},4), fresh=e({0,0,0},1);
    require(d.offer(0,0,1,std::span<const AppliedBrickEdit>(&removed,1),gpu)==RegionDirtyOfferResult::Accepted);
    require(d.offer(0,0,2,std::span<const AppliedBrickEdit>(&fresh,1),gpu)==RegionDirtyOfferResult::Accepted);
    WorkBudget budget;budget.patchBrickLimit=1;
    const RegionKey camera{0,0,0,0,0};
    gm.throwOnRebuild=true;
    auto s=d.tick(camera,budget,lookup);
    require(s.failed==1 && s.gpuTickets==0 && q.task_count()==2 && gm.rebuilds==0);
    gm.throwOnRebuild=false;
    s=d.tick(camera,budget,lookup);
    require(s.gpuRebuilds==1 && s.gpuTickets==1 && gm.rebuilds==1 && gp.rebuilds==1 && q.task_count()==1);
    s=d.tick(camera,budget,lookup); // the other object is not blocked behind it
    require(s.gpuTickets==1 && hm.calls==1 && q.task_count()==0);
    // Regression: authority generation lower than the queued ticket's.
    VoxelObject older; older.bricks.emplace(BrickKey{0,0,0},Brick{2});
    GpuBrickMirror om;
    const auto lookup2=[&](std::uint64_t){return RegionDirtyTargets{&older,&om};};
    auto newer=e({0,0,0},7);
    require(d.offer(0,0,3,std::span<const AppliedBrickEdit>(&newer,1),gpu)==RegionDirtyOfferResult::Accepted);
    s=d.tick(camera,budget,lookup2);
    require(s.gpuTickets==1 && om.rebuilds==1 && om.calls==0 && q.task_count()==0);
}

static void explicit_geometry_replacement_rebuilds_once() {
    RegionCatalog c;
    RegionDirtyIndex q(c);
    RegionDirtyDispatcher d(q);
    VoxelObject o; o.bricks.emplace(BrickKey{0,0,0},Brick{1});
    GpuBrickMirror m; PackedBrickmapScene p;
    const auto lookup=[&](std::uint64_t id){return id==9?RegionDirtyTargets{&o,&m,&p}:RegionDirtyTargets{};};
    WorkBudget budget;
    const RegionKey camera{0,0,0,0,0};
    d.mark_geometry_replaced(0); // reserved id ignored
    d.mark_geometry_replaced(9);
    d.mark_geometry_replaced(77); // unresolved object: retained, never blocks others
    auto s=d.tick(camera,budget,lookup);
    require(s.gpuRebuilds==1 && m.rebuilds==1 && p.rebuilds==1);
    s=d.tick(camera,budget,lookup);
    require(s.gpuRebuilds==0 && m.rebuilds==1);
    d.forget_object(77);
    d.mark_geometry_replaced(9);
    d.forget_object(9); // forgetting cancels a pending rebuild request
    s=d.tick(camera,budget,lookup);
    require(s.gpuRebuilds==0 && m.rebuilds==1);
    // A failed rebuild keeps the request for the next tick.
    d.mark_geometry_replaced(9);
    m.throwOnRebuild=true;
    s=d.tick(camera,budget,lookup);
    require(s.failed==1 && s.gpuRebuilds==0);
    m.throwOnRebuild=false;
    s=d.tick(camera,budget,lookup);
    require(s.gpuRebuilds==1 && m.rebuilds==2);
}

// Forwarding a large accepted batch must honour the time budget and keep its cursor.
static void ingress_forwarding_is_time_budgeted() {
    constexpr std::size_t kEdits=200000U;
    RegionCatalog c;
    RegionDirtyIndex q(c);
    RegionDirtyDispatcher d(q,{.maximumBufferedEdits=kEdits,.maximumTicketsPerTick=1,.maximumDispatcherMicroseconds=50,.interestRadius=1});
    std::vector<AppliedBrickEdit> edits;
    edits.reserve(kEdits);
    for(std::size_t i=0;i<kEdits;++i)edits.push_back(e({static_cast<std::int32_t>(i),0,0},1));
    require(d.offer(0,0,5,edits,region_dirty_channel(RegionDirtyConsumer::Persistence))==RegionDirtyOfferResult::Accepted);
    const auto lookup=[&](std::uint64_t){return RegionDirtyTargets{};};
    WorkBudget budget;
    auto s=d.tick({0,0,0,0,0},budget,lookup);
    require(s.hitTimeBudget && s.forwarded<kEdits && s.buffered>0 && s.buffered==d.buffered_edit_count());
    require(s.forwarded+s.buffered==kEdits && q.task_count()==s.forwarded);
    std::size_t ticks=1;
    while(d.buffered_edit_count()!=0 && ticks<2000000U){ s=d.tick({0,0,0,0,0},budget,lookup); ++ticks; }
    require(d.buffered_edit_count()==0 && q.task_count()==kEdits && ticks>1);
}

int main(){budgeted_delivery();stale_and_failed();channels_and_limits();no_navigation_starvation();
    removed_or_regressed_bricks_reconcile();explicit_geometry_replacement_rebuilds_once();ingress_forwarding_is_time_budgeted();}
