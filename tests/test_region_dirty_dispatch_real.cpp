#include "dve/region_dirty_dispatch.hpp"
#include "dve/damage.hpp"
#include "dve/gpu_brick_mirror.hpp"
#include "dve/packed_brickmap.hpp"
#include "dve/v235_foundations.hpp"
#include <stdexcept>
#include <vector>
using namespace dve;
static void require(bool ok) { if (!ok) throw std::runtime_error("R3.1 real DVE regression failed"); }
int main() {
    VoxelObject obj(55);
    obj.fill_brick({0,0,0},1);
    GpuBrickMirror mirror;
    PackedBrickmapScene packed;
    mirror.rebuild(obj);
    packed.rebuild(obj);
    RegionCatalog catalog;
    RegionDirtyIndex queue(catalog);
    RegionDirtyDispatcher dispatch(queue,{.maximumBufferedEdits=200,.maximumTicketsPerTick=1,
        .maximumDispatcherMicroseconds=500000,.interestRadius=2});
    // Production voxel damage operation: no synthetic edit generations.
    std::vector<SphereDamageCommand> damage{{{3.0F,3.0F,3.0F},2.0F,1}};
    auto report=apply_damage_commands(obj,std::move(damage));
    require(report.removedVoxelCount>0 && !report.edits.empty());
    const RegionDirtyChannels channels=static_cast<RegionDirtyChannels>(
        region_dirty_channel(RegionDirtyConsumer::Gpu) |
        region_dirty_channel(RegionDirtyConsumer::Navigation) |
        region_dirty_channel(RegionDirtyConsumer::Collision) |
        region_dirty_channel(RegionDirtyConsumer::Persistence));
    require(dispatch.offer(0,0,obj.id(),report.edits,channels)==RegionDirtyOfferResult::Accepted);
    DynamicNavigationWorld nav;
    const auto lookup=[&](std::uint64_t id){
        return id==obj.id()?RegionDirtyTargets{&obj,&mirror,&packed,&nav,0.1F}:RegionDirtyTargets{};
    };
    WorkBudget budget;
    budget.patchBrickLimit=1;
    const RegionKey camera{0,0,0,0,0};
    auto s=dispatch.tick(camera,budget,lookup);
    require(s.gpuTickets==1 && mirror.validate_against(obj) && packed.validate_against(obj));
    require(queue.task_count()==3); // nav + collision + persistence
    s=dispatch.tick(camera,budget,lookup);
    require(s.navigationInvalidations==1 && nav.telemetry().editsObserved>0);
    require(queue.task_count()==2); // collision and persistence remain outstanding
    dispatch.forget_object(obj.id());
    require(queue.task_count()==0);

    // Replacement semantics: the authority loses a brick that the mirror still holds.
    // Incremental updates never drop slots, so the host flags the replacement.
    VoxelObject twoBricks(56);
    twoBricks.fill_brick({0,0,0},1);
    twoBricks.fill_brick({1,0,0},1);
    GpuBrickMirror m2;
    PackedBrickmapScene p2;
    m2.rebuild(twoBricks);
    p2.rebuild(twoBricks);
    VoxelObject oneBrick(56);
    oneBrick.fill_brick({0,0,0},1);
    require(!m2.validate_against(oneBrick) && !p2.validate_against(oneBrick));
    const AppliedBrickEdit survivor{{0,0,0},Bitset512::full(),oneBrick.find_brick({0,0,0})->generation()};
    require(dispatch.offer(0,0,56,std::span<const AppliedBrickEdit>(&survivor,1),
        region_dirty_channel(RegionDirtyConsumer::Gpu))==RegionDirtyOfferResult::Accepted);
    dispatch.mark_geometry_replaced(56);
    const auto lookup2=[&](std::uint64_t id){
        return id==56?RegionDirtyTargets{&oneBrick,&m2,&p2,nullptr,0.1F}:RegionDirtyTargets{};
    };
    s=dispatch.tick(camera,budget,lookup2);
    require(s.gpuRebuilds==1 && s.gpuTickets==1 && queue.task_count()==0);
    require(m2.validate_against(oneBrick) && p2.validate_against(oneBrick));
}
