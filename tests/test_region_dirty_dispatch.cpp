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

int main(){budgeted_delivery();stale_and_failed();channels_and_limits();}
