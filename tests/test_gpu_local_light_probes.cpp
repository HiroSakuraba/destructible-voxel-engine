// LP-2B Vulkan integration: scene-owned SH9 irradiance, reflection captures,
// revision invalidation, fallback. No device -> explicit skip (or hard fail with
// DVE_REQUIRE_VULKAN=1). This is a rendered-image test, not merely source checks.
#include "dve_test_skip.hpp"
#include "dve/irradiance_probe_grid.hpp"
#include "dve/player/player_renderer.hpp"
#include "dve/player/vulkan_player_renderer.hpp"
#include "dve/render/voxel_reference_renderer.hpp"
#include "dve/voxel_object.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
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

// A directional, procedural environment with a bright panel and a floor/sky
// gradient. It is not a scene capture: it exercises reflection direction and
// roughness while the two-room capture workflow is still being developed.
EnvironmentCube room_capture(Float3 tint, bool movedPanel){
    constexpr unsigned size=16;
    EnvironmentCube cube{size,std::vector<Float3>(6U*size*size)};
    for(unsigned face=0;face<6;++face)for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){
        const Float3 d=cube_lookup_to_direction(static_cast<CubeFace>(face),
            {(x+.5F)/size,(y+.5F)/size});
        const float panel=std::pow(std::max(0.0F,(movedPanel?-d.x:d.x)*.8F+d.y*.45F-d.z*.35F),32.0F)*12.0F;
        const float level=.15F+.5F*std::max(0.0F,d.y)+panel;
        cube.texels[face*size*size+y*size+x]={tint.x*level,tint.y*level,tint.z*level};
    }
    return cube;
}

// Capture linear HDR from the real CPU voxel reference renderer. The scene
// and camera are in voxel units; each face follows CubeFace's UV convention.
EnvironmentCube capture_voxel_room(const VoxelObject& room,
        std::span<const VoxelMaterialDefinition> materials,Float3 position){
    constexpr unsigned size=32;
    EnvironmentCube cube{size,std::vector<Float3>(6U*size*size)};
    const std::array<Float3,6> directions{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
    const std::array<Float3,6> ups{{{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}}};
    render::VoxelReferenceInstance instance;instance.objectId=101;instance.object=&room;instance.materials=materials;
    render::ReferenceVoxelRenderer renderer;renderer.threadCount=2;renderer.metersPerVoxel=.1F;
    RenderEnvironment env;env.sunIntensity=0;env.skyColor={.03F,.03F,.03F};env.groundColor=env.skyColor;
    env.ambientOcclusionSamples=0;env.globalIlluminationMode=GlobalIlluminationMode::AmbientHemisphere;
    render::PolygonRenderTarget target;target.resize(size,size);
    for(unsigned face=0;face<6;++face){
        target.clear({.03F,.03F,.03F,1});
        render::PolygonCamera camera;camera.position=position;camera.target={position.x+directions[face].x,
            position.y+directions[face].y,position.z+directions[face].z};
        camera.up=ups[face];camera.verticalFieldOfViewRadians=1.57079632679F;
        camera.nearPlane=.5F;camera.farPlane=1000;
        (void)renderer.render(std::span(&instance,1),camera,env,target,true);
        // The renderer's camera right is cross(forward, up), which runs
        // opposite to increasing U in cube_lookup_to_direction on all six
        // faces, so each rendered row is stored reversed: without the flip
        // the baked SH9 and reflections would be horizontally mirrored
        // (the V axis already agrees and is copied unchanged).
        for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){
            const auto c=target.hdrColor[y*size+x];
            cube.texels[face*size*size+y*size+(size-1U-x)]={c.x,c.y,c.z};}
    }
    return cube;
}

