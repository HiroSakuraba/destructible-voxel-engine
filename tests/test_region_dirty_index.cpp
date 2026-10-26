#include "dve/region_dirty_index.hpp"
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

using namespace dve;
static void require(bool ok) { if (!ok) throw std::runtime_error("R3 region dirty contract"); }
static AppliedBrickEdit edit(BrickKey key, std::uint32_t generation, unsigned bit) {
    AppliedBrickEdit result{key, {}, generation};
    result.changedMask.set(static_cast<std::uint16_t>(bit));
    return result;
}
static constexpr auto gpu = region_dirty_channel(RegionDirtyConsumer::Gpu);
static constexpr auto nav = region_dirty_channel(RegionDirtyConsumer::Navigation);
static constexpr auto save = region_dirty_channel(RegionDirtyConsumer::Persistence);

static void coalescing_and_channels(RegionIndexBackend backend) {
    RegionCatalog catalog({16, backend, 65536});
    RegionDirtyIndex index(catalog);
    const BrickKey brick{-1,0,2};
    require(index.submit(7,0,12,edit(brick,1,2),gpu|nav)==RegionDirtySubmitResult::Inserted);
    require(index.submit(7,0,12,edit(brick,1,2),gpu|nav)==RegionDirtySubmitResult::Ignored);
    require(index.submit(7,0,12,edit(brick,1,3),gpu|nav)==RegionDirtySubmitResult::Coalesced);
    require(index.submit(7,0,12,edit(brick,2,9),gpu|nav)==RegionDirtySubmitResult::Coalesced);
    require(index.submit(7,0,12,edit(brick,1,11),gpu|nav)==RegionDirtySubmitResult::Stale);
    auto batch=index.lease_box(7,0,{-1,0,0},{-1,0,0},3);
    require(batch && batch->size()==2 && index.inflight_count()==2);
    for (const auto& ticket:*batch) {
        require(ticket.generation==2 && ticket.changedMask.test(2) &&
                ticket.changedMask.test(3) && ticket.changedMask.test(9));
        if (ticket.key.consumer==RegionDirtyConsumer::Gpu) {
            auto value=RegionDirtyIndex::gpu_edit(ticket);
            require(value.key==brick && value.changedMask==ticket.changedMask);
        }
        require(index.complete(ticket,true)==RegionDirtyCompletion::Retired);
        require(index.complete(ticket,true)==RegionDirtyCompletion::UnknownTicket);
    }
    require(index.task_count()==0 && index.inflight_count()==0);
    require(index.submit(7,0,12,edit(brick,3,42),save)==RegionDirtySubmitResult::Inserted);
    require(index.forget_object(12)==1 && index.task_count()==0);
}

static void supersession_and_failure() {
    RegionCatalog catalog;
    RegionDirtyIndex index(catalog);
    require(index.submit(1,0,9,edit({0,0,0},2,2),gpu)==RegionDirtySubmitResult::Inserted);
    auto old=index.lease_box(1,0,{0,0,0},{0,0,0},1);
    require(old && old->size()==1);
    require(index.submit(1,0,9,edit({0,0,0},3,7),gpu)==RegionDirtySubmitResult::Coalesced);
    require(index.complete(old->front(),true)==RegionDirtyCompletion::Superseded);
    auto now=index.lease_box(1,0,{0,0,0},{0,0,0},1);
    require(now && now->size()==1 && now->front().generation==3);
    require(now->front().changedMask.test(2) && now->front().changedMask.test(7));
    require(index.complete(old->front(),true)==RegionDirtyCompletion::UnknownTicket);
    require(index.complete(now->front(),false)==RegionDirtyCompletion::Retry);
    auto retry=index.lease_box(1,0,{0,0,0},{0,0,0},1);
    require(retry && retry->size()==1 && retry->front().leaseId!=now->front().leaseId);
    require(index.forget_object(9)==1);
    require(index.complete(retry->front(),true)==RegionDirtyCompletion::UnknownTicket);
}

