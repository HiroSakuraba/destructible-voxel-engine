// Deterministic CPU fog comparison. Optional argument writes the accelerated
// image as PPM; no timing assertion depends on the machine running it.
#include "dve/render/cpu_fog.hpp"
#include <cmath>
#include <fstream>
#include <iostream>

int main(int argc,char** argv){
    using namespace dve;using namespace dve::render;
    GaborVolumeAsset asset;
    for(int z=0;z<3;++z)for(int y=-7;y<=7;++y)for(int x=-7;x<=7;++x){
        GaborVolumePrimitive p;p.center={static_cast<float>(x)*0.2F,static_cast<float>(y)*0.2F,static_cast<float>(z)*0.6F};
        p.scale={0.065F,0.065F,0.1F};p.opacity=4;p.extent=3;asset.primitives.push_back(p);
    }
    asset.material.albedoTint={0.3F,0.6F,0.9F};asset.recompute_bounds_and_hash();
    CpuFogVolume volume{&asset,{{0,0,4},{}}};
    PolygonCamera camera;camera.position={0,0,0};camera.target={0,0,4};camera.farPlane=12;
    RenderEnvironment environment;environment.fogDensityPerMeter=0;
    CpuFogSettings settings;settings.maximumSteps=128;settings.stepMeters=0.025F;settings.minimumTransmittance=0;settings.adaptiveSampling=false;
    PolygonRenderTarget target;target.resize(96,64);target.clear();
    CpuFogStats baseline;std::string error;
    if(!apply_cpu_fog(target,camera,environment,1,std::span(&volume,1),settings,&baseline,&error)){std::cerr<<error;return 1;}
    const auto original=target.hdrColor;
    CpuFogDensityCache cache;settings.densityCache=&cache;settings.threadCount=4;
    target.clear();CpuFogStats accelerated;
    if(!apply_cpu_fog(target,camera,environment,1,std::span(&volume,1),settings,&accelerated,&error)){std::cerr<<error;return 1;}
    double sum=0;float worst=0;
    for(std::size_t i=0;i<original.size();++i){const auto a=original[i],b=target.hdrColor[i];
        for(float d:{a.x-b.x,a.y-b.y,a.z-b.z}){sum+=d*d;worst=std::max(worst,std::abs(d));}}
    std::cout<<"mode,milliseconds,rays,steps,primitive_evaluations\n"
        <<"linear_single_thread,"<<baseline.milliseconds<<','<<baseline.rays<<','<<baseline.steps<<','<<baseline.primitiveEvaluations<<'\n'
        <<"bvh_four_threads,"<<accelerated.milliseconds<<','<<accelerated.rays<<','<<accelerated.steps<<','<<accelerated.primitiveEvaluations<<'\n'
        <<"rgb_rmse,"<<std::sqrt(sum/static_cast<double>(original.size()*3))<<"\nmax_rgb_error,"<<worst<<'\n';
    if(argc>1){std::ofstream out(argv[1],std::ios::binary);out<<"P6\n96 64\n255\n";
        for(const auto& c:target.hdrColor)for(float v:{c.x,c.y,c.z})out.put(static_cast<char>(std::lround(std::pow(std::clamp(v,0.0F,1.0F),1/2.2F)*255)));}
    return 0;
}
