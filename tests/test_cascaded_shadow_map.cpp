#include "dve/render/cascaded_shadow_map.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x + " at " + std::to_string(__LINE__)); } while(false)
bool close(float a,float b,float e=1.0e-4F){return std::abs(a-b)<=e;}
}

int main(){
    try{
        dve::camera::CameraPose camera; camera.position={0,3,8};camera.target={0,1,0};camera.lens.nearPlaneMeters=0.1F;camera.lens.farPlaneMeters=1000.0F;camera.lens.aspectRatio=16.0F/9.0F;
        dve::render::CascadedShadowSettings settings;settings.cascadeCount=4;settings.cascadeResolution=1024;settings.maximumDistanceMeters=200;settings.splitLambda=0.7F;
        const auto plan=dve::render::make_cascaded_shadow_plan(camera,{0.4F,0.8F,0.2F},settings);
        std::string error;CHECK(plan.validate(&error));CHECK(plan.cascades.size()==4U);CHECK(plan.atlasWidth==2048U&&plan.atlasHeight==2048U);
        for(std::size_t i=1;i<plan.cascades.size();++i)CHECK(plan.cascades[i].splitNearMeters>=plan.cascades[i-1].splitFarMeters-1.0e-4F);
        const auto nearSelection=dve::render::select_shadow_cascade(plan,1.0F);CHECK(nearSelection.primaryCascade==0U&&!nearSelection.beyondShadowDistance);
        const auto farSelection=dve::render::select_shadow_cascade(plan,500.0F);CHECK(farSelection.beyondShadowDistance);
        const auto& c=plan.cascades.front();const float sx=std::abs(c.snappedCenter.x-c.center.x)+std::abs(c.snappedCenter.y-c.center.y)+std::abs(c.snappedCenter.z-c.center.z);CHECK(std::isfinite(sx));CHECK(c.texelSizeMeters>0.0F);
        auto moved=camera;moved.position.x+=c.texelSizeMeters*0.2F;moved.target.x+=c.texelSizeMeters*0.2F;const auto stable=dve::render::make_cascaded_shadow_plan(moved,{0.4F,0.8F,0.2F},settings);CHECK(close(stable.cascades[0].snappedCenter.x,c.snappedCenter.x,c.texelSizeMeters*1.5F));
        const auto touched=dve::render::shadow_cascades_intersecting_sphere(plan,c.center,1.0F);CHECK(!touched.empty()&&touched.front()==0U);
        std::cout<<"dve_cascaded_shadow_map_tests: PASS\n";return 0;
    }catch(const std::exception& e){std::cerr<<"dve_cascaded_shadow_map_tests: FAIL: "<<e.what()<<'\n';return 1;}
}
