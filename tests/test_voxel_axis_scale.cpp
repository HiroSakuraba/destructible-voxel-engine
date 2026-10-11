#include "dve/editor_document.hpp"
#include "dve/dvox.hpp"
#include "dve/query.hpp"
#include "dve/voxel_metric.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace dve;
using namespace dve::editor;
namespace {
void check(bool v,const char* message){if(!v) throw std::runtime_error(message);}
void run(){
    const Float3 spacing{2,0.5F,3};
    const auto pose=make_rigid_transform({4,5,6},quaternion_from_axis_angle({0,1,0},0.4F));
    const auto p=voxel_to_world(pose,spacing,{0.3F,-2,5});
    check(length(subtract(world_to_voxel(pose,spacing,p),{0.3F,-2,5}))<0.00001F,"coordinate round trip");
    VoxelObject voxels(1); (void)voxels.set_voxel({0,0,0},1);
    const auto origin=voxel_to_world(pose,spacing,{-2,0.5F,0.5F});
    const auto hit=raycast_voxels_scaled(voxels,pose,spacing,origin,rotate(pose.rotation,{1,0,0}),20);
    check(hit && std::abs(hit->objectHit.distance-4)<0.0001F,"scaled ray world distance");
    check(length(subtract(hit->worldNormal,rotate(pose.rotation,{-1,0,0})))<0.0001F,"scaled normal");
    check(!raycast_voxels_scaled(voxels,pose,{0,1,1},origin,{1,0,0},20),"invalid spacing refused");
    const auto directory=std::filesystem::temp_directory_path()/"dve-axis-scale-data-test";
    std::filesystem::create_directories(directory);
    EditorDocument doc("axis scale"); EditorObject object(1,"scaled");
    object.voxelScale=spacing; object.anchors={{0,0,0}}; (void)object.voxels->set_voxel({0,0,0},1);
    doc.add_object(std::move(object));
    check(clone_editor_object(*doc.find_object(1)).voxelScale.x==2,"clone preserves scale");
    const auto saved=doc.save_transactional(directory/"scene.dvescene"); check(saved.success,"scene save");
    auto loaded=EditorDocument::load(saved.manifestPath); check(loaded && loaded->find_object(1)->voxelScale.z==3,"scene scale round trip");
    CookedVoxelAsset asset(2); asset.voxelScale=spacing; (void)asset.object.set_voxel({0,0,0},1);
    check(write_dvox(directory/"scaled.dvox",asset),"scaled DVOX write");
    auto cooked=read_dvox(directory/"scaled.dvox"); check(cooked.success && cooked.asset.voxelScale.y==0.5F,"DVOX scale round trip");
    std::fstream stream(directory/"scaled.dvox",std::ios::binary|std::ios::in|std::ios::out);
    stream.seekp(64); const float changed=4; stream.write(reinterpret_cast<const char*>(&changed),4); stream.close();
    check(!read_dvox(directory/"scaled.dvox").success,"scale is covered by integrity hash");
    asset.voxelScale={1,1,1}; check(write_dvox(directory/"legacy.dvox",asset),"unit DVOX write");
    check(read_dvox(directory/"legacy.dvox").success,"unit DVOX compatible");
    asset.voxelScale={std::numeric_limits<float>::infinity(),1,1};
    check(!write_dvox(directory/"invalid.dvox",asset),"invalid scale refused");
    std::filesystem::remove_all(directory);
}
}
int main(){try{run();std::cout<<"Axis scale data passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