// The aggregate color checks in the fixture cannot detect a mirrored
// capture, so pin the orientation directly: one emissive voxel at
// (4,0,-2) from the capture point must bake toward (0.894,0,-0.447),
// not toward its horizontal mirror (0.894,0,+0.447). CPU-only; runs
// with or without a Vulkan device.
void check_capture_orientation(){
    VoxelObject probe{101};
    (void)probe.set_voxel({4,0,-2},1);
    std::vector<VoxelMaterialDefinition> materials(2);
    for(unsigned i=0;i<materials.size();++i)materials[i]=default_voxel_material(static_cast<MaterialId>(i));
    materials[1].emissive={8,8,8};
    const auto cube=capture_voxel_room(probe,materials,{0,0,0});
    const auto toward=sample_environment_cube(cube,{.894F,0,-.447F});
    const auto mirrored=sample_environment_cube(cube,{.894F,0,.447F});
    check(toward.x>1.0F&&toward.x>4.0F*mirrored.x,
          "scene capture is horizontally mirrored relative to CubeFace UVs");
}

void two_room_fixture(IPlayerRenderer& gpu,std::string& error){
    VoxelObject room{101};
    auto put=[&](int x,int y,int z,MaterialId id){(void)room.set_voxel({x,y,z},id);};
    for(int x=-40;x<40;++x)for(int z=-20;z<20;++z)for(int y=-2;y<0;++y)put(x,y,z,1);
    for(int x=-40;x<40;++x)for(int y=0;y<26;++y)put(x,y,-20,1);
    for(int z=-20;z<20;++z)for(int y=0;y<26;++y)put(0,y,z,1);
    for(int side : {-1,1})for(int x=0;x<8;++x)for(int z=0;z<8;++z)for(int y=0;y<12;++y){
        put(side*20+x-13,y,z-4,2);
        put(side*20+x+5,y,z-4,3);
    }
    for(int x=15;x<25;++x)for(int y=10;y<20;++y)put(x,y,-19,4);
    std::vector<VoxelMaterialDefinition> materials(5);
    for(unsigned i=0;i<materials.size();++i)materials[i]=default_voxel_material(static_cast<MaterialId>(i));
    materials[1].baseColor={.55F,.55F,.55F,1};materials[1].roughness=.9F;
    materials[2].baseColor={.8F,.8F,.8F,1};materials[2].metallic=1;materials[2].roughness=.12F;
    materials[3]=materials[2];materials[3].roughness=.8F;
    GameRenderObject object;object.id=101;object.kind=GameGeometryKind::Voxel;
    object.voxels=&room;object.voxelSizeMeters=.1F;object.materials=materials;
    std::array<GameRenderObject,1> objects{object};
    PlayerRenderView view;view.objects=objects;
    view.camera.verticalFieldOfViewRadians=.95F;view.camera.nearPlane=.05F;view.camera.farPlane=100;
    view.environment.sunIntensity=0;view.environment.skyColor={.03F,.03F,.03F};
    view.environment.groundColor={.03F,.03F,.03F};
    view.environment.globalIlluminationMode=GlobalIlluminationMode::AmbientHemisphere;
    view.environment.ambientOcclusionSamples=0;view.environment.volumetricFogDensityPerMeter=0;
    const bool visual=std::getenv("DVE_PROBE_VISUAL")!=nullptr;
    const bool resized=gpu.resize(visual?1280U:320U,visual?720U:180U,&error);
    check(resized,"two-room resize: "+error);
    auto frame=[&](const char* name){
        error.clear();const bool rendered=gpu.render(view,&error);check(rendered,"two-room render: "+error);
        const bool presented=gpu.present(&error);check(presented,"two-room present: "+error);
        const auto* image=gpu.readback();check(image&&image->valid(),"two-room readback failed");
        if(const char* dir=std::getenv("DVE_PROBE_CAPTURE_DIR")){
            std::string saveError;const bool saved=write_png(std::filesystem::path(dir)/name,*image,&saveError);
            check(saved,"two-room capture: "+saveError);
        }
        return *image;
    };
    IblBakeSettings bake;bake.irradianceResolution=4;bake.specularResolution=16;
    bake.specularMipLevels=5;bake.sampleCount=64;bake.brdfResolution=16;
    const auto warmSource=room_capture({3.0F,1.1F,.3F},false);
    const auto coolSource=room_capture({.3F,1.1F,3.0F},false);
    auto warm=bake_image_based_lighting(warmSource,bake);
    auto cool=bake_image_based_lighting(coolSource,bake);
    const auto warmDiffuse=bake_irradiance_probe_sh9(warmSource);
    const auto coolDiffuse=bake_irradiance_probe_sh9(coolSource);
    std::array<ReflectionProbe,2> volumes{
        ReflectionProbe{1,{-2,1,0},{2,2,3},.2F,1,1,true},
        ReflectionProbe{2,{2,1,0},{2,2,3},.2F,1,1,true}};
    std::array<ReflectionProbeRadiance,2> captures{ReflectionProbeRadiance{1,&warm},ReflectionProbeRadiance{2,&cool}};
    IrradianceProbeGrid grid;grid.origin={-4,-1,-2};grid.cellSize={4,4,4};
    grid.dimensions={3,2,2};grid.probes.resize(12);
    for(unsigned z=0;z<2;++z)for(unsigned y=0;y<2;++y)for(unsigned x=0;x<3;++x){
        auto& p=grid.probes[x+3*(y+2*z)];
        p=x==2?coolDiffuse:warmDiffuse;
        if(x==1)for(auto& coefficient:p.sh)coefficient={};
    }
    check(grid.validate(),"two-room irradiance grid invalid");
    view.camera.position={-2,1.5F,4.0F};view.camera.target={-2,.8F,0};
    const auto baseline=frame("rooms-warm-sky-only.png");
    view.localIrradianceGrid=&grid;view.localReflectionVolumes=volumes;
    view.localReflectionCaptures=captures;view.localProbeRevision=10;
    const auto warmFrame=frame("rooms-warm-probes.png");
    check(changed(warmFrame,baseline)>500,"warm room probes did not change the image");
    const auto wm=mean_rgb(warmFrame);check(wm[0]>wm[2]+1,"warm room lighting is not warm");
    view.localReflectionVolumes={};view.localReflectionCaptures={};++view.localProbeRevision;
    const auto diffuseOnly=frame("rooms-warm-diffuse-only.png");
    check(changed(diffuseOnly,warmFrame)>100,"local reflections did not change the warm room");
    view.localReflectionVolumes=volumes;view.localReflectionCaptures=captures;++view.localProbeRevision;
    view.camera.position={2,1.5F,4.0F};view.camera.target={2,.8F,0};
    const auto coolFrame=frame("rooms-cool-probes.png");
    const auto cm=mean_rgb(coolFrame);check(cm[2]>cm[0]+1,"cool room lighting is not cool");
    materials[2].roughness=.8F;materials[3].roughness=.12F;
    const auto roughnessSwapped=frame("rooms-cool-roughness-swapped.png");
    check(changed(roughnessSwapped,coolFrame)>40,"metal roughness did not affect local reflections");
    materials[2].roughness=.12F;materials[3].roughness=.8F;
    view.camera.position={2.7F,1.6F,3.5F};
    const auto moved=frame("rooms-cool-moved-camera.png");
    check(changed(moved,coolFrame)>500,"camera movement did not change the image");
    cool=bake_image_based_lighting(room_capture({.3F,1.1F,3.0F},true),bake);
    ++view.localProbeRevision;
    const auto rebaked=frame("rooms-cool-rebaked.png");
    check(changed(rebaked,moved)>30,"rebaked directional capture did not change reflections");
    // Capture the real voxel scene twice with different emitter colors, bake
    // each HDR cubemap, and hold the visible scene fixed during GPU comparison.
    materials[4].emissive={8,.1F,.1F};
    const auto redScene=capture_voxel_room(room,materials,{20,12,5});
    materials[4].emissive={.1F,.1F,8};
    const auto blueScene=capture_voxel_room(room,materials,{20,12,5});
    materials[4].emissive={};
    auto redSceneIbl=bake_image_based_lighting(redScene,bake);
    auto blueSceneIbl=bake_image_based_lighting(blueScene,bake);
    const auto redSceneSh=bake_irradiance_probe_sh9(redScene);
    const auto blueSceneSh=bake_irradiance_probe_sh9(blueScene);
    for(unsigned z=0;z<2;++z)for(unsigned y=0;y<2;++y)grid.probes[2+3*(y+2*z)]=redSceneSh;
    captures[1].baked=&redSceneIbl;++view.localProbeRevision;
    const auto sceneRed=frame("rooms-scene-capture-red.png");
    for(unsigned z=0;z<2;++z)for(unsigned y=0;y<2;++y)grid.probes[2+3*(y+2*z)]=blueSceneSh;
    captures[1].baked=&blueSceneIbl;++view.localProbeRevision;
    const auto sceneBlue=frame("rooms-scene-capture-blue.png");
    check(changed(sceneRed,sceneBlue)>100,"scene-captured emitter change did not affect GPU lighting");
    const auto redMean=mean_rgb(sceneRed),blueMean=mean_rgb(sceneBlue);
    check(redMean[0]>blueMean[0]+.1 && blueMean[2]>redMean[2]+.1,
          "scene-captured emitter colors did not reach GPU lighting");
    std::cout<<"two-room irradiance, reflection, roughness, camera and directional rebake: PASS\n";
    std::cout<<"CPU HDR voxel capture to SH9 and reflection bake to GPU: PASS\n";
}
}

