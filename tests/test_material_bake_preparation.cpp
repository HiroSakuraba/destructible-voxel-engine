#include "dve/editor_native.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/editor_material_inspection.hpp"
#include "dve/editor_prefab.hpp"
#include "dve/rgba8_png.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace dve;using namespace dve::editor;using namespace dve::render;
namespace {
void check(bool v,const std::string& e){if(!v)throw std::runtime_error(e);}
MaterialTextureBakeSettings settings(){MaterialTextureBakeSettings s;s.generateAtlas=true;s.targetUv=1;s.resolution=64;s.paddingPixels=2;s.samplesPerAxis=2;s.raySamples=32;return s;}
float average(const PolygonImage& image,bool rgbm=false,float range=1){double sum=0;unsigned count=0;for(std::size_t n=0;n<image.rgba8.size();n+=4)if(image.rgba8[n+3]&&(image.rgba8[n]||image.rgba8[n+1]||image.rgba8[n+2])){sum+=image.rgba8[n]/255.F*(rgbm?image.rgba8[n+3]/255.F*range:1);++count;}return count?float(sum/count):0;}
void atlas_and_materials(){
    auto a=make_editor_polygon_plane(1,1);a.contentHash=0;a.materials[0].baseColor={.8F,0,0,1};a.materials.push_back(a.materials[0]);a.materialBindings.push_back(a.materialBindings[0]);a.materials[1].baseColor={0,0,.6F,1};a.submeshes={{"red",0,3,0},{"blue",3,3,1}};
    for(auto& v:a.vertices){v.texcoord={0,0};v.texcoord1={0,0};}const auto source=polygon_asset_content_hash(a);
    auto s=settings();s.allAssignedSlots=true;auto v=validate_material_bake(a,s);check(v.valid,v.error);check(v.resolution==64&&v.minimumTexelsPerMeter>0&&v.utilization>0,"atlas preflight statistics");
    auto b=bake_material_textures(a,s);check(b.success,b.error);auto repeated=bake_material_textures(a,s);check(repeated.success&&b.targetUvs.size()==repeated.targetUvs.size(),"repeat atlas");for(unsigned n=0;n<b.targetUvs.size();++n)check(b.targetUvs[n].x==repeated.targetUvs[n].x&&b.targetUvs[n].y==repeated.targetUvs[n].y,"deterministic atlas");check(b.images.at(MaterialBakeMap::BaseColor).rgba8==repeated.images.at(MaterialBakeMap::BaseColor).rgba8,"deterministic maps");
    std::string error;auto derived=make_baked_material_asset(a,b,&error);check(bool(derived),error);check(derived->materials.size()==4&&derived->submeshes[0].materialIndex!=derived->submeshes[1].materialIndex,"separate residual material properties share atlas maps");check(polygon_asset_content_hash(a)==source,"atlas preserves source");for(unsigned n=0;n<a.vertices.size();++n)check(derived->vertices[n].texcoord.x==a.vertices[n].texcoord.x&&derived->vertices[n].texcoord1.y==a.vertices[n].texcoord1.y,"original vertices retained");
    auto existing=s;existing.generateAtlas=false;check(!validate_material_bake(a,existing).valid,"existing overlapping/degenerate UVs rejected");
    auto density=s;density.texelsPerMeter=100;auto dense=validate_material_bake(a,density);check(dense.valid&&dense.resolution>64&&dense.minimumTexelsPerMeter>=100,"density increases atlas size");density.texelsPerMeter=100000;check(!validate_material_bake(a,density).valid,"impossible density rejected");
    s.materialSlots={1,0};s.allAssignedSlots=false;check(!validate_material_bake(a,s).valid,"unsorted selections rejected");
    std::atomic<bool> stop{true};check(!prepare_material_bake_atlas(a,settings(),{&stop,{}}).valid,"cancel atlas");
    // UV1 is a source sampling stream too. Atlas generation must not replace it before evaluation.
    auto textured=make_editor_polygon_plane(2,1);textured.contentHash=0;textured.materials[0].baseColor={1,1,1,1};for(auto& vert:textured.vertices)vert.color={1,1,1,1};
    textured.images.push_back({"source","image/raw",2,1,{255,0,0,255,0,0,255,255}});textured.samplers.push_back({ImportedWrapMode::ClampToEdge,ImportedWrapMode::ClampToEdge,ImportedTextureFilter::Nearest,ImportedTextureFilter::Nearest});textured.textures.push_back({"source",0,0});textured.materialBindings[0].baseColor={0,1,PolygonTextureColorSpace::Srgb};textured.materialBindings[0].mapping.mappingMode=MaterialMappingMode::UV1;
    for(auto& vert:textured.vertices)vert.texcoord1={0,0};auto red=bake_material_textures(textured,settings());check(red.success,red.error);
    for(auto& vert:textured.vertices)vert.texcoord1={1,0};auto blue=bake_material_textures(textured,settings());check(blue.success,blue.error);check(red.images.at(MaterialBakeMap::BaseColor).rgba8!=blue.images.at(MaterialBakeMap::BaseColor).rgba8,"source UV1 evaluated independently of generated atlas");
    auto scaled=settings();scaled.scale={2,1,2};auto large=prepare_material_bake_atlas(a,scaled);check(large.valid&&large.minimumTexelsPerMeter<v.minimumTexelsPerMeter*.6F,"world scale changes texel density");scaled.scale.x=0;check(!bake_material_textures(a,scaled).success,"invalid scale rejected before division");
}
void ao_and_lighting(const std::filesystem::path& root){
    auto floor=make_editor_polygon_plane(1,1);auto s=settings();s.maps=96;s.lighting.sunIntensity=1;s.lighting.sunColor={1,1,1};s.lighting.sunDirection={0,1,0};s.lighting.skyColor=s.lighting.groundColor={0,0,0};s.lighting.shadowMode=ShadowMode::Hard;s.lighting.shadowStrength=1;s.lighting.pathTraceBounces=0;s.lighting.globalIlluminationIntensity=1;
    auto open=bake_material_textures(floor,s);check(open.success,open.error);check(average(open.images.at(MaterialBakeMap::AmbientOcclusion))>.98F,"open plane AO white");float light=average(open.images.at(MaterialBakeMap::DiffuseLighting),true,s.lightingRange);check(std::abs(light-1)<.03F,"analytic sun diffuse unit-white response");
    MaterialBakeScene scene;auto ceiling=make_editor_polygon_plane(2,20);scene.push_back({ceiling,{{0,.1F,0},{}}});auto blocked=bake_material_textures(floor,s,{},scene);check(blocked.success,blocked.error);check(average(blocked.images.at(MaterialBakeMap::AmbientOcclusion))<.55F,"near ceiling darkens AO");check(average(blocked.images.at(MaterialBakeMap::DiffuseLighting),true,s.lightingRange)<.01F,"ceiling shadows sun");
    auto nearAo=average(blocked.images.at(MaterialBakeMap::AmbientOcclusion));scene[0].pose.position.y=.8F;auto far=bake_material_textures(floor,s,{},scene);check(far.success&&average(far.images.at(MaterialBakeMap::AmbientOcclusion))>nearAo+.3F,"AO finite-distance falloff");
    scene.clear();s.lighting.sunIntensity=0;s.lighting.skyColor=s.lighting.groundColor={.4F,.4F,.4F};auto sky=bake_material_textures(floor,s);check(sky.success&&std::abs(average(sky.images.at(MaterialBakeMap::DiffuseLighting),true,s.lightingRange)-.4F)<.02F,"unoccluded constant environment");
    s.lighting.skyColor=s.lighting.groundColor={0,0,0};SceneLight point;point.positionMeters={0,1,0};point.intensity=3;point.color={1,.2F,.1F};s.lighting.lights={point};auto local=bake_material_textures(floor,s);check(local.success&&average(local.images.at(MaterialBakeMap::DiffuseLighting),true,s.lightingRange)>.1F,"point light bake");
    SceneLight area;area.kind=SceneLightKind::Quad;area.positionMeters={0,1,0};area.edgeU={2,0,0};area.edgeV={0,0,2};area.twoSided=true;area.intensity=1;s.lighting.lights={area};auto litArea=bake_material_textures(floor,s);check(litArea.success&&average(litArea.images.at(MaterialBakeMap::DiffuseLighting),true,s.lightingRange)>.1F,"area light bake");
    s.lighting.lights.clear();auto emitter=make_editor_polygon_plane(3,2);emitter.contentHash=0;emitter.materials[0].emissive={2,.2F,.1F};emitter.materialBindings[0].doubleSided=true;scene={{emitter,{{0,1,0},{}}}};auto emitted=bake_material_textures(floor,s,{},scene);check(emitted.success&&average(emitted.images.at(MaterialBakeMap::DiffuseLighting),true,s.lightingRange)>.05F,"emissive surface bake");
    // Same rig, with and without secondary reflection from the underside of a matte ceiling.
    scene[0].asset.materials[0].emissive={};scene[0].asset.materials[0].baseColor={.8F,.1F,.1F,1};scene[0].asset.contentHash=polygon_asset_content_hash(scene[0].asset);
    point.positionMeters={0,.5F,0};s.lighting.lights={point};s.lighting.globalIlluminationMode=GlobalIlluminationMode::VoxelOneBounce;s.lighting.pathTraceBounces=0;
    auto directOnly=bake_material_textures(floor,s,{},scene);check(directOnly.success,directOnly.error);s.lighting.pathTraceBounces=2;
    auto bounce=bake_material_textures(floor,s,{},scene);check(bounce.success,bounce.error);
    check(average(bounce.images.at(MaterialBakeMap::DiffuseLighting),true,s.lightingRange)>average(directOnly.images.at(MaterialBakeMap::DiffuseLighting),true,s.lightingRange)+.01F,"secondary reflected illumination increases same-rig lighting");
    s.lightingRange=.0001F;check(!bake_material_textures(floor,s,{},scene).success,"lighting overflow refused without clipping");s.lightingRange=16;
    auto encoded=encode_material_bake_settings(s);MaterialTextureBakeSettings decoded;check(decode_material_bake_settings(encoded,decoded)&&encode_material_bake_settings(decoded)==encoded,"lighting settings round trip");check(!decode_material_bake_settings(encoded+" garbage",decoded),"settings trailing data rejected");
    s.maps=127;auto full=bake_material_textures(floor,s,{},scene);check(full.success,full.error);std::string error;auto derived=make_baked_material_asset(floor,full,&error);check(bool(derived),error);check(derived->materialBindings.back().baseColor.texture.has_value(),"material map retained");auto inspect=render_material_inspection(floor,*derived,full.settings,MaterialInspectionChannel::DiffuseLighting,MaterialInspectionView::Baked);check(inspect.error.empty(),inspect.error);
    check(write_material_bake_bundle(root/"ray-bundle",floor,full,{},&error,scene),error);check(!write_material_bake_bundle(root/"ray-bundle",floor,full,{},&error,scene),"no overwrite");check(std::filesystem::exists(root/"ray-bundle"/"scene-0.dmesh"),"bundle includes neighbours");
    std::ofstream ppm(root/"lighting.ppm",std::ios::binary);ppm<<"P6\n"<<inspect.size<<' '<<inspect.size<<"\n255\n";for(auto c:inspect.rgb){ppm.put(char(c>>16));ppm.put(char(c>>8));ppm.put(char(c));}
    std::atomic<bool> cancel{};auto cancelled=bake_material_textures(floor,s,{&cancel,[&](float p,std::string_view){if(p>.1F)cancel=true;}},scene);check(!cancelled.success&&cancelled.cancelled,"ray bake cancellation");
    auto huge=s;huge.resolution=4096;huge.raySamples=256;check(!validate_material_bake(floor,huge).valid,"bounded ray work");
}
void reviewed_recipe_paths(const std::filesystem::path& root){
    auto source=make_editor_polygon_plane(1,1);MaterialTextureBakeSettings s;s.resolution=32;s.paddingPixels=2;auto bake=bake_material_textures(source,s);check(bake.success,bake.error);std::string error;auto derived=make_baked_material_asset(source,bake,&error);check(bool(derived),error);
    MaterialBakeRecipe r;r.source=source;r.baked=*derived;r.settings=r.bakedSettings=s;r.bakedSourceHash=bake.sourceHash;r.bakedAssetHash=polygon_asset_content_hash(*derived);
    EditorDocument d("Reviewed recipes");EditorObject o(1,"Baked");o.polygon=*derived;o.materialBake=r;d.add_object(std::move(o));
    auto copy=clone_editor_object(*d.find_object(1));copy.id=2;auto& duplicated=d.add_object(std::move(copy));check(duplicated.materialBake->source.objectId==2&&duplicated.materialBake->baked.objectId==2&&!material_bake_status(duplicated,{}).stale,"ordinary duplicate remaps recipe ownership");
    duplicated.materialBake->showingSource=true;duplicated.polygon=duplicated.materialBake->source;const auto key=material_bake_status(duplicated,{}).key;duplicated.polygon->materials[0].roughness=.13F;duplicated.polygon->contentHash=polygon_asset_content_hash(*duplicated.polygon);check(material_bake_status(duplicated,{}).key!=key,"source edits cannot cancel in ordered inspection hash");
    EditorPrefabAsset prefab("Recipe update");prefab.templateDocument.add_object(clone_editor_object(*d.find_object(1)));prefab.rootTemplateObjectIds={1};prefab.contentHash=editor_prefab_content_hash(prefab);prefab.assetPath=root/"recipe-update.dveprefab";
    EditorDocument instances("Instances");auto inst=instantiate_editor_prefab(instances,prefab);check(inst.success,inst.error);
    prefab.templateDocument.find_object(1)->materialBake->settings.paddingPixels=3;prefab.contentHash=editor_prefab_content_hash(prefab);auto update=apply_editor_prefab_instance_update(instances,inst.instanceId,prefab);check(update.success,update.error);auto* target=instances.find_object(inst.rootObjectIds[0]);check(target->materialBake&&target->materialBake->settings.paddingPixels==3&&target->materialBake->source.objectId==target->id,"prefab update copies/remaps recipe");
    prefab.templateDocument.find_object(1)->materialBake.reset();prefab.contentHash=editor_prefab_content_hash(prefab);update=apply_editor_prefab_instance_update(instances,inst.instanceId,prefab);check(update.success&&!instances.find_object(inst.rootObjectIds[0])->materialBake,"prefab update clears removed recipe");
    check(write_material_bake_recipe(root/"legacy-record",r,&error),error);
    std::ofstream out(root/"legacy-record"/"recipe.txt");out<<"DVE_MATERIAL_BAKE 1\n0 "<<r.bakedSourceHash<<' '<<r.bakedAssetHash<<" \"\"\n";
    for(unsigned n=0;n<2;++n)out<<"0 0 32 2 2 15 536870912 1 0 0 0 0 0 0 1\n";out<<"0 0\n";out.close();auto old=read_material_bake_recipe(root/"legacy-record",&error);check(old&&old->scene.empty()&&material_bake_recipe_hash(*old)==material_bake_recipe_hash(r),"version-1 recipe and historical prefab hash compatibility");
}
void queue_and_recipe(const std::filesystem::path& root){
    EditorDocument document("Batch 9");for(unsigned id=1;id<=3;++id){EditorObject o(id,"Object");o.polygon=make_editor_polygon_plane(id,1);o.transform.position={float(id)*2,0,0};if(id==3)o.flags.locked=true;check(ensure_modeling_topology(o),"modeling identities");document.add_object(std::move(o));}
    NativeEditorController controller{EditorWorkspace(std::move(document))};auto& w=controller.workspace();w.select_object(1);w.add_to_selection(2);w.add_to_selection(3);controller.material_panel().bake=settings();controller.material_panel().bake.maps=127;
    check(controller.material_panel_bake(true,root/"queue"),controller.material_panel().message);check(controller.wait_for_long_job(30),"queue complete");check(controller.material_panel().bakeQueueReport.size()==3,"per-object results");check(w.document().find_object(1)->materialBake&&w.document().find_object(2)->materialBake&&!w.document().find_object(3)->materialBake,"successful objects applied, locked target skipped");check(w.commands().size()==2,"one undo per object");check(w.commands().undo(w.document()).success&&!w.document().find_object(2)->materialBake,"queue undo");check(w.commands().redo(w.document()).success,"queue redo");
    auto* object=w.document().find_object(1);auto recipe=*object->materialBake;check(recipe.scene.size()==2,"neighbours captured including locked visible geometry");std::string error;check(write_material_bake_recipe(root/"recipe",recipe,&error),error);auto loaded=read_material_bake_recipe(root/"recipe",&error);check(loaded&&material_bake_recipe_hash(*loaded)==material_bake_recipe_hash(recipe),error+" recipe round trip");
    auto repeated=bake_material_textures(loaded->source,loaded->bakedSettings,{},loaded->scene);check(repeated.success,repeated.error);auto repeatAsset=make_baked_material_asset(loaded->source,repeated,&error);check(repeatAsset&&polygon_asset_content_hash(*repeatAsset)==loaded->bakedAssetHash,"frozen scene rebake deterministic");
    auto changed=loaded->scene;changed[0].pose.position.y+=1;check(material_bake_status(*object,object->transform,&changed).stale,"neighbour changes stale");auto rig=recipe.bakedSettings.lighting;rig.sunIntensity+=1;check(material_bake_status(*object,object->transform,&recipe.scene,&rig).stale,"rig changes stale");
    auto save=w.document().save_transactional(root/"scene.dvescene");check(save.success,save.error);std::filesystem::remove_all(root/"queue");auto reopened=EditorDocument::load(root/"scene.dvescene",&error);check(reopened&&reopened->find_object(1)->materialBake->scene.size()==2,"scene owns neighbours after removing bundle");
    w.select_object(1);check(controller.material_panel_bake_source(true),controller.material_panel().message);check(controller.material_panel_bake_source(false),controller.material_panel().message);
    auto baseline=polygon_asset_content_hash(*w.document().find_object(1)->polygon);check(controller.material_panel_bake(true,root/"cancel-queue"),"start cancelled queue");check(controller.cancel_long_job(),"cancel selected queue");check(polygon_asset_content_hash(*w.document().find_object(1)->polygon)==baseline,"queue cancellation retains assigned result");
}
}
int main(){const auto root=std::filesystem::temp_directory_path()/("dve-batch9-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));try{std::filesystem::create_directories(root);atlas_and_materials();ao_and_lighting(root);reviewed_recipe_paths(root);queue_and_recipe(root);std::cout<<"Batch 9: PASS "<<root<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<"; artifacts "<<root<<'\n';return 1;}}
