#include "dve/environment_lighting.hpp"

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
}

int main(){try{test_cube_coordinates();test_constant_environment_bake();test_equirectangular_and_probes();std::cout<<"dve_environment_lighting_tests: PASS\n";return 0;}catch(const std::exception& e){std::cerr<<"dve_environment_lighting_tests: FAIL: "<<e.what()<<'\n';return 1;}}
