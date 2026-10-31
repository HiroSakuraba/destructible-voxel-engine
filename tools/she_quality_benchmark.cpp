// Deterministic, offline, CPU-only image-based reflection quality comparison.
// Prints per-material/view linear-HDR error against a high-sample GGX oracle.
// No GPU timing or hardware speedup claims are implied by this tool.
#include "dve/spherical_harmonic_exponentials.hpp"
#include "dve/environment_lighting.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace dve;
namespace {
float local_dot(Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float error_sq(Float3 a,Float3 b){
    const float x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;
    return (x*x+y*y+z*z)/3.0F;
}
EnvironmentCube fixture(int kind){
    EnvironmentCube e;e.resolution=16U;
    e.texels.resize(6U*e.resolution*e.resolution);
    for(int face=0;face<6;++face)
        for(std::uint32_t y=0;y<e.resolution;++y)
            for(std::uint32_t x=0;x<e.resolution;++x){
                const auto d=cube_lookup_to_direction(static_cast<CubeFace>(face),
                    {(float(x)+0.5F)/e.resolution,(float(y)+0.5F)/e.resolution});
                Float3 rgb{1,1,1};
                if(kind==1)rgb={
                    0.1F+2.0F*std::max(0.0F,d.y),
                    0.1F+0.6F*std::max(0.0F,d.y),
                    0.2F+0.3F*std::max(0.0F,d.y)};
                if(kind==2){
                    const float spot=local_dot(d,{0.0F,1.0F,0.0F})>0.985F?1.0F:0.0F;
                    rgb={0.04F+60.0F*spot,0.07F+30.0F*spot,0.12F+5.0F*spot};
                }
                e.texels[(static_cast<std::size_t>(face)*e.resolution+y)*e.resolution+x]=rgb;
            }
    return e;
}
struct Metrics{double sheSq{},splitSq{};std::uint64_t count{};};
}
int main(int argc,char** argv){
    try{
        std::uint32_t referenceSamples=4096U;
        if(argc==2){
            const unsigned long n=std::strtoul(argv[1],nullptr,10);
            if(n<256U || n>1000000U)throw std::runtime_error("reference samples: 256..1000000");
            referenceSamples=static_cast<std::uint32_t>(n);
        }else if(argc!=1)throw std::runtime_error("usage: dve_she_quality_benchmark [reference_samples]");
        std::cout<<"scene,roughness,ndotv,she_rmse_rgb,split_sum_rmse_rgb,reference_rgb_r,reference_rgb_g,reference_rgb_b\n";
        const std::array<std::string_view,3> names{"uniform","horizon","sunspot"};
        const std::array<float,4> roughnesses{0.5F,0.65F,0.8F,0.95F};
        const std::array<float,3> cosines{1.0F,0.65F,0.25F};
        std::array<Metrics,3> metrics{};
        for(int scene=0;scene<3;++scene){
            const auto source=fixture(scene);
            if(!source.validate())throw std::runtime_error("invalid fixture");
            SheBakeSettings fitSettings;fitSettings.trainingSamples=384U;
            fitSettings.ggxSamplesPerTrainingPoint=48U;
            const auto fit=bake_she_probe(source,fitSettings);
            if(!fit)throw std::runtime_error("SHE fit failed");
            const IblBakeSettings iblSettings{8U,32U,5U,64U,64U};
            const auto ibl=bake_image_based_lighting(source,iblSettings);
            for(float roughness:roughnesses)for(float cosine:cosines){
                // Two outward normals also probe direction dependence.
                for(int orientation=0;orientation<2;++orientation){
                    const Float3 n=orientation==0?Float3{0,1,0}:Float3{0,0,1};
                    const Float3 t{1,0,0};
                    const float sine=std::sqrt(std::max(0.0F,1.0F-cosine*cosine));
                    const Float3 v{n.x*cosine+t.x*sine,n.y*cosine+t.y*sine,n.z*cosine+t.z*sine};
                    const auto truth=reference_ggx_environment_integral(
                        source,n,v,roughness*roughness,referenceSamples);
                    if(!truth)throw std::runtime_error("reference integral failed");
                    IblMaterialInput m;m.normal=n;m.viewDirection=v;
                    m.baseColor={1,1,1};m.metallic=1.0F;
                    m.ambientOcclusion=1.0F;m.roughness=roughness;
                    const auto split=evaluate_image_based_lighting(ibl,m);
                    m.sheProbe=&fit->coefficients;
                    const auto she=evaluate_image_based_lighting(ibl,m);
                    const float se=error_sq(she,*truth),be=error_sq(split,*truth);
                    if(!std::isfinite(se)||!std::isfinite(be))throw std::runtime_error("non-finite IBL error");
                    metrics[scene].sheSq+=se;metrics[scene].splitSq+=be;++metrics[scene].count;
                    std::cout<<names[scene]<<','<<roughness<<','<<cosine<<','
                             <<std::sqrt(se)<<','<<std::sqrt(be)<<','
                             <<truth->x<<','<<truth->y<<','<<truth->z<<'\n';
                }
            }
        }
        for(int i=0;i<3;++i){
            const auto& m=metrics[i];
            std::cerr<<"SHE quality "<<names[i]<<": linear HDR RMS, SHE="
                     <<std::sqrt(m.sheSq/m.count)<<", split-sum="
                     <<std::sqrt(m.splitSq/m.count)
                     <<" (samples="<<referenceSamples<<", no exposure/tonemapping)\n";
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<"SHE benchmark: "<<e.what()<<'\n';return 1;}
}
