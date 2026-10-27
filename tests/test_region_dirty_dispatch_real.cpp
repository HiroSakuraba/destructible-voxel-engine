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
}
