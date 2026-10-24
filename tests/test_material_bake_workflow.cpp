#include "dve/editor_native.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/editor_prefab.hpp"
#include "dve/editor_scene_export.hpp"
#include "dve/editor_material_inspection.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace dve;using namespace dve::editor;using namespace dve::render;
namespace {
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
void rehash(EditorObject& object){object.polygon->contentHash=polygon_asset_content_hash(*object.polygon);}
EditorDocument document(){EditorDocument d("Batch 8");EditorObject object(1,"Source");object.polygon=make_editor_polygon_plane(1,1);for(auto& v:object.polygon->vertices){v.texcoord1=v.texcoord;v.color={.6F,.7F,.8F,1};}object.polygon->materials[0].baseColor={.3F,.6F,.8F,1};object.polygon->materials[0].roughness=.4F;rehash(object);check(ensure_modeling_topology(object),"modeling metadata");d.add_object(std::move(object));return d;}
void run_bake(NativeEditorController& controller,const std::filesystem::path& path){check(controller.material_panel_bake(true,path),controller.material_panel().message);check(controller.wait_for_long_job(30),"background bake timed out");check(std::filesystem::exists(path/"baked.dmesh"),"export published");}
void inspection(const EditorObject& o,const std::filesystem::path& root){
    const auto& r=*o.materialBake;std::vector<MaterialPreviewImage> gallery;
    for(unsigned channel=0;channel<8;++channel){auto c=MaterialInspectionChannel(channel);auto source=render_material_inspection(r.source,r.baked,r.settings,c,MaterialInspectionView::Source);auto baked=render_material_inspection(r.source,r.baked,r.settings,c,MaterialInspectionView::Baked);auto split=render_material_inspection(r.source,r.baked,r.settings,c,MaterialInspectionView::Split);
        check(source.error.empty()&&baked.error.empty()&&split.error.empty(),source.error+baked.error+split.error);
        unsigned visible=0;for(unsigned y=0;y<source.size;++y)for(unsigned x=0;x<source.size;++x){auto i=y*source.size+x;if(x<source.size/2)check(split.rgb[i]==source.rgb[i],"source half of comparison");else if(x>source.size/2)check(split.rgb[i]==baked.rgb[i],"baked half of comparison");if(source.rgb[i]!=0x172133&&source.rgb[i]!=0x293447)++visible;}
        check(visible>100,"actual geometry visible in every channel");gallery.push_back(std::move(split));
    }
    check(gallery[0].rgb!=gallery[1].rgb&&gallery[2].rgb!=gallery[3].rgb&&gallery[3].rgb!=gallery[4].rgb,"independent channel outputs");
    auto cancelled=render_material_inspection(r.source,r.baked,r.settings,MaterialInspectionChannel::Beauty,MaterialInspectionView::Split,[]{return true;});check(!cancelled.error.empty(),"inspection cancellation");
    MaterialPreviewWorker worker;worker.request_inspection(r.source,r.baked,r.settings,MaterialInspectionChannel::BaseColor,MaterialInspectionView::Source);worker.request_inspection(r.source,r.baked,r.settings,MaterialInspectionChannel::Metallic,MaterialInspectionView::Baked);
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(worker.busy()&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(2));auto image=worker.image();auto expected=render_material_inspection(r.source,r.baked,r.settings,MaterialInspectionChannel::Metallic,MaterialInspectionView::Baked);check(image&&image->error.empty()&&image->rgb==expected.rgb,"latest inspection generation wins");
    std::ofstream ppm(root/"inspection.ppm",std::ios::binary);ppm<<"P6\n512 64\n255\n";for(unsigned y=0;y<64;++y)for(const auto& panel:gallery)for(unsigned x=0;x<64;++x){auto c=panel.rgb[y*64+x];const char bytes[]{char(c>>16),char(c>>8),char(c)};ppm.write(bytes,3);}
}
void ownership(const EditorDocument& baked){
    // Duplicate / paste: polygon hashes include the object id, so the snapshots must follow the copy.
    auto scene=clone_editor_document(baked);EditorObject copy=clone_editor_object(*scene.find_object(1));copy.id=0;
    const auto& added=scene.add_object(std::move(copy));
    check(added.id!=1&&added.materialBake&&added.materialBake->source.objectId==added.id&&added.materialBake->baked.objectId==added.id,"duplicate recipe snapshots are owned by the duplicate");
    check(!material_bake_status(added,added.transform).stale,"duplicate keeps a current bake");
    std::string error;check(validate_material_bake_recipe(*added.materialBake,&error),error);
    // Prefab instance update must carry the recipe with the geometry (absent -> present, then changed).
    EditorDocument plain=clone_editor_document(baked);plain.find_object(1)->materialBake.reset();
    EditorPrefabAsset oldPrefab("Plain");oldPrefab.templateDocument=std::move(plain);oldPrefab.rootTemplateObjectIds={1};oldPrefab.contentHash=editor_prefab_content_hash(oldPrefab);oldPrefab.assetPath="plain.dveprefab";
    auto instances=clone_editor_document(baked);instances.remove_object(1);auto made=instantiate_editor_prefab(instances,oldPrefab);check(made.success,made.error);
    const auto instanceId=made.instanceId;const auto objectId=made.rootObjectIds.front();check(!instances.find_object(objectId)->materialBake,"plain instance has no recipe");
    EditorPrefabAsset newPrefab("Baked");newPrefab.templateDocument=clone_editor_document(baked);newPrefab.rootTemplateObjectIds={1};newPrefab.contentHash=editor_prefab_content_hash(newPrefab);newPrefab.assetPath="plain.dveprefab";
    auto updated=apply_editor_prefab_instance_update(instances,instanceId,newPrefab,false);check(updated.success,updated.error);
    const auto* target=instances.find_object(objectId);check(target&&target->materialBake&&target->materialBake->source.objectId==objectId&&target->materialBake->baked.objectId==objectId,"prefab update installs the recipe with remapped ownership");
    check(polygon_asset_content_hash(*target->polygon)==target->materialBake->bakedAssetHash&&!material_bake_status(*target,target->transform).stale,"prefab update leaves a current recipe");
    // Source of the template goes back to a plain mesh: the instance's recipe must be cleared too.
    EditorDocument plainAgain=clone_editor_document(baked);plainAgain.find_object(1)->materialBake.reset();
    EditorPrefabAsset third("Plain2");third.templateDocument=std::move(plainAgain);third.rootTemplateObjectIds={1};third.templateDocument.find_object(1)->materialBake.reset();third.templateDocument.find_object(1)->polygon->materials[0].roughness=.9F;third.templateDocument.find_object(1)->polygon->contentHash=polygon_asset_content_hash(*third.templateDocument.find_object(1)->polygon);third.contentHash=editor_prefab_content_hash(third);third.assetPath="plain.dveprefab";
    updated=apply_editor_prefab_instance_update(instances,instanceId,third,false);check(updated.success,updated.error);check(!instances.find_object(objectId)->materialBake,"prefab update clears a recipe the template no longer has");
}
void export_only(const std::filesystem::path& root){
    auto d=document();d.find_object(1)->modelingTopology.reset();d.find_object(1)->modelingNextId=1;
    NativeEditorController controller{EditorWorkspace(std::move(d))};controller.workspace().select_object(1);controller.material_panel().bake.resolution=32;controller.material_panel().bake.paddingPixels=2;
    check(controller.material_panel_bake(false,root/"export-only"),"export-only starts");check(controller.wait_for_long_job(30),"export-only completes");const auto* o=controller.workspace().document().find_object(1);
    check(o->materialBake&&o->materialBake->showingSource&&!o->modelingTopology&&!material_bake_status(*o,{}).stale,"export-only recipe retains source assignment");
    auto saved=controller.workspace().document().save_transactional(root/"bare.dvescene");check(saved.success,saved.error);std::string error;auto read=EditorDocument::load(root/"bare.dvescene",&error);check(read&&read->find_object(1)->materialBake.has_value(),"format 14 with empty modeling and selection sections: "+error);
}
void workflow(const std::filesystem::path& root){
    NativeEditorController first{EditorWorkspace(document())};first.workspace().select_object(1);auto& panel=first.material_panel();panel.bake.resolution=32;panel.bake.paddingPixels=2;panel.layerMode=true;
    auto* object=first.workspace().document().find_object(1);const auto original=polygon_asset_content_hash(*object->polygon);const auto topology=*object->modelingTopology;
    run_bake(first,root/"initial");object=first.workspace().document().find_object(1);check(object->materialBake&&!object->materialBake->showingSource,"apply saves persistent association");check(!material_bake_status(*object,object->transform).stale,"fresh bake");
    const auto bakedHash=polygon_asset_content_hash(*object->polygon);const auto firstSettings=object->materialBake->settings;
    EditorPrefabAsset prefab("Baked prefab");prefab.templateDocument=clone_editor_document(first.workspace().document());prefab.rootTemplateObjectIds={1};const auto prefabHash=editor_prefab_content_hash(prefab);prefab.contentHash=prefabHash;prefab.assetPath=root/"baked.dveprefab";
    auto instances=clone_editor_document(first.workspace().document());auto instance=instantiate_editor_prefab(instances,prefab);check(instance.success,instance.error);auto* instanced=instances.find_object(instance.rootObjectIds.front());check(instanced->materialBake->source.objectId==instanced->id&&!material_bake_status(*instanced,instanced->transform).stale,"prefab ownership remap retains fresh recipe");
    ownership(first.workspace().document());
    prefab.templateDocument.find_object(1)->materialBake->settings.paddingPixels=3;check(editor_prefab_content_hash(prefab)!=prefabHash,"prefab integrity includes bake authoring metadata");
    auto cloned=clone_editor_document(first.workspace().document());check(cloned.find_object(1)->materialBake.has_value(),"cloning retains recipe");
    auto saved=first.workspace().document().save_transactional(root/"scene.dvescene");check(saved.success,saved.error);
    std::filesystem::remove_all(root/"initial");std::string error;auto reopened=EditorDocument::load(root/"scene.dvescene",&error);check(bool(reopened),error);
    NativeEditorController controller{EditorWorkspace(std::move(*reopened))};auto& w=controller.workspace();w.select_object(1);controller.material_panel().layerMode=true;controller.material_panel().open=true;
    object=w.document().find_object(1);check(object->materialBake&&object->materialBake->settings.resolution==32&&!material_bake_status(*object,object->transform).stale,"reopen recipe without export directory");
    check(controller.material_panel_bake_source(true),controller.material_panel().message);object=w.document().find_object(1);check(polygon_asset_content_hash(*object->polygon)==original&&object->modelingTopology->faces==topology.faces,"restore source mesh and identities");
    check(w.commands().undo(w.document()).success&&!object->materialBake->showingSource&&polygon_asset_content_hash(*object->polygon)==bakedHash,"undo restore");check(w.commands().redo(w.document()).success,"redo restore");
    const auto keyBefore=material_bake_status(*object,object->transform).key;object->polygon->materials[0].baseColor.x=.8F;rehash(*object);check(material_bake_status(*object,object->transform).stale,"source edit invalidates bake");check(material_bake_status(*object,object->transform).key!=keyBefore,"editing the active source changes the inspection key");
    auto edited=polygon_asset_content_hash(*object->polygon);saved=w.document().save_transactional(root/"edited.dvescene");check(saved.success,saved.error);auto editing=EditorDocument::load(root/"edited.dvescene",&error);check(editing&&material_bake_status(*editing->find_object(1),{}).stale,"source edit survives restart");
    check(controller.material_panel_bake_source(false),controller.material_panel().message);check(object->materialBake->source.contentHash==edited&&material_bake_status(*object,object->transform).stale,"show baked keeps source edits");
    const auto count=w.commands().size();run_bake(controller,root/"rebaked");object=w.document().find_object(1);check(w.commands().size()==count+1&&!material_bake_status(*object,object->transform).stale,"rebake one transaction and clear stale status");check(object->materialBake->bakedSourceHash==edited,"rebake uses retained editable source");
    const auto after=polygon_asset_content_hash(*object->polygon);check(w.commands().undo(w.document()).success&&material_bake_status(*object,object->transform).stale&&polygon_asset_content_hash(*object->polygon)==bakedHash,"undo rebake restores previous output and stale source");check(w.commands().redo(w.document()).success&&polygon_asset_content_hash(*object->polygon)==after,"redo rebake");
    check(controller.material_panel_layer_action("bakeSamples"),"edit saved settings");check(material_bake_status(*object,object->transform).stale&&object->materialBake->settings.samplesPerAxis!=firstSettings.samplesPerAxis,"settings dirty");check(w.commands().undo(w.document()).success&&!material_bake_status(*object,object->transform).stale,"undo recipe edit");
    object->transform.position.x=2;check(material_bake_status(*object,object->transform).stale,"pose invalidation");object->transform.position.x=0;
    auto changed=*object->materialBake;changed.source.images.push_back({"Dependency","image/raw",1,1,{255,0,0,255}});changed.source.contentHash=polygon_asset_content_hash(changed.source);auto originalRecord=*object->materialBake;object->materialBake=changed;check(material_bake_status(*object,{}).stale,"embedded image dependency invalidation");object->materialBake=originalRecord;
    object->polygon->materials.back().roughness=.1F;rehash(*object);check(material_bake_status(*object,{}).stale&&!controller.material_panel_bake_source(true),"edited baked data is protected during switching");object->polygon=object->materialBake->baked;
    inspection(*object,root);
    auto good=*object->materialBake;check(write_material_bake_recipe(root/"record",good,&error),error);auto loaded=read_material_bake_recipe(root/"record",&error);check(loaded&&material_bake_recipe_hash(*loaded)==material_bake_recipe_hash(good),"standalone recipe round trip");
    {std::ofstream broken(root/"record"/"recipe.txt",std::ios::binary|std::ios::app);broken<<"garbage";}check(!read_material_bake_recipe(root/"record",&error),"trailing malformed metadata rejected");
    saved=w.document().save_transactional(root/"final.dvescene");check(saved.success,saved.error);auto final=EditorDocument::load(root/"final.dvescene",&error);check(bool(final),error);check(!material_bake_status(*final->find_object(1),{}).stale,"rebaked scene round trip");
    auto repeated=bake_material_textures(object->materialBake->source,object->materialBake->bakedSettings);check(repeated.success,repeated.error);auto repeatedAsset=make_baked_material_asset(object->materialBake->source,repeated,&error);check(repeatedAsset&&polygon_asset_content_hash(*repeatedAsset)==after,"same persisted source and recipe rebake deterministically");
    check(controller.material_panel_bake(true,root/"stale-recipe"),"start recipe mutation test");const auto samples=object->materialBake->settings.samplesPerAxis;object->materialBake->settings.samplesPerAxis=samples%4+1;w.document().mark_dirty();check(controller.wait_for_long_job(30),"finish stale recipe job");check(polygon_asset_content_hash(*object->polygon)==after&&material_bake_status(*object,{}).stale,"recipe changes reject stale async application");object->materialBake->settings.samplesPerAxis=samples;
    EditorMaterialLibrary materials;auto exported=export_editor_scene(w.document(),materials,root/"runtime"/"scene.dvoxscene.json");check(exported.success,exported.error);check(exported.objects.size()==1,"runtime exports assigned mesh only");for(const auto& entry:std::filesystem::recursive_directory_iterator(root/"runtime"))check(entry.path().filename()!="recipe.txt"&&entry.path().filename()!="source.dmesh", "authoring snapshots are not shipped");
    check(controller.material_panel_bake(true,root/"cancelled"),"start cancellation");check(controller.cancel_long_job(),"cancel rebake");check(polygon_asset_content_hash(*object->polygon)==after,"cancel leaves prior result");
}
}
int main(){const auto root=std::filesystem::temp_directory_path()/("dve-batch8-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));try{std::filesystem::create_directories(root);export_only(root);workflow(root);std::cout<<"Batch 8 bake workflow: PASS; previews "<<root/"inspection.ppm"<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
