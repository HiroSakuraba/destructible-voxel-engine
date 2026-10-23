#include "dve/editor_material_presets.hpp"
#include "dve/voxel_normal_map.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <cctype>
#include <stdexcept>
#include <fstream>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <iomanip>
namespace dve::editor {
namespace {
constexpr std::array<MaterialPresetControl, unsigned(MaterialPresetProperty::Count)> controls{{
    {MaterialPresetProperty::ColorR,"Color R","linear","Pigment for dielectric; reflectance for metal",0,1,.025F,false},
    {MaterialPresetProperty::ColorG,"Color G","linear","Pigment for dielectric; reflectance for metal",0,1,.025F,false},
    {MaterialPresetProperty::ColorB,"Color B","linear","Pigment for dielectric; reflectance for metal",0,1,.025F,false},
    {MaterialPresetProperty::Roughness,"Roughness","","Highlight and transmission spread",0,1,.025F,false},
    {MaterialPresetProperty::Metallic,"Metallic","","Clean full metal has no diffuse pigment lobe",0,1,.05F,true},
    {MaterialPresetProperty::Specular,"Reflection","","Opaque dielectric F0 control; 0.5 = about 4%",0,1,.025F,true},
    {MaterialPresetProperty::Coverage,"Coverage alpha","","Surface coverage, separate from solid transmission",0,1,.025F,true},
    {MaterialPresetProperty::Transmission,"Transmission","","Solid dielectric; requires closed geometry",0,1,.025F,false},
    {MaterialPresetProperty::Ior,"IOR","","Index of refraction controls glass Fresnel and bending",1,3,.025F,false},
    {MaterialPresetProperty::AbsorptionR,"Absorption R","linear","Transmittance after absorption distance",0,1,.025F,true},
    {MaterialPresetProperty::AbsorptionG,"Absorption G","linear","Transmittance after absorption distance",0,1,.025F,true},
    {MaterialPresetProperty::AbsorptionB,"Absorption B","linear","Transmittance after absorption distance",0,1,.025F,true},
    {MaterialPresetProperty::AbsorptionDistance,"Absorption distance","m","0 disables volume absorption",0,100,.025F,false},
    {MaterialPresetProperty::Coat,"Clear coat","","Weight of separate dielectric surface coat",0,1,.05F,false},
    {MaterialPresetProperty::CoatRoughness,"Coat roughness","","Spread of coat highlight",0,1,.025F,false},
    {MaterialPresetProperty::DetailScale,"Detail repeat","m","One repeat in object space",.0001F,10000,.01F,true},
    {MaterialPresetProperty::DetailStrength,"Detail strength","","Normal-map tilt; does not displace geometry",0,4,.05F,true}
}};
std::string lower(std::string_view s) { std::string r(s); for(char& c:r)c=char(std::tolower(static_cast<unsigned char>(c))); return r; }
void physical(VoxelMaterialDefinition& to,const VoxelMaterialDefinition& from) {
    to.densityKilogramsPerCubicMeter=from.densityKilogramsPerCubicMeter;
    to.structuralStrength=from.structuralStrength; to.fractureResistance=from.fractureResistance;
    to.flammability=from.flammability; to.thermalConductivity=from.thermalConductivity; to.structural=from.structural;
}
bool fail(std::string* e,std::string message){if(e)*e=std::move(message);return false;}
}
std::span<const MaterialPresetControl> material_preset_controls(){return controls;}
float material_preset_value(const VoxelMaterialDefinition& m, MaterialPresetProperty p){
    switch(p){
#define VALUE(P,V) case MaterialPresetProperty::P:return m.V;
    VALUE(ColorR,baseColor.x) VALUE(ColorG,baseColor.y) VALUE(ColorB,baseColor.z)
    VALUE(Roughness,roughness) VALUE(Metallic,metallic) VALUE(Specular,specular) VALUE(Coverage,baseColor.w)
    VALUE(Transmission,transmission) VALUE(Ior,indexOfRefraction)
    VALUE(AbsorptionR,attenuationColor.x) VALUE(AbsorptionG,attenuationColor.y) VALUE(AbsorptionB,attenuationColor.z)
    VALUE(AbsorptionDistance,attenuationDistanceMeters) VALUE(Coat,clearCoat) VALUE(CoatRoughness,clearCoatRoughness)
    VALUE(DetailScale,normalMapTileMeters) VALUE(DetailStrength,normalMapStrength)
#undef VALUE
    default:return 0;
    }
}
bool set_material_preset_value(VoxelMaterialDefinition& m,MaterialPresetProperty p,float v,std::string* e){
    const auto i=unsigned(p); if(i>=controls.size()||!std::isfinite(v)||v<controls[i].minimum||v>controls[i].maximum)
        return fail(e,"Value outside the displayed finite range");
    switch(p){
#define SET(P,V) case MaterialPresetProperty::P:m.V=v;break;
    SET(ColorR,baseColor.x) SET(ColorG,baseColor.y) SET(ColorB,baseColor.z)
    SET(Roughness,roughness) SET(Metallic,metallic) SET(Specular,specular) SET(Coverage,baseColor.w)
    SET(Transmission,transmission) SET(Ior,indexOfRefraction)
    SET(AbsorptionR,attenuationColor.x) SET(AbsorptionG,attenuationColor.y) SET(AbsorptionB,attenuationColor.z)
    SET(AbsorptionDistance,attenuationDistanceMeters) SET(Coat,clearCoat) SET(CoatRoughness,clearCoatRoughness)
    SET(DetailScale,normalMapTileMeters) SET(DetailStrength,normalMapStrength)
#undef SET
    default:return false;
    }
    if(p==MaterialPresetProperty::Coverage)m.blendMode=v<1?MaterialBlendMode::Translucent:MaterialBlendMode::Opaque;
    if(p==MaterialPresetProperty::Coat)m.shadingModel=v>0?MaterialShadingModel::ClearCoat:MaterialShadingModel::StandardPBR;
    m.transparent=m.transmission>0||m.blendMode==MaterialBlendMode::Translucent;
    if(!m.preset.id.empty())m.preset.overrides|=std::uint64_t(1)<<i;
    return true;
}
bool validate_material_preset(const MaterialPresetAsset& a,std::string* e){
    if(a.schema!=1||a.revision==0||a.id.empty()||a.id.size()>128||a.name.empty()||a.family.empty()||a.provenance.empty()||a.mappingUnits!="meters")
        return fail(e,"Invalid preset identity, schema, revision, provenance or mapping units");
    if(a.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-_")!=std::string::npos)
        return fail(e,"Preset ID must be a stable lowercase identifier");
    for(auto feature:a.requirements)if(unsigned(feature)>unsigned(MaterialPresetFeature::NormalDetail))return fail(e,"Unknown required shader feature");
    for(const auto& d:a.dependencies)if(d!="embedded:normal"||!a.defaults.normalMap)return fail(e,"Unresolved preset dependency");
    if(!a.defaults.preset.id.empty()||!a.defaults.layers.empty())return fail(e,"Preset defaults cannot reference instances or external material slots");
    auto copy=a.defaults; copy.name=a.name;
    EditorMaterialLibrary check;
    if(!check.upsert({10,copy,false},e))return false;
    for(unsigned i=0;i<7;++i){const float v=material_preset_value(copy,MaterialPresetProperty(i));if(!std::isfinite(v)||v<0||v>1)return fail(e,"Color or surface parameter outside [0,1]");}
    const auto has=[&](MaterialPresetFeature f){return std::find(a.requirements.begin(),a.requirements.end(),f)!=a.requirements.end();};
    if(copy.shadingModel!=MaterialShadingModel::StandardPBR && copy.shadingModel!=MaterialShadingModel::ClearCoat)
        return fail(e,"This preset schema release supports Standard PBR and Clear Coat models");
    if(copy.transmission==0&&!has(MaterialPresetFeature::Opaque))return fail(e,"Opaque capability is undeclared");
    if(copy.transmission>0&&!has(MaterialPresetFeature::SolidTransmission))return fail(e,"Transmission capability is undeclared");
    if(copy.transmission>0&&copy.roughness>0&&!has(MaterialPresetFeature::RoughTransmission))return fail(e,"Rough transmission capability is undeclared");
    if(copy.clearCoat>0&&!has(MaterialPresetFeature::ClearCoat))return fail(e,"Coat capability is undeclared");
    if(copy.normalMap&&!has(MaterialPresetFeature::NormalDetail))return fail(e,"Normal-detail capability is undeclared");
    return true;
}
bool write_material_preset(const std::filesystem::path& path,const MaterialPresetAsset& a,std::string* e){
    if(!validate_material_preset(a,e))return false;
    try{
        if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
        const auto tmp=std::filesystem::path(path.string()+".tmp");std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
        out<<"DVE_MATERIAL_PRESET "<<a.schema<<' '<<a.revision<<' '<<a.thumbnailRevision<<'\n';
        out<<std::quoted(a.id)<<' '<<std::quoted(a.name)<<' '<<std::quoted(a.family)<<' '<<std::quoted(a.tags)<<'\n';
        out<<std::quoted(a.description)<<' '<<std::quoted(a.provenance)<<' '<<std::quoted(a.mappingUnits)<<'\n';
        out<<a.requirements.size();for(auto f:a.requirements)out<<' '<<unsigned(f);out<<'\n';
        out<<a.dependencies.size();for(const auto& d:a.dependencies)out<<' '<<std::quoted(d);out<<'\n';
        auto values=EditorMaterialLibrary::make_minimal();if(!values.upsert({10,a.defaults,false},e)||!values.write_stream(out,e))return false;
        out.close();if(!out)return fail(e,"Failed to write preset asset");
#if defined(_WIN32)
        if(!MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return fail(e,"Could not replace preset asset");
#else
        std::error_code ec;std::filesystem::rename(tmp,path,ec);if(ec)return fail(e,ec.message());
#endif
        return true;
    }catch(const std::exception& ex){return fail(e,ex.what());}
}
std::optional<MaterialPresetAsset> read_material_preset(const std::filesystem::path& path,std::string* e){
    std::error_code ec;const auto bytes=std::filesystem::file_size(path,ec);
    if(ec||bytes>32U*1024U*1024U){fail(e,"Preset asset missing or exceeds 32 MiB");return {};}
    std::ifstream in(path,std::ios::binary);MaterialPresetAsset a;std::string magic;std::size_t count{};
    in>>magic>>a.schema>>a.revision>>a.thumbnailRevision;
    if(magic!="DVE_MATERIAL_PRESET"||a.schema!=1){fail(e,"Unsupported preset schema");return {};}
    in>>std::quoted(a.id)>>std::quoted(a.name)>>std::quoted(a.family)>>std::quoted(a.tags)>>std::quoted(a.description)>>std::quoted(a.provenance)>>std::quoted(a.mappingUnits);
    in>>count;if(!in||count>32){fail(e,"Invalid preset requirement count");return {};}
    for(std::size_t i=0;i<count;++i){unsigned feature{};in>>feature;a.requirements.push_back(MaterialPresetFeature(feature));}
    in>>count;if(!in||count>32){fail(e,"Invalid preset dependency count");return {};}
    for(std::size_t i=0;i<count;++i){std::string d;in>>std::quoted(d);a.dependencies.push_back(std::move(d));}
    auto values=EditorMaterialLibrary::read_stream(in,e);if(!values||!values->find(10))return {};
    a.defaults=values->find(10)->definition;if(!validate_material_preset(a,e))return {};
    in>>std::ws;if(!in.eof()){fail(e,"Trailing preset asset data");return {};}return a;
}
bool MaterialPresetCatalog::add(MaterialPresetAsset a,std::string* e){
    if(find(a.id))return fail(e,"Duplicate preset ID");
    if(!validate_material_preset(a,e))return false;
    entries_.push_back(std::move(a));return true;
}
const MaterialPresetAsset* MaterialPresetCatalog::find(std::string_view id)const{for(const auto& a:entries_)if(a.id==id)return &a;return nullptr;}
std::vector<const MaterialPresetAsset*> MaterialPresetCatalog::search(std::string_view q,std::string_view family)const{
    std::vector<const MaterialPresetAsset*> r;const auto query=lower(q);
    for(const auto& a:entries_)if((family.empty()||a.family==family)&&lower(a.name+" "+a.family+" "+a.tags).find(query)!=std::string::npos)r.push_back(&a);
    return r;
}
std::string_view material_preset_support_label(MaterialPresetSupport s){switch(s){case MaterialPresetSupport::Supported:return "Supported";case MaterialPresetSupport::Approximate:return "Approximate";default:return "Unavailable";}}
MaterialPresetCapability material_preset_capability(const MaterialPresetAsset& a,MaterialPresetBackend b){
#if !defined(DVE_EDITOR_MATERIAL_PREVIEW_RENDERER)
    if(b==MaterialPresetBackend::CpuReference || b==MaterialPresetBackend::CpuPlayer || b==MaterialPresetBackend::Gpu)
        return {MaterialPresetSupport::Unavailable,"Renderer disabled in this build; enable DVE_BUILD_RHI"};
#endif
    if(b==MaterialPresetBackend::CpuReference)return {MaterialPresetSupport::Supported,"CPU reference transport; artistic RGB defaults"};
    if(b==MaterialPresetBackend::CpuPlayer)return {MaterialPresetSupport::Approximate,"CPU reference transport exists; player dispatch and scene parity require acceptance"};
    if(b==MaterialPresetBackend::NativePreview)return {MaterialPresetSupport::Approximate,"Viewport uses color markers; inspect the CPU material preview for shading"};
    for(auto f:a.requirements)if(f==MaterialPresetFeature::SolidTransmission||f==MaterialPresetFeature::RoughTransmission)
        return {MaterialPresetSupport::Unavailable,"Solid/rough glass camera transport is not wired to the GPU path"};
    return {MaterialPresetSupport::Approximate,"GPU PBR foundations exist; this preset has not passed backend acceptance"};
}
VoxelMaterialDefinition instantiate_material_preset(const MaterialPresetAsset& a,const VoxelMaterialDefinition& from){
    auto out=a.defaults;physical(out,from);out.name=a.name;out.preset={a.id,a.revision,0};return out;
}
bool reset_material_preset_property(VoxelMaterialDefinition& m,const MaterialPresetCatalog& catalog,MaterialPresetProperty p,std::string* e){
    const auto* a=catalog.find(m.preset.id);
    if(!a||a->revision!=m.preset.revision)return fail(e,"Original preset revision unavailable; explicitly update before reset");
    if(!set_material_preset_value(m,p,material_preset_value(a->defaults,p),e))return false;
    m.preset.overrides&=~(std::uint64_t(1)<<unsigned(p));return true;
}
bool update_material_preset(VoxelMaterialDefinition& m,const MaterialPresetAsset& a,bool preserve,std::string* e){
    if(m.preset.id!=a.id)return fail(e,"Preset IDs differ");
    if(!validate_material_preset(a,e))return false;
    auto next=instantiate_material_preset(a,m);
    if(preserve)for(unsigned i=0;i<unsigned(MaterialPresetProperty::Count);++i)if(m.preset.overrides&(std::uint64_t(1)<<i))
        if(!set_material_preset_value(next,MaterialPresetProperty(i),material_preset_value(m,MaterialPresetProperty(i)),e))return false;
    m=std::move(next);return true;
}
MaterialPresetCatalog MaterialPresetCatalog::starter(){
    MaterialPresetCatalog c;
    auto add=[&](std::string id,std::string name,std::string family,Float3 color,float rough,float metal=0){
        MaterialPresetAsset a;a.id="dve."+id;a.name=name;a.family=family;a.tags=family+" "+name;
        a.defaults.name=name;a.defaults.baseColor={color.x,color.y,color.z,1};a.defaults.roughness=rough;a.defaults.metallic=metal;
        a.requirements={MaterialPresetFeature::Opaque};return a;
    };
    auto put=[&](MaterialPresetAsset a){std::string e;if(!c.add(std::move(a),&e))throw std::logic_error(e);};
    for(unsigned i=0;i<3;++i){
        auto a=add(i==0?"glass.clear":i==1?"glass.frosted":"glass.bottle-green",i==0?"Clear solid glass":i==1?"Frosted solid glass":"Green bottle glass","Glass",{1,1,1},i==1?.25F:0);
        a.tags+=" transparent refractive dielectric window bottle";a.defaults.transmission=1;a.defaults.transparent=true;
        a.requirements={MaterialPresetFeature::SolidTransmission};if(i==1)a.requirements.push_back(MaterialPresetFeature::RoughTransmission);
        if(i==2){a.defaults.attenuationColor={.18F,.65F,.25F};a.defaults.attenuationDistanceMeters=.1F;}
        a.description="Solid dielectric: use closed geometry; coverage stays 1. Tint comes from volume absorption.";put(std::move(a));
    }
    for(unsigned i=0;i<3;++i)put(add(i==0?"paint.matte":i==1?"paint.satin":"paint.gloss",i==0?"Matte wall paint":i==1?"Satin paint":"Gloss enamel","Paint",{.35F,.08F,.035F},i==0?.85F:i==1?.4F:.16F));
    auto car=add("paint.automotive-solid","Solid automotive paint","Paint",{.25F,.015F,.01F},.28F);
    car.tags+=" car lacquer clearcoat";car.defaults.shadingModel=MaterialShadingModel::ClearCoat;car.defaults.clearCoat=1;car.defaults.clearCoatRoughness=.08F;
    car.requirements.push_back(MaterialPresetFeature::ClearCoat);car.description="Solid pigment plus dielectric coat; no metallic flakes or pearl layer.";put(std::move(car));
    auto plaster=add("mineral.plaster","Plaster","Mineral",{.72F,.70F,.66F},.92F);
    // Original deterministic, seamless normal detail; 2 cm repeat, restrained tilt.
    std::vector<std::uint8_t> pixels(32*32*4);
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x){unsigned at=(y*32+x)*4;const float u=float(x)*6.2831853F/32,v=float(y)*6.2831853F/32;
        pixels[at]=std::uint8_t(128+12*std::cos(3*u)*std::sin(5*v));pixels[at+1]=std::uint8_t(128+12*std::sin(3*u)*std::cos(5*v));pixels[at+2]=254;pixels[at+3]=255;}
    plaster.defaults.normalMap=make_voxel_normal_map("DVE original plaster detail",32,32,std::move(pixels));
    plaster.defaults.normalMapTileMeters=.02F;plaster.defaults.normalMapStrength=.3F;plaster.requirements.push_back(MaterialPresetFeature::NormalDetail);plaster.dependencies={"embedded:normal"};
    plaster.description="Fine original bundled normal detail; no silhouette displacement.";put(std::move(plaster));
    put(add("mineral.ceramic-unglazed","Unglazed ceramic","Mineral",{.42F,.26F,.15F},.7F));
    const std::array<std::string,6> names{"Steel","Aluminum","Copper","Brass","Gold","Chrome"};
    const std::array<std::string,6> ids{"steel","aluminum","copper","brass","gold","chrome"};
    const std::array<Float3,6> colors{{{.56F,.57F,.58F},{.91F,.92F,.92F},{.95F,.64F,.54F},{.78F,.57F,.27F},{1,.77F,.34F},{.55F,.56F,.56F}}};
    for(unsigned i=0;i<6;++i){auto a=add("metal."+ids[i],names[i],"Metal",colors[i],i==5?.06F:.24F,1);a.description="Artistic linear RGB metallic reflectance; no measured complex-IOR fit or anisotropy.";put(std::move(a));}
    put(add("plastic.gloss","Gloss plastic","Plastic & rubber",{.025F,.08F,.25F},.16F));
    put(add("plastic.matte","Matte plastic","Plastic & rubber",{.025F,.08F,.25F},.65F));
    put(add("rubber.hard","Hard rubber","Plastic & rubber",{.015F,.018F,.02F},.9F));
    put(add("rubber.soft","Soft rubber","Plastic & rubber",{.025F,.025F,.025F},.98F));
    return c;
}
}
