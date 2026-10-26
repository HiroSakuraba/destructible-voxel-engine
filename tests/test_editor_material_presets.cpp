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
// A tiny uncompressed (stored-deflate) RGBA PNG, enough for the map importer.
void write_test_png(const std::filesystem::path& path,unsigned w,unsigned h,std::uint8_t value){
    std::vector<std::uint8_t> raw;for(unsigned y=0;y<h;++y){raw.push_back(0);for(unsigned x=0;x<w;++x){raw.push_back(value);raw.push_back(value);raw.push_back(value);raw.push_back(255);}}
    std::uint32_t table[256];for(std::uint32_t n=0;n<256;++n){std::uint32_t c=n;for(int k=0;k<8;++k)c=(c&1)?0xedb88320U^(c>>1):c>>1;table[n]=c;}
    const auto crc=[&](const std::vector<std::uint8_t>& d){std::uint32_t c=0xffffffffU;for(auto b:d)c=table[(c^b)&255]^(c>>8);return c^0xffffffffU;};
    const auto be=[](std::vector<std::uint8_t>& v,std::uint32_t x){for(int i=3;i>=0;--i)v.push_back(std::uint8_t(x>>(8*i)));};
    std::vector<std::uint8_t> out{0x89,'P','N','G','\r','\n',0x1a,'\n'};
    const auto chunk=[&](const char* type,const std::vector<std::uint8_t>& data){be(out,std::uint32_t(data.size()));std::vector<std::uint8_t> t(type,type+4);t.insert(t.end(),data.begin(),data.end());out.insert(out.end(),t.begin(),t.end());be(out,crc(t));};
    std::vector<std::uint8_t> ihdr;be(ihdr,w);be(ihdr,h);ihdr.insert(ihdr.end(),{8,6,0,0,0});chunk("IHDR",ihdr);
    std::vector<std::uint8_t> z{0x78,0x01,1,std::uint8_t(raw.size()&255),std::uint8_t(raw.size()>>8),std::uint8_t(~raw.size()&255),std::uint8_t((~raw.size()>>8)&255)};
    z.insert(z.end(),raw.begin(),raw.end());std::uint32_t a=1,b=0;for(auto v:raw){a=(a+v)%65521;b=(b+a)%65521;}be(z,(b<<16)|a);chunk("IDAT",z);chunk("IEND",{});
    std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(out.data()),std::streamsize(out.size()));
}
// Texturing controls: the five new properties, the panel showing them where they matter,
// weathering a voxel object and a polygon slot (one undo each, saved with the scene) and the
// Layers rows that edit a layer's rules.
void texturing_controls(){
    std::string e;
    VoxelMaterialDefinition m;m.name="Cobble";m.surfaceDetail.pattern=SurfacePattern::Cellular;m.surfaceDetail.periodMeters=.2F;
    ok(set_material_preset_value(m,MaterialPresetProperty::DisplacementDepth,.04F,&e),e);
    ok(set_material_preset_value(m,MaterialPresetProperty::DisplacementMode,1,&e),e);
    require(!set_material_preset_value(m,MaterialPresetProperty::DisplacementMode,.5F,&e),"displacement mode is a whole number");
    require(!set_material_preset_value(m,MaterialPresetProperty::TextureAntiTiling,.7F,&e),"anti-tiling above 0.5 is refused");
    ok(set_material_preset_value(m,MaterialPresetProperty::TextureAntiTiling,.25F,&e),e);
    ok(set_material_preset_value(m,MaterialPresetProperty::TextureTile,.5F,&e),e);
    require(m.displacementDepthMeters==.04F&&m.displacementMode==1&&m.textureAntiTiling==.25F&&m.textureTileMeters==.5F&&
            material_preset_value(m,MaterialPresetProperty::DisplacementMode)==1,"texturing properties read back");

    NativeEditorController n{EditorWorkspace(scene()),root/"texturing.settings"};n.resize(1280,800);auto& w=n.workspace();auto& d=w.document();
    n.open_material_panel();n.material_panel().draft=m;
    const auto shows=[&](MaterialPresetProperty property){for(const auto& c:n.material_panel_controls())if(c.property==property)return true;return false;};
    require(shows(MaterialPresetProperty::DisplacementDepth)&&shows(MaterialPresetProperty::DisplacementMode),"a height source shows the displacement controls");
    require(!shows(MaterialPresetProperty::TextureTile)&&!shows(MaterialPresetProperty::TextureAntiTiling),"no texture set: no texture-set controls");
    n.material_panel().draft.surfaceDetail.pattern=SurfacePattern::NoPattern;
    require(!shows(MaterialPresetProperty::DisplacementDepth),"no height source: no displacement controls");
    n.close_material_panel();

    // Voxels: both materials the object uses gain a moss layer from one new source slot.
    w.select_object(1);const auto history=w.commands().size();
    require(n.weather_selection(WeatheringKind::Moss,.7F),n.material_panel().message);
    require(w.commands().size()==history+1,"weathering is one undo step");
    {
        const auto& palette=d.find_object(1)->voxelMaterials;
        require(palette.contains(1)&&palette.contains(3),"both used materials are weathered");
        const auto& a=palette.at(1);const auto& b=palette.at(3);
        require(a.layers.size()==1&&b.layers.size()==1&&a.layers[0].sourceMaterial==b.layers[0].sourceMaterial,"one shared source slot");
        const auto source=a.layers[0].sourceMaterial;
        require(palette.contains(source)&&palette.at(source).name=="Moss"&&a.layers[0].rules.enabled&&a.layers[0].rules.occlusionMax>.7F,"moss source and its rules");
    }
    auto saved=d.save_transactional(root/"weather.dvescene");require(saved.success,saved.error);
    auto loaded=EditorDocument::load(root/"weather.dvescene",&e);require(bool(loaded),e);
    {
        const auto& palette=loaded->find_object(1)->voxelMaterials;
        require(palette.contains(1)&&palette.at(1).layers.size()==1&&palette.at(1).layers[0].rules.enabled&&
                palette.at(1).layers[0].rules.seed==d.find_object(1)->voxelMaterials.at(1).layers[0].rules.seed,"the scene keeps the weathering rules");
    }
    // Voxel overlays in Layers mode: material 1's moss layer, its rules (with the voxel-only AO
    // and edge rows), one undo step per edit.
    n.open_material_panel();n.material_panel().slot=1;n.material_panel().layerIndex=0;
    {
        bool edgeRow=false,occlusionRow=false;for(const auto& [key,text]:n.material_panel_layer_controls()){edgeRow=edgeRow||key=="ruleEdge";occlusionRow=occlusionRow||key=="ruleOcclusion";}
        require(edgeRow&&occlusionRow,"voxel overlays list the AO and edge rules");
        const auto before=w.commands().size();
        require(n.material_panel_layer_action("ruleEdge",.05F),n.material_panel().message);
        require(w.commands().size()==before+1&&d.find_object(1)->voxelMaterials.at(1).layers[0].rules.edgeWidthMeters==.05F,"edit a voxel overlay rule");
        require(!n.material_panel_layer_action("ruleOcclusion",2.0F)&&d.find_object(1)->voxelMaterials.at(1).layers[0].rules.occlusionMax<=1.0F,"an out-of-range voxel rule is refused");
        require(n.material_panel_layer_action("weight",.4F)&&d.find_object(1)->voxelMaterials.at(1).layers[0].weight==.4F,"voxel overlay weight");
        require(w.commands().undo(d).success&&w.commands().undo(d).success&&d.find_object(1)->voxelMaterials.at(1).layers[0].rules.edgeWidthMeters!=.05F,"undo voxel overlay edits");
    }
    n.close_material_panel();
    require(w.commands().undo(d).success,"undo weathering");
    {
        const auto& palette=d.find_object(1)->voxelMaterials;
        require(!palette.contains(1)||palette.at(1).layers.empty(),"undo removes the weathering");
    }
    require(!n.weather_selection(WeatheringKind::Snow,1.5F),"amount above 1 is refused");

    // Texture maps: import into the draft, apply to voxels, refused on meshes, cleared.
    write_test_png(root/"stone_height.png",4,4,200);
    n.open_material_panel();
    require(!n.material_panel_import_map(2,root/"missing.png"),"a missing image is refused");
    require(n.material_panel_import_map(2,root/"stone_height.png")&&n.material_panel().draft.heightMap&&n.material_panel().draft.heightMap->role==2U,n.material_panel().message);
    require(n.material_panel_import_map(0,root/"stone_height.png")&&n.material_panel().draft.baseColorMap->role==1U,"base colour is a colour map");
    n.material_panel().draft.displacementDepthMeters=.03F;
    w.select_object(2);require(!n.apply_material_panel(),"maps are not applied to meshes");
    w.select_object(1);require(n.apply_material_panel(),n.material_panel().message);
    bool mapped=false;for(const auto& [slot,material]:d.find_object(1)->voxelMaterials)mapped=mapped||(material.heightMap&&material.baseColorMap&&material.displacementDepthMeters==.03F);
    require(mapped,"the voxels' material carries the imported maps");
    n.material_panel_clear_maps();require(!material_has_textures(n.material_panel().draft),"clear maps");
    n.close_material_panel();

    // Polygon slot 0 from the Layers rows: next kind (Snow -> Moss), amount, add; then rules.
    w.select_object(2);n.open_material_panel();n.material_panel().slot=0;n.material_panel().layerIndex=0;
    require(n.dispatch_action("material.weather_kind")&&n.material_panel().weatherKind==WeatheringKind::Moss,"next weathering kind");
    require(n.material_panel_layer_action("weatherAmount",.9F)&&n.material_panel().weatherAmount==.9F,"weathering amount");
    require(n.material_panel_layer_action("weatherAdd"),n.material_panel().message);
    {
        const auto& mesh=*d.find_object(2)->polygon;
        require(mesh.materials[0].layers.size()==1&&mesh.materialBindings[0].layers.size()==1,"one overlay with its binding");
        const auto source=mesh.materials[0].layers[0].sourceMaterial;
        require(source<mesh.materials.size()&&mesh.materials[source].name=="Moss"&&mesh.materials[0].layers[0].rules.enabled,"moss overlay with rules");
    }
    bool rulesRow=false;for(const auto& [key,text]:n.material_panel_layer_controls())rulesRow=rulesRow||key=="ruleSlope";
    require(rulesRow,"Layers rows list the rules");
    require(n.material_panel_layer_action("ruleSlope",.2F)&&d.find_object(2)->polygon->materials[0].layers[0].rules.slopeMinCos==.2F,"edit a rule");
    require(!n.material_panel_layer_action("ruleSlope",3.0F)&&d.find_object(2)->polygon->materials[0].layers[0].rules.slopeMinCos==.2F,"an out-of-range rule is refused");
    require(n.material_panel_layer_action("rules")&&!d.find_object(2)->polygon->materials[0].layers[0].rules.enabled,"rules off");
    n.close_material_panel();
}
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
int main(){try{root=std::filesystem::temp_directory_path()/"dve-material-presets-batch1";std::filesystem::remove_all(root);std::filesystem::create_directories(root);const auto previous=std::filesystem::current_path();std::filesystem::current_path(root);catalog_and_files();native_workflow();texturing_controls();component_domains();undo_redo_exact();project_switch_rebuilds_catalog();preview_jobs();std::filesystem::current_path(previous);std::filesystem::remove_all(root);std::cout<<"Material preset catalog, persistence, native workflow and asynchronous preview checks passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
