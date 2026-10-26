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
    {MaterialPresetProperty::DetailStrength,"Detail strength","","Normal-map tilt; does not displace geometry",0,4,.05F,true},
    {MaterialPresetProperty::Pattern,"Surface pattern","","0 none, 1 wood, 2 leather, 3 paper, 4 mineral, 5 paint, 6 marble, 7 strata, 8 cellular stone",0,8,1,true},
    {MaterialPresetProperty::DetailSpace,"Detail mapping","","0 object metres, 1 world metres, 2 UV0, 3 UV1",0,3,1,false},
    {MaterialPresetProperty::PatternScale,"Pattern repeat","m","Physical grain/pores repeat; changing geometry size does not stretch it",.00001F,10000,.001F,false},
    {MaterialPresetProperty::PatternRotationX,"Grain turn X","rad","Rotate the complete procedural field about X",-10000,10000,.1F,true},
    {MaterialPresetProperty::PatternRotationY,"Grain turn Y","rad","Rotate the complete procedural field about Y",-10000,10000,.1F,false},
    {MaterialPresetProperty::PatternRotationZ,"Grain turn Z","rad","Rotate the complete procedural field about Z",-10000,10000,.1F,true},
    {MaterialPresetProperty::GrainStretch,"Grain stretch","","Length/width ratio along the grain axis",1,1000,1,false},
    {MaterialPresetProperty::ColorVariation,"Color variation","","Strength of the original procedural color map",0,1,.025F,false},
    {MaterialPresetProperty::RoughnessVariation,"Roughness variation","","Procedural roughness offset; coat roughness stays separate",0,1,.025F,false},
    {MaterialPresetProperty::BumpHeight,"Bump height","m","Physical height used for shading normals; no silhouette displacement",0,2500,.00001F,false},
    {MaterialPresetProperty::PatternSeed,"Pattern seed","","Deterministic integer seed; editor range uses exactly representable floats",0,16777215,1,true},
    {MaterialPresetProperty::UvTileMeters,"UV tile size","m","Metric size assigned to one UV tile; UV mapping can stretch if UVs are stretched",.00001F,10000,.1F,true},
    {MaterialPresetProperty::ScatterDistance,"Scatter distance","m","Scattering length in closed subsurface geometry",0,1,0.0005F,false},
    {MaterialPresetProperty::ScatterR,"Scatter albedo R","linear","Red channel scattering albedo",0,1,0.025F,false},
    {MaterialPresetProperty::ScatterG,"Scatter albedo G","linear","Green channel scattering albedo",0,1,0.025F,false},
    {MaterialPresetProperty::ScatterB,"Scatter albedo B","linear","Blue channel scattering albedo",0,1,0.025F,false},
    {MaterialPresetProperty::SheenR,"Sheen R","linear","Velvet or fine-fiber reflection color",0,1,0.025F,false},
    {MaterialPresetProperty::SheenG,"Sheen G","linear","Velvet or fine-fiber reflection color",0,1,0.025F,false},
    {MaterialPresetProperty::SheenB,"Sheen B","linear","Velvet or fine-fiber reflection color",0,1,0.025F,false},
    {MaterialPresetProperty::SheenRoughness,"Sheen roughness","","Spread of the Charlie sheen lobe",0,1,0.025F,false},
    {MaterialPresetProperty::Iridescence,"Thin-film weight","","Blend ordinary Fresnel with thin-film reflection",0,1,0.05F,false},
    {MaterialPresetProperty::FilmIor,"Film IOR","","Refractive index of the film itself",1,3,0.025F,false},
    {MaterialPresetProperty::FilmThickness,"Film thickness","nm","Physical thin-film thickness; zero removes the film",0,100000,10.0F,false},
    {MaterialPresetProperty::TextureTile,"Texture repeat","m","One repeat of the texture set (colour, ORM, height, emission maps)",.0001F,10000,.05F,false},
    {MaterialPresetProperty::TextureSharpness,"Texture blend","","Triplanar blend exponent: higher keeps each face's projection crisper",1,64,1,true},
    {MaterialPresetProperty::TextureAntiTiling,"Anti-tiling","tiles","Randomly offset tiles blended across this band; 0 shows the plain repeat",0,.5F,.05F,false},
    {MaterialPresetProperty::DisplacementDepth,"Displacement depth","m","How deep the height map or surface pattern carves voxel faces (capped at half a voxel); 0 is off",0,10,.005F,false},
    {MaterialPresetProperty::DisplacementMode,"Displacement mode","","0 traced (real relief: outlines, shadows), 1 parallax (shading only)",0,1,1,false}
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
    case MaterialPresetProperty::Pattern:return float(m.surfaceDetail.pattern);
    case MaterialPresetProperty::DetailSpace:return float(m.surfaceDetail.space);
    case MaterialPresetProperty::PatternSeed:return float(m.surfaceDetail.seed);
    VALUE(PatternScale,surfaceDetail.periodMeters) VALUE(PatternRotationX,surfaceDetail.rotationRadians.x) VALUE(PatternRotationY,surfaceDetail.rotationRadians.y) VALUE(PatternRotationZ,surfaceDetail.rotationRadians.z)
    VALUE(GrainStretch,surfaceDetail.grainStretch) VALUE(ColorVariation,surfaceDetail.colorVariation) VALUE(RoughnessVariation,surfaceDetail.roughnessVariation) VALUE(BumpHeight,surfaceDetail.bumpHeightMeters) VALUE(UvTileMeters,surfaceDetail.uvTileMeters)
