#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/dvox.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
using namespace dve;using namespace dve::editor;
namespace {
void require(bool c,const std::string& message){if(!c)throw std::runtime_error(message);}
void ok(bool c,std::string& e){require(c,e);e.clear();}
std::filesystem::path root;
void catalog_and_files(){
    auto c=MaterialPresetCatalog::starter();require(c.entries().size()==44,"44 starter presets");std::string e;
    require(c.search("REFRACTIVE").size()==6,"case-insensitive glass synonym");require(!c.search("car").empty(),"car paint search");
    require(!c.add(c.entries().front(),&e),"duplicate ID refused");auto bad=c.entries().front();bad.id="dve.invalid";bad.requirements.push_back(MaterialPresetFeature(100));require(!c.add(bad,&e),"unknown feature refused");
    bad=c.entries().front();bad.id="dve.invalid";bad.defaults.roughness=std::numeric_limits<float>::infinity();require(!c.add(bad,&e),"nonfinite refused");
    bad=c.entries().front();bad.dependencies={"missing.png"};require(!validate_material_preset(bad,&e),"missing dependency refused");
    auto m=instantiate_material_preset(*c.find("dve.glass.clear"),{});require(m.baseColor.w==1&&m.transmission==1&&m.roughness==0&&m.indexOfRefraction==1.5F,"solid glass contract");
    auto old=m;require(!set_material_preset_value(m,MaterialPresetProperty::Roughness,std::numeric_limits<float>::quiet_NaN(),&e)&&m.roughness==old.roughness,"invalid edit does not mutate");
    ok(set_material_preset_value(m,MaterialPresetProperty::Roughness,.17F,&e),e);
    auto newer=*c.find(m.preset.id);newer.revision=2;newer.defaults.indexOfRefraction=1.6F;newer.defaults.roughness=.3F;newer.requirements.push_back(MaterialPresetFeature::RoughTransmission);
    ok(update_material_preset(m,newer,true,&e),e);require(m.roughness==.17F&&m.indexOfRefraction==1.6F&&m.preset.revision==2,"explicit update preserves overrides");
    require(!reset_material_preset_property(m,c,MaterialPresetProperty::Ior,&e),"missing old revision never silently resets");
    for(const auto& a:c.entries()){
        const auto path=root/(a.id+".dvepreset");ok(write_material_preset(path,a,&e),e);auto read=read_material_preset(path,&e);require(read.has_value(),e);require(read->id==a.id&&read->requirements==a.requirements&&read->defaults.roughness==a.defaults.roughness,"preset asset round trip");
        std::ifstream first(path,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(first)),{});first.close();ok(write_material_preset(path,a,&e),e);std::ifstream second(path,std::ios::binary);require(std::string((std::istreambuf_iterator<char>(second)),{})==bytes,"deterministic preset serialization");
        auto resolved=instantiate_material_preset(a,{});EditorMaterialLibrary lib;ok(lib.upsert({10,resolved,false},&e),e);ok(lib.save(root/"library.dvematerials",&e),e);auto loaded=EditorMaterialLibrary::load(root/"library.dvematerials",&e);require(loaded&&loaded->find(10)->definition.preset==resolved.preset,"library origin round trip");
        auto mesh=make_editor_polygon_cube(5);mesh.materials[0]=resolved;mesh.contentHash=polygon_asset_content_hash(mesh);ok(write_dmesh(root/"sample.dmesh",mesh,&e),e);auto readMesh=read_dmesh(root/"sample.dmesh");require(bool(readMesh),readMesh.error);require(readMesh.asset.materials[0].preset==resolved.preset,"DMESH origin round trip");require(bool(readMesh.asset.materials[0].normalMap)==bool(resolved.normalMap),"DMESH detail round trip");
        CookedVoxelAsset voxel(5);voxel.materials={VoxelMaterialDefinition{},resolved};voxel.object.set_voxel({0,0,0},1);ok(write_dvox(root/"sample.dvox",voxel,{},&e),e);auto readVoxel=read_dvox(root/"sample.dvox");require(readVoxel.success,readVoxel.error);require(readVoxel.asset.materials[1].preset==resolved.preset,"DVOX origin round trip");
        for(unsigned shape=0;shape<4;++shape)require(bool(validate_polygon_asset(material_preview_fixture(resolved,MaterialPreviewShape(shape)))),"valid preview fixture");
    }
    const auto* glass=c.find("dve.glass.clear");require(material_preset_capability(*glass,MaterialPresetBackend::Gpu).support==MaterialPresetSupport::Approximate,"GPU clear glass approximate");
    {   // Sheen round trips: DMESH minor 7 and DVOX minor 8 carry the lobe; values survive.
        auto velvet=instantiate_material_preset(*c.find("dve.glass.clear"),{});
        velvet.sheenColor={0.7F,0.25F,0.1F};velvet.sheenRoughness=0.5F;
        velvet.iridescence=0.9F;velvet.iridescenceIor=1.29F;velvet.iridescenceThicknessNm=350.0F;
        auto mesh=make_editor_polygon_cube(5);mesh.materials[0]=velvet;mesh.contentHash=polygon_asset_content_hash(mesh);
        ok(write_dmesh(root/"sheen.dmesh",mesh,&e),e);auto readMesh=read_dmesh(root/"sheen.dmesh");require(bool(readMesh),readMesh.error);
        require(readMesh.asset.materials[0].sheenColor.x==0.7F&&readMesh.asset.materials[0].sheenColor.y==0.25F&&readMesh.asset.materials[0].sheenRoughness==0.5F,"DMESH sheen round trip");
        require(readMesh.asset.materials[0].iridescence==0.9F&&readMesh.asset.materials[0].iridescenceIor==1.29F&&readMesh.asset.materials[0].iridescenceThicknessNm==350.0F,"DMESH iridescence round trip");
        require(readMesh.asset.contentHash==mesh.contentHash,"DMESH sheen content hash stable");
        CookedVoxelAsset voxel(5);voxel.materials={VoxelMaterialDefinition{},velvet};voxel.object.set_voxel({0,0,0},1);
        ok(write_dvox(root/"sheen.dvox",voxel,{},&e),e);auto readVoxel=read_dvox(root/"sheen.dvox");require(readVoxel.success,readVoxel.error);
        require(readVoxel.asset.materials[1].sheenColor.z==0.1F&&readVoxel.asset.materials[1].sheenRoughness==0.5F,"DVOX sheen round trip");
        require(readVoxel.asset.materials[1].iridescence==0.9F&&readVoxel.asset.materials[1].iridescenceThicknessNm==350.0F,"DVOX iridescence round trip");
    }
}
EditorDocument scene(){EditorDocument d("Material batch 1");EditorObject v(1,"Mixed voxel profile");v.voxels->set_voxel({0,0,0},1);v.voxels->set_voxel({1,0,0},3);d.add_object(std::move(v));EditorObject p(2,"Polygon");p.polygon=make_editor_polygon_cube(2);p.polygon->materials[0].densityKilogramsPerCubicMeter=4321;d.add_object(std::move(p));return d;}
void native_workflow(){
    NativeEditorController n{EditorWorkspace(scene()),root/"user.settings"};n.resize(1280,800);auto& w=n.workspace();auto& d=w.document();w.select_object(1);w.add_to_selection(2);
    require(n.dispatch_action("material.library")&&n.material_panel().open,"native browser opens");n.material_panel_select("dve.glass.frosted");
    const auto before=w.commands().size();require(n.apply_material_panel(),n.material_panel().message);require(w.commands().size()==before+1,"one apply one undo");
    auto* v=d.find_object(1);const auto first=v->voxels->material_at({0,0,0}),second=v->voxels->material_at({1,0,0});require(first!=second&&first>9&&second>9,"mixed physical profiles receive separate slots");
    require(v->voxelMaterials.at(first).densityKilogramsPerCubicMeter==2400&&v->voxelMaterials.at(second).flammability==.8F,"physical profiles preserved");
    auto* p=d.find_object(2);const auto slot=p->polygon->submeshes[0].materialIndex;require(p->polygon->materials[slot].densityKilogramsPerCubicMeter==4321,"polygon physical profile preserved");
    require(p->polygon->materials[slot].transmission==1,"polygon assigned actual optics");
    require(w.commands().undo(d).success,"undo");require(d.find_object(1)->voxelMaterials.empty()&&d.find_object(1)->voxels->material_at({0,0,0})==1&&d.find_object(2)->polygon->materials.size()==1,"undo definitions and cells");
    require(w.commands().redo(d).success,"redo");auto save=d.save_transactional(root/"presets.dvescene");require(save.success,save.error);std::string e;auto loaded=EditorDocument::load(root/"presets.dvescene",&e);require(bool(loaded),e);
    require(loaded->find_object(1)->voxelMaterials.at(first).preset.id=="dve.glass.frosted","scene voxel provenance");require(loaded->find_object(2)->polygon->materials[slot].preset.id=="dve.glass.frosted","scene polygon provenance");
    d.find_object(2)->flags.locked=true;const auto history=w.commands().size();n.material_panel_select("dve.metal.copper");require(!n.apply_material_panel()&&w.commands().size()==history,"mixed locked selection refuses atomically");require(d.find_object(1)->voxels->material_at({0,0,0})==first,"refusal changes nothing");d.find_object(2)->flags.locked=false;
    n.material_panel().backend=MaterialPresetBackend::Gpu;n.material_panel_select("dve.glass.frosted");require(!n.apply_material_panel(),"unsupported backend blocks rough glass apply");n.material_panel().backend=MaterialPresetBackend::CpuReference;
    w.select_object(2);n.material_panel().slot=slot;require(n.load_material_panel_slot(),"load assigned slot");n.material_panel().draft.roughness=.44F;require(n.apply_material_panel(true),n.material_panel().message);require(d.find_object(2)->polygon->materials[slot].roughness==.44F,"edit assigned shared slot");require(w.commands().undo(d).success&&d.find_object(2)->polygon->materials[slot].roughness==.25F,"slot edit undo");
    n.material_panel().draft.roughness=std::numeric_limits<float>::infinity();require(!n.apply_material_panel(true),"invalid draft never mutates a slot");
    n.material_panel_select("dve.mineral.plaster");require(n.apply_material_panel(),n.material_panel().message);save=d.save_transactional(root/"detail.dvescene");require(save.success,save.error);loaded=EditorDocument::load(root/"detail.dvescene",&e);require(bool(loaded),e);const auto& mesh=*loaded->find_object(2)->polygon;require(bool(mesh.materials[mesh.submeshes[0].materialIndex].normalMap),"scene embeds detail dependency");
    // Slot editing must replace the texture binding, then undo restore every old reference.
    auto& original=*d.find_object(2)->polygon;const auto current=original.submeshes[0].materialIndex;original.materialBindings[current].normalScale=.37F;n.material_panel().slot=current;require(n.load_material_panel_slot(),"load detail slot");n.material_panel_select("dve.paint.matte");require(n.apply_material_panel(true),"replace existing material slot");require(w.commands().undo(d).success&&d.find_object(2)->polygon->materialBindings[current].normalScale==.37F,"undo restores binding state");
    n.material_panel().favorites.insert("dve.glass.clear");n.material_panel_select("dve.paint.satin");
    require(n.save_material_panel_preset(),n.material_panel().message);const auto customId=n.material_panel().selected;
    const auto savedCount=n.material_panel().catalog.entries().size();require(n.save_material_panel_preset()&&n.material_panel().catalog.entries().size()==savedCount,"saving the same user preset is idempotent");n.close_material_panel();
    NativeEditorController next{EditorWorkspace{},root/"user.settings"};next.open_material_panel();require(next.material_panel().favorites.contains("dve.glass.clear"),"favorites persist");require(next.material_panel().catalog.find(customId),"user preset reloads from project assets");next.close_material_panel();
    for(const auto size:std::array<std::pair<int,int>,3>{{{640,480},{1280,800},{1920,1080}}}){n.resize(size.first,size.second);const auto l=n.material_panel_layout();for(const auto r:{l.close,l.search,l.apply,l.editSlot,l.preview})require(r.x>=l.panel.x&&r.y>=l.panel.y&&r.x+r.width<=l.panel.x+l.panel.width&&r.y+r.height<=l.panel.y+l.panel.height,"panel geometry stays inside bounds");}
}
void component_domains(){
    NativeEditorController n{EditorWorkspace(scene())};n.resize(1280,800);auto& w=n.workspace();auto& d=w.document();
    w.select_object(2);n.set_selection_domain(SelectionDomain::Polygons);require(n.set_polygon_mode(PolygonSelectionMode::Face),"face domain");n.select_polygon_components({0},SelectionOperation::Replace);
    n.open_material_panel();n.material_panel_select("dve.paint.gloss");const auto faces=n.polygon_selection().components;require(n.apply_material_panel(),n.material_panel().message);
    const auto& mesh=*d.find_object(2)->polygon;std::size_t assigned=0;for(const auto& sub:mesh.submeshes)if(sub.materialIndex!=0)assigned+=sub.indexCount/3;
    require(assigned==1&&n.polygon_selection().components==faces,"face assignment preserves component identity and leaves other faces untouched");
    require(w.commands().undo(d).success&&d.find_object(2)->polygon->materials.size()==1,"face assignment undo");
    n.close_material_panel();w.select_object(1);n.set_selection_domain(SelectionDomain::Voxels);n.select_voxels(1,{{0,0,0}},SelectionOperation::Replace);
    n.open_material_panel();n.material_panel_select("dve.metal.steel");const auto before=n.active_material();require(n.apply_material_panel(),n.material_panel().message);
    require(d.find_object(1)->voxels->material_at({0,0,0})>9&&d.find_object(1)->voxels->material_at({1,0,0})==3,"voxel-region assignment leaves other cells untouched");
    require(w.commands().undo(d).success&&n.active_material()==before,"undo restores active paint material");
    n.material_panel_select("dve.paint.satin");n.material_panel().draft.preset={"missing.preset",9,0};require(n.apply_material_panel(),n.material_panel().message);
    const auto slot=d.find_object(1)->voxels->material_at({0,0,0});const auto save=d.save_transactional(root/"missing-origin.dvescene");require(save.success,save.error);auto loaded=EditorDocument::load(root/"missing-origin.dvescene");
    require(loaded&&loaded->find_object(1)->voxelMaterials.at(slot).roughness==.4F,"missing preset falls back to saved resolved values");n.close_material_panel();
}
// Undo/redo must restore the document and active material exactly, not just the touched fields.
// Voxel brick generation counters are monotonic cache stamps and intentionally keep advancing,
// so voxel state is compared by content (cells and every material definition) instead of DVOX bytes.
std::string file_bytes(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);return std::string((std::istreambuf_iterator<char>(in)),{});}
std::string snapshot(EditorDocument& d){
    std::string out;std::string e;
    for(EditorObjectId id:{EditorObjectId(1),EditorObjectId(2)}){const auto* o=d.find_object(id);require(o!=nullptr,"object exists");
        auto lib=EditorMaterialLibrary::make_minimal();for(const auto& [slot,m]:o->voxelMaterials)if(slot!=0)ok(lib.upsert({slot,m,false},&e),e);
        std::ostringstream s;ok(lib.write_stream(s,&e),e);out+=s.str();
        if(o->voxels)for(int x=0;x<2;++x)out+=std::to_string(o->voxels->material_at({x,0,0}))+",";
        if(o->polygon){auto mesh=*o->polygon;const auto path=root/"snapshot.dmesh";ok(write_dmesh(path,mesh,&e),e);out+=file_bytes(path);}}
    return out;}
