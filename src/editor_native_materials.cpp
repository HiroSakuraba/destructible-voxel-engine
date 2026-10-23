#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/editor_subdivision.hpp"
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
    l.loadSlot={rx,y+104,right,26};l.advanced={rx,y+136,right,26};
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
        if(!materialPanel_.advanced){
            if(m.transmission==0&&(c.property==MaterialPresetProperty::Ior||c.property==MaterialPresetProperty::AbsorptionDistance))continue;
            if(m.clearCoat==0&&c.property==MaterialPresetProperty::CoatRoughness)continue;
        }r.push_back(c);
    }return r;
}
void NativeEditorController::material_panel_select(std::string_view id){
    if(const auto* a=materialPanel_.catalog.find(id)){materialPanel_.selected=a->id;materialPanel_.draft=instantiate_material_preset(*a,{});materialPanel_.editing.reset();materialPanel_.controlFirst=0;materialPanel_.message=a->description;material_panel_preview();}
}
void NativeEditorController::material_panel_preview(){
    if(!materialPanel_.preview)materialPanel_.preview=std::make_unique<MaterialPreviewWorker>();
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
    auto fail=[&](std::string error){p.message=std::move(error);set_status(p.message,true);return false;};
    if(playSession_.active() || longJob_ != nullptr)return fail("Finish the active simulation or background edit before applying materials");
    if(p.slot>255)return fail("Material slot must be in [0,255]");
    if(p.editing&&!material_panel_key("enter",false,false,false))return false;
    EditorMaterialLibrary validation;
    std::string draftError;
    if(!validation.upsert({10,p.draft,false},&draftError))return fail(draftError);
    // Derive requirements from edited values rather than the name of the selected preset.
    MaterialPresetAsset requested;requested.defaults=p.draft;
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
            mesh.contentHash=polygon_asset_content_hash(mesh);const auto valid=validate_polygon_asset(mesh);if(!valid)return fail(valid.message);c.afterMesh=std::move(mesh);
        }else{
            if(!o->voxels)return fail(o->name+": no assignable surface");
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
    auto& p=materialPanel_;if(p.editing){if(p.replaceText){p.typed.clear();p.replaceText=false;}if(p.typed.size()+text.size()<=64)p.typed+=text;}
    else if(p.searchFocus&&p.query.size()+text.size()<=128){p.query+=text;p.first=0;}
}
bool NativeEditorController::material_panel_key(std::string_view key,bool control,bool shift,bool alt){
    (void)alt;auto& p=materialPanel_;if(!p.open)return false;
    if(key=="escape"){if(p.editing)p.editing.reset();else close_material_panel();return true;}
    if(control&&(key=="z"||key=="y")){auto result=key=="z"?workspace_.commands().undo(workspace_.document()):workspace_.commands().redo(workspace_.document());p.message=result.success?"Material history updated":result.message;return true;}
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
    if(key=="tab"){auto fields=material_panel_controls();if(!fields.empty()){p.searchFocus=false;p.editing=fields[(std::min)(p.controlFirst,fields.size()-1)].property;p.typed=value_text(material_preset_value(p.draft,*p.editing));p.replaceText=true;}return true;}
    if(key=="enter"||key=="return")return apply_material_panel(shift);
    if(key=="pageup"||key=="pagedown"){auto count=material_panel_layout().rows.size();if(key=="pageup")p.first=p.first>count?p.first-count:0;else if(p.first+count<entries.size())p.first+=count;return true;}
    return true;
}
bool NativeEditorController::material_panel_pointer(int x,int y){
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
    else if(l.savePreset.contains(x,y))return save_material_panel_preset();
    else if(l.apply.contains(x,y))return apply_material_panel(false);
    else if(l.editSlot.contains(x,y))return apply_material_panel(true);
    else if(l.loadSlot.contains(x,y)){if(x>l.loadSlot.x+l.loadSlot.width-30)p.slot=(p.slot+1)%256;else return load_material_panel_slot();}
    else if(l.advanced.contains(x,y)){p.advanced=!p.advanced;p.controlFirst=0;p.editing.reset();}
    else if(l.reset.contains(x,y)){if(const auto* a=p.catalog.find(p.draft.preset.id)){if(a->revision!=p.draft.preset.revision)p.message="Original revision unavailable; use Update explicitly";else{p.draft=instantiate_material_preset(*a,p.draft);material_panel_preview();}}}
    else if(l.update.contains(x,y)){if(const auto* a=p.catalog.find(p.draft.preset.id)){if(update_material_preset(p.draft,*a,true,&p.message)){p.message="Updated draft to preset revision "+std::to_string(a->revision)+"; kept local overrides; Apply commits";material_panel_preview();}}else p.message="Origin preset missing; saved values remain usable";}
    else{auto fields=material_panel_controls();for(std::size_t i=0;i<l.fields.size()&&p.controlFirst+i<fields.size();++i){auto c=fields[p.controlFirst+i];
            if(l.resets[i].contains(x,y)){if(reset_material_preset_property(p.draft,p.catalog,c.property,&p.message))material_panel_preview();return true;}
            if(l.fields[i].contains(x,y)){p.searchFocus=false;p.editing=c.property;p.typed=value_text(material_preset_value(p.draft,c.property));p.replaceText=true;p.message=std::string(c.help)+" ["+value_text(c.minimum)+", "+value_text(c.maximum)+"] "+std::string(c.unit);return true;}}}
    return true;
}
void render_material_panel(const IEditorCanvas& canvas,NativeEditorController& controller){
    auto& p=controller.material_panel();if(!p.open)return;const auto l=controller.material_panel_layout();RectClipCanvas painter(canvas,l.panel);
    constexpr std::uint32_t bg=0x172133,button=0x26374d,ink=0xe2e9f2,muted=0x9eafc3,accent=0x66cddd;
    painter.fill(l.panel,bg);painter.outline(l.panel,accent);
    auto text=[&](UiRect r,std::string s){painter.text(r.x+5,r.y+17,elide_text_to_width(painter,s,r.width-10),ink);};
    auto btn=[&](UiRect r,std::string s,bool on=false){painter.fill(r,on?0x355f70:button);text(r,s);};
    painter.text(l.panel.x+12,l.panel.y+24,"Material presets | Draft preview | Apply is one undo step",ink);btn(l.close,"X");
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
    constexpr std::array<const char*,4> shapes{"Sphere","Flat patch","Rounded box","Thickness wedge"};btn(l.shape,shapes[unsigned(p.shape)]);btn(l.savePreset,"Save as Preset");
    text({l.shape.x,l.shape.y+30,l.shape.width*2,24},"CPU studio | "+std::to_string(controller.workspace().selection_count())+" targets");
    const auto* origin=p.catalog.find(p.draft.preset.id);
    text({l.shape.x,l.shape.y+54,l.shape.width*2,24},p.draft.preset.id.empty()?"Saved material (no preset origin)":"Origin r"+std::to_string(p.draft.preset.revision)+(origin?" | library r"+std::to_string(origin->revision):" | preset missing"));
    btn(l.loadSlot,"Load slot "+std::to_string(p.slot)+"  +");btn(l.advanced,p.advanced?"Advanced: on":"Advanced: off");
    const auto fields=controller.material_panel_controls();
    for(std::size_t i=0;i<l.fields.size()&&p.controlFirst+i<fields.size();++i){const auto c=fields[p.controlFirst+i];std::string value=p.editing==c.property?p.typed:value_text(material_preset_value(p.draft,c.property));
        const bool overridden=(p.draft.preset.overrides&(std::uint64_t(1)<<unsigned(c.property)))!=0;btn(l.fields[i],std::string(c.label)+(overridden?"* ":" ")+value+" "+std::string(c.unit),p.editing==c.property);btn(l.resets[i],"R");}
    btn(l.reset,"Reset draft");btn(l.update,"Update draft");
    btn(l.apply,"Apply Copy / Make Unique");btn(l.editSlot,"Apply to Slot (shared faces/cells)");
    auto asset=p.catalog.find(p.draft.preset.id);MaterialPresetAsset requested;if(asset)requested=*asset;if(p.draft.transmission>0)requested.requirements.push_back(MaterialPresetFeature::SolidTransmission);
    const auto cap=material_preset_capability(requested,p.backend);const int y=l.panel.y+l.panel.height-118;
    text({l.preview.x,y,l.loadSlot.x-l.preview.x-12,24},std::string(material_preset_support_label(cap.support))+": "+cap.reason);
    painter.text(l.panel.x+12,l.panel.y+l.panel.height-8,elide_text_to_width(painter,p.message.empty()?"Arrows: presets | Ctrl+F: favorite | [ ]: slot | Wheel: list/controls | Enter: Apply Copy":p.message,l.panel.width-24),muted);
}
}
