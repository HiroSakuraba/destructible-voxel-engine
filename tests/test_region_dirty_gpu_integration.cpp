#include "dve/region_dirty_index.hpp"
#include "dve/gpu_brick_mirror.hpp"
#include "dve/packed_brickmap.hpp"
#include <array>
#include <stdexcept>

using namespace dve;
static void require(bool ok) { if (!ok) throw std::runtime_error("R3 real DVE GPU integration failed"); }

int main() {
    VoxelObject object(51);
    const BrickKey key{0,0,0};
    object.fill_brick(key,1);
    GpuBrickMirror mirror;
    PackedBrickmapScene packed;
    mirror.rebuild(object);
    packed.rebuild(object);
    require(mirror.validate_against(object) && packed.validate_against(object));

    RegionCatalog catalog;
    RegionDirtyIndex index(catalog);
    const auto first=object.set_voxel({1,1,1},2);
    require(first.changed);
    const AppliedBrickEdit a{key,first.changedMask,first.generation};
    require(index.submit(0,0,object.id(),a,region_dirty_channel(RegionDirtyConsumer::Gpu))
            ==RegionDirtySubmitResult::Inserted);
    auto old=index.lease_box(0,0,{0,0,0},{0,0,0},2);
    require(old && old->size()==1);

    const auto second=object.set_voxel({2,2,2},3);
    require(second.changed);
    const AppliedBrickEdit b{key,second.changedMask,second.generation};
    require(index.submit(0,0,object.id(),b,region_dirty_channel(RegionDirtyConsumer::Gpu))
            ==RegionDirtySubmitResult::Coalesced);

    // An async result from before the second edit must not replace newer data.
    auto obsolete=std::array{RegionDirtyIndex::gpu_edit(old->front())};
    const auto stale=mirror.update(object,obsolete);
    require(stale.staleDropped==1);
    require(index.complete(old->front(),true)==RegionDirtyCompletion::Superseded);

    auto current=index.lease_box(0,0,{0,0,0},{0,0,0},2);
    require(current && current->size()==1 && current->front().generation==second.generation);
    auto latest=std::array{RegionDirtyIndex::gpu_edit(current->front())};
    require(latest.front().changedMask.test(voxel_index({1,1,1})));
    require(latest.front().changedMask.test(voxel_index({2,2,2})));
    const auto mirrored=mirror.update(object,latest);
    const auto packedStats=packed.update(object,latest);
    require(mirrored.published>0 && packedStats.published>0);
    require(mirror.validate_against(object) && packed.validate_against(object));
    require(index.complete(current->front(),true)==RegionDirtyCompletion::Retired);
    require(index.task_count()==0);
}
