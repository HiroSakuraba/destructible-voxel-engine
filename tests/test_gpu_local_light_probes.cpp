// LP-2B Vulkan integration: scene-owned SH9 irradiance, reflection captures,
// revision invalidation, fallback. No device -> explicit skip (or hard fail with
// DVE_REQUIRE_VULKAN=1). This is a rendered-image test, not merely source checks.
#include "dve_test_skip.hpp"
#include "dve/irradiance_probe_grid.hpp"
#include "dve/player/player_renderer.hpp"
#include "dve/player/vulkan_player_renderer.hpp"
#include "dve/voxel_object.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dve;
using namespace dve::player;
namespace {
void check(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
std::size_t changed(const Rgba8Image& a,const Rgba8Image& b,int tolerance=3){
    check(a.width==b.width&&a.height==b.height,"image shape changed");
    std::size_t count=0;
    for(std::size_t i=0;i<a.pixels.size();i+=4U){
        int difference=0;
        for(int j=0;j<3;++j)difference+=std::abs(int(a.pixels[i+j])-int(b.pixels[i+j]));
        if(difference>tolerance)++count;
    }
    return count;
}
std::array<double,3> mean_rgb(const Rgba8Image& image){
    std::array<double,3> sum{};
    for(std::size_t i=0;i<image.pixels.size();i+=4U)
        for(std::size_t k=0;k<3;++k)sum[k]+=image.pixels[i+k];
    for(double& value:sum)value/=image.width*image.height;
    return sum;
}
}

int main(){
  try {
    if(!vulkan_player_renderer_available())
        return dve::testing::skip("local probe test: Vulkan SPIR-V unavailable");
    std::string error;
    auto gpu=make_vulkan_player_renderer({},&error);
    if(!gpu){
        if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(error);
        return dve::testing::skip("local probe test: Vulkan device unavailable: "+error);
    }
    check(gpu->resize(128U,96U,&error),error);
    VoxelObject ground{1},pillar{2};
    for(int x=-14;x<14;++x)for(int z=-14;z<14;++z)
        for(int y=-2;y<0;++y)(void)ground.set_voxel({x,y,z},3);
    for(int x=0;x<8;++x)for(int z=0;z<8;++z)
        for(int y=0;y<14;++y)(void)pillar.set_voxel({x,y,z},9);
    GameRenderObject slab;slab.id=1;slab.kind=GameGeometryKind::Voxel;
    slab.voxels=&ground;slab.voxelSizeMeters=.1F;
    GameRenderObject block;block.id=2;block.kind=GameGeometryKind::Voxel;
    block.voxels=&pillar;block.voxelSizeMeters=.1F;
    block.transform.position={-.4F,0,-.4F};
    std::vector<GameRenderObject> objects{slab,block};
    PlayerRenderView view;
    view.objects=objects;
    view.camera.position={2.6F,2.0F,3.4F};
    view.camera.target={0.0F,.6F,0.0F};
    view.camera.verticalFieldOfViewRadians=.9F;
    view.camera.nearPlane=.05F;view.camera.farPlane=100;
    view.environment.sunIntensity=0.0F;
    view.environment.skyColor={.03F,.03F,.03F};
    view.environment.groundColor={.03F,.03F,.03F};
    view.environment.globalIlluminationMode=GlobalIlluminationMode::AmbientHemisphere;
    view.environment.ambientOcclusionSamples=0;
    view.environment.volumetricFogDensityPerMeter=0;
    auto frame=[&]() {
        error.clear();
        check(gpu->render(view,&error)&&gpu->present(&error),"LP2B frame: "+error);
        const auto* image=gpu->readback();
        check(image&&image->valid(),"LP2B GPU readback failed");
        return *image;
    };
    const auto fallback=frame();

    // Two-by-two-by-two uniform cubemap capture; the entire voxel scene
    // lies inside the grid and is lit red. This ensures the *shader* reads
    // the SH9 data, rather than merely passing a CPU unit test.
    IrradianceProbeGrid grid;
    grid.origin={-2,-2,-2};grid.cellSize={4,4,4};
    grid.dimensions={2,2,2};grid.probes.resize(8U);
    constexpr float invY00=3.54490770181F;
    for(auto& probe:grid.probes)
        probe.sh[0]={2.0F*invY00,0,0};
    check(grid.validate(),"LP2B test SH9 grid invalid");
    view.localIrradianceGrid=&grid;
    view.localProbeRevision=1U;
    const auto red=frame();
    const auto redMean=mean_rgb(red);
    check(changed(red,fallback)>150U,"SH9 grid not visible in Vulkan frame");
    check(redMean[0]>mean_rgb(fallback)[0]+2.0,"red probe added no red light");

    // In-place bake with a new revision must update the GPU storage.
    for(auto& probe:grid.probes)
        probe.sh[0]={0,0,2.0F*invY00};
    view.localProbeRevision=2U;
    const auto blue=frame();
    const auto blueMean=mean_rgb(blue);
    check(changed(blue,red)>150U,"revision change did not update GPU SH9");
    check(blueMean[2]>redMean[2]+2.0,"blue SH9 bake added no blue light");

    // For specular, local captures override sky but not traced diffuse.
    IblBakeSettings bake; bake.irradianceResolution=4;
    bake.specularResolution=8; bake.specularMipLevels=4;
    bake.sampleCount=32; bake.brdfResolution=8;
    EnvironmentCube amber{8U,std::vector<Float3>(6ULL*8U*8U,{4.0F,1.0F,.1F})};
    auto baked=bake_image_based_lighting(amber,bake);
    std::array<ReflectionProbe,1> volumes{
        ReflectionProbe{1,{0,0,0},{3,3,3},.5F,1,1,true}};
    std::array<ReflectionProbeRadiance,1> captures{
        ReflectionProbeRadiance{1,&baked}};
    view.localIrradianceGrid=nullptr;
    view.localReflectionVolumes=volumes;
    view.localReflectionCaptures=captures;
    view.localProbeRevision=3U;
    const auto specular=frame();
    check(changed(specular,fallback)>100U,
          "local cubemap reflection not visible in Vulkan frame");

    // Clearing all probes must restore the original fallback. This catches
    // stale local cubemap descriptors and stale SH9 storage.
    view.localReflectionVolumes={};
    view.localReflectionCaptures={};
    view.localProbeRevision=4U;
    const auto restored=frame();
    check(changed(restored,fallback)<15U,
          "global sky fallback not restored after local probes removed");
    std::cout<<"LP-2B local diffuse, specular, revision, fallback: PASS\n";
    return 0;
  } catch(const std::exception& ex) {
    std::cerr<<"LP-2B Vulkan test failed: "<<ex.what()<<'\n';
    return 1;
  }
}
