#include "dve/environment_lighting.hpp"
#include "dve/irradiance_probe_grid.hpp"
#include "dve/render/local_light_probes_gpu.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x + " at " + std::to_string(__LINE__)); } while(false)
bool close(float a,float b,float e=2.0e-2F){return std::abs(a-b)<=e;}
float luminance(dve::Float3 v){return 0.2126F*v.x+0.7152F*v.y+0.0722F*v.z;}

void test_cube_coordinates(){
    using namespace dve;
    for(std::uint32_t f=0;f<6;++f){
        const auto face=static_cast<CubeFace>(f);
        const Float3 d=cube_lookup_to_direction(face,{0.5F,0.5F});
        const CubeLookup lookup=direction_to_cube_lookup(d);
        CHECK(lookup.face==face);
        CHECK(close(lookup.uv.x,0.5F,1.0e-4F));
        CHECK(close(lookup.uv.y,0.5F,1.0e-4F));
    }
}

void test_constant_environment_bake(){
    using namespace dve;
    EnvironmentCube source{8U,std::vector<Float3>(6ULL*8U*8U,{0.4F,0.2F,0.1F})};
    IblBakeSettings settings; settings.irradianceResolution=4;settings.specularResolution=8;settings.specularMipLevels=4;settings.sampleCount=32;settings.brdfResolution=8;
    const auto baked=bake_image_based_lighting(source,settings);
    std::string error;CHECK(baked.validate(&error));
    const Float3 irradiance=sample_environment_cube(baked.diffuseIrradiance,{0,1,0});
    CHECK(close(irradiance.x,0.4F*3.14159265F,0.08F));
    const Float3 specular=sample_environment_cube_lod(baked.specularPrefilter,{1,0,0},3.0F);
    CHECK(close(specular.x,0.4F,0.03F));
    IblMaterialInput material;material.normal={0,1,0};material.viewDirection={0,1,0};material.baseColor={0.8F,0.7F,0.6F};material.roughness=0.5F;
    const Float3 lit=evaluate_image_based_lighting(baked,material);
    CHECK(std::isfinite(lit.x)&&std::isfinite(lit.y)&&std::isfinite(lit.z));
    CHECK(luminance(lit)>0.05F);
}

void test_equirectangular_and_probes(){
    using namespace dve;
    EnvironmentImage2D image{8U,4U,std::vector<Float3>(32U)};
    for(std::uint32_t y=0;y<4;++y)for(std::uint32_t x=0;x<8;++x)image.pixels[y*8+x]={static_cast<float>(x)/7.0F,static_cast<float>(y)/3.0F,0.25F};
    const auto cube=convert_equirectangular_to_cube(image,4U);CHECK(cube.validate());
    CHECK(sample_environment_cube(cube,{1,0,0}).x>=0.0F);
    std::vector<ReflectionProbe> probes{{1,{0,0,0},{2,2,2},1,1,0,true},{2,{1,0,0},{2,2,2},1,2,1,true},{3,{20,0,0},{1,1,1},1,1,10,true}};
    const auto weights=select_reflection_probes(probes,{0,0,0},2U);CHECK(weights.size()==2U);CHECK(weights.front().id==2U);CHECK(close(weights[0].weight+weights[1].weight,1.0F,1.0e-5F));
    const Float3 projected=box_project_reflection_direction(probes.front(),{0,0,0},{1,0.2F,0});CHECK(projected.x>0.8F);
}

