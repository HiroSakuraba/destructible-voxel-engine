#include "dve/material_layers.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dve;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool near(float a,float b,float e=1.0e-4F){return std::abs(a-b)<=e;}

void test_channel_specific_blending(){
    MaterialSurfaceSample base;
    base.baseColor={0.2F,0.4F,0.6F,0.8F};base.normal={0,0,1};base.metallic=0.9F;
    base.roughness=0.8F;base.emissive={0.1F,0.0F,0.0F};base.opacity=0.8F;base.height=0.2F;
    MaterialLayerSample layer;
    layer.semantic=MaterialLayerSemantic::Wetness;layer.mask=0.5F;layer.authoredWeight=1.0F;
    layer.heightBlendStrength=0.0F;layer.surface.baseColor={0.1F,0.2F,0.3F,0.4F};
    layer.surface.normal={0.6F,0.0F,0.8F};layer.surface.metallic=1.5F;
    layer.surface.roughness=0.1F;layer.surface.emissive={0.2F,0.3F,0.4F};
    layer.surface.opacity=0.25F;layer.opacityPolicy=MaterialLayerOpacityPolicy::Multiply;
    float coverage{};const auto out=blend_material_surface_layer(base,layer,&coverage);
    require(near(coverage,0.5F),"uniform layer coverage drifted");
    require(near(out.baseColor.x,0.15F)&&near(out.roughness,0.45F),"color/roughness blend failed");
    require(out.metallic>=0.0F&&out.metallic<=1.0F,"metallic escaped physical bounds");
    require(near(out.emissive.y,0.15F)&&near(out.opacity,0.5F),"emissive/opacity policy failed");
    require(out.normal.x>0.1F&&out.normal.z>0.7F,"RNM normal was not applied");
}

void test_height_aware_mask(){
    MaterialLayerSample layer;layer.mask=0.75F;layer.authoredWeight=0.8F;
    layer.surface.height=0.9F;layer.heightBlendStrength=1.0F;layer.heightBlendTransition=0.2F;
    const float raised=height_aware_material_layer_coverage(0.1F,layer);
    layer.surface.height=0.0F;const float recessed=height_aware_material_layer_coverage(0.9F,layer);
    require(raised>0.55F&&recessed<0.05F,"height-aware coverage ordering failed");
    layer.mask=0.0F;require(near(height_aware_material_layer_coverage(0.0F,layer),0.0F),"height invented coverage outside the mask");
}

void test_world_frame_rnm(){
    const Float3 result=apply_reoriented_tangent_material_normal({0,1,0},{1,0,0},{0,0,-1},{0.5F,0.0F,0.8660254F},1.0F);
    require(result.x>0.3F&&result.y>0.7F,"world-frame RNM did not preserve the base frame");
}
}
int main(){try{test_channel_specific_blending();test_height_aware_mask();test_world_frame_rnm();std::cout<<"material layer tests: PASS\n";return 0;}catch(const std::exception& e){std::cerr<<"material layer tests: FAIL: "<<e.what()<<'\n';return 1;}}
