#include "dve_test_skip.hpp"
#include "dve/game_world.hpp"
#include "dve/player/player_renderer.hpp"
#include "dve/player/vulkan_player_renderer.hpp"
#include "dve/voxel_object.hpp"
#include "dve/voxel_textures.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace dve;
using namespace dve::player;
static void require(bool c,const std::string& s){if(!c)throw std::runtime_error(s);}
static std::size_t changed(const Rgba8Image& a,const Rgba8Image& b){
    require(a.width==b.width&&a.height==b.height,"image size mismatch");std::size_t n=0;
    for(std::size_t i=0;i<a.pixels.size();i+=4)if(a.pixels[i]!=b.pixels[i]||a.pixels[i+1]!=b.pixels[i+1]||a.pixels[i+2]!=b.pixels[i+2])++n;
    return n;
}
int main(){try{
    if(!vulkan_player_renderer_available())return dve::testing::skip("Vulkan shaders unavailable");
#ifdef _WIN32
    _putenv_s("DVE_SPLIT_RC_EXPERIMENT","1");
#else
    setenv("DVE_SPLIT_RC_EXPERIMENT","1",1);
#endif
    VoxelObject ground{1},tower{2};
    for(int x=-12;x<12;++x)for(int z=-12;z<12;++z)for(int y=-2;y<0;++y)(void)ground.set_voxel({x,y,z},3);
    for(int x=0;x<6;++x)for(int z=0;z<6;++z)for(int y=0;y<12;++y)(void)tower.set_voxel({x,y,z},9);
    GameRenderObject slab;slab.id=1;slab.kind=GameGeometryKind::Voxel;slab.voxels=&ground;slab.voxelSizeMeters=.1F;
    GameRenderObject block;block.id=2;block.kind=GameGeometryKind::Voxel;block.voxels=&tower;block.voxelSizeMeters=.1F;block.transform.position={-.3F,0,-.3F};
    std::vector<GameRenderObject> objects{slab,block};PlayerRenderView view;view.objects=objects;
    view.camera.position={2.6F,2.0F,3.4F};view.camera.target={0,.5F,0};view.camera.verticalFieldOfViewRadians=.9F;view.camera.nearPlane=.05F;view.camera.farPlane=100;
    view.environment.globalIlluminationMode=GlobalIlluminationMode::VoxelOneBounce;view.environment.ambientOcclusionSamples=0;view.environment.volumetricFogDensityPerMeter=0;
    VulkanPlayerRendererOptions options;options.globalIllumination=PlayerGlobalIlluminationMode::RadianceCascades;
    std::string error;auto gpu=make_vulkan_player_renderer(options,&error);
    if(!gpu){if(std::getenv("DVE_REQUIRE_VULKAN"))throw std::runtime_error(error);return dve::testing::skip("Vulkan device unavailable: "+error);}
    require(gpu->resize(128,96,&error),error);
    auto frame=[&](){require(gpu->render(view,&error)&&gpu->present(&error),"render: "+error);const auto* image=gpu->readback();require(image&&image->valid(),"invalid image");return *image;};
    const auto first=frame(),repeat=frame();const auto stable=changed(first,repeat);std::cout<<"static changed pixels: "<<stable<<"\n";
    require(stable<10,"unchanged scene produced unstable image");
    // With all external lighting dark, changing only the block emission should
    // illuminate the surrounding floor through the split cascade, not just the block.
    std::vector<VoxelMaterialDefinition> materials(10);
    for(std::size_t i=0;i<materials.size();++i)materials[i]=default_voxel_material(static_cast<MaterialId>(i));
    objects[1].materials=materials;
    view.environment.sunIntensity=0;view.environment.skyColor={0,0,0};view.environment.groundColor={0,0,0};view.environment.bloomIntensity=0;
    const auto dark=frame();materials[9].emissive={8,0,0};const auto emissive=frame();
    materials[9].emissive={0,0,0};const auto darkAgain=frame();
    require(changed(dark,darkAgain)<10,"indirect light persisted after emission was removed");
    materials[9].emissive={8,0,0};
    view.environment.globalIlluminationMode=GlobalIlluminationMode::Off;
    const auto noIndirect=frame();
    std::size_t floorChanged=0;for(std::uint32_t y=70;y<94;++y)for(std::uint32_t x=15;x<113;++x){
        const auto i=(static_cast<std::size_t>(y)*dark.width+x)*4U;
        if(emissive.pixels[i]>dark.pixels[i]+3 && emissive.pixels[i]>noIndirect.pixels[i]+3)++floorChanged;
    }
    std::cout<<"emission changed floor-region pixels: "<<floorChanged<<"\n";
    require(floorChanged>10,"emissive block did not illuminate the floor region");
    // A named, hidden voxel proxy must still contribute indirect light.
    const std::string proxyName="lighting-only-proxy";objects[1].name=&proxyName;
#ifdef _WIN32
    _putenv_s("DVE_SPLIT_RC_PROXY_NAME",proxyName.c_str());
#else
    setenv("DVE_SPLIT_RC_PROXY_NAME",proxyName.c_str(),1);
#endif
    view.environment.globalIlluminationMode=GlobalIlluminationMode::VoxelOneBounce;
    materials[9].emissive={0,0,0};const auto proxyDark=frame();
    materials[9].emissive={8,0,0};const auto proxyLit=frame();
    require(gpu->last_stats().voxelInstances==1U,"lighting proxy was visibly submitted");
    std::size_t proxyChanged=0;for(std::size_t i=0;i<proxyDark.pixels.size();i+=4U)
        if(proxyLit.pixels[i]>proxyDark.pixels[i]+3)++proxyChanged;
    require(proxyChanged>10,"hidden voxel proxy did not contribute indirect emission");
    view.environment.globalIlluminationIntensity=0.0F;const auto proxyDisabled=frame();
    require(changed(proxyDark,proxyDisabled)<10,"disabling indirect lighting left stale proxy light");
    view.environment.globalIlluminationIntensity=0.65F;const auto proxyRestored=frame();
    require(changed(proxyLit,proxyRestored)<10,"re-enabling indirect lighting did not restore proxy light");
#ifdef _WIN32
    _putenv_s("DVE_SPLIT_RC_PROXY_NAME","");
#else
    unsetenv("DVE_SPLIT_RC_PROXY_NAME");
#endif
    objects[1].name=nullptr;
    objects[1].materials={};view.environment=RenderEnvironment{};view.environment.ambientOcclusionSamples=0;view.environment.volumetricFogDensityPerMeter=0;
    const auto restored=frame();
    view.environment.sunDirection={-.8F,.5F,.2F};const auto relit=frame();require(changed(restored,relit)>100,"changing the sun had no visible effect");
    view.camera.position={-2.2F,1.8F,3.0F};const auto moved=frame();require(changed(relit,moved)>500,"camera movement had no visible effect");
    for(int x=0;x<6;++x)for(int z=0;z<6;++z)for(int y=6;y<12;++y)(void)tower.set_voxel({x,y,z},kAirMaterial);
    const auto destroyed=frame();require(changed(moved,destroyed)>50,"voxel destruction had no visible effect");
    require(gpu->resize(96,64,&error),error);const auto resized=frame();require(resized.width==96&&resized.height==64,"resize failed");
    std::cout<<"split RC player frame, light, camera, destruction and resize passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