void test_irradiance_probe_sh9_bake(){
    using namespace dve;
    // For a uniform full-sphere radiance C, integrated Lambertian
    // irradiance must approach pi*C, independent of shading normal.
    constexpr float pi=3.14159265358979323846F;
    EnvironmentCube white{16U,std::vector<Float3>(6ULL*16U*16U,{1.0F,.5F,.25F})};
    const IrradianceProbe baked=bake_irradiance_probe_sh9(white);
    IrradianceProbeGrid grid;
    grid.probes={baked};
    CHECK(grid.validate());
    for(Float3 direction:std::array<Float3,5>{{{0,1,0},{1,0,0},{0,0,1},
                                               {-.3F,.2F,-.9F},{0,-1,0}}}){
        const auto value=sample_irradiance_probe_grid(grid,{0,0,0},direction);
        CHECK(close(value.coverage,1.0F,1e-5F));
        CHECK(close(value.irradiance.x,pi,.045F));
        CHECK(close(value.irradiance.y,pi*.5F,.025F));
        CHECK(close(value.irradiance.z,pi*.25F,.015F));
    }
    // Environment emits only above the horizon: facing up must receive
    // far more irradiance than facing down.
    EnvironmentCube horizon=white;
    for(std::uint32_t face=0;face<6U;++face){
        for(std::uint32_t y=0;y<16U;++y)for(std::uint32_t x=0;x<16U;++x){
            const auto direction=cube_lookup_to_direction(static_cast<CubeFace>(face),
                {(static_cast<float>(x)+.5F)/16.0F,(static_cast<float>(y)+.5F)/16.0F});
            horizon.texels[horizon.face_offset(static_cast<CubeFace>(face))+
                           static_cast<std::size_t>(y)*16U+x]=
                direction.y>0?Float3{1,0,0}:Float3{};
        }
    }
    grid.probes={bake_irradiance_probe_sh9(horizon)};
    const auto up=sample_irradiance_probe_grid(grid,{0,0,0},{0,1,0});
    const auto down=sample_irradiance_probe_grid(grid,{0,0,0},{0,-1,0});
    CHECK(up.irradiance.x>1.0F);
    CHECK(down.irradiance.x<up.irradiance.x*.15F);
    // The bake does not silently accept incomplete cubemap data.
    bool rejected=false;
    try{(void)bake_irradiance_probe_sh9(EnvironmentCube{});}
    catch(const std::invalid_argument&){rejected=true;}
    CHECK(rejected);
}

void test_probe_grid_capture_driver(){
    using namespace dve;
    IrradianceProbeGrid grid;
    grid.dimensions={2,1,1};
    grid.probes.resize(2);
    grid.probes[1].confidence=.25F;
    int calls=0;
    const auto source=[&](Float3 position){
        ++calls;
        const float strength=position.x<.5F?1.0F:2.0F;
        return EnvironmentCube{8U,
            std::vector<Float3>(6ULL*8U*8U,{strength,0,0})};
    };
    CHECK(bake_irradiance_probe_grid_sh9(grid,source));
    CHECK(calls==2);
    CHECK(close(grid.probes[0].confidence,1.0F));
    CHECK(close(grid.probes[1].confidence,.25F));
    const auto left=sample_irradiance_probe_grid(grid,{0,0,0},{0,1,0});
    const auto right=sample_irradiance_probe_grid(grid,{1,0,0},{0,1,0});
    CHECK(close(left.irradiance.x,3.14159265F,.08F));
    CHECK(close(right.irradiance.x,2.0F*.25F*3.14159265F,.08F));
    const auto old=grid.probes[0].sh[0].x;
    const auto failCapture=[&](Float3 p){
        if(p.x>.5F)return EnvironmentCube{};
        return EnvironmentCube{8U,std::vector<Float3>(6ULL*8U*8U,{99,0,0})};
    };
    CHECK(!bake_irradiance_probe_grid_sh9(grid,failCapture));
    CHECK(close(grid.probes[0].sh[0].x,old,1.0e-5F));
}

void test_visibility_gated_irradiance(){
    using namespace dve;
    IrradianceProbeGrid grid;
    grid.dimensions={2,1,1};
    grid.probes.resize(2);
    const float base=1.0F/0.28209479177387814F;
    grid.probes[0].sh[0]={base,0,0}; // left room red
    grid.probes[1].sh[0]={0,0,base}; // right room blue
    CHECK(grid.validate());
    const auto at=.4F;
    const auto unrestricted=sample_irradiance_probe_grid(grid,{at,0,0},{0,1,0});
    CHECK(unrestricted.irradiance.z>.3F); // blue leaks without visibility
    const auto blocked=sample_irradiance_probe_grid_visible(
        grid,{at,0,0},{0,1,0},
        [](Float3 from,Float3 receiver){
            // A barrier at x=.5 only blocks the far probe, not the left one.
            return (from.x<.5F)==(receiver.x<.5F);
        });
    CHECK(close(blocked.irradiance.x,.6F,1e-4F));
    CHECK(close(blocked.irradiance.z,0.0F,1e-4F));
    CHECK(close(blocked.coverage,.6F,1e-4F));
}

