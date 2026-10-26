#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/editor_subdivision.hpp"
#include "dve/editor_voxel_selection.hpp"
#include "dve/editor_runtime_settings.hpp"
#include "dve/render/layered_surface.hpp"
#include "dve/voxel_displacement.hpp"
#include "dve/weathering.hpp"
#include <functional>
#include "editor_native_long_job.hpp"
#include <chrono>
#include <limits>
#include <memory>
#include <cmath>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
namespace dve::editor {
namespace {
struct ObjectMaterialChange {
    EditorObjectId id{};
    std::map<MaterialId,VoxelMaterialDefinition> beforePalette,afterPalette;
    std::optional<CookedPolygonAsset> beforeMesh,afterMesh;
    std::vector<VoxelChange> cells;
};
class ApplyMaterialPresetCommand final:public IEditorCommand {
    EditorMaterialLibrary& library_;
    MaterialId& active_;
    MaterialId beforeActive_,afterActive_;
    EditorMaterialLibrary before_,after_;
    std::vector<ObjectMaterialChange> changes_;
    std::string label_;
    CommandResult apply(EditorDocument& d,bool forward){
        for(const auto& c:changes_){auto* o=d.find_object(c.id);if(!o)return CommandResult::fail("Material target no longer exists");}
        library_=forward?after_:before_;
        active_=forward?afterActive_:beforeActive_;
        for(const auto& c:changes_){auto* o=d.find_object(c.id);o->voxelMaterials=forward?c.afterPalette:c.beforePalette;
            if(c.beforeMesh){o->polygon=forward?c.afterMesh:c.beforeMesh;o->subdivisionCache.reset();o->subdivisionCacheHash=0;}
            for(const auto& v:c.cells)o->voxels->set_voxel(v.voxel,forward?v.after:v.before);}
        d.mark_dirty();return CommandResult::ok();
    }
public:
    ApplyMaterialPresetCommand(EditorMaterialLibrary& lib,MaterialId& active,MaterialId paint,EditorMaterialLibrary after,std::vector<ObjectMaterialChange> changes,std::string label)
        :library_(lib),active_(active),beforeActive_(active),afterActive_(paint),before_(lib),after_(std::move(after)),changes_(std::move(changes)),label_(std::move(label)){}
    std::string_view label()const noexcept override{return label_;}
    CommandResult execute(EditorDocument& d)override{return apply(d,true);}
    CommandResult undo(EditorDocument& d)override{return apply(d,false);}
};
class MaterialBakeCommand final:public IEditorCommand {
    EditorObjectId id_;std::string label_;std::optional<MaterialBakeRecipe> before_,after_;
    std::unique_ptr<ReplacePolygonCommand> geometry_;
public:
    MaterialBakeCommand(const EditorObject& object,MaterialBakeRecipe after,std::string label,
        std::optional<CookedPolygonAsset> mesh={},std::optional<ModelingTopology> topology={})
        :id_(object.id),label_(std::move(label)),before_(object.materialBake),after_(std::move(after)){
        if(mesh)geometry_=std::make_unique<ReplacePolygonCommand>(id_,*object.polygon,std::move(*mesh),label_,0,std::nullopt,false,std::move(topology));
    }
    std::string_view label()const noexcept override{return label_;}
    CommandResult execute(EditorDocument& d)override{
        auto* object=d.find_object(id_);if(!object||!object->polygon)return CommandResult::fail("Bake target no longer exists");
        std::string error;if(after_&&!validate_material_bake_recipe(*after_,&error))return CommandResult::fail(error);
        auto recipe=after_;
        if(geometry_){auto result=geometry_->execute(d);if(!result.success)return result;}
        object->materialBake.swap(recipe);d.mark_dirty();return CommandResult::ok();
    }
    CommandResult undo(EditorDocument& d)override{
        auto* object=d.find_object(id_);if(!object)return CommandResult::fail("Bake target no longer exists");
        auto recipe=before_;
        if(geometry_){auto result=geometry_->undo(d);if(!result.success)return result;}
        object->materialBake.swap(recipe);d.mark_dirty();return CommandResult::ok();
    }
};
#if defined(DVE_EDITOR_MATERIAL_PREVIEW_RENDERER)
// Geometry and rig capture happen on the UI thread; workers own immutable copies.
bool capture_bake_scene(const EditorDocument& document,const EditorMaterialLibrary& materials,EditorObjectId target,
    render::MaterialBakeScene& scene,std::string& error){
    std::uint64_t bytes=0,triangles=0;
    for(const auto& [id,o]:document.objects()){
        if(id==target||!o.flags.visible)continue;
        const auto* mirror=find_component_by_type(std::span<const Component>(o.components),kLiveMirrorComponent);
        if(mirror&&mirror->enabled){error="Apply neighbouring Live Mirror before capturing lighting";return false;}
        const CookedPolygonAsset* mesh=o.materialBake&&!o.materialBake->showingSource?&o.materialBake->source:editor_surface_mesh(o);std::optional<EditorObject> converted;
        if(!mesh&&o.voxels&&o.voxels->occupied_voxel_count()>0){auto result=voxels_to_polygon(o,id,materials);if(!result.object){error=result.error;return false;}converted=std::move(result.object);mesh=converted->polygon?&*converted->polygon:nullptr;}
        if(!mesh){if(o.text3d||o.gaborVolume){error="Convert visible text/volume geometry to polygons before scene baking";return false;}continue;}
        triangles+=mesh->indices.size()/3;bytes+=mesh->vertices.size()*sizeof(PolygonVertex)+mesh->indices.size()*4;for(const auto& image:mesh->images)bytes+=image.rgba8.size()*8ULL;
        if(scene.size()>=4096||triangles>250000||bytes>256ULL*1024*1024){error="Neighbour scene capture exceeds 250,000 triangles or 256 MiB";return false;}
        const auto scale=converted?converted->voxelScale:o.voxelScale;
        if(scale.x<=0||scale.y<=0||scale.z<=0){error="Scene bake requires positive object scales";return false;}
        scene.push_back({render::material_bake_scaled_mesh(*mesh,scale),document.world_transform(id).value_or(o.transform)});
    }return true;
}
RenderEnvironment capture_bake_lights(EditorWorkspace& workspace,unsigned bounces){
    auto e=render_environment_settings(workspace.settings());e.pathTraceBounces=bounces;e.pathTraceSamples=1;e.localLightSamples=(std::min)(16U,e.localLightSamples);e.lightingTimeSeconds=0;
    for(const auto& [id,o]:workspace.document().objects())if(o.flags.visible)for(const auto& c:o.components)if(c.enabled&&c.type==kSceneLightComponent){if(auto light=scene_light_from_component(c,workspace.document().world_transform(id).value_or(o.transform)))e.lights.push_back(*light);else e.sunIntensity=std::numeric_limits<float>::quiet_NaN();}
    return e;
}
std::optional<ModelingTopology> bake_topology(const CookedPolygonAsset& source,const CookedPolygonAsset& derived,
    const std::optional<ModelingTopology>& original,ModelingId floor,std::string* error){
    if(!original)return {};
    if(!validate_modeling_topology(source,nullptr,*original,error))return {};
    auto topology=*original;topology.vertices.resize(derived.vertices.size());topology.attributes.resize(derived.vertices.size());topology.nextId=(std::max)(topology.nextId,floor);
    for(std::size_t corner=0;corner<source.indices.size();++corner){auto destination=derived.indices[corner];if(destination<source.vertices.size())continue;
        if(topology.nextId==(std::numeric_limits<ModelingId>::max)()){if(error)*error="Modeling identity space exhausted";return {};}
        topology.vertices[destination]=original->vertices[source.indices[corner]];topology.attributes[destination]=topology.nextId++;
    }
    if(!validate_modeling_topology(derived,nullptr,topology,error))return {};
    return topology;
}
#endif
std::filesystem::path preference_path(const std::filesystem::path& settings){return settings.empty()?std::filesystem::path{}:std::filesystem::path(settings.string()+".material-browser");}
// A slot edit can break another material's stack that uses the slot as an overlay source
// (for example by turning the source into a nested stack). Reject any stack that was valid
// before the edit and is not valid after it; already-invalid imported stacks do not block edits.
bool dependent_stacks_valid(const CookedPolygonAsset& before,const CookedPolygonAsset& after,std::string* error){
    for(std::size_t i=0;i<after.materials.size();++i){
        if(after.materials[i].layers.empty())continue;
        std::string reason;if(render::validate_layered_surface(after,std::uint32_t(i),&reason))continue;
        if(i<before.materials.size()&&before.materials.size()==before.materialBindings.size()&&!render::validate_layered_surface(before,std::uint32_t(i),nullptr))continue;
        if(error)*error="Material slot "+std::to_string(i)+" layer stack would become invalid: "+reason;
        return false;
    }
    return true;
}
std::string value_text(float v){std::ostringstream s;s<<std::setprecision(6)<<v;return s.str();}
}
MaterialPanelLayout NativeEditorController::material_panel_layout()const{
    MaterialPanelLayout l;const int w=(std::max)(300,(std::min)(1100,width_-32)),h=(std::max)(240,(std::min)(650,height_-64));
    l.panel={(width_-w)/2,(height_-h)/2,w,h};const int x=l.panel.x,y=l.panel.y;
    l.close={x+w-34,y+8,24,24};l.search={x+12,y+38,w-24,26};
    const int bw=(w-30)/5;
    l.family={x+12,y+70,bw,26};l.favorites={x+15+bw,y+70,bw,26};l.recent={x+18+2*bw,y+70,bw,26};l.backend={x+21+3*bw,y+70,bw,26};l.supported={x+24+4*bw,y+70,bw-6,26};
    const int left=(std::max)(100,w*28/100),right=(std::max)(130,w*32/100),center=w-left-right-40;
    const int count=(std::max)(1,(h-202)/32);
    for(int i=0;i<count;++i)l.rows.push_back({x+12,y+104+i*32,left,30});
    l.previous={x+12,y+h-90,44,24};l.next={x+60,y+h-90,44,24};
    const int size=(std::max)(32,(std::min)(center,h-330));l.preview={x+left+22,y+112,size,size};
    l.shape={l.preview.x,l.preview.y+size+12,(std::max)(16,center/2-3),26};
    l.savePreset={l.preview.x+center/2+3,l.shape.y,(std::max)(16,center/2-3),26};
    const int rx=x+w-right-12;
    l.loadSlot={rx,y+104,right,26};l.advanced={rx,y+136,right/2-2,26};l.layered={rx+right/2+2,y+136,right/2-2,26};
    for(int i=0;i<(std::max)(1,(h-272)/28);++i){l.fields.push_back({rx,y+170+i*28,right-30,26});l.resets.push_back({rx+right-27,y+170+i*28,27,26});}
    l.reset={rx,y+h-90,right/2-2,24};l.update={rx+right/2+2,y+h-90,right/2-2,24};
    l.apply={x+12,y+h-56,(w-30)/2,28};l.editSlot={x+18+(w-30)/2,y+h-56,(w-30)/2,28};return l;
}
std::vector<const MaterialPresetAsset*> NativeEditorController::material_panel_entries()const{
    auto r=materialPanel_.catalog.search(materialPanel_.query,materialPanel_.family);
    std::erase_if(r,[&](const auto* a){return (materialPanel_.favoritesOnly&&!materialPanel_.favorites.contains(a->id)) ||
        (materialPanel_.recentOnly&&std::find(materialPanel_.recent.begin(),materialPanel_.recent.end(),a->id)==materialPanel_.recent.end()) ||
        (materialPanel_.supportedOnly&&material_preset_capability(*a,materialPanel_.backend).support!=MaterialPresetSupport::Supported);});
    if(materialPanel_.recentOnly)std::sort(r.begin(),r.end(),[&](auto* a,auto* b){return std::find(materialPanel_.recent.begin(),materialPanel_.recent.end(),a->id)<std::find(materialPanel_.recent.begin(),materialPanel_.recent.end(),b->id);});
    return r;
}
std::vector<MaterialPresetControl> NativeEditorController::material_panel_controls()const{
    std::vector<MaterialPresetControl> r;const auto& m=materialPanel_.draft;
    for(auto c:material_preset_controls()){
        if(c.advanced&&!materialPanel_.advanced)continue;
        const bool texturing=unsigned(c.property)>=unsigned(MaterialPresetProperty::TextureTile);
        if(!materialPanel_.advanced && !texturing && unsigned(c.property)>=unsigned(MaterialPresetProperty::DetailSpace) && m.surfaceDetail.pattern==SurfacePattern::NoPattern)continue;
        // Texture-set controls only with a texture set; displacement only with a height source.
        if(!materialPanel_.advanced && texturing){
            const bool displacement=c.property==MaterialPresetProperty::DisplacementDepth||c.property==MaterialPresetProperty::DisplacementMode;
            if(displacement?!material_has_height_source(m):!material_has_textures(m))continue;
        }
        if(!materialPanel_.advanced){
            if(m.transmission==0&&(c.property==MaterialPresetProperty::Ior||c.property==MaterialPresetProperty::AbsorptionDistance))continue;
            if(m.clearCoat==0&&c.property==MaterialPresetProperty::CoatRoughness)continue;
        }r.push_back(c);
    }return r;
}
void NativeEditorController::material_panel_select(std::string_view id){
    if(materialPanel_.layerMode){materialPanel_.selected=std::string(id);materialPanel_.message="Preset selected for Add preset; the assigned stack is unchanged";return;}
    if(const auto* a=materialPanel_.catalog.find(id)){materialPanel_.selected=a->id;materialPanel_.draft=instantiate_material_preset(*a,{});materialPanel_.editing.reset();materialPanel_.controlFirst=0;materialPanel_.message=a->description;material_panel_preview();}
}
void NativeEditorController::material_panel_preview(){
    if(!materialPanel_.preview)materialPanel_.preview=std::make_unique<MaterialPreviewWorker>();
    if(materialPanel_.inspection){for(auto id:workspace_.selected_objects())if(const auto* object=workspace_.document().find_object(id);object&&object->polygon&&object->materialBake){
        const auto* mirror=find_component_by_type(std::span<const Component>(object->components),kLiveMirrorComponent);
        if(object->subdivision||object->is_nurbs()||(mirror&&mirror->enabled)){materialPanel_.preview->cancel();materialPanel_.message="Apply modifiers before comparing the baked surface";return;}
        const auto& recipe=*object->materialBake;auto settings=recipe.settings;settings.lightingRange=recipe.bakedSettings.lightingRange;settings.pose=workspace_.document().world_transform(id).value_or(object->transform);settings.scale=object->voxelScale;
        const auto& source=recipe.showingSource?*object->polygon:recipe.source;const auto& baked=recipe.showingSource?recipe.baked:*object->polygon;
        materialPanel_.preview->request_inspection(source,baked,settings,materialPanel_.inspectionChannel,materialPanel_.inspectionView);return;
    }}
    if(materialPanel_.layerMode){for(auto id:workspace_.selected_objects())if(const auto* object=workspace_.document().find_object(id);object&&object->polygon){materialPanel_.slot=(std::min)(materialPanel_.slot,unsigned(object->polygon->materials.size()-1));materialPanel_.preview->request(*object->polygon,materialPanel_.slot,materialPanel_.shape);return;}}
    materialPanel_.preview->request(materialPanel_.draft,materialPanel_.shape);
}
void NativeEditorController::material_panel_tick(){
    auto& p=materialPanel_;
    const auto now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if(now>=p.nextBakeStatusTime){p.nextBakeStatusTime=now+.5;bool found=false;
        if(workspace_.selected_objects().size()==1)for(auto id:workspace_.selected_objects())if(const auto* o=workspace_.document().find_object(id);o&&o->materialBake){
            found=true;p.bake=o->materialBake->settings;const auto status=material_bake_status(*o,workspace_.document().world_transform(id).value_or(o->transform));p.bakeStatus=status.reason;if(!status.stale&&(o->materialBake->settings.maps&96U))p.bakeStatus+=" (click to check scene)";
            if(p.bakeStatusObject!=id){p.bake=o->materialBake->settings;p.bakeStatusObject=id;p.slot=o->materialBake->showingSource?p.bake.materialIndex:unsigned(o->materialBake->baked.materials.size()-1);}
            if(p.inspection&&p.inspectionKey!=status.key){p.inspectionKey=status.key;material_panel_preview();}
        }
        if(!found){p.bakeStatus="No saved bake recipe";p.bakeStatusObject=0;p.inspection=false;}
    }
    if(!p.thumbnails)p.thumbnails=std::make_unique<MaterialPreviewWorker>();
    if(!p.thumbnailPending.empty())if(auto image=p.thumbnails->image()){p.thumbnailCache[p.thumbnailPending]=image;p.thumbnailPending.clear();}
    if(!p.thumbnailPending.empty())return;
    const auto entries=material_panel_entries();const auto count=material_panel_layout().rows.size();
    for(std::size_t i=p.first;i<entries.size()&&i<p.first+count;++i){const auto* a=entries[i];const auto key=a->id+"/"+std::to_string(a->revision)+"/"+std::to_string(a->thumbnailRevision);
        if(!p.thumbnailCache.contains(key)){p.thumbnailPending=key;p.thumbnails->request(a->defaults,MaterialPreviewShape::Sphere);return;}}
}
void NativeEditorController::open_material_panel(){
    if(playSession_.active()){set_status("Stop Play before editing material assignments",true);return;}
    clear_navigation_input();close_top_level_menu();close_context_menu();
    const auto customDir=(projectRoot_.empty()?userSettingsPath_.parent_path():projectRoot_)/"Materials"/"Presets";
    if(materialPanel_.catalogDirectory!=customDir){
        // User presets belong to one project: drop the previous project's entries (and their
        // thumbnails) so its IDs neither leak into nor shadow presets of the new project.
        materialPanel_.catalog=MaterialPresetCatalog::starter();materialPanel_.thumbnailCache.clear();materialPanel_.thumbnailPending.clear();
        if(materialPanel_.thumbnails)materialPanel_.thumbnails->cancel();
        materialPanel_.catalogDirectory=customDir;
        if(!materialPanel_.catalog.find(materialPanel_.selected))materialPanel_.selected="dve.glass.clear";
    }
    std::error_code ec;
    if(std::filesystem::is_directory(customDir,ec)) {unsigned count=0;for(const auto& item:std::filesystem::directory_iterator(customDir,ec)){
        if(ec||++count>256)break;
        if(item.path().extension()!=".dvepreset")continue;
        auto a=read_material_preset(item.path());if(a&&!materialPanel_.catalog.find(a->id))(void)materialPanel_.catalog.add(std::move(*a));}}

    if(!materialPanel_.preferencesLoaded){materialPanel_.preferencesLoaded=true;const auto path=preference_path(userSettingsPath_);std::ifstream in(path);std::string kind,id;unsigned count=0;
        while(count++<256&&in>>kind>>std::quoted(id))if(materialPanel_.catalog.find(id)){if(kind=="favorite")materialPanel_.favorites.insert(id);else if(kind=="recent"&&materialPanel_.recent.size()<12)materialPanel_.recent.push_back(id);}}
    if(const auto id=workspace_.selected_object())if(const auto* o=workspace_.document().find_object(*id)){
        if(o->polygon&&!o->polygon->submeshes.empty())materialPanel_.slot=o->polygon->submeshes.front().materialIndex;
        else if(o->voxels){bool found=false;for(const auto& [key,brick]:o->voxels->bricks()){(void)key;brick.occupancy().for_each_set([&](std::uint16_t at){if(!found){materialPanel_.slot=brick.material(at);found=true;}});if(found)break;}}
    }
    materialPanel_.open=true;materialPanel_.searchFocus=true;material_panel_select(materialPanel_.selected);set_status("Material presets: choose, preview, then Apply Copy; changes are undoable");
}
bool NativeEditorController::save_material_panel_preset(){
    auto& p=materialPanel_;MaterialPresetAsset a;
    if(const auto* source=p.catalog.find(p.selected))a=*source;
    a.defaults=p.draft;a.defaults.preset={};
    auto identity=a.defaults;identity.name.clear();
    const auto key=material_preview_fixture(identity,MaterialPreviewShape::Sphere).contentHash;
    a.id="user.material-"+std::to_string(key);a.name=p.draft.name.starts_with("Custom ")?p.draft.name:"Custom "+p.draft.name;a.family="User";
    a.tags="custom user "+p.draft.name;a.revision=1;a.defaults.name=a.name;
    a.requirements={p.draft.transmission>0?MaterialPresetFeature::SolidTransmission:MaterialPresetFeature::Opaque};
    if(p.draft.transmission>0&&p.draft.roughness>0)a.requirements.push_back(MaterialPresetFeature::RoughTransmission);
    if(p.draft.surfaceDetail.pattern!=SurfacePattern::NoPattern)a.requirements.push_back(MaterialPresetFeature::SurfaceDetail);
    if(p.draft.clearCoat>0)a.requirements.push_back(MaterialPresetFeature::ClearCoat);
    if(p.draft.shadingModel==MaterialShadingModel::Subsurface)a.requirements.push_back(MaterialPresetFeature::Subsurface);
    if(material_has_sheen(p.draft))a.requirements.push_back(MaterialPresetFeature::Sheen);
    if(material_has_iridescence(p.draft))a.requirements.push_back(MaterialPresetFeature::Iridescence);
    a.dependencies.clear();if(p.draft.normalMap){a.requirements.push_back(MaterialPresetFeature::NormalDetail);a.dependencies={"embedded:normal"};}
    if(p.catalog.find(a.id)){p.message="This draft is already saved as a user preset";return true;}
    const auto base=projectRoot_.empty()?userSettingsPath_.parent_path():projectRoot_;
    if(base.empty()){p.message="Open a project before saving a reusable preset";return false;}
    const auto path=base/"Materials"/"Presets"/(a.id+".dvepreset");
    if(!write_material_preset(path,a,&p.message))return false;
    if(!p.catalog.add(a,&p.message))return false;
    material_panel_select(a.id);p.message="Saved reusable preset: "+path.generic_string();return true;
}
void NativeEditorController::save_material_panel_preferences(){
    const auto path=preference_path(userSettingsPath_);if(path.empty())return;
    std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);std::ofstream out(path);
    for(const auto& id:materialPanel_.favorites)out<<"favorite "<<std::quoted(id)<<'\n';
    for(const auto& id:materialPanel_.recent)out<<"recent "<<std::quoted(id)<<'\n';
}
void NativeEditorController::close_material_panel(){materialPanel_.open=false;materialPanel_.editing.reset();if(materialPanel_.preview)materialPanel_.preview->cancel();if(materialPanel_.thumbnails)materialPanel_.thumbnails->cancel();materialPanel_.thumbnailPending.clear();save_material_panel_preferences();}
bool NativeEditorController::load_material_panel_slot(){
    const auto primary=workspace_.selected_object();const auto* o=primary?workspace_.document().find_object(*primary):nullptr;
    const VoxelMaterialDefinition* m=nullptr;
    if(o&&o->polygon){if(materialPanel_.slot<o->polygon->materials.size())m=&o->polygon->materials[materialPanel_.slot];}
    else if(o){const auto id=MaterialId(materialPanel_.slot);if(auto it=o->voxelMaterials.find(id);it!=o->voxelMaterials.end())m=&it->second;else if(const auto* a=materials_.find(id))m=&a->definition;}
    if(!m){materialPanel_.message="Choose a valid assigned slot using [ and ]";return false;}
    materialPanel_.draft=*m;if(materialPanel_.catalog.find(m->preset.id))materialPanel_.selected=m->preset.id;
    materialPanel_.message="Editing a draft of slot "+std::to_string(materialPanel_.slot)+"; Apply to Slot changes every face/cell using that slot on selected objects";
    material_panel_preview();return true;
}
namespace {
// One Layers-row edit of a layer's rules ("rules" toggles; "rule*" sets one field and turns the
// rules on). Returns an error message, empty on success; the layer is unchanged on error.
std::string edit_layer_rule(VoxelMaterialLayer& layer,std::string_view action,float value){
    if(action=="rules"){layer.rules.enabled=!layer.rules.enabled;return {};}
    auto rules=layer.rules;rules.enabled=true;
    if(action=="ruleSlope")rules.slopeMinCos=value;
    else if(action=="ruleHeightMin")rules.heightMinMeters=value;
    else if(action=="ruleHeightMax")rules.heightMaxMeters=value;
    else if(action=="ruleOcclusion")rules.occlusionMax=value;
    else if(action=="ruleEdge")rules.edgeWidthMeters=value;
    else if(action=="ruleBaseHeight")rules.baseHeightInfluence=value;
    else if(action=="ruleNoise")rules.noiseAmount=value;
    else if(action=="ruleNoiseScale")rules.noiseScaleMeters=value;
    else if(action=="ruleSoftness")rules.softness=value;
    else return "Unknown rule control";
    if(!material_layer_rules_valid(rules))return "Rule out of range: slope in [-1,1] below its maximum, heights ordered, AO in [0,1], edge 0..100 m, bias in [-1,1], noise in [0,1], scale >= 0.0001 m, softness in [0,1]";
    layer.rules=rules;return {};
}
void add_rule_rows(const MaterialLayerRules& rules,bool voxels,const std::function<void(std::string,std::string)>& add){
    add("rules",rules.enabled?"Rules: on (where the layer goes)":"Rules: off (uniform weight)");
    if(!rules.enabled)return;
    add("ruleSlope","Faces up at least (cos): "+value_text(rules.slopeMinCos));
    add("ruleHeightMin","Above height: "+value_text(rules.heightMinMeters)+" m");
    add("ruleHeightMax","Below height: "+value_text(rules.heightMaxMeters)+" m");
    if(voxels){   // meshes have no AO or voxel edges in the rules
        add("ruleOcclusion","In hollows (AO at most): "+value_text(rules.occlusionMax));
        add("ruleEdge","Along exposed edges within: "+value_text(rules.edgeWidthMeters)+" m (0 = off)");
    }
    add("ruleBaseHeight","Base height bias: "+value_text(rules.baseHeightInfluence));
    add("ruleNoise","Noise breakup: "+value_text(rules.noiseAmount));
    add("ruleNoiseScale","Noise scale: "+value_text(rules.noiseScaleMeters)+" m");
    add("ruleSoftness","Edge softness: "+value_text(rules.softness));
}
} // namespace
std::optional<VoxelMaterialDefinition> NativeEditorController::voxel_layer_material(const EditorObject& object,MaterialId id)const{
    if(const auto it=object.voxelMaterials.find(id);it!=object.voxelMaterials.end())return it->second;
    if(const auto* entry=materials_.find(id))return entry->definition;
    return std::nullopt;
}
bool NativeEditorController::material_panel_import_map(unsigned slot,const std::filesystem::path& path){
    auto& p=materialPanel_;
    auto fail=[&](std::string error){p.message=std::move(error);set_status(p.message,true);return false;};
    if(slot>3U)return fail("Map slot must be base colour, ORM, height or emission");
    const auto absolute=(path.has_root_directory()||path.has_root_name())?path:projectRoot_/path;
    // Colour maps (base colour, emission) are sRGB; ORM and height are data.
    const std::uint8_t role=slot==0U||slot==3U?1U:2U;
    std::string error;auto image=import_voxel_texture(absolute,role,&error);
    if(!image)return fail("Could not read "+absolute.filename().string()+": "+error);
    std::shared_ptr<const VoxelNormalMap>* slots[4]={&p.draft.baseColorMap,&p.draft.ormMap,&p.draft.heightMap,&p.draft.emissiveMap};
    *slots[slot]=std::move(image);
    constexpr const char* names[4]{"Base colour","ORM","Height","Emission"};
    p.message=std::string(names[slot])+" map loaded into the draft; Apply assigns it to voxels";set_status(p.message);
    material_panel_preview();return true;
}
void NativeEditorController::material_panel_clear_maps(){
    auto& p=materialPanel_;p.draft.baseColorMap.reset();p.draft.ormMap.reset();p.draft.heightMap.reset();p.draft.emissiveMap.reset();
    p.message="Texture-set maps cleared from the draft";set_status(p.message);material_panel_preview();
}
bool NativeEditorController::weather_selection(WeatheringKind kind,float amount){
    auto& p=materialPanel_;auto& d=workspace_.document();
    auto fail=[&](std::string error){p.message=std::move(error);set_status(p.message,true);return false;};
    if(playSession_.active()||longJob_!=nullptr)return fail("Finish the active simulation or background edit before weathering");
    if(!(amount>=0.0F&&amount<=1.0F))return fail("Weathering amount must be in [0,1]");
    const auto targets=workspace_.selected_objects();if(targets.empty())return fail("Select objects or a voxel region to weather");
    std::array<bool,256> reserved{};reserved[0]=true;
    for(const auto& e:materials_.entries())reserved[e.id]=true;
    for(const auto& [id,o]:d.objects()){(void)id;for(const auto& [slot,m]:o.voxelMaterials){(void)m;reserved[slot]=true;}
        if(o.voxels)for(const auto& [key,brick]:o.voxels->bricks()){(void)key;brick.occupancy().for_each_set([&](std::uint16_t at){reserved[brick.material(at)]=true;});}}
    auto allocate=[&]()->std::optional<MaterialId>{for(unsigned i=1;i<256;++i)if(!reserved[i]){reserved[i]=true;return MaterialId(i);}return {};};
    std::vector<ObjectMaterialChange> changes;
    for(const auto id:targets){const auto* o=d.find_object(id);if(!o)return fail("Weathering target missing");
        if(o->flags.locked||!o->flags.visible||is_isolated_out(id))return fail(o->name+": unlock and show this target before weathering");
        const auto seed=static_cast<std::uint32_t>(id*31U+unsigned(kind)+1U);
        WeatheringLayer recipe=make_weathering_layer(kind,amount,{0.0F,1.0F,0.0F},seed);
        ObjectMaterialChange c;c.id=id;c.beforePalette=o->voxelMaterials;c.afterPalette=c.beforePalette;
        if(o->polygon){
            auto mesh=*o->polygon;const auto slot=o->is_nurbs()?0U:p.slot;
            if(slot>=mesh.materials.size())return fail(o->name+": material slot "+std::to_string(slot)+" does not exist");
            if(mesh.materials[slot].layers.size()>=kMaximumVoxelMaterialLayers||mesh.materials.size()>=256)return fail(o->name+": maximum four overlays and 256 material slots");
            while(mesh.materialBindings[slot].layers.size()<mesh.materials[slot].layers.size()){PolygonMaterialLayerBinding legacy;legacy.heightBlendStrength=0;mesh.materialBindings[slot].layers.push_back(legacy);}
            recipe.layer.sourceMaterial=MaterialId(mesh.materials.size());
            mesh.materials.push_back(recipe.source);mesh.materialBindings.emplace_back();
            mesh.materials[slot].layers.push_back(recipe.layer);
            PolygonMaterialLayerBinding binding;binding.heightBlendStrength=0;mesh.materialBindings[slot].layers.push_back(binding);
            {std::string stackError;if(!dependent_stacks_valid(*o->polygon,mesh,&stackError))return fail(o->name+": "+stackError);}
            mesh.contentHash=polygon_asset_content_hash(mesh);const auto valid=validate_polygon_asset(mesh);if(!valid)return fail(valid.message);
            c.beforeMesh=*o->polygon;c.afterMesh=std::move(mesh);
        }else if(o->voxels){
            const auto cells=selectionDomain_==SelectionDomain::Voxels&&voxelSelectionObject_==id?voxelSelection_:all_occupied_voxels(*o->voxels);
            std::set<MaterialId> used;for(const auto cell:cells)if(const auto m=o->voxels->material_at(cell);m!=0)used.insert(m);
            if(used.empty())return fail(o->name+": no occupied target cells");
            const auto source=allocate();if(!source)return fail("Material palette full; free a slot before weathering");
            recipe.layer.sourceMaterial=*source;c.afterPalette[*source]=recipe.source;
            for(const auto old:used){
                VoxelMaterialDefinition definition;
                if(const auto it=o->voxelMaterials.find(old);it!=o->voxelMaterials.end())definition=it->second;
                else if(const auto* entry=materials_.find(old))definition=entry->definition;
                else return fail(o->name+": material "+std::to_string(old)+" has no definition");
                if(definition.layers.size()>=kMaximumVoxelMaterialLayers)return fail(o->name+": "+definition.name+" already has four layers");
                definition.layers.push_back(recipe.layer);c.afterPalette[old]=std::move(definition);
            }
        }else return fail(o->name+": weathering supports voxel and polygon objects");
        changes.push_back(std::move(c));
    }
    auto result=workspace_.commands().execute(d,std::make_unique<ApplyMaterialPresetCommand>(materials_,activeMaterial_,activeMaterial_,materials_,std::move(changes),
        std::string("Add weathering: ")+weathering_kind_label(kind)));
    if(!result.success)return fail(result.message);
    p.message=std::string("Added ")+weathering_kind_label(kind)+" at "+value_text(amount)+" to "+std::to_string(targets.size())+" objects; one Undo removes it";
    set_status(p.message);if(p.layerMode)material_panel_preview();return true;
}
bool NativeEditorController::apply_material_panel(bool editSlot){
    auto& p=materialPanel_;auto& d=workspace_.document();
    if(p.layerMode){p.message="Layer actions edit assigned slots immediately; use their controls or leave Layers mode";return false;}
    if(!p.draft.layers.empty()){p.message="Edit this assigned stack in Layers mode; applying a leaf preset would replace its maps";return false;}
    auto fail=[&](std::string error){p.message=std::move(error);set_status(p.message,true);return false;};
    if(playSession_.active() || longJob_ != nullptr)return fail("Finish the active simulation or background edit before applying materials");
    if(p.slot>255)return fail("Material slot must be in [0,255]");
    if(p.editing&&!material_panel_key("enter",false,false,false))return false;
    EditorMaterialLibrary validation;
    std::string draftError;
    if(!validation.upsert({10,p.draft,false},&draftError))return fail(draftError);
    // Derive requirements from edited values rather than the name of the selected preset.
    MaterialPresetAsset requested;requested.defaults=p.draft;
    if(p.draft.surfaceDetail.pattern!=SurfacePattern::NoPattern)requested.requirements.push_back(MaterialPresetFeature::SurfaceDetail);
    if(p.draft.transmission>0)requested.requirements.push_back(MaterialPresetFeature::SolidTransmission);
    const auto capability=material_preset_capability(requested,p.backend);
    if(capability.support==MaterialPresetSupport::Unavailable)return fail(capability.reason);
    const auto targets=workspace_.selected_objects();if(targets.empty())return fail("Select objects or a face/voxel region before Apply");
    EditorMaterialLibrary after=materials_;std::array<bool,256> reserved{};reserved[0]=true;
    for(const auto& e:materials_.entries())reserved[e.id]=true;
    for(const auto& [id,o]:d.objects()) { (void)id;for(const auto& [slot,m]:o.voxelMaterials){(void)m;reserved[slot]=true;}
        if(o.voxels)for(const auto& [key,brick]:o.voxels->bricks()){(void)key;brick.occupancy().for_each_set([&](std::uint16_t at){reserved[brick.material(at)]=true;});}}
    auto allocate=[&]()->std::optional<MaterialId>{for(unsigned i=1;i<256;++i)if(!reserved[i]){reserved[i]=true;return MaterialId(i);}return {};};
    auto withPhysical=[&](const VoxelMaterialDefinition& old){auto m=p.draft;m.densityKilogramsPerCubicMeter=old.densityKilogramsPerCubicMeter;m.structuralStrength=old.structuralStrength;m.fractureResistance=old.fractureResistance;m.flammability=old.flammability;m.thermalConductivity=old.thermalConductivity;m.structural=old.structural;return m;};
    std::vector<ObjectMaterialChange> changes;std::string error;MaterialId paint=activeMaterial_;
    for(const auto id:targets){const auto* o=d.find_object(id);if(!o)return fail("Material target missing");
        if(o->flags.locked||!o->flags.visible||is_isolated_out(id))return fail(o->name+": unlock and show this target before Apply");
        if(o->text3d||o->gaborVolume)return fail(o->name+": this material workflow supports voxels and polygon/subdivision/NURBS surfaces");
        ObjectMaterialChange c;c.id=id;c.beforePalette=o->voxelMaterials;c.afterPalette=c.beforePalette;
        if(o->polygon&&material_has_textures(p.draft))return fail(o->name+": texture-set maps apply to voxel materials; meshes use their own texture bindings");
        if(o->polygon){c.beforeMesh=*o->polygon;auto mesh=*o->polygon;
            if(editSlot||o->is_nurbs()){
                const auto slot=o->is_nurbs()?0:p.slot;if(slot>=mesh.materials.size())return fail(o->name+": slot does not exist");
                // The draft can predate a stack edit; never silently drop an assigned stack and its maps.
                if(!mesh.materials[slot].layers.empty())return fail(o->name+": slot "+std::to_string(slot)+" has a layer stack; edit it in Layers mode");
                mesh.materials[slot]=withPhysical(mesh.materials[slot]);mesh.materialBindings[slot]={};
            }else{
                if(mesh.materials.size()>=256)return fail(o->name+": all material slots are occupied");
                PolygonSelection sel{id,PolygonSelectionMode::Face,{}};
                if(selectionDomain_==SelectionDomain::Polygons&&polygonSelection_.object==id){
                    if(polygonSelection_.components.empty())return fail("Select polygon faces before applying");
                    sel=convert_polygon_selection(mesh,polygonSelection_,PolygonSelectionMode::Face);
                }else for(std::size_t i=0;i<mesh.indices.size()/3;++i)sel.components.insert(i);
                if(sel.components.empty())return fail(o->name+": no faces to assign");
                // Preserve physical profiles independently for faces that used different slots.
                std::map<unsigned,std::set<std::uint64_t>> groups;
                for(const auto& sub:mesh.submeshes)for(unsigned i=sub.firstIndex/3;i<(sub.firstIndex+sub.indexCount)/3;++i)if(sel.components.contains(i))groups[sub.materialIndex].insert(i);
                for(const auto& [old,faces]:groups){if(mesh.materials.size()>=256)return fail(o->name+": not enough material slots");
                    const auto next=unsigned(mesh.materials.size());mesh.materials.push_back(withPhysical(mesh.materials[old]));mesh.materialBindings.emplace_back();
                    mesh.contentHash=polygon_asset_content_hash(mesh);
                    PolygonEditSettings settings;settings.material=next;auto result=edit_polygon(mesh,{id,PolygonSelectionMode::Face,faces},PolygonEdit::AssignMaterial,settings);
                    if(!result)return fail(result.error);
                    mesh=std::move(result.mesh);}
            }
            {std::string stackError;if(!dependent_stacks_valid(*o->polygon,mesh,&stackError))return fail(o->name+": "+stackError);}
            mesh.contentHash=polygon_asset_content_hash(mesh);const auto valid=validate_polygon_asset(mesh);if(!valid)return fail(valid.message);c.afterMesh=std::move(mesh);
        }else{
            if(!o->voxels)return fail(o->name+": no assignable surface");
            if(p.draft.surfaceDetail.pattern!=SurfacePattern::NoPattern&&unsigned(p.draft.surfaceDetail.space)>=unsigned(SurfaceDetailSpace::UV0))return fail(o->name+": voxel surfaces have no UV set; choose object or world mapping");
            std::map<MaterialId,MaterialId> remap;
            auto cells=selectionDomain_==SelectionDomain::Voxels&&voxelSelectionObject_==id?voxelSelection_:all_occupied_voxels(*o->voxels);
            if(cells.empty())return fail(o->name+": no occupied target cells");
            if(cells.size()>kMaxVoxelSelection)return fail(o->name+": apply in regions of at most 500,000 cells");
            for(const auto cell:cells){const auto old=o->voxels->material_at(cell);if(old==0||(editSlot&&old!=p.slot))continue;
                if(!remap.contains(old)){
                    VoxelMaterialDefinition physical;const auto it=o->voxelMaterials.find(old);if(it!=o->voxelMaterials.end())physical=it->second;else if(const auto* entry=materials_.find(old))physical=entry->definition;
                    const auto next=editSlot?std::optional<MaterialId>(old):allocate();if(!next)return fail("Material palette full; reuse an assigned slot");
                    auto m=withPhysical(physical);c.afterPalette[*next]=m;remap[old]=*next;
                    // Owned slot edits stay local; new copies also become usable paint materials.
                    if(!editSlot&&!after.upsert({*next,m,false},&error))return fail(error);
                    if(!editSlot)paint=*next;
                }
                if(!editSlot)c.cells.push_back({cell,old,remap[old]});
            }
            if(remap.empty())return fail(o->name+": selected cells do not use this slot");
        }
        changes.push_back(std::move(c));
    }
    auto result=workspace_.commands().execute(d,std::make_unique<ApplyMaterialPresetCommand>(materials_,activeMaterial_,paint,std::move(after),std::move(changes),editSlot?"Edit assigned material slot":"Apply material preset copy"));
    if(!result.success)return fail(result.message);
    std::erase(p.recent,p.selected);p.recent.insert(p.recent.begin(),p.selected);if(p.recent.size()>12)p.recent.resize(12);save_material_panel_preferences();
    p.message="Applied to "+std::to_string(targets.size())+" objects; one Undo restores definitions, maps and assignments";set_status(p.message);return true;
}
void NativeEditorController::material_panel_text(std::string_view text){
    auto& p=materialPanel_;if(longJob_)return;if(p.editing||p.layerEditing){if(p.replaceText){p.typed.clear();p.replaceText=false;}if(p.typed.size()+text.size()<=64)p.typed+=text;}
    else if(p.searchFocus&&p.query.size()+text.size()<=128){p.query+=text;p.first=0;}
}
bool NativeEditorController::material_panel_key(std::string_view key,bool control,bool shift,bool alt){
    (void)alt;auto& p=materialPanel_;if(!p.open)return false;
    if(longJob_){if(key=="escape")return cancel_long_job();return true;}
    if(p.layerEditing){
        if(key=="escape"){p.layerEditing.reset();return true;}
        if(key=="backspace"){if(p.replaceText){p.typed.clear();p.replaceText=false;}else if(!p.typed.empty())p.typed.pop_back();return true;}
        if(key=="enter"||key=="return"||key=="tab"){
            float value{};const auto parsed=std::from_chars(p.typed.data(),p.typed.data()+p.typed.size(),value);
            if(parsed.ec!=std::errc{}||parsed.ptr!=p.typed.data()+p.typed.size()||!std::isfinite(value)){p.message="Enter a finite layer value";return false;}
            auto action=*p.layerEditing;p.layerEditing.reset();return material_panel_layer_action(action,value);
        }
        return true;
    }
    if(key=="escape"){if(p.editing)p.editing.reset();else close_material_panel();return true;}
    if(control&&(key=="z"||key=="y")){auto result=key=="z"?workspace_.commands().undo(workspace_.document()):workspace_.commands().redo(workspace_.document());p.message=result.success?"Material history updated":result.message;if(p.layerMode)material_panel_preview();return true;}
    if(p.editing){if(key=="enter"||key=="return"||key=="tab"){
            float value{};const auto parsed=std::from_chars(p.typed.data(),p.typed.data()+p.typed.size(),value);
            if(parsed.ec!=std::errc{}||parsed.ptr!=p.typed.data()+p.typed.size()){p.message="Enter a finite number in the displayed range";return false;}
            if(!set_material_preset_value(p.draft,*p.editing,value,&p.message))return false;
            p.editing.reset();p.message="Draft changed; Apply commits it";material_panel_preview();return true;}
        if(key=="backspace"){if(p.replaceText){p.typed.clear();p.replaceText=false;}else if(!p.typed.empty())p.typed.pop_back();}return true;
    }
    if(key=="["){if(p.slot>0)--p.slot;return true;}if(key=="]"){p.slot=(std::min)(255U,p.slot+1);return true;}
    if(key=="backspace"&&p.searchFocus){if(!p.query.empty())p.query.pop_back();p.first=0;return true;}
    auto entries=material_panel_entries();auto it=std::find_if(entries.begin(),entries.end(),[&](auto* a){return a->id==p.selected;});
    if(key=="up"||key=="down"){if(!entries.empty()){auto index=it==entries.end()?0:std::size_t(it-entries.begin());index=key=="up"?(index?index-1:0):(std::min)(index+1,entries.size()-1);material_panel_select(entries[index]->id);const auto count=material_panel_layout().rows.size();if(index<p.first)p.first=index;else if(index>=p.first+count)p.first=index-count+1;}return true;}
    if(key=="f"&&control){if(p.favorites.contains(p.selected))p.favorites.erase(p.selected);else p.favorites.insert(p.selected);save_material_panel_preferences();return true;}
    if(key=="tab"&&p.layerMode)return true;
    if(key=="tab"){auto fields=material_panel_controls();if(!fields.empty()){p.searchFocus=false;p.editing=fields[(std::min)(p.controlFirst,fields.size()-1)].property;p.typed=value_text(material_preset_value(p.draft,*p.editing));p.replaceText=true;}return true;}
    if(key=="enter"||key=="return")return apply_material_panel(shift);
    if(key=="pageup"||key=="pagedown"){auto count=material_panel_layout().rows.size();if(key=="pageup")p.first=p.first>count?p.first-count:0;else if(p.first+count<entries.size())p.first+=count;return true;}
    return true;
}
bool NativeEditorController::material_panel_pointer(int x,int y){
    if(longJob_){if(long_job_layout().cancelButton.contains(x,y))return cancel_long_job();return true;}
    auto& p=materialPanel_;const auto l=material_panel_layout();
    if(l.close.contains(x,y)){close_material_panel();return true;}
    if(l.search.contains(x,y)){p.editing.reset();p.searchFocus=true;return true;}
    auto entries=material_panel_entries();
    for(std::size_t i=0;i<l.rows.size();++i)if(l.rows[i].contains(x,y)&&p.first+i<entries.size()){
        const auto* a=entries[p.first+i];if(x>=l.rows[i].x+l.rows[i].width-24){if(p.favorites.contains(a->id))p.favorites.erase(a->id);else p.favorites.insert(a->id);save_material_panel_preferences();}else material_panel_select(a->id);return true;}
    if(l.family.contains(x,y)){std::vector<std::string> families{""};for(const auto& a:p.catalog.entries())if(std::find(families.begin(),families.end(),a.family)==families.end())families.push_back(a.family);auto it=std::find(families.begin(),families.end(),p.family);p.family=families[(std::size_t(it-families.begin())+1)%families.size()];p.first=0;}
    else if(l.favorites.contains(x,y)){p.favoritesOnly=!p.favoritesOnly;p.first=0;}
    else if(l.recent.contains(x,y)){p.recentOnly=!p.recentOnly;p.first=0;}
    else if(l.backend.contains(x,y)){p.backend=MaterialPresetBackend((unsigned(p.backend)+1)%4);p.first=0;if(const auto* a=p.catalog.find(p.selected))p.message=material_preset_capability(*a,p.backend).reason;}
    else if(l.supported.contains(x,y)){p.supportedOnly=!p.supportedOnly;p.first=0;}
    else if(l.previous.contains(x,y))p.first=p.first>l.rows.size()?p.first-l.rows.size():0;
    else if(l.next.contains(x,y)&&p.first+l.rows.size()<entries.size())p.first+=l.rows.size();
    else if(l.shape.contains(x,y)){p.shape=MaterialPreviewShape((unsigned(p.shape)+1)%4);material_panel_preview();}
    else if(l.savePreset.contains(x,y)){if(p.layerMode){p.message="Layer stacks and maps are saved with the scene; leaf presets remain reusable";return true;}return save_material_panel_preset();}
    else if(l.apply.contains(x,y)){if(p.layerMode){auto result=workspace_.commands().undo(workspace_.document());p.message=result.success?"Layer history undone":result.message;material_panel_preview();return true;}return apply_material_panel(false);}
    else if(l.editSlot.contains(x,y)){if(p.layerMode){auto result=workspace_.commands().redo(workspace_.document());p.message=result.success?"Layer history redone":result.message;material_panel_preview();return true;}return apply_material_panel(true);}
    else if(l.loadSlot.contains(x,y)){if(x>l.loadSlot.x+l.loadSlot.width-30)p.slot=(p.slot+1)%256;else return load_material_panel_slot();}
    else if(l.layered.contains(x,y)){p.layerMode=!p.layerMode;p.controlFirst=0;p.editing.reset();p.layerEditing.reset();p.message="Layers: edit the assigned polygon slot immediately; every action has Undo";material_panel_preview();}
    else if(p.layerMode){auto controls=material_panel_layer_controls();for(std::size_t i=0;i<l.fields.size()&&p.controlFirst+i<controls.size();++i)if(l.fields[i].contains(x,y)){
        auto action=controls[p.controlFirst+i].first;
        if(action=="weight"||action=="maskScale"||action=="maskBias"||action=="heightStrength"||action=="heightBias"||action=="heightTransition"||action=="normalScale"||action=="sourceScale"||action=="sourceOffsetX"||action=="sourceOffsetY"||action=="sourceRotation"||
           action=="weatherAmount"||(action.starts_with("rule")&&action!="rules")){
            p.layerEditing=action;p.typed="";p.replaceText=true;p.searchFocus=false;p.message="Type a number, then Enter to commit one undo step";return true;
        }
        return material_panel_layer_action(action);
    }return true;}
    else if(l.advanced.contains(x,y)){p.advanced=!p.advanced;p.controlFirst=0;p.editing.reset();}
    else if(l.reset.contains(x,y)){if(const auto* a=p.catalog.find(p.draft.preset.id)){if(a->revision!=p.draft.preset.revision)p.message="Original revision unavailable; use Update explicitly";else{p.draft=instantiate_material_preset(*a,p.draft);material_panel_preview();}}}
    else if(l.update.contains(x,y)){if(const auto* a=p.catalog.find(p.draft.preset.id)){if(update_material_preset(p.draft,*a,true,&p.message)){p.message="Updated draft to preset revision "+std::to_string(a->revision)+"; kept local overrides; Apply commits";material_panel_preview();}}else p.message="Origin preset missing; saved values remain usable";}
    else{auto fields=material_panel_controls();for(std::size_t i=0;i<l.fields.size()&&p.controlFirst+i<fields.size();++i){auto c=fields[p.controlFirst+i];
            if(l.resets[i].contains(x,y)){if(reset_material_preset_property(p.draft,p.catalog,c.property,&p.message))material_panel_preview();return true;}
            if(l.fields[i].contains(x,y)){p.searchFocus=false;p.editing=c.property;p.typed=value_text(material_preset_value(p.draft,c.property));p.replaceText=true;p.message=std::string(c.help)+" ["+value_text(c.minimum)+", "+value_text(c.maximum)+"] "+std::string(c.unit);return true;}}}
    return true;
}
std::vector<std::pair<std::string,std::string>> NativeEditorController::material_panel_layer_controls()const{
    const auto& p=materialPanel_;auto bake=p.bake;
    for(auto id:workspace_.selected_objects())if(const auto* o=workspace_.document().find_object(id);o&&o->materialBake){bake=o->materialBake->settings;break;}
    std::vector<std::pair<std::string,std::string>> rows{
        {"bakeAtlas",bake.generateAtlas?"Bake UVs: generate separate UV1 atlas":"Bake UVs: use existing charts"},
        {"bakeSlots",bake.allAssignedSlots?"Bake slots: all assigned":"Bake slots: current / saved subset"},
        {"bakeDensity","Minimum texels/m: "+value_text(bake.texelsPerMeter)+" (0 = fit size)"},
        {"bakeAo",(bake.maps&32U)?"AO: on (separate visibility map)":"AO: off"},
        {"bakeLighting",(bake.maps&64U)?"Lighting: on (separate RGBM map)":"Lighting: off"},
        {"bakeRays","AO / light samples: "+std::to_string(bake.raySamples)},
        {"bakeAoDistance","AO distance: "+value_text(bake.aoDistanceMeters)+" m"},
        {"bakeBounces","Lighting bounces: "+std::to_string(bake.lighting.pathTraceBounces)},
        {"bakeRange","Lighting RGBM range: "+value_text(bake.lightingRange)},
        {"bakeQueueReport","Queue report: "+std::to_string(p.bakeQueueReport.size())+" objects"},
        {"bakeSlot","Bake source slot: "+std::to_string(bake.materialIndex)},
        {"bakeUv","Bake target: UV"+std::to_string(bake.targetUv)},
        {"bakeResolution","Bake size: "+std::to_string(bake.resolution)},
        {"bakeSamples","Bake samples/axis: "+std::to_string(bake.samplesPerAxis)},
        {"bakePadding","Bake padding: "+std::to_string(bake.paddingPixels)+" px"},
        {"bakeOpacity","Separate opacity: "+std::string((bake.maps&16)?"on":"off")},
        {"bakeValidate","Validate bake target"},{"bakeExport","Bake / export new bundle"},{"bakeApply","Bake / export / apply copy"},
        {"add","Add selected preset"},{"recipeMetal","Recipe: paint / rust / metal"},{"recipeWood","Recipe: worn painted wood"},{"recipePlaster","Recipe: dirty / scorched plaster"},
        {"weatherKind",std::string("Weathering: ")+weathering_kind_label(p.weatherKind)},{"weatherAmount","Weathering amount: "+value_text(p.weatherAmount)},{"weatherAdd","Add weathering overlay"}};
    for(auto id:workspace_.selected_objects())if(const auto* object=workspace_.document().find_object(id);object&&object->materialBake){
        rows.insert(rows.begin(),{{"bakeStatus",p.bakeStatus.empty()?"Bake recipe saved":p.bakeStatus},
            {"bakeSource","Restore / edit source"},{"bakeResult","Show baked result"},{"bakeRebake","Rebake saved recipe"},
            {"bakeInspect",p.inspection?"Inspection: on":"Inspect source / baked"},
            {"inspectChannel",std::string("Inspect: ")+material_inspection_channel_label(p.inspectionChannel)},
            {"inspectView",p.inspectionView==MaterialInspectionView::Split?"Compare: Source | Baked":p.inspectionView==MaterialInspectionView::Source?"Compare: Source":"Compare: Baked"}});break;
    }
    for(auto id:workspace_.selected_objects()){
        // Voxel objects: the overlays of palette/library material p.slot, edited in the object's palette.
        const auto* voxelObject=workspace_.document().find_object(id);
        if(!voxelObject||!voxelObject->voxels||voxelObject->polygon)continue;
        const auto material=voxel_layer_material(*voxelObject,MaterialId(p.slot));
        if(!material||material->layers.empty())return rows;
        const auto at=(std::min)(p.layerIndex,material->layers.size()-1);const auto& layer=material->layers[at];
        const auto source=voxelObject->voxelMaterials.contains(layer.sourceMaterial)?voxelObject->voxelMaterials.at(layer.sourceMaterial).name:
            materials_.find(layer.sourceMaterial)?materials_.find(layer.sourceMaterial)->definition.name:"missing source";
        rows.insert(rows.begin(),{"select","Overlay "+std::to_string(at+1)+"/"+std::to_string(material->layers.size())+" of material "+std::to_string(p.slot)+": "+source});
        auto add=[&](std::string key,std::string text){rows.emplace_back(std::move(key),std::move(text));};
        add("toggle",layer.enabled?"Mute overlay":"Enable overlay");add("remove","Remove overlay");
        add("weight","Weight: "+value_text(layer.weight));
        constexpr const char* blends[]{"Lerp","Multiply","Additive"};add("blend",std::string("Color blend: ")+blends[unsigned(layer.blendMode)%3]);
        add_rule_rows(layer.rules,true,add);
        return rows;
    }
    for(auto id:workspace_.selected_objects()){
        const auto* object=workspace_.document().find_object(id);
        if(!object||!object->polygon||p.slot>=object->polygon->materials.size())continue;
        const auto& mesh=*object->polygon;const auto& material=mesh.materials[p.slot];
        if(material.layers.empty())return rows;
        const auto at=(std::min)(p.layerIndex,material.layers.size()-1);const auto& layer=material.layers[at];
        const auto binding=at<mesh.materialBindings[p.slot].layers.size()?mesh.materialBindings[p.slot].layers[at]:PolygonMaterialLayerBinding{};
        const auto name=layer.sourceMaterial<mesh.materials.size()?mesh.materials[layer.sourceMaterial].name:"missing source";
        rows.insert(rows.begin(),{"select","Overlay "+std::to_string(at+1)+"/"+std::to_string(material.layers.size())+": "+name});
        auto add=[&](std::string key,std::string text){rows.emplace_back(std::move(key),std::move(text));};
        add("toggle",layer.enabled?"Mute overlay":"Enable overlay");add("solo","Solo overlay (Undo restores)");
        add("up","Move toward base");add("down","Move toward top");add("duplicate","Duplicate overlay");add("remove","Remove overlay");
        add("weight","Weight: "+value_text(layer.weight));add("source","Cycle source slot: "+std::to_string(layer.sourceMaterial));
        constexpr const char* blends[]{"Lerp","Multiply","Additive"};add("blend",std::string("Color blend: ")+blends[unsigned(layer.blendMode)%3]);
        add("semantic",std::string("Role: ")+material_layer_semantic_label(binding.semantic));
        add("mask","Mask map: "+(binding.mask.texture?std::to_string(*binding.mask.texture):"none"));add("maskUv","Mask UV: "+std::to_string(binding.mask.texcoord));
        add("maskScale","Mask scale: "+value_text(binding.maskScale));add("maskBias","Mask bias: "+value_text(binding.maskBias));
        add("height","Height override: "+(binding.height.texture?std::to_string(*binding.height.texture):"source"));add("heightUv","Height UV: "+std::to_string(binding.height.texcoord));
        add("heightStrength","Height strength: "+value_text(binding.heightBlendStrength));add("heightBias","Height bias: "+value_text(binding.heightBlendBias));add("heightTransition","Height transition: "+value_text(binding.heightBlendTransition));
        add("opacity",std::string("Opacity: ")+material_layer_opacity_policy_label(binding.opacityPolicy));
        // Rule-driven coverage (dve/material_layer_rules.hpp); meshes have no voxel edges or AO.
        add_rule_rows(layer.rules,false,add);
        if(layer.sourceMaterial<mesh.materialBindings.size()){
            const auto& source=mesh.materialBindings[layer.sourceMaterial];
            add("normalScale","Source normal strength: "+value_text(source.normalScale));
            add("sourceMapping","Source mapping: "+std::to_string(unsigned(source.mapping.mappingMode)));
            add("sourceScale","Source UV scale: "+value_text(source.mapping.baseTransform.scale.x));
            add("sourceOffsetX","Source U offset: "+value_text(source.mapping.baseTransform.offset.x));add("sourceOffsetY","Source V offset: "+value_text(source.mapping.baseTransform.offset.y));
            add("sourceRotation","Source rotation (rad): "+value_text(source.mapping.baseTransform.rotationRadians));
        }
        break;
    }
    return rows;
}
bool NativeEditorController::material_panel_layer_action(std::string_view action,float value){
    auto& p=materialPanel_;auto& document=workspace_.document();
    auto fail=[&](std::string message){p.message=std::move(message);return false;};
    if(playSession_.active()||longJob_!=nullptr)return fail("Finish simulation/background edits first");
    if(action=="bakeStatus"){
#if defined(DVE_EDITOR_MATERIAL_PREVIEW_RENDERER)
        if(workspace_.selected_objects().size()==1){const auto id=*workspace_.selected_objects().begin();const auto* o=document.find_object(id);if(o&&o->materialBake&&(o->materialBake->settings.maps&96U)){
            render::MaterialBakeScene scene;std::string error;auto e=capture_bake_lights(workspace_,o->materialBake->settings.lighting.pathTraceBounces);
            if(!capture_bake_scene(document,materials_,id,scene,error))p.bakeStatus=error;
            else p.bakeStatus=material_bake_status(*o,document.world_transform(id).value_or(o->transform),&scene,(o->materialBake->settings.maps&64U)?&e:nullptr).reason;
        }}
#endif
        p.message=p.bakeStatus;return true;
    }
    if(action=="bakeSource"||action=="bakeResult")return material_panel_bake_source(action=="bakeSource");
    if(action=="bakeInspect"){p.inspection=!p.inspection;material_panel_preview();return true;}
    if(action=="inspectChannel"){p.inspection=true;p.inspectionChannel=MaterialInspectionChannel((unsigned(p.inspectionChannel)+1)%10);material_panel_preview();return true;}
    if(action=="inspectView"){p.inspection=true;p.inspectionView=MaterialInspectionView((unsigned(p.inspectionView)+1)%3);material_panel_preview();return true;}
    if(action=="bakeQueueReport"){p.message.clear();for(const auto& line:p.bakeQueueReport)p.message+=line+"; ";return true;}
    if(action=="bakeRebake")return material_panel_bake(true);
    if(action=="bakeAtlas"||action=="bakeSlots"||action=="bakeDensity"||action=="bakeAo"||action=="bakeLighting"||action=="bakeRays"||action=="bakeAoDistance"||action=="bakeBounces"||action=="bakeRange"||action=="bakeSlot"||action=="bakeUv"||action=="bakeResolution"||action=="bakeSamples"||action=="bakePadding"||action=="bakeOpacity"){
        const EditorObject* target=nullptr;if(workspace_.selected_objects().size()==1)target=document.find_object(*workspace_.selected_objects().begin());
        if(target&&(target->flags.locked||!target->flags.visible||is_isolated_out(target->id)))return fail("Unlock and show the bake target before editing its recipe");
        if(target&&target->materialBake)p.bake=target->materialBake->settings;
        if(action=="bakeSlot"){const auto* mesh=target&&target->materialBake?&(target->materialBake->showingSource?*target->polygon:target->materialBake->source):target&&target->polygon?&*target->polygon:nullptr;if(!mesh)return fail("Select a polygon bake source");p.bake.materialIndex=(p.bake.materialIndex+1)%unsigned(mesh->materials.size());if(!target->materialBake||target->materialBake->showingSource)p.slot=p.bake.materialIndex;}
        if(action=="bakeUv"){p.bake.targetUv=1-p.bake.targetUv;p.bake.generateAtlas=false;p.bake.texelsPerMeter=0;}
        if(action=="bakeAtlas"){p.bake.generateAtlas=!p.bake.generateAtlas;if(p.bake.generateAtlas)p.bake.targetUv=1;else p.bake.texelsPerMeter=0;}
        if(action=="bakeSlots"){p.bake.allAssignedSlots=!p.bake.allAssignedSlots;p.bake.materialSlots.clear();}
        if(action=="bakeDensity"){p.bake.generateAtlas=true;p.bake.targetUv=1;p.bake.texelsPerMeter=p.bake.texelsPerMeter==0?64:p.bake.texelsPerMeter>=512?0:p.bake.texelsPerMeter*2;}
        if(action=="bakeAo")p.bake.maps^=32U;
        if(action=="bakeLighting")p.bake.maps^=64U;
        if(action=="bakeRays")p.bake.raySamples=p.bake.raySamples>=64?4:p.bake.raySamples*2;
        if(action=="bakeAoDistance")p.bake.aoDistanceMeters=p.bake.aoDistanceMeters>=8?.25F:p.bake.aoDistanceMeters*2;
        if(action=="bakeBounces")p.bake.lighting.pathTraceBounces=(p.bake.lighting.pathTraceBounces+1)%5;
        if(action=="bakeRange")p.bake.lightingRange=p.bake.lightingRange>=256?4:p.bake.lightingRange*2;
        if(action=="bakeResolution")p.bake.resolution=p.bake.resolution>=2048?256:p.bake.resolution*2;
        if(action=="bakeSamples")p.bake.samplesPerAxis=p.bake.samplesPerAxis%4+1;
        if(action=="bakePadding")p.bake.paddingPixels=p.bake.paddingPixels>=32?2:p.bake.paddingPixels*2;
        if(action=="bakeOpacity")p.bake.maps^=16;
        if(target&&target->materialBake){auto recipe=*target->materialBake;recipe.settings=p.bake;
            auto result=workspace_.commands().execute(document,std::make_unique<MaterialBakeCommand>(*target,std::move(recipe),"Edit saved bake recipe"));if(!result.success)return fail(result.message);}
        p.nextBakeStatusTime=0;return true;
    }
    if(action=="bakeExport"||action=="bakeApply")return material_panel_bake(action=="bakeApply");
    if(action=="bakeValidate"){
#if defined(DVE_EDITOR_MATERIAL_PREVIEW_RENDERER)
        const auto targets=workspace_.selected_objects();if(targets.size()!=1)return fail("Select one static polygon object to bake");
        const auto* o=document.find_object(*targets.begin());if(!o||!o->polygon||o->subdivision||o->is_nurbs())return fail("Bake requires an evaluated static polygon mesh; apply subdivision first");
        if(const auto* mirror=find_component_by_type(std::span<const Component>(o->components),kLiveMirrorComponent);mirror&&mirror->enabled)return fail("Apply Live Mirror before baking the visible surface");
        auto settings=o->materialBake?o->materialBake->settings:p.bake;if(!o->materialBake)settings.materialIndex=p.slot;settings.pose=document.world_transform(o->id).value_or(o->transform);settings.scale=o->voxelScale;
        auto source=std::make_shared<CookedPolygonAsset>(o->materialBake&&!o->materialBake->showingSource?o->materialBake->source:*o->polygon);auto validation=std::make_shared<render::MaterialBakeValidation>();
        p.message="Validating bake target; Esc cancels";
        return start_long_job(LongJobKind::BakeMaterial,"Validate material bake",longJobThreshold_+1,
            [source,settings,validation](LongJob& job){*validation=render::validate_material_bake(*source,settings,{&job.cancel,{}});},
            [this,validation](LongJob&){materialPanel_.message=validation->valid?"Bake preflight passed; raster/gutter checks run during baking. Budget estimate: "+std::to_string(validation->estimatedBytes/1048576)+" MiB; size "+std::to_string(validation->resolution)+"; atlas use "+value_text(validation->utilization*100)+"%; min texels/m "+value_text(validation->minimumTexelsPerMeter)+(validation->warnings.empty()?"":"; "+validation->warnings.front()):validation->error;set_status(materialPanel_.message,!validation->valid);return validation->valid;});
#else
        return fail("Texture baking requires a renderer-enabled editor build");
#endif
    }
    if(action=="weatherKind"){p.weatherKind=WeatheringKind((unsigned(p.weatherKind)+1U)%6U);p.message=std::string("Weathering: ")+weathering_kind_label(p.weatherKind);return true;}
    if(action=="weatherAmount"){if(!(value>=0.0F&&value<=1.0F))return fail("Weathering amount must be in [0,1]");p.weatherAmount=value;return true;}
    if(action=="weatherAdd")return weather_selection(p.weatherKind,p.weatherAmount);
    if(!std::isfinite(value))return fail("Layer values must be finite");
    const auto targets=workspace_.selected_objects();if(targets.empty())return fail("Select polygon objects first");
    // Voxel objects: edit the overlays of material p.slot in each object's palette (one undo step).
    if(std::all_of(targets.begin(),targets.end(),[&](EditorObjectId id){const auto* o=document.find_object(id);return o&&o->voxels&&!o->polygon;})){
        if(action=="select"){++p.layerIndex;if(const auto m=voxel_layer_material(*document.find_object(*targets.begin()),MaterialId(p.slot))){const auto count=m->layers.size();p.layerIndex=count?p.layerIndex%count:0;}p.controlFirst=0;return true;}
        std::vector<ObjectMaterialChange> voxelChanges;
        for(auto id:targets){const auto* o=document.find_object(id);
            if(o->flags.locked||!o->flags.visible||is_isolated_out(id))return fail("Unlock and show every selected target first");
            auto material=voxel_layer_material(*o,MaterialId(p.slot));
            if(!material)return fail(o->name+": material "+std::to_string(p.slot)+" has no definition");
            if(material->layers.empty())return fail(o->name+": material "+std::to_string(p.slot)+" has no overlays; add weathering first");
            const auto at=(std::min)(p.layerIndex,material->layers.size()-1);auto& layer=material->layers[at];
            if(action=="toggle")layer.enabled=!layer.enabled;
            else if(action=="remove")material->layers.erase(material->layers.begin()+std::ptrdiff_t(at));
            else if(action=="weight"){if(value<0||value>1)return fail("Weight must be in [0,1]");layer.weight=value;}
            else if(action=="blend")layer.blendMode=MaterialLayerBlendMode((unsigned(layer.blendMode)+1)%3);
            else if(action.starts_with("rule")){const auto error=edit_layer_rule(layer,action,value);if(!error.empty())return fail(error);}
            else return fail("This overlay control applies to polygon slots only");
            ObjectMaterialChange c;c.id=id;c.beforePalette=o->voxelMaterials;c.afterPalette=c.beforePalette;c.afterPalette[MaterialId(p.slot)]=std::move(*material);
            voxelChanges.push_back(std::move(c));
        }
        auto result=workspace_.commands().execute(document,std::make_unique<ApplyMaterialPresetCommand>(materials_,activeMaterial_,activeMaterial_,materials_,std::move(voxelChanges),"Edit voxel overlay"));
        if(!result.success)return fail(result.message);
        p.message="Voxel overlay updated; one Undo restores it";return true;
    }
    if(action=="select"){++p.layerIndex;for(auto id:targets){const auto* o=document.find_object(id);if(o&&o->polygon&&p.slot<o->polygon->materials.size()){const auto count=o->polygon->materials[p.slot].layers.size();p.layerIndex=count?p.layerIndex%count:0;break;}}p.controlFirst=0;return true;}
    std::vector<ObjectMaterialChange> changes;
    for(auto id:targets){const auto* object=document.find_object(id);
        if(!object||!object->polygon||object->is_nurbs())return fail("Layer editing requires polygon or subdivision objects");
        if(object->flags.locked||!object->flags.visible||is_isolated_out(id))return fail("Unlock and show every selected target first");
        auto mesh=*object->polygon;if(p.slot>=mesh.materials.size())return fail("Assigned material slot does not exist");
        auto& material=mesh.materials[p.slot];auto& binding=mesh.materialBindings[p.slot];
        while(binding.layers.size()<material.layers.size()){PolygonMaterialLayerBinding legacy;legacy.heightBlendStrength=0;binding.layers.push_back(legacy);}
        if(action.starts_with("recipe")){
            if(mesh.materials.size()+3>256)return fail("Recipe needs three free material slots");
            const bool wood=action=="recipeWood",plaster=action=="recipePlaster";
            auto base=material;base.layers.clear();base.transmission=0;base.blendMode=MaterialBlendMode::Opaque;base.shadingModel=MaterialShadingModel::StandardPBR;base.normalMap.reset();base.clearCoat=0;base.transparent=false;base.preset={};
            base.baseColor=wood?Float4{.25F,.09F,.025F,1}:plaster?Float4{.72F,.7F,.66F,1}:Float4{.56F,.57F,.58F,1};base.metallic=wood||plaster?0:1;base.roughness=wood?.65F:plaster?.92F:.24F;
            base.surfaceDetail.pattern=wood?SurfacePattern::Wood:plaster?SurfacePattern::Mineral:SurfacePattern::NoPattern;
            mesh.materials[p.slot]=base;mesh.materialBindings[p.slot]={};
            for(unsigned n=0;n<3;++n){VoxelMaterialDefinition source;source.name=plaster?(n==0?"Dirt":n==1?"Scorch":"Dust"):(n==0?"Paint":n==1?"Rust":"Dirt");
                source.baseColor=plaster?(n==1?Float4{.025F,.02F,.015F,1}:Float4{.14F,.09F,.045F,1}):(n==0?Float4{.05F,.19F,.3F,1}:n==1?Float4{.28F,.065F,.018F,1}:Float4{.07F,.045F,.025F,1});
                source.roughness=n==0&&!plaster?.4F:.9F;source.surfaceDetail.pattern=n==0&&!plaster?SurfacePattern::Paint:SurfacePattern::Mineral;
                const auto sourceSlot=MaterialId(mesh.materials.size());mesh.materials.push_back(source);mesh.materialBindings.emplace_back();
                PolygonImage mask;mask.name="Original layered recipe mask";mask.mimeType="image/raw";mask.width=mask.height=32;mask.rgba8.resize(32*32*4);
                for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x){const float wave=.5F+.5F*std::sin(float(x)*float(n+1)*.37F+std::sin(float(y)*.43F));const auto byte=std::uint8_t(std::lround(std::clamp(wave,0.F,1.F)*255));const auto at=(y*32+x)*4;mask.rgba8[at]=mask.rgba8[at+1]=mask.rgba8[at+2]=byte;mask.rgba8[at+3]=255;}
                const auto image=unsigned(mesh.images.size());mesh.images.push_back(std::move(mask));const auto texture=unsigned(mesh.textures.size());mesh.textures.push_back({"Recipe coverage",image,{}});
                mesh.materials[p.slot].layers.push_back({sourceSlot,n==0?.85F:.45F,MaterialLayerBlendMode::Lerp,true});
                PolygonMaterialLayerBinding lb;lb.heightBlendStrength=0;lb.semantic=plaster?(n==1?MaterialLayerSemantic::Scorch:MaterialLayerSemantic::Dirt):(n==0?MaterialLayerSemantic::Paint:n==1?MaterialLayerSemantic::Rust:MaterialLayerSemantic::Dirt);lb.mask.texture=texture;mesh.materialBindings[p.slot].layers.push_back(lb);
            }
        }else if(action=="add"){
            if(material.layers.size()>=4||mesh.materials.size()>=256)return fail("Maximum four overlays and 256 material slots");
            const auto* preset=p.catalog.find(p.selected);if(!preset)return fail("Select an overlay preset");
            auto source=instantiate_material_preset(*preset,{});const auto slot=MaterialId(mesh.materials.size());
            material.layers.push_back({slot,1,MaterialLayerBlendMode::Lerp,true});PolygonMaterialLayerBinding lb;lb.heightBlendStrength=0;binding.layers.push_back(lb);
            mesh.materials.push_back(std::move(source));mesh.materialBindings.emplace_back();
        }else{
            if(material.layers.empty())return fail("Add an overlay first");
            const auto at=(std::min)(p.layerIndex,material.layers.size()-1);auto& layer=material.layers[at];auto& lb=binding.layers[at];
            if(action=="remove"){material.layers.erase(material.layers.begin()+std::ptrdiff_t(at));binding.layers.erase(binding.layers.begin()+std::ptrdiff_t(at));}
            else if(action=="duplicate"){if(material.layers.size()>=4)return fail("Maximum four overlays");const auto copy=layer;const auto b=lb;material.layers.push_back(copy);binding.layers.push_back(b);}
            else if(action=="up"||action=="down"){const auto other=action=="up"?(at?at-1:at):(std::min)(at+1,material.layers.size()-1);std::swap(material.layers[at],material.layers[other]);std::swap(binding.layers[at],binding.layers[other]);}
            else if(action=="toggle")layer.enabled=!layer.enabled;
            else if(action=="solo")for(std::size_t n=0;n<material.layers.size();++n)material.layers[n].enabled=n==at;
            else if(action=="weight"){if(value<0||value>1)return fail("Weight must be in [0,1]");layer.weight=value;}
            else if(action.starts_with("rule")){const auto error=edit_layer_rule(layer,action,value);if(!error.empty())return fail(error);}
            else if(action=="blend")layer.blendMode=MaterialLayerBlendMode((unsigned(layer.blendMode)+1)%3);
            else if(action=="opacity")lb.opacityPolicy=MaterialLayerOpacityPolicy((unsigned(lb.opacityPolicy)+1)%5);
            else if(action=="semantic")lb.semantic=MaterialLayerSemantic((unsigned(lb.semantic)+1)%8);
            else if(action=="source"){bool found=false;for(unsigned step=1;step<=mesh.materials.size();++step){const auto next=(unsigned(layer.sourceMaterial)+step)%mesh.materials.size();if(next!=0&&next!=p.slot&&mesh.materials[next].layers.empty()){auto candidate=mesh;candidate.materials[p.slot].layers[at].sourceMaterial=MaterialId(next);if(render::validate_layered_surface(candidate,p.slot)){layer.sourceMaterial=MaterialId(next);found=true;break;}}}if(!found)return fail("No compatible leaf source slot");}
            else if(action=="mask"||action=="height"){auto& map=action=="mask"?lb.mask:lb.height;if(mesh.textures.empty())return fail("Import a mesh with textures or start with a recipe mask");const auto next=map.texture?*map.texture+1:0;map.texture=next<mesh.textures.size()?std::optional<unsigned>(next):std::nullopt;}
            else if(action=="maskUv")lb.mask.texcoord=1-lb.mask.texcoord;
            else if(action=="heightUv")lb.height.texcoord=1-lb.height.texcoord;
            else if(action=="maskScale")lb.maskScale=value;
            else if(action=="maskBias")lb.maskBias=value;
            else if(action=="heightBias")lb.heightBlendBias=value;
            else if(action=="heightStrength"){if(value<0)return fail("Height strength must be nonnegative");lb.heightBlendStrength=value;}
            else if(action=="heightTransition"){if(value<0)return fail("Height transition must be nonnegative");lb.heightBlendTransition=value;}
            else if(action=="sourceMapping")mesh.materialBindings[layer.sourceMaterial].mapping.mappingMode=MaterialMappingMode((unsigned(mesh.materialBindings[layer.sourceMaterial].mapping.mappingMode)+1)%4);
            else if(action=="sourceScale"){if(std::abs(value)<1e-6F||std::abs(value)>10000)return fail("Source scale magnitude must be in [0.000001,10000]");mesh.materialBindings[layer.sourceMaterial].mapping.baseTransform.scale={value,value};}
            else if(action=="sourceOffsetX")mesh.materialBindings[layer.sourceMaterial].mapping.baseTransform.offset.x=value;
            else if(action=="sourceOffsetY")mesh.materialBindings[layer.sourceMaterial].mapping.baseTransform.offset.y=value;
            else if(action=="sourceRotation")mesh.materialBindings[layer.sourceMaterial].mapping.baseTransform.rotationRadians=value;
            else if(action=="normalScale"){if(value<0||value>8)return fail("Normal strength must be in [0,8]");mesh.materialBindings[layer.sourceMaterial].normalScale=value;}
            else return fail("Unknown layer edit");
        }
        std::string error;if(!render::validate_layered_surface(mesh,p.slot,&error))return fail(error);
        // The edited slot may also be an overlay source of another stack (Codex review).
        if(!dependent_stacks_valid(*object->polygon,mesh,&error))return fail(error);
        mesh.contentHash=polygon_asset_content_hash(mesh);const auto valid=validate_polygon_asset(mesh);if(!valid)return fail(valid.message);
        ObjectMaterialChange change;change.id=id;change.beforePalette=change.afterPalette=object->voxelMaterials;change.beforeMesh=*object->polygon;change.afterMesh=std::move(mesh);changes.push_back(std::move(change));
    }
    auto result=workspace_.commands().execute(document,std::make_unique<ApplyMaterialPresetCommand>(materials_,activeMaterial_,activeMaterial_,materials_,std::move(changes),"Edit layered surface: "+std::string(action)));
    if(!result.success)return fail(result.message);
    for(auto id:targets)if(const auto* object=document.find_object(id);object&&object->polygon){
        const auto count=object->polygon->materials[p.slot].layers.size();
        if(action=="up"&&p.layerIndex>0)--p.layerIndex;
        if(action=="down"&&p.layerIndex+1<count)++p.layerIndex;
        p.layerIndex=count?(std::min)(p.layerIndex,count-1):0;break;
    }
    p.message="Layer stack changed; Ctrl+Z restores the complete edit";material_panel_preview();return true;
}
bool NativeEditorController::material_panel_bake_source(bool showSource){
    auto fail=[&](std::string message){materialPanel_.message=message;set_status(message,true);return false;};
    if(playSession_.active()||longJob_)return fail("Finish simulation/background edits first");
    if(workspace_.selected_objects().size()!=1)return fail("Select one object with a saved bake recipe");
    const auto* object=workspace_.document().find_object(*workspace_.selected_objects().begin());
    if(!object||!object->polygon||!object->materialBake)return fail("No saved bake recipe on this object");
    if(object->subdivision||object->is_nurbs()||object->flags.locked||!object->flags.visible||is_isolated_out(object->id))return fail("Apply modifiers, unlock and show the target first");
    if(const auto* mirror=find_component_by_type(std::span<const Component>(object->components),kLiveMirrorComponent);mirror&&mirror->enabled)return fail("Apply Live Mirror before restoring a bake snapshot");
    auto recipe=*object->materialBake;if(recipe.showingSource==showSource)return true;
    if(recipe.showingSource){recipe.source=*object->polygon;recipe.sourceTopology=object->modelingTopology;}
    else if(polygon_asset_content_hash(*object->polygon)!=recipe.bakedAssetHash)return fail("Assigned baked mesh was edited; Undo those edits or rebake before switching");
    recipe.showingSource=showSource;auto mesh=showSource?recipe.source:recipe.baked;auto topology=showSource?recipe.sourceTopology:recipe.bakedTopology;
    const auto slot=showSource?recipe.settings.materialIndex:unsigned(recipe.baked.materials.size()-1);
    auto result=workspace_.commands().execute(workspace_.document(),std::make_unique<MaterialBakeCommand>(*object,std::move(recipe),showSource?"Restore editable bake source":"Show baked material",std::move(mesh),std::move(topology)));
    if(!result.success)return fail(result.message);
    materialPanel_.slot=slot;materialPanel_.nextBakeStatusTime=0;material_panel_preview();return true;
}
bool NativeEditorController::material_panel_bake(bool apply,const std::filesystem::path& outputDirectory){
#if !defined(DVE_EDITOR_MATERIAL_PREVIEW_RENDERER)
    (void)apply;(void)outputDirectory;materialPanel_.message="Texture baking requires a renderer-enabled editor build";set_status(materialPanel_.message,true);return false;
#else
    auto fail=[&](std::string message){materialPanel_.message=message;set_status(message,true);return false;};
    if(playSession_.active()||longJob_)return fail("Finish simulation/background edits first");
    const auto targets=workspace_.selected_objects();if(targets.empty()||targets.size()>64)return fail("Select 1..64 static polygon bake objects");
    if(outputDirectory.empty()&&projectRoot_.empty())return fail("Open a project before exporting bake bundles");
    struct Item {
        EditorObjectId id{};render::MaterialTextureBakeSettings settings;CookedPolygonAsset source;
        std::optional<ModelingTopology> topology;render::MaterialBakeScene scene;
        std::uint64_t targetHash{};std::filesystem::path directory;std::string error;
        render::MaterialTextureBakeResult result;std::optional<CookedPolygonAsset> derived;
    };
    auto items=std::make_shared<std::vector<Item>>();items->reserve(targets.size());
    const auto stamp=std::chrono::system_clock::now().time_since_epoch().count();
    std::filesystem::path root=outputDirectory.empty()?projectRoot_/"Materials"/"Bakes"/("queue-"+std::to_string(stamp)):outputDirectory;
    std::uint64_t snapshotBytes=0;
    for(auto id:targets){
        Item item;item.id=id;const auto* object=workspace_.document().find_object(id);
        if(!object||!object->polygon||object->subdivision||object->is_nurbs())item.error="Apply modifiers to a static polygon target first";
        else if(object->flags.locked||!object->flags.visible||is_isolated_out(id))item.error="Unlock and show the target first";
        else if(const auto* mirror=find_component_by_type(std::span<const Component>(object->components),kLiveMirrorComponent);mirror&&mirror->enabled)item.error="Apply Live Mirror first";
        else if(const auto* exact=find_component_by_type(std::span<const Component>(object->components),"dve.exact_brep");apply&&exact&&exact->enabled)item.error="Export a new mesh or remove the Exact CAD Source before applying";
        if(item.error.empty()){
            item.settings=object->materialBake?object->materialBake->settings:materialPanel_.bake;if(!object->materialBake)item.settings.materialIndex=materialPanel_.slot;
            auto& settings=item.settings;settings.pose=workspace_.document().world_transform(id).value_or(object->transform);settings.scale=object->voxelScale;
            if((settings.maps&64U)!=0)settings.lighting=capture_bake_lights(workspace_,settings.lighting.pathTraceBounces);
            if(!render::valid_material_bake_settings(settings))item.error="Invalid bake settings or unsupported lighting sample/bounce limits";
            const auto& source=object->materialBake&&!object->materialBake->showingSource?object->materialBake->source:*object->polygon;
            snapshotBytes+=source.vertices.size()*sizeof(PolygonVertex)+source.indices.size()*4;for(const auto& image:source.images)snapshotBytes+=image.rgba8.size()*8ULL;
            if(snapshotBytes>256ULL*1024*1024)return fail("Selected source snapshots exceed 256 MiB; split the queue");
            item.source=source;item.source.objectId=id;item.source.contentHash=polygon_asset_content_hash(item.source);item.targetHash=polygon_asset_content_hash(*object->polygon);
            item.topology=object->materialBake&&!object->materialBake->showingSource?object->materialBake->sourceTopology:object->modelingTopology;
            if((settings.maps&96U)&&item.error.empty())(void)capture_bake_scene(workspace_.document(),materials_,id,item.scene,item.error);
            // Queued recipes retain their own neighbours; cap combined snapshot ownership too.
            for(const auto& m:item.scene){snapshotBytes+=m.asset.vertices.size()*sizeof(PolygonVertex)+m.asset.indices.size()*4;for(const auto& image:m.asset.images)snapshotBytes+=image.rgba8.size()*8ULL;}
            if(snapshotBytes>512ULL*1024*1024)return fail("Queued source and lighting snapshots exceed 512 MiB; split the selection");
        }
        item.directory=targets.size()==1?root:root/("object-"+std::to_string(id));items->push_back(std::move(item));
    }
    if(items->size()==1&&!items->front().error.empty())return fail(items->front().error);
    materialPanel_.bakeQueueReport.clear();materialPanel_.message="Baking "+std::to_string(items->size())+" objects; Esc cancels. AO/light maps are separate exports.";
    return start_long_job(LongJobKind::BakeMaterial,"Bake selected materials, AO and lighting",longJobThreshold_+1,
        [items,apply,snapshotBytes](LongJob& job){
            std::uint64_t retainedBytes=0;
            for(std::size_t n=0;n<items->size();++n){auto& item=(*items)[n];if(job.cancelled())return;if(!item.error.empty())continue;
                render::MaterialBakeControl control{&job.cancel,[&](float value,std::string_view phase){job.report((float(n)+value*.9F)/float(items->size()),"Object "+std::to_string(item.id)+": "+std::string(phase));}};
                const auto preflight=render::validate_material_bake(item.source,item.settings,control);
                if(!preflight.valid){item.error=preflight.error;continue;}
                const std::uint64_t derivedEstimate=std::uint64_t(preflight.resolution)*preflight.resolution*4*(4+bool(item.settings.maps&32U)+bool(item.settings.maps&64U));
                if(snapshotBytes+retainedBytes+preflight.estimatedBytes+derivedEstimate>512ULL*1024*1024){item.error="Queue snapshots, pending results and current bake exceed 512 MiB; split the queue or reduce size";continue;}
                item.result=render::bake_material_textures(item.source,item.settings,control,item.scene);
                if(!item.result.success){item.error=item.result.error;item.result.targetUvs.clear();continue;}item.settings=item.result.settings;
                if(apply||(item.settings.maps&render::kMaterialBakeDefaultMaps)==render::kMaterialBakeDefaultMaps){item.derived=render::make_baked_material_asset(item.source,item.result,&item.error);if(!item.derived){item.result.images.clear();item.result.targetUvs.clear();continue;}}
                if(!render::write_material_bake_bundle(item.directory,item.source,item.result,control,&item.error,item.scene)){item.derived.reset();item.result.images.clear();item.result.targetUvs.clear();continue;}
                item.result.images.clear();item.result.targetUvs.clear();
                if(item.derived){retainedBytes+=item.derived->vertices.size()*sizeof(PolygonVertex)+item.derived->indices.size()*4;for(const auto& image:item.derived->images)retainedBytes+=image.rgba8.size();}
            }
            job.report(1,"Queue exports finished");
        },
        [this,items,apply](LongJob&){
            unsigned success=0;materialPanel_.bakeQueueReport.clear();
            for(auto& item:*items)if(item.error.empty()&&(item.settings.maps&96U)){
                render::MaterialBakeScene current;std::string error;
                if(!capture_bake_scene(workspace_.document(),materials_,item.id,current,error)||render::material_bake_scene_hash(current)!=render::material_bake_scene_hash(item.scene))item.error="Scene changed; exported lighting was not applied";
                if(item.error.empty()&&(item.settings.maps&64U)){auto settings=item.settings;settings.lighting=capture_bake_lights(workspace_,settings.lighting.pathTraceBounces);if(render::encode_material_bake_settings(settings)!=render::encode_material_bake_settings(item.settings))item.error="Light rig changed; exported lighting was not applied";}
            }
            for(auto& item:*items){
                auto error=item.error;const auto* target=workspace_.document().find_object(item.id);
                if(error.empty()&&(!target||!target->polygon||polygon_asset_content_hash(*target->polygon)!=item.targetHash))error="Target changed; exported bundle was not applied";
                if(error.empty()&&item.derived&&(apply||!target->materialBake)){
                    auto topology=bake_topology(item.source,*item.derived,item.topology,target->modelingNextId,&error);
                    if(item.topology&&!topology){if(error.empty())error="Could not preserve modeling identities";}
                    if(error.empty()){
                        MaterialBakeRecipe recipe;recipe.showingSource=!apply;recipe.source=item.source;recipe.baked=*item.derived;recipe.sourceTopology=item.topology;recipe.bakedTopology=topology;
                        recipe.settings=recipe.bakedSettings=item.settings;recipe.scene=item.scene;recipe.bakedSourceHash=item.result.sourceHash;recipe.bakedAssetHash=polygon_asset_content_hash(*item.derived);
                        recipe.outputDirectory=item.directory;const auto relative=item.directory.lexically_relative(projectRoot_);if(!projectRoot_.empty()&&!relative.empty()&&*relative.begin()!="..")recipe.outputDirectory=relative;
                        auto committed=workspace_.commands().execute(workspace_.document(),std::make_unique<MaterialBakeCommand>(*target,std::move(recipe),apply?"Apply baked material and recipe":"Save material bake recipe",apply?std::optional<CookedPolygonAsset>{std::move(*item.derived)}:std::nullopt,apply?std::move(topology):target->modelingTopology));
                        if(!committed.success)error=committed.message;
                    }
                }
                if(error.empty()){++success;materialPanel_.lastBakeDirectory=item.directory;materialPanel_.bake=item.settings;}
                materialPanel_.bakeQueueReport.push_back("Object "+std::to_string(item.id)+": "+(error.empty()?"saved "+item.directory.string():error));
            }
            if(items->size()==1&&success){auto* target=workspace_.document().find_object(items->front().id);materialPanel_.slot=apply?unsigned(target->polygon->materials.size()-1):items->front().settings.materialIndex;}
            materialPanel_.nextBakeStatusTime=0;materialPanel_.inspectionKey=0;material_panel_preview();
            materialPanel_.message="Bake queue: "+std::to_string(success)+" / "+std::to_string(items->size())+" saved; "+(items->size()==1?materialPanel_.bakeQueueReport.front():"open Queue report for object results. One undo step per applied object.");
            set_status(materialPanel_.message,success!=items->size());return success>0;
        });
#endif
}
void render_material_panel(const IEditorCanvas& canvas,NativeEditorController& controller){
    auto& p=controller.material_panel();if(!p.open)return;const auto l=controller.material_panel_layout();RectClipCanvas painter(canvas,l.panel);
    constexpr std::uint32_t bg=0x172133,button=0x26374d,ink=0xe2e9f2,muted=0x9eafc3,accent=0x66cddd;
    painter.fill(l.panel,bg);painter.outline(l.panel,accent);
    auto text=[&](UiRect r,std::string s){painter.text(r.x+5,r.y+17,elide_text_to_width(painter,s,r.width-10),ink);};
    auto btn=[&](UiRect r,std::string s,bool on=false){painter.fill(r,on?0x355f70:button);text(r,s);};
    painter.text(l.panel.x+12,l.panel.y+24,p.layerMode?"Layered Surface | Assigned slot | Each action is one undo step":"Material presets | Draft preview | Apply is one undo step",ink);btn(l.close,"X");
    btn(l.search,(p.searchFocus?"> ":"Search: ")+p.query);btn(l.family,p.family.empty()?"All families":p.family);
    btn(l.favorites,"Favorites",p.favoritesOnly);btn(l.recent,"Recent",p.recentOnly);
    constexpr std::array<const char*,4> backends{"CPU reference","CPU player","Native viewport","GPU"};btn(l.backend,backends[unsigned(p.backend)]);btn(l.supported,"Supported only",p.supportedOnly);
    const auto entries=controller.material_panel_entries();
    for(std::size_t i=0;i<l.rows.size()&&p.first+i<entries.size();++i){const auto* a=entries[p.first+i];const auto r=l.rows[i];
        painter.fill(r,a->id==p.selected?0x355f70:button);auto name=r;name.x+=30;name.width-=72;text(name,a->name);
        const auto key=a->id+"/"+std::to_string(a->revision)+"/"+std::to_string(a->thumbnailRevision);
        if(auto it=p.thumbnailCache.find(key);it!=p.thumbnailCache.end()&&it->second->error.empty()){const auto& image=*it->second;
            for(int ty=0;ty<24;++ty)for(int tx=0;tx<24;++tx)painter.fill({r.x+3+tx,r.y+3+ty,1,1},image.rgb[unsigned(ty)*image.size/24*image.size+unsigned(tx)*image.size/24]);}
        else painter.fill({r.x+3,r.y+3,24,24},0x525e6e);
const auto support=material_preset_capability(*a,p.backend).support;
        painter.text(r.x+r.width-38,r.y+18,support==MaterialPresetSupport::Supported?"S":support==MaterialPresetSupport::Approximate?"A":"U",support==MaterialPresetSupport::Unavailable?0xf2b086:accent);
        painter.text(r.x+r.width-20,r.y+18,p.favorites.contains(a->id)?"*":"+",accent);}
    btn(l.previous,"<");btn(l.next,">");
    if(p.preview){if(const auto image=p.preview->image()){if(!image->error.empty())text(l.preview,image->error);else for(unsigned y=0;y<image->size;++y)for(unsigned x=0;x<image->size;++x){const int x0=l.preview.x+int(x)*l.preview.width/int(image->size),y0=l.preview.y+int(y)*l.preview.height/int(image->size);const int x1=l.preview.x+int(x+1)*l.preview.width/int(image->size),y1=l.preview.y+int(y+1)*l.preview.height/int(image->size);painter.fill({x0,y0,x1-x0,y1-y0},image->rgb[y*image->size+x]);}}
        else text(l.preview,"Rendering CPU preview...");}
    constexpr std::array<const char*,4> shapes{"Sphere","Flat patch","Rounded box","Thickness wedge"};btn(l.shape,p.inspection?"Actual geometry":shapes[unsigned(p.shape)]);btn(l.savePreset,p.layerMode?"Saved with scene":"Save as Preset");
    text({l.shape.x,l.shape.y+30,l.shape.width*2,24},p.inspection?std::string(material_inspection_channel_label(p.inspectionChannel))+" | Source / Baked":("CPU studio | "+std::to_string(controller.workspace().selection_count())+" targets"));
    const auto* origin=p.catalog.find(p.draft.preset.id);
    text({l.shape.x,l.shape.y+54,l.shape.width*2,24},p.draft.preset.id.empty()?"Saved material (no preset origin)":"Origin r"+std::to_string(p.draft.preset.revision)+(origin?" | library r"+std::to_string(origin->revision):" | preset missing"));
    btn(l.loadSlot,"Load slot "+std::to_string(p.slot)+"  +");if(!p.layerMode)btn(l.advanced,p.advanced?"Advanced: on":"Advanced: off");btn(l.layered,"Layers",p.layerMode);
    if(p.layerMode){const auto controls=controller.material_panel_layer_controls();for(std::size_t i=0;i<l.fields.size()&&p.controlFirst+i<controls.size();++i){const auto& c=controls[p.controlFirst+i];auto r=l.fields[i];r.width+=30;btn(r,p.layerEditing==c.first?c.second+": "+p.typed:c.second,p.layerEditing==c.first);}}
    const auto fields=p.layerMode?std::vector<MaterialPresetControl>{}:controller.material_panel_controls();
    for(std::size_t i=0;i<l.fields.size()&&p.controlFirst+i<fields.size();++i){const auto c=fields[p.controlFirst+i];std::string value=p.editing==c.property?p.typed:value_text(material_preset_value(p.draft,c.property));
        const bool overridden=(p.draft.preset.overrides&(std::uint64_t(1)<<unsigned(c.property)))!=0;btn(l.fields[i],std::string(c.label)+(overridden?"* ":" ")+value+" "+std::string(c.unit),p.editing==c.property);btn(l.resets[i],"R");}
    if(!p.layerMode){btn(l.reset,"Reset draft");btn(l.update,"Update draft");}
    btn(l.apply,p.layerMode?"Undo stack edit (Ctrl+Z)":"Apply Copy / Make Unique");btn(l.editSlot,p.layerMode?"Redo stack edit (Ctrl+Y)":"Apply to Slot (shared faces/cells)");
    auto asset=p.catalog.find(p.draft.preset.id);MaterialPresetAsset requested;if(asset)requested=*asset;if(p.draft.surfaceDetail.pattern!=SurfacePattern::NoPattern)requested.requirements.push_back(MaterialPresetFeature::SurfaceDetail);if(p.draft.transmission>0)requested.requirements.push_back(MaterialPresetFeature::SolidTransmission);
    const auto cap=material_preset_capability(requested,p.backend);const int y=l.panel.y+l.panel.height-118;
    text({l.preview.x,y,l.loadSlot.x-l.preview.x-12,24},p.inspection?(p.inspectionChannel==MaterialInspectionChannel::UvCoverage?"Cyan: coverage | Gray: padding | Red: overlaps (schematic)":(unsigned(p.inspectionChannel)>=8?"UV output map | AO: linear | Lighting: RGBM decoded + display curve":"Fixed lighting/camera | World normals | Linear scalar channels")):(p.layerMode?"Layered Surface | Opaque / masked polygons":std::string(material_preset_support_label(cap.support))+": "+cap.reason));
    painter.text(l.panel.x+12,l.panel.y+l.panel.height-8,elide_text_to_width(painter,p.message.empty()?"Arrows: presets | Ctrl+F: favorite | [ ]: slot | Wheel: list/controls | Enter: Apply Copy":p.message,l.panel.width-24),muted);
}
}
