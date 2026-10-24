#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/editor_subdivision.hpp"
#include "dve/render/layered_surface.hpp"
#include "editor_native_long_job.hpp"
#include <chrono>
#include <limits>
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
        if(!materialPanel_.advanced && unsigned(c.property)>=unsigned(MaterialPresetProperty::DetailSpace) && m.surfaceDetail.pattern==SurfacePattern::NoPattern)continue;
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
    if(materialPanel_.layerMode){for(auto id:workspace_.selected_objects())if(const auto* object=workspace_.document().find_object(id);object&&object->polygon){materialPanel_.preview->request(*object->polygon,materialPanel_.slot,materialPanel_.shape);return;}}
    materialPanel_.preview->request(materialPanel_.draft,materialPanel_.shape);
}
void NativeEditorController::material_panel_tick(){
    auto& p=materialPanel_;if(!p.thumbnails)p.thumbnails=std::make_unique<MaterialPreviewWorker>();
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
        if(action=="weight"||action=="maskScale"||action=="maskBias"||action=="heightStrength"||action=="heightBias"||action=="heightTransition"||action=="normalScale"||action=="sourceScale"||action=="sourceOffsetX"||action=="sourceOffsetY"||action=="sourceRotation"){
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
    const auto& p=materialPanel_;
    std::vector<std::pair<std::string,std::string>> rows{
        {"bakeUv","Bake target: UV"+std::to_string(p.bake.targetUv)},
        {"bakeResolution","Bake size: "+std::to_string(p.bake.resolution)},
        {"bakeSamples","Bake samples/axis: "+std::to_string(p.bake.samplesPerAxis)},
        {"bakePadding","Bake padding: "+std::to_string(p.bake.paddingPixels)+" px"},
        {"bakeOpacity","Separate opacity: "+std::string((p.bake.maps&16)?"on":"off")},
        {"bakeValidate","Validate bake target"},{"bakeExport","Bake / export new bundle"},{"bakeApply","Bake / export / apply copy"},
        {"add","Add selected preset"},{"recipeMetal","Recipe: paint / rust / metal"},{"recipeWood","Recipe: worn painted wood"},{"recipePlaster","Recipe: dirty / scorched plaster"}};
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
    if(action=="bakeUv"){p.bake.targetUv=1-p.bake.targetUv;return true;}
    if(action=="bakeResolution"){p.bake.resolution=p.bake.resolution>=2048?256:p.bake.resolution*2;return true;}
    if(action=="bakeSamples"){p.bake.samplesPerAxis=p.bake.samplesPerAxis%4+1;return true;}
    if(action=="bakePadding"){p.bake.paddingPixels=p.bake.paddingPixels>=32?2:p.bake.paddingPixels*2;return true;}
    if(action=="bakeOpacity"){p.bake.maps^=16;return true;}
    if(action=="bakeExport"||action=="bakeApply")return material_panel_bake(action=="bakeApply");
    if(action=="bakeValidate"){
#if defined(DVE_EDITOR_MATERIAL_PREVIEW_RENDERER)
        const auto targets=workspace_.selected_objects();if(targets.size()!=1)return fail("Select one static polygon object to bake");
        const auto* o=document.find_object(*targets.begin());if(!o||!o->polygon||o->subdivision||o->is_nurbs())return fail("Bake requires an evaluated static polygon mesh; apply subdivision first");
        if(const auto* mirror=find_component_by_type(std::span<const Component>(o->components),kLiveMirrorComponent);mirror&&mirror->enabled)return fail("Apply Live Mirror before baking the visible surface");
        auto settings=p.bake;settings.materialIndex=p.slot;settings.pose=document.world_transform(o->id).value_or(o->transform);
        auto source=std::make_shared<CookedPolygonAsset>(*o->polygon);auto validation=std::make_shared<render::MaterialBakeValidation>();
        p.message="Validating bake target; Esc cancels";
        return start_long_job(LongJobKind::BakeMaterial,"Validate material bake",longJobThreshold_+1,
            [source,settings,validation](LongJob& job){*validation=render::validate_material_bake(*source,settings,{&job.cancel,{}});},
            [this,validation](LongJob&){materialPanel_.message=validation->valid?"Bake preflight passed; raster/gutter checks run during baking. Budget estimate: "+std::to_string(validation->estimatedBytes/1048576)+" MiB":validation->error;set_status(materialPanel_.message,!validation->valid);return validation->valid;});
#else
        return fail("Texture baking requires a renderer-enabled editor build");
#endif
    }
    if(!std::isfinite(value))return fail("Layer values must be finite");
    const auto targets=workspace_.selected_objects();if(targets.empty())return fail("Select polygon objects first");
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
bool NativeEditorController::material_panel_bake(bool apply,const std::filesystem::path& outputDirectory){
#if !defined(DVE_EDITOR_MATERIAL_PREVIEW_RENDERER)
    (void)apply;(void)outputDirectory;
    materialPanel_.message="Texture baking requires a renderer-enabled editor build";
    set_status(materialPanel_.message,true);return false;
#else
    auto fail=[&](std::string message){materialPanel_.message=message;set_status(message,true);return false;};
    if(playSession_.active()||longJob_)return fail("Finish simulation/background edits first");
    const auto targets=workspace_.selected_objects();if(targets.size()!=1)return fail("Select one static polygon object to bake");
    const auto id=*targets.begin();const auto* object=workspace_.document().find_object(id);
    if(!object||!object->polygon||object->subdivision||object->is_nurbs())return fail("Bake requires an evaluated static polygon mesh; apply subdivision first");
    if(object->flags.locked||!object->flags.visible||is_isolated_out(id))return fail("Unlock and show the bake target first");
    if(const auto* mirror=find_component_by_type(std::span<const Component>(object->components),kLiveMirrorComponent);mirror&&mirror->enabled)return fail("Apply Live Mirror before baking the visible surface");
    if(apply){const auto* exact=find_component_by_type(std::span<const Component>(object->components),"dve.exact_brep");if(exact&&exact->enabled)return fail("Export a new mesh or remove the Exact CAD Source before applying a material bake");}
    auto settings=materialPanel_.bake;settings.materialIndex=materialPanel_.slot;
    settings.pose=workspace_.document().world_transform(id).value_or(object->transform);
    // Full UV preflight stays on the worker. The scene snapshot is immutable there.
    auto source=std::make_shared<CookedPolygonAsset>(*object->polygon);
    auto result=std::make_shared<render::MaterialTextureBakeResult>();
    auto derived=std::make_shared<std::optional<CookedPolygonAsset>>();
    std::filesystem::path directory=outputDirectory;
    if(directory.empty()){
        if(projectRoot_.empty())return fail("Open a project before exporting a bake bundle");
        const auto stamp=std::chrono::system_clock::now().time_since_epoch().count();
        directory=projectRoot_/"Materials"/"Bakes"/("object-"+std::to_string(id)+"-slot-"+std::to_string(settings.materialIndex)+"-"+std::to_string(stamp));
    }
    materialPanel_.message="Baking material; Esc cancels. Output: "+directory.string();
    return start_long_job(LongJobKind::BakeMaterial,"Bake material textures",longJobThreshold_+1,
        [source,result,derived,settings,directory,apply](LongJob& job){
            render::MaterialBakeControl control{&job.cancel,[&](float value,std::string_view phase){job.report(value*.9F,std::string(phase));}};
            *result=render::bake_material_textures(*source,settings,control);
            if(!result->success){if(!job.cancelled())job.error=result->error;return;}
            if(apply){*derived=render::make_baked_material_asset(*source,*result,&job.error);if(!*derived)return;}
            job.report(.95F,"Writing new source, maps and provenance bundle");
            // Export is independent of scene application. A stale scene never receives the copy.
            if(!render::write_material_bake_bundle(directory,*source,*result,control,&job.error))return;
            job.report(1,"Bundle exported");
        },
        [this,id,apply,source,derived,directory](LongJob&){
            if(apply){
                const auto* target=workspace_.document().find_object(id);
                if(!target||!target->polygon||polygon_asset_content_hash(*target->polygon)!=polygon_asset_content_hash(*source))return false;
                std::optional<ModelingTopology> topology=target->modelingTopology;
                if(topology){
                    std::string error;
                    if(!validate_modeling_topology(*source,nullptr,*topology,&error)){materialPanel_.message=error;set_status(error,true);return false;}
                    const auto originalSize=source->vertices.size();topology->vertices.resize((**derived).vertices.size());topology->attributes.resize((**derived).vertices.size());
                    topology->nextId=(std::max)(topology->nextId,target->modelingNextId);
                    for(std::size_t corner=0;corner<source->indices.size();++corner){
                        const auto destination=(**derived).indices[corner];if(destination<originalSize)continue;
                        if(topology->nextId==(std::numeric_limits<ModelingId>::max)()){set_status("Modeling identity space exhausted",true);return false;}
                        topology->vertices[destination]=target->modelingTopology->vertices[source->indices[corner]];
                        topology->attributes[destination]=topology->nextId++;
                    }
                    if(!validate_modeling_topology(**derived,nullptr,*topology,&error)){materialPanel_.message=error;set_status(error,true);return false;}
                }
                // Attribute splits preserve geometric/corner/face/edge identities. Reuse the
                // geometry command so prefab overrides and modeling metadata follow undo.
                auto committed=workspace_.commands().execute(workspace_.document(),std::make_unique<ReplacePolygonCommand>(id,*source,std::move(**derived),"Apply baked material copy",0,std::nullopt,false,std::move(topology)));
                if(!committed.success){materialPanel_.message=committed.message;return false;}
                materialPanel_.slot=unsigned(workspace_.document().find_object(id)->polygon->materials.size()-1);
                material_panel_preview();
            }
            materialPanel_.lastBakeDirectory=directory;
            materialPanel_.message="Bake saved: "+directory.string()+"; source.dmesh preserves the editable stack";
            set_status(materialPanel_.message);return true;
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
    constexpr std::array<const char*,4> shapes{"Sphere","Flat patch","Rounded box","Thickness wedge"};btn(l.shape,shapes[unsigned(p.shape)]);btn(l.savePreset,p.layerMode?"Saved with scene":"Save as Preset");
    text({l.shape.x,l.shape.y+30,l.shape.width*2,24},"CPU studio | "+std::to_string(controller.workspace().selection_count())+" targets");
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
    text({l.preview.x,y,l.loadSlot.x-l.preview.x-12,24},p.layerMode?"Layered Surface | Opaque / masked polygons":std::string(material_preset_support_label(cap.support))+": "+cap.reason);
    painter.text(l.panel.x+12,l.panel.y+l.panel.height-8,elide_text_to_width(painter,p.message.empty()?"Arrows: presets | Ctrl+F: favorite | [ ]: slot | Wheel: list/controls | Enter: Apply Copy":p.message,l.panel.width-24),muted);
}
}