void test_irradiance_probe_grid(){
    using namespace dve;
    IrradianceProbeGrid grid;
    grid.origin={0,0,0};
    grid.cellSize={1,1,1};
    grid.dimensions={2,1,1};
    grid.probes.resize(2);
    const float y00=0.28209479177387814F;
    grid.probes[0].sh[0]={1.0F/y00,0.5F/y00,0};
    grid.probes[1].sh[0]={3.0F/y00,2.0F/y00,0};
    grid.probes[1].confidence=.5F;
    CHECK(grid.validate());
    const auto middle=sample_irradiance_probe_grid(grid,{.5F,0,0},{0,1,0});
    CHECK(close(middle.coverage,.75F,1e-5F));
    CHECK(close(middle.irradiance.x,1.25F,.005F));
    CHECK(close(middle.irradiance.y,.75F,.005F));
    // 25% of the total irradiance comes from the global sky fallback;
    // grid sampling itself does not add the fallback or double-count it.
    CHECK(close(middle.coverage+(1.0F-middle.coverage),1.0F));
    const auto off=sample_irradiance_probe_grid(grid,{2,0,0},{0,1,0});
    CHECK(close(off.coverage,0.0F));
    const auto invalidNormal=sample_irradiance_probe_grid(grid,{0,0,0},{0,0,0});
    CHECK(close(invalidNormal.coverage,0.0F));
    grid.probes[0].confidence=0;
    const auto half=sample_irradiance_probe_grid(grid,{0,0,0},{0,1,0});
    CHECK(close(half.coverage,0.0F));
    grid.probes[0].confidence=2;
    CHECK(!grid.validate());
    CHECK(close(sample_irradiance_probe_grid(grid,{0,0,0},{0,1,0}).coverage,0.0F));
    grid.probes[0].confidence=1;
    grid.dimensions={262,1,1};
    CHECK(!grid.validate());
}

void test_local_probe_gpu_abi(){
    using namespace dve;
    using namespace dve::render;
    const auto empty=pack_local_probe_gpu(nullptr);
    CHECK(empty.words.size()==kLocalProbeHeaderWords+2*kLocalReflectionStrideWords);
    CHECK(!empty.irradiance_enabled());
    CHECK(empty.words[0][3]==0.0F && empty.words[3][0]==0.0F);
    IrradianceProbeGrid grid;
    grid.dimensions={2,1,1};
    grid.cellSize={1,1,1};
    grid.probes.resize(2);
    grid.probes[0].sh[0]={1,2,3};
    grid.probes[1].sh[0]={4,5,6};
    grid.probes[1].confidence=.25F;
    CHECK(grid.validate());
    const ReflectionProbe probe{1,{0,0,0},{2,2,2},.25F,1.0F,2,true};
    std::array<ReflectionProbe,1> volumes{probe};
    std::array<float,1> mips{3.0F};
    const auto packed=pack_local_probe_gpu(&grid,volumes,mips);
    CHECK(packed.irradiance_enabled());
    CHECK(packed.words.size()==kLocalProbeHeaderWords+
          2*kLocalProbeStrideWords+2*kLocalReflectionStrideWords);
    CHECK(packed.words[0][0]==2.0F && packed.words[3][0]==1.0F);
    CHECK(packed.words[4][0]==1.0F && packed.words[14][0]==4.0F);
    CHECK(close(packed.words[23][0],.25F));
    CHECK(close(packed.words[24][3],2.0F));
    CHECK(close(packed.words[26][1],3.0F));
    const auto sky=pack_local_probe_gpu(nullptr,volumes,mips);
    CHECK(!sky.irradiance_enabled());
    CHECK(sky.words[3][0]==1.0F); // local specular independent of SH9
}

