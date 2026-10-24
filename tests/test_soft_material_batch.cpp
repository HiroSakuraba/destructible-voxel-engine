#include "dve/editor_material_presets.hpp"
#include "dve/editor_material_preview.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/dvox.hpp"
#include "dve/render/material_bsdf.hpp"
#include "dve/render/scene_lighting.hpp"
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace dve;
using namespace dve::editor;
using namespace dve::render;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
bool near(float a,float b,float e=2e-5F){return std::abs(a-b)<e;}
float brightness(Float3 a){return a.x+a.y+a.z;}
void bubble_energy(){
    auto catalog=MaterialPresetCatalog::starter();auto m=catalog.find("dve.bubble.soap-thin")->defaults;
    const Float3 n{0,0,1},v{.6F,0,.8F};
    auto f=material_interface_reflectance(m,.8F,1,1);
    check(brightness(f)>0.001F,"film over air must reflect even though substrate IOR equals air");
    check(std::abs(f.x-f.y)+std::abs(f.y-f.z)>.001F,"bubble reflection must have interference color");
    const auto reflected=sample_material_bsdf(m,n,v,1,1,0,.3F,.6F,0);
    const auto transmitted=sample_material_bsdf(m,n,v,1,1,0,.3F,.6F,.999999F);
    check(reflected.valid&&reflected.delta&&!reflected.transmitted,"soap reflection sampled");
    check(transmitted.valid&&transmitted.delta&&transmitted.transmitted,"soap transmission sampled");
    check(near(transmitted.direction.x,-v.x)&&near(transmitted.direction.z,-v.z),"air interior must not produce glass-sphere bending");
    check(near(reflected.weight.x*reflected.pdf,f.x)&&near(reflected.weight.y*reflected.pdf,f.y)&&near(reflected.weight.z*reflected.pdf,f.z),"reflection estimator preserves RGB Fresnel");
    check(near(transmitted.weight.x*transmitted.pdf+f.x,1)&&near(transmitted.weight.y*transmitted.pdf+f.y,1)&&near(transmitted.weight.z*transmitted.pdf+f.z,1),"reflection plus transmission conserves energy per channel");
    const auto thick=catalog.find("dve.bubble.soap-thick")->defaults;
    const auto f2=material_interface_reflectance(thick,.8F,1,1);
    check(std::abs(f.x-f2.x)+std::abs(f.y-f2.y)+std::abs(f.z-f2.z)>.01F,"film thickness changes color");
    m.iridescenceThicknessNm=0;const auto vanished=material_interface_reflectance(m,.8F,1,1);
    check(brightness(vanished)<.0001F,"zero-thickness film over air disappears");
    for(float cosine:{0.F,.00001F,.1F,1.F}) {
        const auto z=material_interface_reflectance(m,cosine,1,1);
        check(std::isfinite(brightness(z))&&brightness(z)<.0001F,"zero-thickness grazing limit must remain finite and invisible");
    }
    m.iridescence=0;const auto ordinary=sample_material_bsdf(m,n,v,1,1,0,.3F,.6F,0);
    check(ordinary.valid&&ordinary.transmitted&&near(ordinary.weight.x,1),"disabled film retains legacy clear interface");
    m.iridescence=1;m.iridescenceThicknessNm=350;m.roughness=.3F;
    // Rough film on a solid substrate: the sampled mixture PDF equals evaluation.
    for(unsigned i=0;i<64;++i){auto s=sample_material_bsdf(m,n,v,1,1.5F,0,float(i)/64,.41F,.12F);if(!s.valid)continue;
        auto e=evaluate_material_bsdf(m,n,v,s.direction,1,1.5F);check(near(s.pdf,e.pdf,.001F),"rough film eval and sample PDFs agree");}
    auto tir=material_interface_reflectance(m,.1F,1.5F,1);check(near(tir.x,1)&&near(tir.y,1)&&near(tir.z,1),"total internal reflection stays white and lossless");
}
void persistence(const std::filesystem::path& root){
    auto catalog=MaterialPresetCatalog::starter();unsigned count=0;std::string error;
    for(const auto& a:catalog.entries()){
        if(a.family!="Wax"&&a.family!="Skin"&&a.family!="Bubble"&&a.family!="Foam")continue;
        ++count;check(validate_material_preset(a,&error),error.c_str());
        check(material_preset_capability(a,MaterialPresetBackend::Gpu).support==MaterialPresetSupport::Unavailable,"unsupported GPU paths must be declared");
        const auto path=root/(a.id+".dvepreset");check(write_material_preset(path,a,&error),error.c_str());auto loaded=read_material_preset(path,&error);check(bool(loaded),error.c_str());
        check(loaded->defaults.iridescenceThicknessNm==a.defaults.iridescenceThicknessNm&&loaded->defaults.subsurfaceScatterDistanceMeters==a.defaults.subsurfaceScatterDistanceMeters,"preset optical and metric scattering fields survive");
        auto mesh=make_editor_polygon_cube(1);mesh.materials[0]=a.defaults;mesh.contentHash=polygon_asset_content_hash(mesh);
        check(write_dmesh(root/"sample.dmesh",mesh,&error),error.c_str());auto polygon=read_dmesh(root/"sample.dmesh");check(bool(polygon),polygon.error.c_str());
        check(polygon.asset.contentHash==mesh.contentHash,"DMESH hash survives material round trip");
        CookedVoxelAsset voxel(1);voxel.materials={{},a.defaults};voxel.object.set_voxel({0,0,0},1);
        check(write_dvox(root/"sample.dvox",voxel,{},&error),error.c_str());auto vox=read_dvox(root/"sample.dvox");check(vox.success,vox.error.c_str());
        check(vox.asset.materials[1].iridescence==a.defaults.iridescence&&vox.asset.materials[1].subsurfaceColor.x==a.defaults.subsurfaceColor.x,"DVOX transport survives");
    }
    check(count==8,"eight new soft-material presets");
    auto m=instantiate_material_preset(*catalog.find("dve.bubble.soap-thin"),{});
    check(set_material_preset_value(m,MaterialPresetProperty::FilmThickness,425,&error),error.c_str());
    check(m.preset.overrides&(std::uint64_t(1)<<unsigned(MaterialPresetProperty::FilmThickness)),"film override bit persisted");
    check(reset_material_preset_property(m,catalog,MaterialPresetProperty::FilmThickness,&error)&&m.iridescenceThicknessNm==350,"film reset restores origin value");
    check(!set_material_preset_value(m,MaterialPresetProperty::FilmIor,std::numeric_limits<float>::quiet_NaN(),&error),"NaN rejected before mutation");
    {
        // Coat and subsurface are alternative models: editing one must not silently drop the other.
        auto wax=instantiate_material_preset(*catalog.find("dve.wax.paraffin"),{});
        check(!set_material_preset_value(wax,MaterialPresetProperty::Coat,.5F,&error)&&wax.shadingModel==MaterialShadingModel::Subsurface&&wax.clearCoat==0,"coat refused on a subsurface preset without dropping scattering");
        check(set_material_preset_value(wax,MaterialPresetProperty::Coat,0,&error)&&wax.shadingModel==MaterialShadingModel::Subsurface,"zero coat edit keeps subsurface");
        check(reset_material_preset_property(wax,catalog,MaterialPresetProperty::Coat,&error)&&wax.shadingModel==MaterialShadingModel::Subsurface,"coat reset keeps subsurface");
        check(set_material_preset_value(wax,MaterialPresetProperty::ScatterDistance,0,&error)&&wax.shadingModel==MaterialShadingModel::StandardPBR,"zero scatter returns to standard PBR");
        check(set_material_preset_value(wax,MaterialPresetProperty::Coat,.5F,&error)&&wax.shadingModel==MaterialShadingModel::ClearCoat,"coat on a standard material");
        check(set_material_preset_value(wax,MaterialPresetProperty::ScatterDistance,0,&error)&&wax.shadingModel==MaterialShadingModel::ClearCoat,"zero scatter keeps clear coat");
        check(!set_material_preset_value(wax,MaterialPresetProperty::ScatterDistance,.01F,&error)&&wax.shadingModel==MaterialShadingModel::ClearCoat,"scatter refused over an authored coat");
    }
    m.iridescence=-1;EditorMaterialLibrary lib;check(!lib.upsert({10,m,false},&error),"invalid film rejected by in-memory authoring, not only by reader");
}
Float3 scatter_image(VoxelMaterialDefinition m,float distance){
    m.subsurfaceScatterDistanceMeters=distance;
    auto sample=make_editor_polygon_cube(1);sample.materials[0]=m;
    // Closed 2 cm slab, metre coordinates.
    for(auto& vertex:sample.vertices){vertex.position.x-=.5F;vertex.position.y-=.5F;vertex.position.z*=.02F;}
    refresh_editor_polygon(sample);const std::array<PolygonRenderInstance,1> objects{{{1,&sample}}};
    RenderEnvironment env;env.sunIntensity=0;env.skyColor={1,1,1};env.groundColor={1,1,1};env.pathTraceSamples=256;env.pathTraceBounces=6;
    SceneLighting lighting({},objects,env,1);return lighting.camera_transport({0,0,1},{0,0,-1},5,1234).sum();
}
Float3 sun_image(VoxelMaterialDefinition m){
    auto sample=make_editor_polygon_cube(1);sample.materials[0]=m;
    for(auto& vertex:sample.vertices){vertex.position.x-=.5F;vertex.position.y-=.5F;vertex.position.z*=.02F;}
    refresh_editor_polygon(sample);const std::array<PolygonRenderInstance,1> objects{{{1,&sample}}};
    RenderEnvironment env;env.skyColor={0,0,0};env.groundColor={0,0,0};env.sunIntensity=1;env.sunDirection={0,0,1};env.pathTraceSamples=64;env.pathTraceBounces=4;
    SceneLighting lighting({},objects,env,1);return lighting.camera_transport({0,0,1},{0,0,-1},5,99).sum();
}
void subsurface_boundary_highlight(){
    // The rough entry boundary of a subsurface material reflects the sun (a directional light that a
    // sampled reflection can never hit); a smooth boundary has no reflection lobe to evaluate.
    auto wax=MaterialPresetCatalog::starter().find("dve.wax.paraffin")->defaults;
    auto smooth=wax;smooth.roughness=0;
    const float rough=brightness(sun_image(wax)),flat=brightness(sun_image(smooth));
    check(std::isfinite(rough)&&std::isfinite(flat),"sun transport finite");
    check(rough>flat+.05F,"rough subsurface boundary reflects the sun");
}
void subsurface_transport(){
    auto catalog=MaterialPresetCatalog::starter();auto wax=catalog.find("dve.wax.paraffin")->defaults;
    const auto a=scatter_image(wax,.001F),b=scatter_image(wax,.04F);
    check(std::isfinite(brightness(a))&&std::isfinite(brightness(b)),"subsurface transport finite");
    check(brightness(a)>0&&brightness(b)>0,"closed subsurface materials scatter environment lighting");
    check(std::abs(brightness(a)-brightness(b))>.005F,"metric scatter distance reaches path transport");
    const auto light=scatter_image(catalog.find("dve.skin.light")->defaults,.0015F);
    const auto deep=scatter_image(catalog.find("dve.skin.deep")->defaults,.0015F);
    check(brightness(light)>brightness(deep),"skin pigment must affect volumetric transport");
}
}
int main(){auto root=std::filesystem::temp_directory_path()/"dve-soft-material-batch";try{std::filesystem::create_directories(root);bubble_energy();persistence(root);subsurface_transport();subsurface_boundary_highlight();std::filesystem::remove_all(root);std::cout<<"Soft material presets, film energy, persistence and subsurface transport passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