int main(){
  try {
    check_capture_orientation();
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
    auto capture=[&](const char* name,const Rgba8Image& image) {
        if(const char* dir=std::getenv("DVE_PROBE_CAPTURE_DIR")) {
            std::string saveError;
            check(write_png(std::filesystem::path(dir)/name,image,&saveError),"probe capture: "+saveError);
        }
    };
    const auto fallback=frame();
    capture("fallback.png",fallback);

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
    capture("irradiance-red.png",red);
    const auto redMean=mean_rgb(red);
    check(changed(red,fallback)>150U,"SH9 grid not visible in Vulkan frame");
    check(redMean[0]>mean_rgb(fallback)[0]+2.0,"red probe added no red light");

    // In-place bake with a new revision must update the GPU storage.
    for(auto& probe:grid.probes)
        probe.sh[0]={0,0,2.0F*invY00};
    view.localProbeRevision=2U;
    const auto blue=frame();
    capture("irradiance-blue.png",blue);
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
    capture("reflection-amber.png",specular);
    check(changed(specular,fallback)>100U,
          "local cubemap reflection not visible in Vulkan frame");
    const auto amberMean=mean_rgb(specular);
    check(amberMean[0]>amberMean[2]+1.0,"amber reflection has incorrect color");

    // Two captures exercise the GPU priority selector, not just upload
    // slot zero. The higher-priority blue capture must override amber
    // inside their overlapping volumes.
    EnvironmentCube cyan{8U,std::vector<Float3>(6ULL*8U*8U,{.1F,1.0F,4.0F})};
    auto blueCapture=bake_image_based_lighting(cyan,bake);
    std::array<ReflectionProbe,2> pairVolumes{
        volumes[0],
        ReflectionProbe{2,{0,0,0},{3,3,3},.5F,1,3,true}};
    std::array<ReflectionProbeRadiance,2> pairCaptures{
        captures[0],ReflectionProbeRadiance{2,&blueCapture}};
    view.localReflectionVolumes=pairVolumes;
    view.localReflectionCaptures=pairCaptures;
    view.localProbeRevision=4U;
    const auto prioritized=frame();
    capture("reflection-priority-blue.png",prioritized);
    check(changed(prioritized,specular)>100U,
          "higher-priority second reflection capture was ignored");
    const auto priorityMean=mean_rgb(prioritized);
    check(priorityMean[2]>priorityMean[0]+1.0,"priority reflection has incorrect color");

    // Clearing all probes must restore the original fallback. This catches
    // stale local cubemap descriptors and stale SH9 storage.
    view.localReflectionVolumes={};
    view.localReflectionCaptures={};
    view.localProbeRevision=5U;
    const auto restored=frame();
    capture("fallback-restored.png",restored);
    check(changed(restored,fallback)<15U,
          "global sky fallback not restored after local probes removed");
    two_room_fixture(*gpu,error);
    std::cout<<"LP-2B local diffuse, specular, revision, fallback: PASS\n";
    return 0;
  } catch(const std::exception& ex) {
    std::cerr<<"LP-2B Vulkan test failed: "<<ex.what()<<'\n';
    return 1;
  }
}
