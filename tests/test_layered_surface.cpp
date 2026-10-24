#include "dve/editor_native.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/master_material.hpp"
#include "dve/polygon_bvh.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/render/layered_surface.hpp"
#include <bit>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace dve;using namespace dve::editor;using namespace dve::render;
namespace {
void check(bool b,const std::string& why){if(!b)throw std::runtime_error(why);}
void payload(){
    std::vector<VoxelMaterialDefinition> materials(5);materials[0].baseColor={0,0,0,1};
    for(unsigned i=1;i<5;++i){materials[i].baseColor={float(i)/4,0,0,1};materials[0].layers.push_back({MaterialId(i),.5F,MaterialLayerBlendMode::Lerp,true});}
    for(unsigned repeat=0;repeat<64;++repeat){std::string error;const auto resolved=resolve_voxel_material_layers(materials,&error);check(bool(resolved),error);check(std::abs((*resolved)[0].baseColor.x-.765625F)<1e-6F,"all four layers resolve without invalidated iteration");}

    auto a=make_editor_polygon_cube(1);a.materials.push_back({});a.materialBindings.emplace_back();
    a.materials[1].baseColor={.7F,.2F,.1F,.4F};a.materials[0].layers={{1,.6F,MaterialLayerBlendMode::Multiply,true}};
    PolygonMaterialLayerBinding binding;binding.heightBlendStrength=0;binding.mask.texture=0;binding.mask.texcoord=1;a.materialBindings[0].layers={binding};
    a.images={{"mask","image/raw",2,1,{0,0,0,255,255,255,255,255}}};a.textures={{"mask",0,{}}};
    std::string error;auto p=build_layered_surface_payload(a,0,&error);check(bool(p),error);check(p->words[0]==1,"one active layer");
    auto at=p->words[4];check(std::bit_cast<float>(p->words[at])==.7F,"raw source record, not a preflattened value");
    check(std::bit_cast<float>(p->words[at+89])==.6F,"weight ABI");auto mask=p->words[at+89+8];check(p->words[mask+6]==1&&p->words[mask+7]==2,"map UV and original extent");
    auto pair=a;pair.materials[0].layers.push_back(pair.materials[0].layers[0]);pair.materialBindings[0].layers.push_back(binding);pair.materialBindings[0].layers[1].mask.texcoord=0;
    const auto shared=build_layered_surface_payload(pair,0,&error);check(bool(shared),error);
    const auto firstMap=shared->words[shared->words[4]+89+8],secondMap=shared->words[shared->words[5]+89+8];
    check(firstMap!=secondMap&&shared->words[firstMap+9]==shared->words[secondMap+9],"independent UV bindings share typed pixel storage");
    a.materials[0].layers[0].enabled=false;p=build_layered_surface_payload(a,0,&error);check(p&&p->words[0]==0,"muted layer has no image payload");
    a.materials[1].layers={{0,1,MaterialLayerBlendMode::Lerp,true}};check(!build_layered_surface_payload(a,0,&error),"dormant nesting rejected");a.materials[1].layers.clear();
    a.materials[0].layers[0].enabled=true;a.materials[1].transmission=1;check(!validate_layered_surface(a,0),"optical source rejected");a.materials[1].transmission=0;
    a.materials[0].layers[0].weight=std::numeric_limits<float>::quiet_NaN();check(!validate_layered_surface(a,0),"NaN rejected");a.materials[0].layers[0].weight=1;
    a.textures[0].imageIndex=7;check(!build_layered_surface_payload(a,0,&error),"missing image rejected without publication");
}
void infinitesimal_normal_frame(){
    auto a=make_editor_polygon_cube(1);a.materials.push_back({});a.materialBindings.emplace_back();a.materials[0].layers={{1,1,MaterialLayerBlendMode::Lerp,true}};
    a.images={{"normal","image/raw",1,1,{200,128,240,255}}};a.textures={{"normal",0,{}}};a.materialBindings[1].normal={0,1,PolygonTextureColorSpace::Linear};
    for(auto& v:a.vertices)v.texcoord1={v.texcoord.y,v.texcoord.x};
    const auto& x=a.vertices[a.indices[0]];const auto& y=a.vertices[a.indices[1]];const auto& z=a.vertices[a.indices[2]];
    PolygonBvhHit hit;hit.position={(x.position.x+y.position.x+z.position.x)/3,(x.position.y+y.position.y+z.position.y)/3,(x.position.z+y.position.z+z.position.z)/3};hit.normal=x.normal;
    std::vector<PolygonTextureMipChain> mips{generate_texture_mips(a.images[0],true)};
    const auto point=sample_polygon_hit_material(a,hit,{},hit.normal,mips,1,0);
    const auto finite=sample_polygon_hit_material(a,hit,{},hit.normal,mips,1,.0001F);
    check(std::abs(point.normal.x-finite.normal.x)+std::abs(point.normal.y-finite.normal.y)+std::abs(point.normal.z-finite.normal.z)<1e-5F,"UV1 mapped frame is independent of ray-cone width");
}
void editor_workflow(){
    EditorDocument document("Layered Surface");EditorObject object(1,"Cube");object.polygon=make_editor_polygon_cube(1);document.add_object(std::move(object));
    NativeEditorController controller{EditorWorkspace(std::move(document))};auto& workspace=controller.workspace();workspace.select_object(1);
    auto& panel=controller.material_panel();panel.slot=0;panel.layerMode=true;
    auto action=[&](std::string_view name,float value=0){check(controller.material_panel_layer_action(name,value),panel.message);};
    action("recipeMetal");auto& d=workspace.document();auto mesh=[&]()->const CookedPolygonAsset&{return *d.find_object(1)->polygon;};
    check(mesh().materials[0].layers.size()==3&&mesh().materialBindings[0].layers.size()==3,"recipe material and bindings");
    const auto hash=mesh().contentHash;const auto count=workspace.commands().size();action("weight",.25F);
    check(workspace.commands().size()==count+1&&mesh().materials[0].layers[0].weight==.25F,"one action, one undo transaction");
    check(workspace.commands().undo(d).success&&mesh().contentHash==hash,"undo full mesh and dependencies");check(workspace.commands().redo(d).success,"redo layer edit");
    const auto source=mesh().materials[0].layers[0].sourceMaterial;action("down");check(mesh().materials[0].layers[1].sourceMaterial==source&&mesh().materialBindings[0].layers[1].semantic==MaterialLayerSemantic::Paint,"reorder keeps mask with source");
    action("duplicate");check(mesh().materials[0].layers.size()==4,"duplicate");check(!controller.material_panel_layer_action("duplicate"),"limit enforced");
    action("remove");check(mesh().materials[0].layers.size()==3,"remove");action("solo");unsigned enabled=0;for(const auto& l:mesh().materials[0].layers)enabled+=l.enabled;check(enabled==1,"solo");
    check(workspace.commands().undo(d).success,"undo solo");action("maskUv");check(mesh().materialBindings[0].layers[panel.layerIndex].mask.texcoord==1,"independent mask UV");
    action("maskScale",.7F);action("heightStrength",.4F);action("opacity");
    const auto before=mesh().contentHash;check(!controller.material_panel_layer_action("weight",2)&&mesh().contentHash==before,"invalid edit atomic");
    const auto path=std::filesystem::temp_directory_path()/"dve-batch6-layered-scene.dvescene";std::string error;
    auto saved=d.save_transactional(path);check(saved.success,saved.error);auto loaded=EditorDocument::load(path,&error);check(bool(loaded),error);
    check(loaded->find_object(1)->polygon->contentHash==before,"scene stack, images and recipe round-trip");
    std::filesystem::remove_all(path);
    action("recipeWood");check(mesh().materials[0].surfaceDetail.pattern==SurfacePattern::Wood,"wood recipe");action("recipePlaster");check(mesh().materials[0].surfaceDetail.pattern==SurfacePattern::Mineral,"plaster recipe");
    // Pointer + text entry exercises the artist control, not only its command helper.
    controller.resize(1280,800);panel.open=true;auto rows=controller.material_panel_layer_controls();
    auto it=std::find_if(rows.begin(),rows.end(),[](const auto& r){return r.first=="weight";});panel.controlFirst=std::size_t(it-rows.begin());
    auto rect=controller.material_panel_layout().fields[0];check(controller.material_panel_pointer(rect.x+2,rect.y+2),"weight pointer");controller.material_panel_text("0.375");check(controller.material_panel_key("enter",false,false,false),panel.message);check(mesh().materials[0].layers[panel.layerIndex].weight==.375F,"typed exact weight");
}
}
int main(){try{payload();infinitesimal_normal_frame();editor_workflow();std::cout<<"layered surface payload/editor: PASS\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
