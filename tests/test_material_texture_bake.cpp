#include "dve/editor_native.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/render/material_texture_bake.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/polygon_bvh.hpp"
#include "dve/transform.hpp"
#include "dve/rgba8_png.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace dve;using namespace dve::editor;using namespace dve::render;
namespace {
void check(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
void near(float a,float b,float tolerance,const std::string& why){check(std::abs(a-b)<=tolerance,why+": "+std::to_string(a)+" vs "+std::to_string(b));}
CookedPolygonAsset plane(){auto a=make_editor_polygon_plane(1,1);a.contentHash=0;for(auto& v:a.vertices)v.texcoord1={v.texcoord.y,v.texcoord.x};return a;}
MaterialTextureBakeSettings settings(){MaterialTextureBakeSettings s;s.resolution=32;s.paddingPixels=2;return s;}
MaterialTextureBakeResult bake(const CookedPolygonAsset& a,MaterialTextureBakeSettings s=settings()){auto result=bake_material_textures(a,s);check(result.success,result.error);return result;}
PolygonHitSurface sample(const CookedPolygonAsset& a,unsigned slot,const RigidTransform& pose={}){
    const auto& sub=*std::find_if(a.submeshes.begin(),a.submeshes.end(),[&](const auto& sub){return sub.materialIndex==slot;});
    const auto& v0=a.vertices[a.indices[sub.firstIndex]],&v1=a.vertices[a.indices[sub.firstIndex+1]],&v2=a.vertices[a.indices[sub.firstIndex+2]];
    PolygonBvhHit hit;hit.firstIndex=sub.firstIndex;hit.materialIndex=slot;hit.position=multiply(add(add(v0.position,v1.position),v2.position),1.F/3);hit.normal=transform_vector(pose,v0.normal);
    std::vector<PolygonTextureMipChain> chains;for(const auto& image:a.images){auto chain=generate_texture_mips(image);chain.normalLevels=generate_texture_mips(image,true).levels;chain.srgbLevels=generate_texture_mips(image,false,true).levels;chains.push_back(std::move(chain));}
    return sample_polygon_hit_material(a,hit,pose,hit.normal,chains);
}
void channels(){
    auto a=plane();auto& m=a.materials[0];m.baseColor={.25F,.5F,.75F,.8F};m.metallic=.7F;m.roughness=.3F;m.emissive={.1F,.2F,.3F};
    for(auto& v:a.vertices)v.color={.5F,.5F,.5F,.5F};
    auto s=settings();s.maps|=16;auto result=bake(a,s);check(result.coveredPixels==1024&&result.surfaceSamples==4096,"top-left raster rule covers each supersample once");
    const auto& c=result.images.at(MaterialBakeMap::BaseColor).rgba8;near(c[0],99,1,"linear averaging then sRGB");near(c[1],137,1,"base G");near(c[3],102,1,"linear alpha");
    const auto& mr=result.images.at(MaterialBakeMap::MetallicRoughness).rgba8;check(mr[0]==255&&mr[3]==255,"MR has no baked AO");near(mr[1],77,1,"roughness G");near(mr[2],179,1,"metallic B");
    near(result.images.at(MaterialBakeMap::Opacity).rgba8[0],102,1,"separate opacity");
    std::string error;auto derived=make_baked_material_asset(a,result,&error);check(bool(derived),error);check(derived->materials[0].baseColor.x==.25F,"original slot preserved");
    check(derived->vertices[derived->indices[0]].color.x==1&&!derived->materialBindings.back().opacity.texture,"no double vertex color or opacity");
    const auto evaluated=sample(*derived,unsigned(derived->materials.size()-1));near(evaluated.material.baseColor.x,.125F,.005F,"decoded derived color");near(evaluated.material.baseColor.w,.4F,.005F,"decoded derived alpha");
    auto stale=a;stale.materials[0].roughness=.8F;check(!make_baked_material_asset(stale,result),"dependency invalidation");
    auto edited=result;edited.settings.targetUv=1;check(!make_baked_material_asset(a,edited),"settings invalidation");
    edited=result;edited.settings.materialIndex=999;check(!make_baked_material_asset(a,edited),"invalid public result fails safely");
}
void uv_and_budget(){
    check(!write_rgba8_png("unused.png",0x80000000U,0x80000000U,{}),"PNG dimension arithmetic rejects overflow");
    auto a=plane();auto s=settings();check(validate_material_bake(a,s).valid,"shared edge allowed");
    auto overlap=a;overlap.indices.insert(overlap.indices.end(),{0,1,2});overlap.submeshes[0].indexCount=9;check(!validate_material_bake(overlap,s).valid,"continuous overlap");
    auto tiny=a;const auto first=unsigned(tiny.vertices.size());for(unsigned n=0;n<3;++n){auto v=a.vertices[n];v.texcoord={.6F+(n==1?.0001F:0),.2F+(n==2?.0001F:0)};tiny.vertices.push_back(v);}tiny.indices.insert(tiny.indices.end(),{first,first+1,first+2});tiny.submeshes[0].indexCount=9;
    check(!validate_material_bake(tiny,s).valid,"subpixel overlap rejected before raster");
    auto outside=a;outside.vertices[0].texcoord.x=-.001F;check(!validate_material_bake(outside,s).valid,"out-of-range target");
    auto degenerate=a;degenerate.vertices[1].texcoord=degenerate.vertices[0].texcoord;check(!validate_material_bake(degenerate,s).valid,"degenerate target");
    s.targetUv=1;check(validate_material_bake(a,s).valid,"UV1 target");s.memoryBudgetBytes=1;check(!validate_material_bake(a,s).valid,"bounded memory preflight");
    s=settings();a.materials[0].emissive={2,0,0};check(!bake_material_textures(a,s).success,"HDR emission is not silently clipped");s.maps=1;check(bake_material_textures(a,s).success,"unselected HDR channel does not block map-only bake");
    a=plane();a.materialBindings[0].mapping.heightMode=HeightMappingMode::OffsetParallax;check(!validate_material_bake(a,settings()).valid,"parallax rejected");
    std::atomic<bool> cancel{true};check(bake_material_textures(plane(),settings(),{&cancel,{}}).cancelled,"pre-cancellation");cancel=false;
    auto stopped=bake_material_textures(plane(),settings(),{&cancel,[&](float f,std::string_view){if(f>.1F)cancel=true;}});check(stopped.cancelled&&!stopped.success&&stopped.images.empty(),"mid-job cancellation has no partial output");
}
void normal_and_filtering(){
    auto a=plane();a.images={{"normal","image/raw",1,1,{200,180,220,255}}};a.textures={{"normal",0,{}}};a.materialBindings[0].normal={0,0,PolygonTextureColorSpace::Linear};
    for(unsigned uv=0;uv<2;++uv){auto s=settings();s.targetUv=uv;s.pose.rotation=quaternion_from_axis_angle({1,0,0},.7F);s.pose.position={4,2,-1};auto result=bake(a,s);auto derived=make_baked_material_asset(a,result);check(bool(derived),"derived UV normal asset");const auto before=sample(a,0,s.pose),after=sample(*derived,1,s.pose);near(dot(before.normal,after.normal),1,.002F,"mapped normal handedness and frozen pose round-trip");check(derived->vertices[derived->indices[0]].texcoord.x==a.vertices[0].texcoord.x,"UV0 preserved for UV1 output");
        PolygonRenderTarget target;target.resize(32,32);target.enable_passes();target.clear();PolygonCamera camera;camera.position=transform_point(s.pose,{0,2,0});camera.target=s.pose.position;camera.up=transform_vector(s.pose,{0,0,1});
        PolygonRenderInstance instance{1,&*derived,s.pose,{}, {1,1,1,1},true};ReferencePolygonRenderer renderer;
        const auto stats=renderer.render(std::span(&instance,1),camera,{},target,{.preserveExistingDepth=false});check(stats.shadedFragments>0,"derived raster draws");
        const auto center=16*32+16;check(target.objectId[center]==1,"derived raster center covered");near(dot(target.passes.normal[center],before.normal),1,.002F,"CPU raster decodes baked UV1 normal frame");}
    a=plane();for(auto& v:a.vertices){v.texcoord1=v.texcoord;v.texcoord.x*=128;v.texcoord.y*=128;}
    PolygonImage checker;checker.name="checker";checker.mimeType="image/raw";checker.width=checker.height=64;checker.rgba8.resize(64*64*4);for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){auto at=(y*64+x)*4;checker.rgba8[at]=checker.rgba8[at+1]=checker.rgba8[at+2]=(x+y)%2?255:0;checker.rgba8[at+3]=255;}
    a.images={checker};a.textures={{"checker",0,{}}};a.materials[0].baseColor={1,1,1,1};a.materialBindings[0].baseColor={0,0,PolygonTextureColorSpace::Srgb};auto s=settings();s.targetUv=1;
    auto result=bake(a,s);near(result.images.at(MaterialBakeMap::BaseColor).rgba8[0],188,2,"target derivatives select typed linear-light minification");
}
void layers(){
    auto a=plane();a.materials[0].baseColor={0,0,0,1};a.materials.emplace_back();a.materialBindings.emplace_back();a.materials[1].baseColor={1,0,0,1};
    a.materials[0].layers={{1,1,MaterialLayerBlendMode::Lerp,true}};PolygonMaterialLayerBinding binding;binding.heightBlendStrength=0;binding.mask={0,0,PolygonTextureColorSpace::Linear};a.materialBindings[0].layers={binding};
    a.materialBindings[0].mapping.mappingMode=MaterialMappingMode::UV1;
    a.images={{"mask","image/raw",2,1,{0,0,0,255,255,255,255,255}}};a.textures={{"mask",0,{}}};
    auto result=bake(a);const auto& image=result.images.at(MaterialBakeMap::BaseColor);
    const auto pixel=[&](unsigned x,unsigned y){return image.rgba8[(y*32+x)*4];};
    check(pixel(8,24)>pixel(8,8)+80,"per-pixel mask is baked using parent UV1 override");near(pixel(24,8),pixel(8,8),1,"mask mapping is independent of target UV");
    auto derived=make_baked_material_asset(a,result);check(derived&&derived->materials.back().layers.empty()&&derived->materials[0].layers.size()==1,"derived leaf preserves editable source stack");
}
void padding(){
    auto a=plane();a.vertices[0].texcoord={.15F,.15F};a.vertices[1].texcoord={.8F,.15F};a.vertices[2].texcoord={.8F,.8F};a.vertices[3].texcoord={.15F,.8F};
    auto result=bake(a);check(result.dilatedPixels>0&&result.chartCount==1,"chart-aware dilation");
    // Two disconnected coplanar triangles, close enough that two-pixel gutters collide.
    a=plane();a.vertices.resize(6);a.indices={0,1,2,3,4,5};a.submeshes[0].indexCount=6;
    for(unsigned n=0;n<3;++n)a.vertices[n+3]=a.vertices[n];
    a.vertices[0].texcoord={.1F,.1F};a.vertices[1].texcoord={.45F,.1F};a.vertices[2].texcoord={.45F,.8F};
    a.vertices[3].texcoord={.5F,.1F};a.vertices[4].texcoord={.85F,.1F};a.vertices[5].texcoord={.5F,.8F};
    auto failed=bake_material_textures(a,settings());check(!failed.success&&failed.error.find("padding")!=std::string::npos,"insufficient chart gutters rejected");
}
void parallax_normal_alignment(){
    // Regression: the CPU rasterizer must sample the normal map at the parallax-displaced UV,
    // exactly like the base-color/MR/emission channels, not at the undisplaced UV.
    auto make=[&](HeightMappingMode mode){
        auto a=plane();a.images={{"normal","image/raw",4,1,{140,128,255,255,170,128,255,255,200,128,255,255,230,128,255,255}},{"height","image/raw",1,1,{255,255,255,255}}};
        a.textures={{"normal",0,{}},{"height",1,{}}};
        a.materialBindings[0].normal={0,0,PolygonTextureColorSpace::Linear};a.materialBindings[0].height={1,0,PolygonTextureColorSpace::Linear};
        a.materialBindings[0].mapping.heightMode=mode;a.materialBindings[0].mapping.heightScale=.3F;a.materialBindings[0].mapping.maximumParallaxDistanceMeters=1000;return a;};
    auto render=[&](const CookedPolygonAsset& asset){
        PolygonRenderTarget target;target.resize(32,32);target.enable_passes();target.clear();PolygonCamera camera;camera.position={0,1.4F,1.4F};camera.target={0,0,0};camera.up={1,0,0};
        PolygonRenderInstance instance{1,&asset,{}, {}, {1,1,1,1},true};ReferencePolygonRenderer renderer;
        const auto stats=renderer.render(std::span(&instance,1),camera,{},target,{.preserveExistingDepth=false});check(stats.shadedFragments>0,"parallax scene draws");
        const auto center=16*32+16;check(target.objectId[center]==1,"parallax scene center covered");return target.passes.normal[center];};
    const auto flat=render(make(HeightMappingMode::Off)),displaced=render(make(HeightMappingMode::OffsetParallax));
    check(std::abs(flat.x-displaced.x)+std::abs(flat.z-displaced.z)>.01F,"normal map follows the parallax-displaced UV "+std::to_string(flat.x)+","+std::to_string(flat.y)+","+std::to_string(flat.z)+" vs "+std::to_string(displaced.x)+","+std::to_string(displaced.y)+","+std::to_string(displaced.z));
}
void files_and_editor(const std::filesystem::path& root,const char* cli){
    auto a=plane();std::string error;auto result=bake(a);const auto bundle=root/"bundle";
    check(write_material_bake_bundle(bundle,a,result,{},&error),error);check(!write_material_bake_bundle(bundle,a,result,{},&error),"never overwrite a bundle");
    auto loaded=read_dmesh(bundle/"baked.dmesh",10485760);check(bool(loaded),loaded.error);check(loaded.asset.materials.back().layers.empty(),"reimported derived slot is independent");
    auto original=read_dmesh(bundle/"source.dmesh",10485760);check(bool(original)&&polygon_asset_content_hash(original.asset)==polygon_asset_content_hash(a),"editable source survives export");
    std::ifstream png(bundle/"base-color.png",std::ios::binary);char signature[8]{};png.read(signature,8);check(std::string(signature,8)==std::string("\x89PNG\r\n\x1a\n",8),"real PNG signature");
    auto mapSettings=settings();mapSettings.maps=1;auto mapOnly=bake(a,mapSettings);check(write_material_bake_bundle(root/"map-only",a,mapOnly,{},&error),error);check(!std::filesystem::exists(root/"map-only"/"baked.dmesh"),"partial map export has no invalid derived material");
    std::atomic<bool> cancel{true};check(!write_material_bake_bundle(root/"cancelled",a,result,{&cancel,{}},&error)&&!std::filesystem::exists(root/"cancelled"),"cancelled export cleans owned staging directory");
    bool publishedA=false,publishedB=false;
    std::thread writerA([&]{publishedA=write_material_bake_bundle(root/"concurrent",a,result);});
    std::thread writerB([&]{publishedB=write_material_bake_bundle(root/"concurrent",a,result);});writerA.join();writerB.join();check(publishedA!=publishedB,"concurrent export has exactly one publisher");
    EditorDocument document("Bake");EditorObject object(1,"Plane");object.polygon=a;check(ensure_modeling_topology(object),"initialize persistent modeling identities");const auto originalTopology=*object.modelingTopology;document.add_object(std::move(object));NativeEditorController controller{EditorWorkspace(std::move(document))};auto& w=controller.workspace();w.select_object(1);controller.material_panel().bake=settings();
    const auto hash=polygon_asset_content_hash(*w.document().find_object(1)->polygon);const auto count=w.commands().size();check(controller.material_panel_bake(true,root/"editor"),"start asynchronous bake");check(controller.wait_for_long_job(30),"finish bake");
    check(w.commands().size()==count+1,"one bake apply, one undo transaction");
    const auto* applied=w.document().find_object(1);check(applied->modelingTopology&&validate_modeling_topology(*applied->polygon,nullptr,*applied->modelingTopology,&error),error);
    check(applied->modelingTopology->faces==originalTopology.faces&&applied->modelingTopology->edges==originalTopology.edges&&applied->modelingTopology->corners==originalTopology.corners,"attribute splits preserve persistent geometric identities");check(w.commands().undo(w.document()).success&&polygon_asset_content_hash(*w.document().find_object(1)->polygon)==hash,"undo exact editable source");check(*w.document().find_object(1)->modelingTopology==originalTopology,"undo modeling metadata");check(w.commands().redo(w.document()).success,"redo derived copy");
    auto saved=w.document().save_transactional(root/"scene.dvescene");check(saved.success,saved.error);auto scene=EditorDocument::load(root/"scene.dvescene",&error);check(bool(scene),error);check(scene->find_object(1)->polygon->materials.back().layers.empty(),"scene round-trip");
    controller.material_panel().slot=1;check(controller.material_panel_bake(true,root/"stale-scene"),"start stale test");w.document().find_object(1)->name="Changed";w.document().mark_dirty();check(controller.wait_for_long_job(30),"wait stale test");check(w.commands().size()==count+1,"changed scene has no apply command");
    controller.material_panel().open=true;check(controller.material_panel_bake(true,root/"editor-cancel"),"start cancellable panel job");
    check(controller.material_panel_key("escape",false,false,false)&&!controller.long_job_view().active,"Esc reaches background cancellation through material panel");check(w.commands().size()==count+1,"cancel has no apply command");
    if(cli){
        const auto command=std::string("\"")+cli+"\" \""+(bundle/"source.dmesh").string()+"\" \""+(root/"cli").string()+"\" --resolution 32 --padding 2";
        // cmd.exe /c strips the outer quotes of a command that starts with a quote, so wrap it once more on Windows.
        const auto run=[](const std::string& line){
#if defined(_WIN32)
            return std::system(("\""+line+"\"").c_str());
#else
            return std::system(line.c_str());
#endif
        };
        check(run(command)==0,"CLI successful export");check(run(command+" --uv 3")!=0,"CLI rejects invalid UV");check(run(command)!=0,"CLI rejects existing output");
    }
}
}
int main(int argc,char** argv){const auto root=std::filesystem::temp_directory_path()/("dve-batch7-bake-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));try{std::filesystem::create_directory(root);channels();uv_and_budget();normal_and_filtering();layers();padding();parallax_normal_alignment();files_and_editor(root,argc>1?argv[1]:nullptr);std::filesystem::remove_all(root);std::cout<<"material texture baking: PASS\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(root);return 1;}}
