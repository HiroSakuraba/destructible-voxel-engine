#include "dve/render/cpu_fog.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace dve;
using namespace dve::render;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+":"+std::to_string(__LINE__));}while(false)
bool close(float a,float b,float e=0.001F){return std::abs(a-b)<e;}
GaborVolumeAsset cloud(){
    GaborVolumeAsset a;GaborVolumePrimitive p;p.scale={0.4F,0.5F,0.6F};p.opacity=2;p.extent=3;
    a.primitives.push_back(p);a.material.albedoTint={0.3F,0.6F,0.9F};a.recompute_bounds_and_hash();return a;
}
PolygonCamera camera(){PolygonCamera c;c.position={0,0,0};c.target={0,0,1};c.nearPlane=0.1F;c.farPlane=12;return c;}
void history_and_threads(){
    auto a=cloud();CpuFogVolume v{&a,{{0,0,4},{}}};
    CpuFogSettings s;s.stepMeters=0.03F;s.maximumSteps=256;s.minimumTransmittance=0;
    PolygonRenderTarget t;t.resize(17,13);RenderEnvironment e;e.fogDensityPerMeter=0;
    auto c=camera();CpuFogHistory history;s.history=&history;s.historyWeight=0.8F;s.jitter=true;
    t.clear();CpuFogStats first;CHECK(apply_cpu_fog(t,c,e,1,std::span(&v,1),s,&first));
    CHECK(first.historyRejected==17*13);
    s.frameIndex++;t.clear();CpuFogStats second;CHECK(apply_cpu_fog(t,c,e,1,std::span(&v,1),s,&second));CHECK(second.historyAccepted==17*13);
    c.position.x=0.1F;t.clear();CpuFogStats moved;CHECK(apply_cpu_fog(t,c,e,1,std::span(&v,1),s,&moved));CHECK(moved.historyAccepted==0);
    s.sceneRevision++;t.clear();CpuFogStats scene;CHECK(apply_cpu_fog(t,c,e,1,std::span(&v,1),s,&scene));CHECK(scene.historyAccepted==0);
    t.clear();t.depth[20]=0.2F;CpuFogStats changed;CHECK(apply_cpu_fog(t,c,e,1,std::span(&v,1),s,&changed));CHECK(changed.historyRejected==1);
    s.history=nullptr;s.historyWeight=0;s.jitter=false;s.threadCount=1;t.clear();CHECK(apply_cpu_fog(t,c,e,1,std::span(&v,1),s));const auto serial=t.hdrColor;
    s.threadCount=4;t.clear();CHECK(apply_cpu_fog(t,c,e,1,std::span(&v,1),s));
    for(std::size_t i=0;i<serial.size();++i){CHECK(serial[i].x==t.hdrColor[i].x);CHECK(serial[i].y==t.hdrColor[i].y);}
    s.resolutionDivisor=2;t.clear();for(std::size_t i=0;i<t.depth.size();++i)if(i%17<8)t.depth[i]=0.12F;
    CpuFogStats low;CHECK(apply_cpu_fog(t,c,e,1,std::span(&v,1),s,&low));CHECK(low.edgeRetraces>0);
    for(std::size_t i=0;i<t.depth.size();++i)if(i%17<8)CHECK(close(t.hdrColor[i].x,0.02F,1e-7F));
    s.enabled=false;s.history=&history;CHECK(apply_cpu_fog(t,c,e,1,std::span(&v,1),s));CHECK(history.samples.empty());
    CHECK(cpu_fog_quality_settings(GaborVolumeQuality::Low).resolutionDivisor==4);
}
void lighting(){
    auto a=cloud();CpuFogVolume v{&a,{{0,0,4},{}}};RenderEnvironment e;e.skyColor={};e.sunColor={1,1,1};e.sunDirection={0,0,1};e.sunIntensity=2;
    CpuFogSettings s;s.scattering=true;s.minimumTransmittance=0;s.maximumSteps=256;s.stepMeters=0.02F;
    const auto lit=trace_cpu_fog({}, {0,0,1},0,8,1,std::span(&v,1),e,s);CHECK(lit.radiance.y>0.01F);
    s.sceneVisibility=[](Float3,Float3,float){return 0.0F;};const auto shadow=trace_cpu_fog({}, {0,0,1},0,8,1,std::span(&v,1),e,s);CHECK(shadow.radiance.y==0);
    s.sceneVisibility={};s.volumeShadows=true;const auto volumeShadow=trace_cpu_fog({}, {0,0,1},0,8,1,std::span(&v,1),e,s);CHECK(volumeShadow.radiance.y<lit.radiance.y);
    s.volumeShadows=false;a.material.anisotropy=0.6F;a.recompute_bounds_and_hash();
    const auto forward=trace_cpu_fog({}, {0,0,1},0,8,1,std::span(&v,1),e,s);CHECK(forward.radiance.y>lit.radiance.y);
    a.material.anisotropy=0;a.recompute_bounds_and_hash();e.sunIntensity=0;
    CpuFogLight point{{0,0,5},{1,1,1},4,8};s.lights=std::span(&point,1);
    const auto local=trace_cpu_fog({}, {0,0,1},0,8,1,std::span(&v,1),e,s);CHECK(local.radiance.y>0);
    CpuFogLightingCache cache;s.lightingCache=&cache;s.lightCacheCellMeters=0.25F;s.lights={};e.sunIntensity=2;
    PolygonRenderTarget t;t.resize(17,13);t.clear();CpuFogStats stats;
    CHECK(apply_cpu_fog(t,camera(),e,1,std::span(&v,1),s,&stats));CHECK(stats.lightingCacheHits>0);
    // A revision invalidates cached shadow transmission.
    s.sceneRevision++;s.sceneVisibility=[](Float3,Float3,float){return 0.0F;};t.clear();
    CHECK(apply_cpu_fog(t,camera(),e,1,std::span(&v,1),s));CHECK(t.hdrColor[6*17+8].y<0.03F);
}
void density_cache(){
    auto a=cloud();a.primitives.clear();
    for(int z=0;z<4;++z)for(int x=-8;x<=8;++x){GaborVolumePrimitive p;p.center={static_cast<float>(x)*2,0,static_cast<float>(z)};
        p.scale={0.2F,0.4F,0.3F};p.rotation=quaternion_from_axis_angle({0,1,0},0.4F);p.extent=3;p.frequency=0.3F;a.primitives.push_back(p);}
    a.recompute_bounds_and_hash();CpuFogDensityCache cache;CHECK(cache.prepare(a));CHECK(cache.resident_primitives()==a.primitives.size());
    CpuFogStats fast;
    for(int z=0;z<30;++z)for(int x=-35;x<35;++x){const Float3 p{static_cast<float>(x)*0.15F,0.1F,static_cast<float>(z)*0.15F};
        CHECK(close(cache.density(a,p,&fast),evaluate_gabor_density(a,p),2e-6F));}
    CHECK(fast.primitiveEvaluations<a.primitives.size()*30*70/4);
    CpuFogDensityCache small(1);CHECK(!small.prepare(a));CHECK(close(small.density(a,{}),evaluate_gabor_density(a,{})));
    const auto empty=cache.occupied(a,{0,10,0},{0,0,1},0,20);CHECK(empty.empty());
    cache.clear();CHECK(cache.resident_primitives()==0);
}
int main(){try{history_and_threads();lighting();density_cache();std::cout<<"CPU fog quality tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