VALUE(ScatterDistance,subsurfaceScatterDistanceMeters) VALUE(ScatterR,subsurfaceColor.x) VALUE(ScatterG,subsurfaceColor.y) VALUE(ScatterB,subsurfaceColor.z) VALUE(SheenR,sheenColor.x) VALUE(SheenG,sheenColor.y) VALUE(SheenB,sheenColor.z) VALUE(SheenRoughness,sheenRoughness) VALUE(Iridescence,iridescence) VALUE(FilmIor,iridescenceIor) VALUE(FilmThickness,iridescenceThicknessNm)
    VALUE(TextureTile,textureTileMeters) VALUE(TextureSharpness,textureSharpness) VALUE(TextureAntiTiling,textureAntiTiling) VALUE(DisplacementDepth,displacementDepthMeters)
    case MaterialPresetProperty::DisplacementMode:return float(m.displacementMode);
#undef VALUE
    default:return 0;
    }
}
bool set_material_preset_value(VoxelMaterialDefinition& m,MaterialPresetProperty p,float v,std::string* e){
    const auto i=unsigned(p); if(i>=controls.size()||!std::isfinite(v)||v<controls[i].minimum||v>controls[i].maximum)
        return fail(e,"Value outside the displayed finite range");
    auto edited=m;
    const bool integer=p==MaterialPresetProperty::Pattern||p==MaterialPresetProperty::DetailSpace||p==MaterialPresetProperty::PatternSeed||p==MaterialPresetProperty::DisplacementMode;
    if(integer&&std::floor(v)!=v)return fail(e,"This control requires an integer");
    switch(p){
#define SET(P,V) case MaterialPresetProperty::P:edited.V=v;break;
    SET(ColorR,baseColor.x) SET(ColorG,baseColor.y) SET(ColorB,baseColor.z)
    SET(Roughness,roughness) SET(Metallic,metallic) SET(Specular,specular) SET(Coverage,baseColor.w)
    SET(Transmission,transmission) SET(Ior,indexOfRefraction)
    SET(AbsorptionR,attenuationColor.x) SET(AbsorptionG,attenuationColor.y) SET(AbsorptionB,attenuationColor.z)
    SET(AbsorptionDistance,attenuationDistanceMeters) SET(Coat,clearCoat) SET(CoatRoughness,clearCoatRoughness)
    SET(DetailScale,normalMapTileMeters) SET(DetailStrength,normalMapStrength)
    case MaterialPresetProperty::Pattern:edited.surfaceDetail.pattern=SurfacePattern(unsigned(v));break;
    case MaterialPresetProperty::DetailSpace:edited.surfaceDetail.space=SurfaceDetailSpace(unsigned(v));break;
    case MaterialPresetProperty::PatternSeed:edited.surfaceDetail.seed=static_cast<std::uint32_t>(v);break;
    SET(PatternScale,surfaceDetail.periodMeters) SET(PatternRotationX,surfaceDetail.rotationRadians.x) SET(PatternRotationY,surfaceDetail.rotationRadians.y) SET(PatternRotationZ,surfaceDetail.rotationRadians.z)
    SET(GrainStretch,surfaceDetail.grainStretch) SET(ColorVariation,surfaceDetail.colorVariation) SET(RoughnessVariation,surfaceDetail.roughnessVariation) SET(BumpHeight,surfaceDetail.bumpHeightMeters) SET(UvTileMeters,surfaceDetail.uvTileMeters)
SET(ScatterDistance,subsurfaceScatterDistanceMeters) SET(ScatterR,subsurfaceColor.x) SET(ScatterG,subsurfaceColor.y) SET(ScatterB,subsurfaceColor.z) SET(SheenR,sheenColor.x) SET(SheenG,sheenColor.y) SET(SheenB,sheenColor.z) SET(SheenRoughness,sheenRoughness) SET(Iridescence,iridescence) SET(FilmIor,iridescenceIor) SET(FilmThickness,iridescenceThicknessNm)
    SET(TextureTile,textureTileMeters) SET(TextureSharpness,textureSharpness) SET(TextureAntiTiling,textureAntiTiling) SET(DisplacementDepth,displacementDepthMeters)
    case MaterialPresetProperty::DisplacementMode:edited.displacementMode=static_cast<std::uint8_t>(v);break;
#undef SET
    default:return false;
    }
    if(!material_surface_detail_valid(edited.surfaceDetail))return fail(e,"Invalid detail settings: bump height must be at most a quarter of the repeat distance");
    // Clear coat and subsurface are alternative shading models: refuse an edit that would silently
    // replace the other one instead of dropping the preset's scattering (or the authored coat).
    if(p==MaterialPresetProperty::Coat&&v>0&&m.shadingModel==MaterialShadingModel::Subsurface)
        return fail(e,"Clear coat cannot be combined with subsurface scattering; set Scatter distance to 0 first");
    if(p==MaterialPresetProperty::ScatterDistance&&v>0&&m.shadingModel==MaterialShadingModel::ClearCoat&&m.clearCoat>0)
        return fail(e,"Subsurface scattering cannot be combined with a clear coat; set Coat to 0 first");
    m=std::move(edited);
    if(p==MaterialPresetProperty::Coverage)m.blendMode=v<1?MaterialBlendMode::Translucent:MaterialBlendMode::Opaque;
    if(p==MaterialPresetProperty::Coat&&m.shadingModel!=MaterialShadingModel::Subsurface)m.shadingModel=v>0?MaterialShadingModel::ClearCoat:MaterialShadingModel::StandardPBR;
    // Only toggle between Standard PBR and Subsurface; never overwrite a coat or another model.
    if(p==MaterialPresetProperty::ScatterDistance){
        if(v>0&&(m.shadingModel==MaterialShadingModel::StandardPBR||(m.shadingModel==MaterialShadingModel::ClearCoat&&m.clearCoat<=0)))m.shadingModel=MaterialShadingModel::Subsurface;
        else if(v<=0&&m.shadingModel==MaterialShadingModel::Subsurface)m.shadingModel=MaterialShadingModel::StandardPBR;
    }
    m.transparent=m.transmission>0||m.blendMode==MaterialBlendMode::Translucent;
    if(!m.preset.id.empty())m.preset.overrides|=std::uint64_t(1)<<i;
    return true;
}
bool validate_material_preset(const MaterialPresetAsset& a,std::string* e){
    if(a.schema!=1||a.revision==0||a.id.empty()||a.id.size()>128||a.name.empty()||a.family.empty()||a.provenance.empty()||a.mappingUnits!="meters")
        return fail(e,"Invalid preset identity, schema, revision, provenance or mapping units");
    if(a.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-_")!=std::string::npos)
        return fail(e,"Preset ID must be a stable lowercase identifier");
    for(auto feature:a.requirements)if(unsigned(feature)>unsigned(MaterialPresetFeature::Iridescence))return fail(e,"Unknown required shader feature");
    for(const auto& d:a.dependencies)if(d!="embedded:normal"||!a.defaults.normalMap)return fail(e,"Unresolved preset dependency");
    if(!a.defaults.preset.id.empty()||!a.defaults.layers.empty())return fail(e,"Preset defaults cannot reference instances or external material slots");
    auto copy=a.defaults; copy.name=a.name;
    EditorMaterialLibrary check;
    if(!check.upsert({10,copy,false},e))return false;
    for(unsigned i=0;i<7;++i){const float v=material_preset_value(copy,MaterialPresetProperty(i));if(!std::isfinite(v)||v<0||v>1)return fail(e,"Color or surface parameter outside [0,1]");}
    const auto has=[&](MaterialPresetFeature f){return std::find(a.requirements.begin(),a.requirements.end(),f)!=a.requirements.end();};
    if(copy.shadingModel!=MaterialShadingModel::StandardPBR && copy.shadingModel!=MaterialShadingModel::ClearCoat && copy.shadingModel!=MaterialShadingModel::Subsurface)
        return fail(e,"This preset schema supports Standard PBR, Clear Coat and Subsurface models");
    if(copy.transmission==0&&!has(MaterialPresetFeature::Opaque))return fail(e,"Opaque capability is undeclared");
    if(copy.transmission>0&&!has(MaterialPresetFeature::SolidTransmission))return fail(e,"Transmission capability is undeclared");
    if(copy.transmission>0&&copy.roughness>0&&!has(MaterialPresetFeature::RoughTransmission))return fail(e,"Rough transmission capability is undeclared");
    if(copy.clearCoat>0&&!has(MaterialPresetFeature::ClearCoat))return fail(e,"Coat capability is undeclared");
    if(copy.surfaceDetail.pattern!=SurfacePattern::NoPattern&&!has(MaterialPresetFeature::SurfaceDetail))return fail(e,"Surface-detail capability is undeclared");
    if(copy.shadingModel==MaterialShadingModel::Subsurface&&(copy.subsurfaceScatterDistanceMeters<=0||!has(MaterialPresetFeature::Subsurface)))return fail(e,"Subsurface capability and a positive scattering distance are required");
    if(material_has_sheen(copy)&&!has(MaterialPresetFeature::Sheen))return fail(e,"Sheen capability is undeclared");
    if(material_has_iridescence(copy)&&!has(MaterialPresetFeature::Iridescence))return fail(e,"Thin-film capability is undeclared");
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
    if(a.defaults.shadingModel==MaterialShadingModel::Subsurface || material_has_sheen(a.defaults) || material_has_iridescence(a.defaults))
        return {MaterialPresetSupport::Unavailable,"Subsurface, sheen and thin-film presets require CPU reference rendering; GPU parity is not implemented"};
    for(auto f:a.requirements)if(f==MaterialPresetFeature::RoughTransmission)
        return {MaterialPresetSupport::Unavailable,"GPU rough transmission is not implemented; use the CPU reference"};
    for(auto f:a.requirements)if(f==MaterialPresetFeature::SolidTransmission){
        const auto& m=a.defaults;
        if(m.transmission!=1||m.roughness!=0||m.metallic!=0||m.clearCoat!=0||m.emissive.x!=0||m.emissive.y!=0||m.emissive.z!=0||!m.layers.empty()||m.surfaceDetail.pattern!=SurfacePattern::NoPattern)
            return {MaterialPresetSupport::Unavailable,"GPU glass requires a clear, unlayered dielectric with full transmission"};
        return {MaterialPresetSupport::Approximate,"Live GPU clear polygon transport; closed geometry and untextured PBR receivers required; voxel/native viewport parity unverified"};
    }
    for(auto f:a.requirements)if(f==MaterialPresetFeature::SurfaceDetail)return {MaterialPresetSupport::Approximate,"Live GPU polygon pass supports metric color/roughness/bump; voxel and native viewport parity remain unverified"};
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
    if(preserve){
        // Period and bump have a coupled bound. Apply the remaining overrides first,
        // then validate the final pair rather than rejecting a valid intermediate edit.
        const auto bumpBit=std::uint64_t(1)<<unsigned(MaterialPresetProperty::BumpHeight);
        const float bump=(m.preset.overrides&bumpBit)?m.surfaceDetail.bumpHeightMeters:next.surfaceDetail.bumpHeightMeters;
        next.surfaceDetail.bumpHeightMeters=0;
        for(unsigned i=0;i<unsigned(MaterialPresetProperty::Count);++i)
            if(i!=unsigned(MaterialPresetProperty::BumpHeight)&&(m.preset.overrides&(std::uint64_t(1)<<i)))
                if(!set_material_preset_value(next,MaterialPresetProperty(i),material_preset_value(m,MaterialPresetProperty(i)),e))return false;
        next.surfaceDetail.bumpHeightMeters=bump;
        if(!material_surface_detail_valid(next.surfaceDetail))return fail(e,"Updated scale and bump are incompatible");
        if(m.preset.overrides&bumpBit)next.preset.overrides|=bumpBit;
    }
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
    auto detail=[&](std::string id,std::string name,std::string family,Float3 color,float rough,SurfacePattern pattern,float repeat,float bump,float variation){
        auto a=add(std::move(id),std::move(name),std::move(family),color,rough);a.defaults.surfaceDetail.pattern=pattern;a.defaults.surfaceDetail.periodMeters=repeat;
        a.defaults.surfaceDetail.bumpHeightMeters=bump;a.defaults.surfaceDetail.colorVariation=variation;a.requirements.push_back(MaterialPresetFeature::SurfaceDetail);
        a.description="Original bundled procedural color, roughness and physical-scale bump; object-attached by default. No geometric displacement.";return a;
    };
    auto coat=[&](MaterialPresetAsset& a,float rough){a.defaults.shadingModel=MaterialShadingModel::ClearCoat;a.defaults.clearCoat=1;a.defaults.clearCoatRoughness=rough;a.requirements.push_back(MaterialPresetFeature::ClearCoat);};
    for(unsigned i=0;i<3;++i){auto a=detail(i==0?"wood.raw":i==1?"wood.sanded":"wood.varnished",i==0?"Raw wood":i==1?"Sanded wood":"Varnished wood","Wood",{.42F,.20F,.065F},i==0?.78F:i==1?.6F:.45F,SurfacePattern::Wood,.006F,i==0?.00004F:.00001F,.45F);
        a.defaults.surfaceDetail.grainStretch=35;a.tags+=" grain timber end-grain board";if(i==2)coat(a,.08F);put(std::move(a));}
    for(unsigned i=0;i<2;++i){auto a=detail(i==0?"leather.smooth":"leather.pebbled",i==0?"Smooth leather":"Pebbled leather","Leather",{.15F,.055F,.025F},i==0?.5F:.65F,SurfacePattern::Leather,i==0?.0015F:.004F,i==0?.000008F:.00008F,.15F);put(std::move(a));}
    for(unsigned i=0;i<2;++i){auto a=detail(i==0?"paper.kraft":"paper.cardboard",i==0?"Opaque kraft paper":"Cardboard","Paper & board",{.45F,.29F,.13F},.88F,SurfacePattern::Paper,i==0?.001F:.0025F,.000007F,.2F);
        a.description="Opaque fiber surface. Backlit thin-sheet transmission is not implemented; corrugated cut edges require geometry.";put(std::move(a));}
    put(detail("mineral.chalk","Chalk","Mineral",{.74F,.73F,.7F},.98F,SurfacePattern::Mineral,.0008F,.000004F,.06F));
    put(detail("mineral.concrete","Concrete surface","Mineral",{.35F,.36F,.37F},.87F,SurfacePattern::Mineral,.01F,.00015F,.25F));
    auto ceramic=detail("mineral.ceramic-glazed","Glazed ceramic","Mineral",{.55F,.65F,.7F},.5F,SurfacePattern::Mineral,.002F,.000001F,.05F);coat(ceramic,.08F);put(std::move(ceramic));
    // Solid-texture stone: the pattern continues through the volume, so cuts and craters show it;
    // with a displacement depth it is also the height that carves the faces.
    const auto stone=[&](std::string id,std::string name,Float3 color,float rough,SurfacePattern pattern,float repeat,float bump,float variation,float stretch,const char* tags){
        auto a=detail(std::move(id),std::move(name),"Stone",color,rough,pattern,repeat,bump,variation);a.defaults.surfaceDetail.grainStretch=stretch;a.tags+=tags;
        a.description="Original procedural solid texture: color, roughness and bump continue through the volume. Set a displacement depth to carve it into voxel faces.";return a;
    };
    put(stone("stone.marble","Polished marble",{.80F,.79F,.76F},.3F,SurfacePattern::Marble,.25F,.00002F,.35F,24," veined calcite polished solid"));
    put(stone("stone.sandstone","Layered sandstone",{.62F,.45F,.30F},.9F,SurfacePattern::Strata,.08F,.002F,.25F,40," strata sedimentary layered cliff solid"));
    put(stone("stone.cobble","Cobblestone",{.42F,.41F,.40F},.85F,SurfacePattern::Cellular,.2F,.02F,.3F,20," cobbles flagstone voronoi pavement solid"));
    auto lacquer=detail("paint.lacquer","Lacquer","Paint",{.18F,.02F,.01F},.25F,SurfacePattern::Paint,.002F,.000002F,.02F);coat(lacquer,.05F);put(std::move(lacquer));
    for(unsigned i=0;i<3;++i){auto a=add(i==0?"glass.amber":i==1?"glass.smoked":"liquid.water",i==0?"Amber bottle glass":i==1?"Smoked glass":"Clear water",i==2?"Liquid":"Glass",{1,1,1},0);
        a.tags+=" refractive transparent dielectric";a.defaults.transmission=1;a.defaults.transparent=true;a.defaults.indexOfRefraction=i==2?1.333F:1.5F;
        a.defaults.attenuationColor=i==0?Float3{.55F,.25F,.06F}:i==1?Float3{.3F,.3F,.3F}:Float3{.8F,.95F,.98F};a.defaults.attenuationDistanceMeters=i==2?5:.1F;
        a.requirements={MaterialPresetFeature::SolidTransmission};a.description="Closed-volume dielectric. Full coverage; distance-based absorption. Water requires a closed volume; this is not an open water-plane shader.";put(std::move(a));}
    for(unsigned i=0;i<2;++i){
        auto a=add(i?"wax.beeswax":"wax.paraffin",i?"Warm beeswax":"Ivory candle wax","Wax",i?Float3{.72F,.39F,.09F}:Float3{.82F,.77F,.62F},.32F);
        a.defaults.shadingModel=MaterialShadingModel::Subsurface;a.defaults.subsurfaceScatterDistanceMeters=i?.004F:.008F;
        a.defaults.subsurfaceColor=i?Float3{.9F,.65F,.24F}:Float3{.95F,.9F,.75F};a.requirements.push_back(MaterialPresetFeature::Subsurface);
        a.tags+=" candle translucent scattering";a.description="Closed-volume wax: metric subsurface diffusion and thickness-dependent backlighting. Not a flame or melting simulation.";put(std::move(a));
    }
    for(unsigned i=0;i<3;++i){
        const std::array<Float3,3> pigment{{{.56F,.26F,.16F},{.28F,.105F,.055F},{.085F,.028F,.015F}}};
        const std::array<const char*,3> ids{"light","medium","deep"},names{"Light skin","Medium skin","Deep skin"};
        auto a=add(std::string("skin.")+ids[i],names[i],"Skin",pigment[i],.46F);
        a.defaults.shadingModel=MaterialShadingModel::Subsurface;a.defaults.subsurfaceScatterDistanceMeters=.0015F;
        a.defaults.subsurfaceColor={.85F,.42F,.25F};a.defaults.surfaceDetail.pattern=SurfacePattern::Leather;
        a.defaults.surfaceDetail.periodMeters=.0007F;a.defaults.surfaceDetail.bumpHeightMeters=.000003F;
        a.defaults.surfaceDetail.colorVariation=.035F;a.defaults.surfaceDetail.roughnessVariation=.04F;
        a.requirements.push_back(MaterialPresetFeature::Subsurface);a.requirements.push_back(MaterialPresetFeature::SurfaceDetail);
        a.tags+=" organic pores flesh scattering";a.description="Artistic skin tone with metric pore detail and subsurface scattering. Single scattering-color model, not a measured multilayer skin fit.";put(std::move(a));
    }
    for(unsigned i=0;i<2;++i){
        auto a=add(i?"bubble.soap-thick":"bubble.soap-thin",i?"Soap bubble 650 nm":"Soap bubble 350 nm","Bubble",{1,1,1},0);
        a.defaults.transmission=1;a.defaults.transparent=true;a.defaults.indexOfRefraction=1;
        a.defaults.iridescence=1;a.defaults.iridescenceIor=1.333F;a.defaults.iridescenceThicknessNm=i?650.F:350.F;
        a.requirements={MaterialPresetFeature::SolidTransmission,MaterialPresetFeature::Iridescence};
        a.tags+=" soap film interference transparent";a.description="Air interior, water film: colored Fresnel reflection and complementary transmission without glass-sphere bending. Each surface crossing represents one film; use a single closed shell. No drainage or bubble simulation.";put(std::move(a));
    }
    auto foam=add("foam.dense","Dense white foam","Foam",{.84F,.86F,.81F},.9F);
    foam.defaults.shadingModel=MaterialShadingModel::Subsurface;foam.defaults.subsurfaceScatterDistanceMeters=.002F;foam.defaults.subsurfaceColor={.94F,.95F,.91F};
    foam.defaults.surfaceDetail.pattern=SurfacePattern::Mineral;foam.defaults.surfaceDetail.periodMeters=.0015F;foam.defaults.surfaceDetail.bumpHeightMeters=.00006F;
    foam.defaults.surfaceDetail.colorVariation=.06F;foam.defaults.surfaceDetail.roughnessVariation=.06F;
    foam.requirements.push_back(MaterialPresetFeature::Subsurface);foam.requirements.push_back(MaterialPresetFeature::SurfaceDetail);
    foam.tags+=" froth shaving porous scattering";foam.description="Opaque effective scattering material for dense foam, with metric surface breakup. Individual cells, silhouettes, wet froth and fluid motion require geometry or simulation.";put(std::move(foam));
    return c;
}
}
