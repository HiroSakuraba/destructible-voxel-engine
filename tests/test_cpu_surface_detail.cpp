#include "dve/render/polygon_renderer.hpp"
#include <cmath>
#include <array>
#include <iostream>
#include <stdexcept>
using namespace dve;
using namespace dve::render;
#define CHECK(x) do { if(!(x))throw std::runtime_error(std::string(#x)+":"+std::to_string(__LINE__)); } while(false)
bool close(float a,float b,float e=0.002F){return std::abs(a-b)<e;}
CookedPolygonAsset quad(){
    CookedPolygonAsset a;a.objectId=1;
    a.materials.push_back({});a.materialBindings.push_back({});a.materialBindings[0].doubleSided=true;
    a.vertices={{{-1,-1,0},{0,0,1},{1,0,0,1},{0,0},{1,1,1,1}},
                {{1,-1,0},{0,0,1},{1,0,0,1},{1,0},{1,1,1,1}},
                {{1,1,0},{0,0,1},{1,0,0,1},{1,1},{1,1,1,1}},
                {{-1,1,0},{0,0,1},{1,0,0,1},{0,1},{1,1,1,1}}};
    a.indices={0,1,2,0,2,3};a.submeshes.push_back({"quad",0,6,0});a.bounds={{-1,-1,0},{1,1,0}};
    return a;
}
void texture(CookedPolygonAsset& a,std::array<std::uint8_t,4> color){
    PolygonImage i;i.width=i.height=1;i.rgba8.assign(color.begin(),color.end());a.images.push_back(i);
    a.textures.push_back({"test",static_cast<std::uint32_t>(a.images.size()-1),{}});
}
void mip_tests(){
    PolygonImage image;image.width=2;image.height=1;image.rgba8={0,0,0,255,255,255,255,255};
    const auto linear=generate_texture_mips(image),srgb=generate_texture_mips(image,false,true);
    CHECK(linear.levels[1].rgba8[0]==128);CHECK(srgb.levels[1].rgba8[0]>=187&&srgb.levels[1].rgba8[0]<=189);
    image.rgba8={218,128,218,255,37,128,218,255};
    const auto normal=generate_texture_mips(image,true);
    CHECK(normal.levels[1].rgba8[2]>250);CHECK(std::abs(static_cast<int>(normal.levels[1].rgba8[0])-128)<=1);
    image.width=3;image.rgba8={0,0,0,255,0,0,0,255,255,255,255,255};
    CHECK(generate_texture_mips(image).levels[1].rgba8[0]==85);
}
void height_tests(){
    const auto ramp=[](Float2 uv){return 0.5F+0.2F*uv.x-0.1F*uv.y;};
    const auto n=height_bump_normal({0.2F,0.3F},{0.001F,0.001F},1,ramp);
    const auto expected=normalize(Float3{0.2F,-0.1F,1});CHECK(close(n.x,expected.x));CHECK(close(n.y,expected.y));
    MaterialMappingSettings s;s.heightMode=HeightMappingMode::ParallaxOcclusion;s.refinementSteps=12;
    const auto flat=[](Float2){return 0.7F;};
    const auto full=apply_parallax_mapping({0.4F,0.5F},{0.5F,0,1},0,s,flat);
    const auto bounded=apply_parallax_mapping({0.4F,0.5F},{0.5F,0,1},0,s,flat,{128,0,{0.7F,0.7F}});
    CHECK(bounded.hit&&bounded.samples==1);CHECK(close(bounded.uv.x,full.uv.x,1e-4F));
    const auto budget=apply_parallax_mapping({0.4F,0.5F},{0.5F,0,1},0,s,flat,{2,0});
    CHECK(budget.samples<=3+s.refinementSteps);
    CHECK(parallax_height_shadow({},0.7F,{0,0,1},s,flat).visibility==1);
    const auto ridge=[](Float2 uv){return uv.x>0.002F?0.1F:0.7F;};
    CHECK(parallax_height_shadow({},0.7F,{1,0,0.2F},s,ridge,32).visibility==0);
    CHECK(parallax_height_shadow({},0.7F,{0,0,-1},s,flat).visibility==0);
    CHECK(!apply_parallax_mapping({}, {0,0,1},0,s,flat,{0,0}).valid);
}
void render_tests(){
    auto a=quad();texture(a,{255,255,255,255});a.materialBindings[0].height.texture=0;
    a.materialBindings[0].mapping.heightMode=HeightMappingMode::ParallaxOcclusion;
    a.contentHash=polygon_asset_content_hash(a);CHECK(validate_polygon_asset(a));
    PolygonCamera camera;camera.position={0,0,3};camera.target={0,0,0};camera.nearPlane=0.1F;camera.farPlane=10;
    PolygonRenderTarget target;target.resize(65,65);target.enable_passes();
    PolygonRenderOptions opts;opts.preserveExistingDepth=false;opts.metersPerUnit=1;
    PolygonRenderInstance instances[]{{1,&a}};RenderEnvironment env;
    const std::size_t center=32*65+32;
    auto stats=ReferencePolygonRenderer{}.render(instances,camera,env,target,opts);
    CHECK(stats.parallaxSamples>0);CHECK(close(target.passes.depthMeters[center],3));
    opts.surfaceDetail.reliefDepthMeters=1;
    stats=ReferencePolygonRenderer{}.render(instances,camera,env,target,opts);
    CHECK(stats.reliefDepthWrites>0);const float expectedDepth=3+0.5F*(1-3/a.materialBindings[0].mapping.maximumParallaxDistanceMeters);
    CHECK(close(target.passes.depthMeters[center],expectedDepth));
    CHECK(close(target.depth[center],(expectedDepth-camera.nearPlane)/(camera.farPlane-camera.nearPlane)));
    CHECK(close(target.passes.light_sum(center).x,target.hdrColor[center].x));
    auto blocker=quad();blocker.contentHash=polygon_asset_content_hash(blocker);
    PolygonRenderInstance pair[]{instances[0],{2,&blocker,{{0,0,-0.25F},{}}}};
    ReferencePolygonRenderer{}.render(pair,camera,env,target,opts);CHECK(target.objectId[center]==2);
    std::swap(pair[0],pair[1]);ReferencePolygonRenderer{}.render(pair,camera,env,target,opts);CHECK(target.objectId[center]==2);
    opts.surfaceDetail.reliefDepthMeters=0;opts.surfaceDetail.heightBumpStrength=0.2F;opts.surfaceDetail.parallaxSelfShadow=true;
    stats=ReferencePolygonRenderer{}.render(instances,camera,env,target,opts);CHECK(stats.bumpSamples>0&&stats.parallaxShadowSamples>0);
    // Neutral detail must preserve an existing tilted normal under object rotation.
    auto normals=quad();texture(normals,{218,128,218,255});texture(normals,{128,128,255,255});
    normals.materialBindings[0].normal.texture=0;normals.contentHash=polygon_asset_content_hash(normals);
    const auto rotation=quaternion_from_axis_angle({0,1,0},0.8F);
    PolygonRenderInstance normalInstance{3,&normals,{{},rotation}};
    camera.position=rotate(rotation,{0,0,3});opts.surfaceDetail={};opts.materialDebugView=PolygonMaterialDebugView::WorldNormal;
    ReferencePolygonRenderer{}.render(std::span(&normalInstance,1),camera,env,target,opts);
    const auto before=target.hdrColor[center];
    normals.materialBindings[0].detailNormal.texture=1;normals.contentHash=polygon_asset_content_hash(normals);
    ReferencePolygonRenderer{}.render(std::span(&normalInstance,1),camera,env,target,opts);
    CHECK(close(before.x,target.hdrColor[center].x,0.01F));CHECK(close(before.z,target.hdrColor[center].z,0.01F));
}
int main(){try{mip_tests();height_tests();render_tests();std::cout<<"CPU surface detail tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
