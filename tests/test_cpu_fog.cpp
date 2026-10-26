#include "dve/render/cpu_fog.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace dve;
using namespace dve::render;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(#x); } while(false)
bool close(float a,float b,float e=2e-4F){return std::abs(a-b)<e;}

GaborVolumeAsset cloud(){
    GaborVolumeAsset a;
    GaborVolumePrimitive p;p.center={0,0,0};p.scale={1,1,1};p.opacity=1;p.extent=3;
    a.primitives.push_back(p);a.material.albedoTint={0.3F,0.6F,0.9F};a.recompute_bounds_and_hash();return a;
}
void tests(){
    RenderEnvironment e;e.fogDensityPerMeter=0.3F;e.fogStartMeters=2;e.fogColor={0.5F,0.7F,0.9F};
    CpuFogSettings s;s.minimumTransmittance=0;s.stepMeters=0.02F;s.maximumSteps=1024;
    const auto homogeneous=trace_cpu_fog({}, {0,0,1},0,10,1,{},e,s);
    CHECK(close(homogeneous.transmittance,std::exp(-2.4F)));
    CHECK(close(homogeneous.radiance.x,e.fogColor.x*(1-homogeneous.transmittance)));
    auto a=cloud();e.fogDensityPerMeter=0;
    CpuFogVolume v{&a};v.transform.position={0,0,6};
    const auto occluded=trace_cpu_fog({}, {0,0,1},0,2,1,std::span(&v,1),e,s);
    CHECK(occluded.transmittance==1);
    const auto single=trace_cpu_fog({}, {0,0,1},0,12,1,std::span(&v,1),e,s);
    CHECK(single.transmittance<0.2F);
    CHECK(close(single.radiance.y,0.6F*(1-single.transmittance)));
    CpuFogVolume pair[]{v,v};
    const auto twice=trace_cpu_fog({}, {0,0,1},0,12,1,pair,e,s);
    CHECK(close(twice.transmittance,single.transmittance*single.transmittance,0.001F));
    auto b=a;b.material.albedoTint={0.9F,0.1F,0.1F};pair[1].asset=&b;
    const auto ab=trace_cpu_fog({}, {0,0,1},0,12,1,pair,e,s);
    std::swap(pair[0],pair[1]);
    const auto ba=trace_cpu_fog({}, {0,0,1},0,12,1,pair,e,s);
    CHECK(close(ab.radiance.x,ba.radiance.x));
    // Same physical scene expressed in ten units/metre.
    auto units=v;units.transform.position={0,0,60};units.scale={10,10,10};
    const auto scaled=trace_cpu_fog({}, {0,0,1},0,120,0.1F,std::span(&units,1),e,s);
    CHECK(close(single.transmittance,scaled.transmittance));
    PolygonRenderTarget target;target.resize(5,5);target.enable_passes();target.clear({0.2F,0.3F,0.4F,1});
    PolygonCamera camera;camera.position={0,0,0};camera.target={0,0,1};camera.nearPlane=0.1F;camera.farPlane=20;
    const auto depth=target.depth;const auto ids=target.objectId;
    CpuFogStats stats;std::string error;
    CHECK(apply_cpu_fog(target,camera,e,1,std::span(&v,1),s,&stats,&error));
    CHECK(stats.steps>0&&stats.densityEvaluations>0);
    CHECK(target.depth==depth&&target.objectId==ids);
    for(std::size_t i=0;i<target.hdrColor.size();++i)CHECK(close(target.passes.light_sum(i).x,target.hdrColor[i].x));
    target.clear({0.2F,0.3F,0.4F,1});for(auto& d:target.depth)d=(2-camera.nearPlane)/(camera.farPlane-camera.nearPlane);
    CHECK(apply_cpu_fog(target,camera,e,1,std::span(&v,1),s));
    CHECK(target.hdrColor[12].x==0.2F);
    // Invalid input refuses before touching the target.
    v.scale.x=0;CHECK(!apply_cpu_fog(target,camera,e,1,std::span(&v,1),s));CHECK(target.hdrColor[12].x==0.2F);
}
int main(){try{tests();std::cout<<"CPU fog tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
