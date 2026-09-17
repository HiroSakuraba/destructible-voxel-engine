#include "dve/material_decal.hpp"

#include <iostream>
#include <stdexcept>

namespace {using namespace dve;void require(bool c,const char*m){if(!c)throw std::runtime_error(m);} }
int main(){try{
    MaterialDecal wet;wet.id=2;wet.semantic=MaterialDecalSemantic::Wetness;wet.priority=5;
    wet.channels=MaterialDecalChannel::Roughness|MaterialDecalChannel::BaseColor;
    wet.layer.surface.baseColor={0.2F,0.2F,0.2F,1};wet.layer.surface.roughness=0.05F;
    wet.layer.surface.metallic=1.0F;wet.layer.mask=1.0F;wet.layer.heightBlendStrength=0.0F;
    require(validate_material_decal(wet),"valid wetness decal rejected");
    MaterialSurfaceSample base;base.baseColor={1,1,1,1};base.roughness=0.8F;base.metallic=0.3F;
    MaterialDecalCompositeStats stats;const auto out=composite_material_decals(base,{0,0,0},{0,0,1},std::span<const MaterialDecal>(&wet,1),&stats);
    require(stats.applied==1U&&out.roughness<0.2F,"wetness decal did not apply");
    require(out.metallic==base.metallic,"channel mask changed metallic");
    require(material_decal_projection_coverage(wet,{2,0,0},{0,0,1})==0.0F,"decal projected outside its bounds");
    MaterialDecal impact=wet;impact.id=1;impact.priority=1;
    const MaterialDecal decals[]{wet,impact};MaterialDecalGrid grid(1.0F);std::string error;
    require(grid.rebuild(decals,&error),error.c_str());const auto candidates=grid.query({0,0,0});
    require(candidates.size()==2U&&grid.decals()[candidates[0]].id==1U,"clustered decal ordering is not deterministic");
    std::cout<<"material decal tests: PASS\n";return 0;
}catch(const std::exception&e){std::cerr<<"material decal tests: FAIL: "<<e.what()<<'\n';return 1;}}
