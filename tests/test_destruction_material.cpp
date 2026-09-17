#include "dve/destruction_material.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dve;
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void populate(VoxelObject& object){
    object.set_voxel({1,0,0},3);object.set_voxel({-1,0,0},3);
    object.set_voxel({0,1,0},3);object.set_voxel({0,-1,0},3);
    object.set_voxel({0,0,1},3);object.set_voxel({0,0,-1},3);
    object.set_voxel({0,0,0},2);
}
}
int main(){try{
    VoxelObject before(1),after(2);populate(before);populate(after);
    const BrickKey key=brick_key_from_voxel({0,0,0});
    const BrickApplyResult removal=after.set_voxel({0,0,0},kAirMaterial);
    AppliedBrickEdit edit{key,removal.changedMask,removal.generation};
    std::array<DestructionMaterialPolicy,8> policies{};
    policies[3].interiorMaterial=5;policies[3].fractureMaterial=6;policies[3].fractureLayerMaterial=7;
    const auto surfaces=extract_newly_exposed_fracture_surfaces(before,after,std::span<const AppliedBrickEdit>(&edit,1),policies);
    require(surfaces.size()==6U,"removing a surrounded voxel should expose six neighbor faces");
    for(const auto& surface:surfaces){require(surface.kind==DestructionSurfaceKind::Fracture,"surface kind mismatch");require(surface.inheritedMaterial==3&&surface.renderMaterial==6,"fracture material policy failed");require(surface.overlayLayerMaterial&&*surface.overlayLayerMaterial==7,"fracture overlay layer missing");}
    std::cout<<"destruction material tests: PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<"destruction material tests: FAIL: "<<e.what()<<'\n';return 1;}}
