#include "dve/render/polygon_renderer.hpp"
#include "dve/player/player_renderer.hpp"
#include "dve/editor_runtime_settings.hpp"
#include "dve/editor_native.hpp"
#include "dve/player/player_app.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {
using namespace dve;
using namespace dve::render;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
CookedPolygonAsset quad(bool coarse=false,unsigned textureSize=4){
    CookedPolygonAsset a;
    VoxelMaterialDefinition material;material.name="test";material.baseColor={1,1,1,1};
    a.materials.push_back(material);a.materialBindings.push_back({});a.materialBindings[0].baseColor.texture=0;
    a.materialBindings[0].doubleSided=true;
    a.vertices={{{-1,-1,0},{0,0,1},{1,0,0,1},{0,1},{1,1,1,1}},
                {{1,-1,0},{0,0,1},{1,0,0,1},{1,1},{1,1,1,1}},
                {{1,1,0},{0,0,1},{1,0,0,1},{1,0},{1,1,1,1}},
                {{-1,1,0},{0,0,1},{1,0,0,1},{0,0},{1,1,1,1}}};
    a.indices=coarse ? std::vector<std::uint32_t>{0,1,2} : std::vector<std::uint32_t>{0,1,2,0,2,3};
    a.submeshes.push_back({"quad",0,static_cast<std::uint32_t>(a.indices.size()),0});
    a.bounds={{-1,-1,0},{1,1,0}};
    PolygonImage image;image.width=textureSize;image.height=textureSize;image.rgba8.resize(static_cast<std::size_t>(textureSize)*textureSize*4U);
    for(std::size_t i=0;i<image.rgba8.size();i+=4){image.rgba8[i]=255;image.rgba8[i+1]=static_cast<std::uint8_t>((i/4)%255);image.rgba8[i+2]=40;image.rgba8[i+3]=255;}
    a.images.push_back(std::move(image));a.samplers.push_back({});a.textures.push_back({"texture",0,0});
    a.contentHash=polygon_asset_content_hash(a);check(static_cast<bool>(validate_polygon_asset(a)),"invalid fixture");return a;
}
PolygonCamera camera(){PolygonCamera c;c.position={0,0,5};c.target={0,0,0};c.verticalFieldOfViewRadians=1.0F;c.nearPlane=0.1F;c.farPlane=50;return c;}
void same_frame(const PolygonRenderTarget& a,const PolygonRenderTarget& b){
    check(a.objectId==b.objectId && a.depth==b.depth && a.materialIndex==b.materialIndex,"depth/selection changed");
    for(std::size_t i=0;i<a.hdrColor.size();++i){const auto x=a.hdrColor[i],y=b.hdrColor[i];
        check(x.x==y.x && x.y==y.y && x.z==y.z && x.w==y.w,"cached/culling pixels changed");}
}
void culling(){
    auto asset=quad();ReferencePolygonRenderer renderer;PolygonRenderTarget on,off;on.resize(96,64);off.resize(96,64);
    const Float3 outside[]{{100,0,0},{-100,0,0},{0,100,0},{0,-100,0},{0,0,6},{0,0,-100}};
    std::vector<PolygonRenderInstance> instances;
    for(const auto p:outside)instances.push_back({instances.size()+1,&asset,{p}, {}, {1,1,1,1},true});
    PolygonRenderOptions options;options.preserveExistingDepth=false;
    const auto culled=renderer.render(instances,camera(),{},on,options);
    options.enableFrustumCulling=false;const auto submitted=renderer.render(instances,camera(),{},off,options);
    same_frame(on,off);check(culled.frustumCulledInstances==6 && culled.submittedTriangles==0 && culled.mipChainsBuilt==0,"six-plane rejection failed");
    check(submitted.submittedTriangles==12,"Off still rejected triangle submission");
    // Rotated, overlapping, near/far-straddling and edge geometry must keep pixels.
    std::mt19937 random(731);std::uniform_real_distribution<float> positions(-8,8),angles(-3,3);
    instances.clear();
    for(unsigned i=0;i<80;++i)instances.push_back({i+1,&asset,{{positions(random),positions(random),positions(random)},quaternion_from_euler_xyz({angles(random),angles(random),angles(random)})},{},{1,1,1,1},true});
    for(float x:{-4.0F,0.0F,4.0F})instances.push_back({100+instances.size(),&asset,{{x,0,0}},{},{1,1,1,1},true});
    options.enableFrustumCulling=true;(void)renderer.render(instances,camera(),{},on,options);
    options.enableFrustumCulling=false;(void)renderer.render(instances,camera(),{},off,options);same_frame(on,off);
    // Bounds may be stale/loose metadata; current vertices define conservative bounds.
    asset.bounds={{100,100,100},{101,101,101}};asset.contentHash=polygon_asset_content_hash(asset);
    PolygonRenderInstance visible{1,&asset};options.enableFrustumCulling=true;
    const auto stats=renderer.render(std::span(&visible,1),camera(),{},on,options);
    check(stats.frustumCulledInstances==0 && stats.shadedFragments>0,"authored bounds hid current vertices");
}
void mip_cache(){
    auto asset=quad();ReferencePolygonRenderer renderer;PolygonRenderCache cache;PolygonRenderTarget cached,reference;
    cached.resize(64,64);reference.resize(64,64);std::vector<PolygonRenderInstance> instances;
    for(unsigned i=0;i<32;++i)instances.push_back({i+1,&asset,{{static_cast<float>(i%4)*0.05F,0,0}},{},{1,1,1,1},true});
    PolygonRenderOptions options;options.cache=&cache;options.preserveExistingDepth=false;
    auto stats=renderer.render(instances,camera(),{},cached,options);
    check(stats.assetValidations==1 && stats.mipChainsBuilt==1 && stats.mipCacheHits==31,"per-instance preparation was not shared");
    stats=renderer.render(instances,camera(),{},cached,options);
    check(stats.mipChainsBuilt==0 && stats.mipCacheHits==32,"warm mip cache rebuilt");
    options.cache=nullptr;(void)renderer.render(instances,camera(),{},reference,options);same_frame(cached,reference);
    // In-place edits with a published new hash cannot reuse old textures.
    asset.images[0].rgba8[0]=0;asset.contentHash=polygon_asset_content_hash(asset);options.cache=&cache;
    stats=renderer.render(instances,camera(),{},cached,options);check(stats.mipChainsBuilt==1,"texture edit reused stale cache");
    options.cache=nullptr;(void)renderer.render(instances,camera(),{},reference,options);same_frame(cached,reference);
    asset.images[0].rgba8[1]^=127;options.cache=&cache;
    stats=renderer.render(instances,camera(),{},cached,options);check(stats.culledInstances==32 && stats.mipChainsBuilt==0,"corrupt hash bypassed validation");
    asset.contentHash=polygon_asset_content_hash(asset);
    PolygonRenderCache tiny(1);options.cache=&tiny;
    stats=renderer.render(instances,camera(),{},cached,options);check(tiny.resident_mip_bytes()==0 && stats.mipChainsBuilt==1,"oversized cache did not stay frame-local");
    options.enableTextures=false;stats=renderer.render(instances,camera(),{},cached,options);check(stats.mipChainsBuilt==0,"disabled textures generated mips");
    cache.clear();check(cache.resident_mip_bytes()==0 && cache.cached_assets()==0 && cache.lod_history_size()==0,"cache clear retained state");
    // Eviction must not release texture chains pinned by transparent triangles.
    auto second=quad();asset.materials[0].baseColor.w=0.5F;asset.materials[0].blendMode=MaterialBlendMode::Translucent;
    second.materials[0].baseColor.w=0.5F;second.materials[0].blendMode=MaterialBlendMode::Translucent;
    second.images[0].rgba8[0]=20;asset.contentHash=polygon_asset_content_hash(asset);second.contentHash=polygon_asset_content_hash(second);
    const PolygonRenderInstance transparent[]{{1,&asset},{2,&second,{{0,0,-0.1F}}}};
    PolygonRenderCache single(84);options.enableTextures=true;options.cache=&single;
    (void)renderer.render(transparent,camera(),{},cached,options);options.cache=nullptr;(void)renderer.render(transparent,camera(),{},reference,options);same_frame(cached,reference);
    check(single.cached_assets()==1 && single.resident_mip_bytes()<=84,"LRU cache exceeded byte budget");
}
void lod_and_player(){
    auto fine=quad(),coarse=quad(true);const PolygonLodLevel levels[]{{&coarse,0},{&fine,40}};
    PolygonRenderInstance object{7,&fine,{},levels};PolygonRenderTarget target;target.resize(64,64);ReferencePolygonRenderer renderer;
    PolygonRenderOptions options;options.preserveExistingDepth=false;options.lodBias=1;
    check(renderer.render(std::span(&object,1),camera(),{},target,options).submittedTriangles==1,"positive LOD bias did not coarsen");
    options.lodBias=-1;check(renderer.render(std::span(&object,1),camera(),{},target,options).submittedTriangles==2,"negative LOD bias did not refine");
    // Place the diameter just above/below 40 px, then cross the 10% hysteresis bands.
    PolygonRenderCache cache;options.cache=&cache;options.lodBias=0;
    const auto at=[&](float diameter){auto c=camera();c.position.z=std::sqrt(2.0F)*64.0F/(std::tan(0.5F)*diameter);
        return renderer.render(std::span(&object,1),c,{},target,options).submittedTriangles;};
    check(at(41)==2 && at(39)==2 && at(35)==1 && at(41)==1 && at(45)==2,"LOD hysteresis flickered or stuck");
    options.lodBias=1;check(at(41)==1,"live bias retained old hysteresis state");
    options.cache=nullptr;options.lodBias=4;check(at(30)==1,"below-lowest threshold reverted to base");
    // Actual player renderer: shared source copies/mips, settings and LOD spans.
    std::vector<GameRenderObject> objects(16);
    for(unsigned i=0;i<objects.size();++i){objects[i].id=i+1;objects[i].kind=GameGeometryKind::Polygon;objects[i].polygon=&fine;objects[i].polygonLods=levels;}
    player::PlayerRenderView view{objects,camera(),{}};view.polygonLodBias=1;
    auto player=player::make_cpu_player_renderer(nullptr);std::string error;check(player->resize(64,64,&error),"player resize failed");
    check(player->render(view,&error),"player render failed");auto stats=player->last_stats();
    check(stats.scaledPolygonCopies==2 && stats.scaledPolygonAssets==2 && stats.polygons.submittedTriangles==16,"player did not share assets/apply LOD");
    check(player->render(view,&error),"second player render failed");stats=player->last_stats();
    check(stats.scaledPolygonCopies==0 && stats.polygons.mipChainsBuilt==0,"player rebuilt warm assets");
    objects[0].transform.position.x=100;view.polygonFrustumCulling=true;
    check(player->render(view,&error) && player->last_stats().polygons.frustumCulledInstances==1,"player culling setting not consumed");
    view.polygonFrustumCulling=false;check(player->render(view,&error) && player->last_stats().polygons.frustumCulledInstances==0,"player Off ignored");
    view.polygonLodBias=-1;check(player->render(view,&error) && player->last_stats().polygons.submittedTriangles==32,"player bias switch ignored");
    view.objects={};check(player->render(view,&error) && player->last_stats().scaledPolygonAssets==0,"unloaded assets retained scaled copies");
}
void settings(){
    auto registry=editor::EditorSettingsRegistry::make_default();std::string error;
    check(registry.set(editor::SettingScope::Project,"polygon.lod_bias",2.0,&error),"bias rejected");
    check(registry.set(editor::SettingScope::Project,"polygon.frustum_culling",false,&error),"culling rejected");
    ui::GameSettings original;original.width=1280;original.masterVolume=0.3F;
    const auto exported=editor::polygon_game_settings(registry,original);
    const auto parsed=ui::GameSettings::parse(exported.serialize(),&error);
    check(parsed && parsed->polygonLodBias==2 && !parsed->polygonFrustumCulling && parsed->width==1280 && parsed->masterVolume==0.3F,"settings bridge/round trip lost values");
    check(!ui::GameSettings::parse("DVE_GAME_SETTINGS=2\npolygon.lod_bias=nan\n") &&
        !ui::GameSettings::parse("DVE_GAME_SETTINGS=2\npolygon.lod_bias=5\n") &&
        !ui::GameSettings::parse("DVE_GAME_SETTINGS=2\npolygon.frustum_culling=maybe\n"),"invalid game polygon policy accepted");
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path=std::filesystem::temp_directory_path()/("polygon_settings_"+std::to_string(stamp)+".txt");
    check(original.save(path,&error) && exported.save(path,&error),"game-settings atomic replacement failed");
    std::ifstream in(path);const std::string text{std::istreambuf_iterator<char>(in),{}};in.close();
    check(ui::GameSettings::parse(text)->polygonLodBias==2,"saved settings unreadable");std::filesystem::remove(path);
}
void export_fixture(const std::filesystem::path& root){
    std::filesystem::create_directories(root);std::string error;
    auto document=editor::make_new_project_document();check(document.save_transactional(root/"scene.dvescene").success,"scene fixture save failed");
    auto registry=editor::EditorSettingsRegistry::make_default();
    check(registry.set(editor::SettingScope::Project,"polygon.lod_bias",2.0,&error),"project bias failed");
    check(registry.set(editor::SettingScope::Project,"polygon.frustum_culling",false,&error),"project culling failed");
    check(registry.save_scope_file(editor::SettingScope::Project,root/".dve/project/editor_settings.txt",&error),"project preferences save failed");
    ui::GameSettings defaults;defaults.width=1280;defaults.masterVolume=0.3F;
    check(defaults.save(root/"game_settings.txt",&error),"game defaults save failed");
    std::ofstream manifest(root/"game.dvegame");
    manifest<<"DVE_GAME 1\nname=Polygon Settings\nversion=1.0\nentryScene=scene.dvoxscene.json\nsettings=game_settings.txt\n";
}
void verify_export(const std::filesystem::path& root){
    std::string error;
    std::vector<std::filesystem::path> inputs;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(root))
        if(entry.is_regular_file())inputs.push_back(std::filesystem::relative(entry.path(),root));
    const auto package=root/"game.dvepak";check(build_dvepak(root,inputs,package,{},nullptr,&error),"fixture package failed");
    for(bool packed:{false,true}){
        std::unique_ptr<ContentSource> source=packed ? std::unique_ptr<ContentSource>(PakContentSource::open(package,&error))
                                                   : std::unique_ptr<ContentSource>(LooseContentSource::open(root,&error));
        player::PlayerBootOptions options;options.enableScripts=false;options.saveDirectory=root/"saves";
        auto app=player::PlayerApp::boot(std::move(source),options,&error);check(static_cast<bool>(app),error.c_str());
        std::vector<GameRenderObject> objects;const auto view=app->render_view(objects,1);
        check(view.polygonLodBias==2 && !view.polygonFrustumCulling && app->settings().width==1280 &&
            app->settings().masterVolume==0.3F,"exported settings did not reach loose/packed player");
    }
}
} // namespace
int main(int argc,char** argv){try{
    if(argc==3 && std::string_view(argv[1])=="--export-fixture"){export_fixture(argv[2]);return 0;}
    if(argc==3 && std::string_view(argv[1])=="--verify-export"){verify_export(argv[2]);return 0;}
    culling();mip_cache();lod_and_player();settings();std::cout<<"dve_polygon_performance_tests: PASS\n";return 0;}
    catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