std::string library_text(const NativeEditorController& n){std::ostringstream s;std::string e;ok(n.materials().write_stream(s,&e),e);return s.str();}
void undo_redo_exact(){
    NativeEditorController n{EditorWorkspace(scene()),root/"user.settings"};n.resize(1280,800);auto& w=n.workspace();auto& d=w.document();w.select_object(1);w.add_to_selection(2);
    const auto original=snapshot(d)+library_text(n);const auto activeBefore=n.active_material();
    require(n.dispatch_action("material.library"),"browser opens");n.material_panel_select("dve.mineral.plaster");require(n.apply_material_panel(),n.material_panel().message);
    const auto applied=snapshot(d)+library_text(n);require(applied!=original,"apply changes the scene");const auto activeAfter=n.active_material();
    require(w.commands().undo(d).success,"undo");require(snapshot(d)+library_text(n)==original,"undo restores materials, cells and meshes exactly");require(n.active_material()==activeBefore,"undo restores active material");
    require(w.commands().redo(d).success,"redo");require(snapshot(d)+library_text(n)==applied,"redo reproduces the applied state exactly");require(n.active_material()==activeAfter,"redo restores applied active material");
    require(w.commands().undo(d).success&&snapshot(d)+library_text(n)==original,"second undo is exact");n.close_material_panel();
}
// Custom presets are per project: switching projects must not leak or shadow them.
void project_switch_rebuilds_catalog(){
    const auto a=root/"project-a",b=root/"project-b";std::filesystem::create_directories(a);std::filesystem::create_directories(b);
    NativeEditorController n{EditorWorkspace(scene()),root/"switch.settings"};n.resize(1280,800);n.configure_ai_assistant(a);
    n.open_material_panel();n.material_panel_select("dve.paint.satin");require(n.save_material_panel_preset(),n.material_panel().message);
    const auto id=n.material_panel().selected;require(std::filesystem::exists(a/"Materials"/"Presets"/(id+".dvepreset")),"preset saved in project A");n.close_material_panel();
    n.configure_ai_assistant(b);n.open_material_panel();require(!n.material_panel().catalog.find(id),"project A preset does not leak into project B");
    n.material_panel_select("dve.paint.satin");require(n.save_material_panel_preset(),n.material_panel().message);
    require(std::filesystem::exists(b/"Materials"/"Presets"/(id+".dvepreset")),"same preset is really written into project B");n.close_material_panel();
}
// Each CPU path-traced thumbnail takes seconds under ASan/UBSan on a loaded 4-vCPU runner, so every
// wait gets its own generous deadline. A single shared 30 s deadline for all three waits expired
// on slow sanitizer runs (CI linux-gcc-asan, test took 34 s) and reported the timeout as
// "stale job cannot replace it" although no stale image was ever committed.
template<class Done> bool wait_until(Done done,std::chrono::seconds limit=std::chrono::seconds(180)){
    const auto until=std::chrono::steady_clock::now()+limit;
    while(!done()){if(std::chrono::steady_clock::now()>=until)return false;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    return true;
}
void preview_jobs(){
    auto c=MaterialPresetCatalog::starter();
    MaterialPreviewWorker worker;worker.request(c.find("dve.glass.frosted")->defaults,MaterialPreviewShape::Sphere);worker.request(c.find("dve.metal.copper")->defaults,MaterialPreviewShape::Wedge);
    require(wait_until([&]{return worker.image()!=nullptr;}),"thumbnail deadline");
    auto image=worker.image();require(image&&!image->rgb.empty()&&image->error.empty(),image?image->error:"thumbnail deadline");auto ready=image;
    MaterialPreviewWorker reference;reference.request(c.find("dve.metal.copper")->defaults,MaterialPreviewShape::Wedge);
    require(wait_until([&]{return reference.image()!=nullptr;}),"reference thumbnail deadline");
    require(reference.image()->error.empty(),reference.image()->error);
    require(reference.image()->rgb==image->rgb,"latest result matches the requested shape/material; stale job cannot replace it");
    worker.request(c.find("dve.metal.copper")->defaults,MaterialPreviewShape::Wedge);require(worker.image()==ready,"identical request retains cached image");
    worker.request(c.find("dve.glass.clear")->defaults,MaterialPreviewShape::Sphere);worker.cancel();require(!worker.image(),"cancel clears stale image");
    require(wait_until([&]{return !worker.busy();}),"cancelled job deadline");
    require(!worker.image(),"cancelled job never commits later");
}
}
int main(){try{root=std::filesystem::temp_directory_path()/"dve-material-presets-batch1";std::filesystem::remove_all(root);std::filesystem::create_directories(root);const auto previous=std::filesystem::current_path();std::filesystem::current_path(root);catalog_and_files();native_workflow();component_domains();undo_redo_exact();project_switch_rebuilds_catalog();preview_jobs();std::filesystem::current_path(previous);std::filesystem::remove_all(root);std::cout<<"Material preset catalog, persistence, native workflow and asynchronous preview checks passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