void test_bounded_local_probe_mixing(){
    using namespace dve;
    std::vector<ReflectionProbe> zones{
        {1,{0,0,0},{2,2,2},1,1,0,true},
        {2,{0.5F,0,0},{2,2,2},1,1,0,true},
        {3,{0,0,0},{1,1,1},0.5F,1,2,true}
    };
    const auto in=select_reflection_probe_mix(zones,{0,0,0});
    CHECK(in.probes.size()==1U && in.probes[0].id==3);
    CHECK(close(in.probes[0].weight,1.0F,1.0e-5F));
    CHECK(close(in.environmentWeight,0.0F,1.0e-5F));
    // Removing the high-priority capture blends two equally ranked zones.
    zones.pop_back();
    const auto shared=select_reflection_probe_mix(zones,{0,0,0});
    CHECK(shared.probes.size()==2U);
    CHECK(close(shared.probes[0].weight+shared.probes[1].weight+
                shared.environmentWeight,1.0F,1.0e-5F));
    const auto faded=select_reflection_probe_mix(zones,{1.95F,0,0});
    CHECK(faded.probes.size()==2U || faded.probes.size()==1U);
    CHECK(faded.environmentWeight>0.01F);
    const auto outside=select_reflection_probe_mix(zones,{3.0F,0,0});
    CHECK(outside.probes.empty() && close(outside.environmentWeight,1.0F));
    CHECK(select_reflection_probe_mix(zones,{0,0,0},0U).probes.empty());
    std::vector<ReflectionProbe> invalid{{4,{0,0,0},{-1,1,1},1,1,10,true}};
    CHECK(select_reflection_probe_mix(invalid,{0,0,0}).probes.empty());

    // Radiance mixing uses local specular cubemaps and the sky remainder,
    // but never adds local diffuse irradiation to traced diffuse GI.
    IblBakeSettings cfg;cfg.irradianceResolution=4;cfg.specularResolution=8;
    cfg.specularMipLevels=4;cfg.sampleCount=32;cfg.brdfResolution=8;
    const auto make=[&](float v){
        EnvironmentCube src{8U,std::vector<Float3>(6ULL*8U*8U,{v,v,v})};
        return bake_image_based_lighting(src,cfg);
    };
    const auto environment=make(.25F), red=make(1.0F), blue=make(2.0F);
    const std::vector<ReflectionProbeRadiance> resident{{1,&red},{2,&blue}};
    const auto shade=[&](const ReflectionProbeMix& mix,
                         std::span<const ReflectionProbeRadiance> entries){
        return sample_reflection_probe_radiance(mix,zones,entries,environment,
                                               {0,0,0},{1,0,0},.75F);
    };
    const Float3 x=shade(shared,resident);
    CHECK(x.x>.25F && x.x<2.01F);
    const Float3 sky=shade(outside,resident);
    CHECK(close(sky.x,.25F,.04F));
    const std::vector<ReflectionProbeRadiance> onlyOne{{1,&red}};
    const Float3 missing=shade(shared,onlyOne);
    CHECK(missing.x<x.x && missing.x>.24F);
    const Float3 zero=sample_reflection_probe_radiance(shared,zones,resident,environment,
                                                       {0,0,0},{0,0,0},.75F);
    CHECK(close(zero.x,0.0F,1.0e-6F));
}
}

int main(){try{test_cube_coordinates();test_constant_environment_bake();test_equirectangular_and_probes();test_bounded_local_probe_mixing();test_local_probe_gpu_abi();test_irradiance_probe_sh9_bake();test_probe_grid_capture_driver();test_visibility_gated_irradiance();test_irradiance_probe_grid();std::cout<<"dve_environment_lighting_tests: PASS\n";return 0;}catch(const std::exception& e){std::cerr<<"dve_environment_lighting_tests: FAIL: "<<e.what()<<'\n';return 1;}}
