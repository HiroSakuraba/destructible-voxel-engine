#include "dve/material_displacement.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace dve;void require(bool c,const char*m){if(!c)throw std::runtime_error(m);}
CookedPolygonAsset asset(){
    CookedPolygonAsset a;a.objectId=174;
    VoxelMaterialDefinition material;material.name="Displaced";a.materials.push_back(material);
    PolygonImage image;image.name="height";image.mimeType="image/raw";image.width=1;image.height=1;image.rgba8={255,255,255,255};a.images.push_back(image);
    a.samplers.push_back({});a.textures.push_back({"height",0U,0U});PolygonMaterialBinding binding;binding.height.texture=0U;a.materialBindings.push_back(binding);
    a.vertices={{{0,0,0},{0,0,1},{1,0,0,1},{0,0},{1,1,1,1},{0,0}},{{1,0,0},{0,0,1},{1,0,0,1},{1,0},{1,1,1,1},{0,0}},{{0,1,0},{0,0,1},{1,0,0,1},{0,1},{1,1,1,1},{0,0}}};
    a.indices={0,1,2};a.submeshes.push_back({"tri",0,3,0});a.bounds={{0,0,0},{1,1,0}};a.contentHash=polygon_asset_content_hash(a);require(static_cast<bool>(validate_polygon_asset(a)),"fixture invalid");return a;
}
}
int main(){try{
    auto source=asset();MaterialVertexDisplacementSettings setting;setting.policy=MaterialDisplacementPolicy::VisualOnly;setting.scaleMeters=1.0F;setting.referencePlane=0.5F;setting.fadeStartMeters=10;setting.fadeEndMeters=20;setting.offlineSubdivisionLevels=1;
    std::string error;auto result=build_displaced_polygon_asset(source,std::span<const MaterialVertexDisplacementSettings>(&setting,1),0.0F,100,&error);require(result.has_value(),error.c_str());
    require(result->visualAsset.vertices.size()==6U&&result->visualAsset.indices.size()==12U,"offline subdivision topology mismatch");
    require(result->visualAsset.bounds.maximum.z>0.49F,"normal-direction displacement failed");require(!result->collisionAsset,"visual-only displacement created collision geometry");
    setting.policy=MaterialDisplacementPolicy::CollisionAffecting;result=build_displaced_polygon_asset(source,std::span<const MaterialVertexDisplacementSettings>(&setting,1),0.0F,100,&error);require(result&&result->collisionAsset,"collision-affecting displacement omitted collision asset");
    require(!validate_material_vertex_displacement_settings(setting,GeometryKind::Voxel,&error),"voxel collision displacement should require revoxelization");
    std::cout<<"material displacement tests: PASS\n";return 0;
}catch(const std::exception&e){std::cerr<<"material displacement tests: FAIL: "<<e.what()<<'\n';return 1;}}