static void backpressure_and_spatial_isolation() {
    RegionCatalog catalog;
    RegionDirtyIndex capped(catalog,{2,1});
    require(capped.submit(1,0,1,edit({0,0,0},1,1),gpu|nav|save)==
            RegionDirtySubmitResult::CapacityExceeded);
    require(capped.task_count()==0);
    require(capped.submit(1,0,1,edit({0,0,0},1,1),gpu|nav)==RegionDirtySubmitResult::Inserted);
    require(!capped.lease_box(1,0,{0,0,0},{0,0,0},1));
    require(capped.pending_count()==2 && capped.inflight_count()==0);
    require(capped.submit(1,0,2,edit({0,0,0},1,1),gpu)==RegionDirtySubmitResult::CapacityExceeded);
    require(capped.submit(1,0,0,edit({0,0,0},1,1),gpu)==RegionDirtySubmitResult::Invalid);
    require(capped.submit(1,0,3,edit({0,0,0},1,1),16)==RegionDirtySubmitResult::Invalid);
    require(capped.submit(1,0,3,AppliedBrickEdit{{0,0,0},{},1},gpu)==RegionDirtySubmitResult::Ignored);
    RegionDirtyIndex index(catalog);
    require(index.submit_full(1,0,1,{-1,0,0},2,gpu)==RegionDirtySubmitResult::Inserted);
    require(index.submit_full(1,0,2,{0,0,0},2,gpu)==RegionDirtySubmitResult::Inserted);
    require(index.submit_full(2,0,3,{0,0,0},2,gpu)==RegionDirtySubmitResult::Inserted);
    require(index.submit_full(1,1,4,{0,0,0},2,gpu)==RegionDirtySubmitResult::Inserted);
    auto negative=index.lease_box(1,0,{-1,0,0},{-1,0,0},4);
    auto positive=index.lease_box(1,0,{0,0,0},{0,0,0},4);
    require(negative && positive && negative->size()==1 && positive->size()==1);
    require(negative->front().key.objectId==1 && positive->front().key.objectId==2);
    require(index.complete(negative->front(),true)==RegionDirtyCompletion::Retired);
    require(index.complete(positive->front(),true)==RegionDirtyCompletion::Retired);
    require(index.task_count()==2);
    auto voxel=RegionDirtyIndex::voxel_bounds({-1,0,2});
    require(voxel && voxel->minimum.x==-8 && voxel->maximum.x==-1 && voxel->maximum.z==23);
    require(!RegionDirtyIndex::voxel_bounds({std::numeric_limits<std::int32_t>::max(),0,0}));
    require(!RegionDirtyIndex::voxel_bounds({std::numeric_limits<std::int32_t>::min(),0,0}));
}

static void seeded_churn() {
    RegionCatalog catalog;
    RegionDirtyIndex index(catalog,{3000,3000});
    std::mt19937 generator(44);
    for (int i=0;i<1200;i++) {
        const auto id=static_cast<std::uint64_t>(1+(generator()%70));
        const BrickKey brick{static_cast<std::int32_t>(generator()%32)-16,0,0};
        const auto channel=(generator()%4)==0 ? nav : gpu;
        auto result=index.submit(3,0,id,edit(brick,static_cast<std::uint32_t>(generator()%20),
                                               generator()%512),channel);
        require(result!=RegionDirtySubmitResult::Invalid && result!=RegionDirtySubmitResult::CapacityExceeded);
        if(i%19==0) {
            auto tickets=index.lease_box(3,0,{-1,0,0},{0,0,0},8);
            require(tickets.has_value());
            for(const auto& ticket:*tickets) {
                const auto outcome=index.complete(ticket,i%38==0);
                require(outcome==RegionDirtyCompletion::Retired || outcome==RegionDirtyCompletion::Retry);
            }
        }
    }
    auto all=index.lease_box(3,0,{-1,0,0},{0,0,0},5000);
    require(all.has_value());
    for(const auto& ticket:*all) require(index.complete(ticket,true)==RegionDirtyCompletion::Retired);
    require(index.task_count()==0);
}

int main() {
    coalescing_and_channels(RegionIndexBackend::HashSorted);
    coalescing_and_channels(RegionIndexBackend::OrderedMap);
    supersession_and_failure();
    backpressure_and_spatial_isolation();
    seeded_churn();
}
