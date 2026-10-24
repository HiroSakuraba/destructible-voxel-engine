#include "dve/editor_material_bake.hpp"
#include "dve/editor_document.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace dve::editor {
namespace {
void settings_io(std::istream& in,render::MaterialTextureBakeSettings& s){
    in>>s.materialIndex>>s.targetUv>>s.resolution>>s.samplesPerAxis>>s.paddingPixels>>s.maps>>s.memoryBudgetBytes>>s.metersPerUnit
      >>s.pose.position.x>>s.pose.position.y>>s.pose.position.z>>s.pose.rotation.x>>s.pose.rotation.y>>s.pose.rotation.z>>s.pose.rotation.w;
}
std::string signature(const render::MaterialTextureBakeSettings& s){return render::encode_material_bake_settings(s);}
std::string historical_signature(const render::MaterialTextureBakeSettings& s){
    render::MaterialTextureBakeSettings old;
    old.materialIndex=s.materialIndex;old.targetUv=s.targetUv;old.resolution=s.resolution;old.samplesPerAxis=s.samplesPerAxis;old.paddingPixels=s.paddingPixels;old.maps=s.maps;old.memoryBudgetBytes=s.memoryBudgetBytes;old.metersPerUnit=s.metersPerUnit;old.pose=s.pose;
    if(signature(old)!=signature(s))return signature(s);
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(9);
    out<<s.materialIndex<<' '<<s.targetUv<<' '<<s.resolution<<' '<<s.samplesPerAxis<<' '<<s.paddingPixels<<' '<<s.maps<<' '<<s.memoryBudgetBytes<<' '<<s.metersPerUnit<<' '
       <<s.pose.position.x<<' '<<s.pose.position.y<<' '<<s.pose.position.z<<' '<<s.pose.rotation.x<<' '<<s.pose.rotation.y<<' '<<s.pose.rotation.z<<' '<<s.pose.rotation.w<<'\n';return out.str();
}
std::uint64_t hash(std::string_view s){std::uint64_t h=14695981039346656037ULL;for(unsigned char c:s){h^=c;h*=1099511628211ULL;}return h;}

// Preserve the upstream ordered hash combination; equal source/active hashes do not cancel.
std::uint64_t combine(std::uint64_t seed,std::uint64_t value){return seed^(value+0x9e3779b97f4a7c15ULL+(seed<<6)+(seed>>2));}

}
void remap_material_bake_ownership(EditorObject& object,std::uint64_t id){
    if(!object.materialBake)return;
    auto& r=*object.materialBake;
    const bool current=polygon_asset_content_hash(r.source)==r.bakedSourceHash;
    r.source.objectId=id;r.source.contentHash=polygon_asset_content_hash(r.source);
    r.baked.objectId=id;r.baked.contentHash=polygon_asset_content_hash(r.baked);r.bakedAssetHash=r.baked.contentHash;
    if(current)r.bakedSourceHash=r.source.contentHash;
}
std::uint64_t material_bake_recipe_hash(const MaterialBakeRecipe& r){
    std::ostringstream out;out.imbue(std::locale::classic());out<<r.showingSource<<' '<<polygon_asset_content_hash(r.source)<<' '<<polygon_asset_content_hash(r.baked)<<' '<<r.bakedSourceHash<<' '<<r.bakedAssetHash<<' '<<historical_signature(r.settings)<<historical_signature(r.bakedSettings)<<r.outputDirectory.generic_string();
    if(r.sourceTopology)out<<modeling_topology_hash(*r.sourceTopology);
    if(r.bakedTopology)out<<modeling_topology_hash(*r.bakedTopology);
    if(!r.scene.empty())out<<render::material_bake_scene_hash(r.scene);
    return hash(out.str());
}
MaterialBakeStatus material_bake_status(const EditorObject& object,const RigidTransform& pose,const render::MaterialBakeScene* liveScene,const RenderEnvironment* liveLighting){
    MaterialBakeStatus result;if(!object.materialBake||!object.polygon){result.reason="No saved bake recipe";return result;}
    const auto& r=*object.materialBake;auto settings=r.settings;settings.pose=pose;settings.scale=object.voxelScale;
    const auto sourceHash=polygon_asset_content_hash(r.showingSource?*object.polygon:r.source);
    const auto activeHash=polygon_asset_content_hash(*object.polygon);
    result.key=combine(combine(combine(combine(0x6d62616b65ULL,sourceHash),activeHash),hash(signature(settings))),material_bake_recipe_hash(r));
    const auto* mirror=find_component_by_type(std::span<const Component>(object.components),kLiveMirrorComponent);
    if(object.subdivision||object.is_nurbs()||(mirror&&mirror->enabled))result.reason="Stale: apply geometry modifiers before rebaking";
    else if(sourceHash!=r.bakedSourceHash)result.reason="Stale: editable source changed";
    else if(signature(settings)!=signature(r.bakedSettings))result.reason="Stale: recipe or world pose changed";
    else if(!r.showingSource&&activeHash!=r.bakedAssetHash)result.reason="Stale: assigned baked mesh was edited";
    else if(liveScene&&render::material_bake_scene_hash(*liveScene)!=render::material_bake_scene_hash(r.scene))result.reason="Stale: neighbouring bake geometry changed";
    else if(liveLighting&&(r.settings.maps&64U)){auto current=settings;current.lighting=*liveLighting;result.reason=signature(current)==signature(r.bakedSettings)?"Bake is current":"Stale: scene light rig changed";}
    else result.reason="Bake is current";
    result.stale=result.reason!="Bake is current";return result;
}
bool validate_material_bake_recipe(const MaterialBakeRecipe& r,std::string* error){
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    if(!render::valid_material_bake_settings(r.settings)||!render::valid_material_bake_settings(r.bakedSettings))return fail("Invalid material bake recipe settings");
    if(!validate_polygon_asset(r.source)||!validate_polygon_asset(r.baked)||r.settings.materialIndex>=256||r.bakedSettings.materialIndex>=256)return fail("Invalid material bake snapshots or slot");
    if(!r.bakedSourceHash||r.bakedAssetHash!=polygon_asset_content_hash(r.baked))return fail("Invalid baked snapshot hash");
    if(r.scene.size()>4096)return fail("Too many bake scene snapshots");
    for(const auto& m:r.scene){auto settings=r.settings;settings.pose=m.pose;if(!render::valid_material_bake_settings(settings)||!validate_polygon_asset(m.asset))return fail("Invalid bake scene snapshot");}
    if(r.sourceTopology&&!validate_modeling_topology(r.source,nullptr,*r.sourceTopology,error))return false;
    if(r.bakedTopology&&!validate_modeling_topology(r.baked,nullptr,*r.bakedTopology,error))return false;
    return true;
}
bool write_material_bake_recipe(const std::filesystem::path& path,const MaterialBakeRecipe& r,std::string* error){
    if(!validate_material_bake_recipe(r,error))return false;
    std::error_code ec;std::filesystem::create_directories(path,ec);if(ec){if(error)*error=ec.message();return false;}
    if(!write_dmesh(path/"source.dmesh",r.source,error)||!write_dmesh(path/"baked.dmesh",r.baked,error))return false;
    std::uint64_t total=0;
    for(std::size_t n=0;n<r.scene.size();++n){const auto file=path/("scene-"+std::to_string(n)+".dmesh");if(!write_dmesh(file,r.scene[n].asset,error))return false;const auto bytes=std::filesystem::file_size(file,ec);if(ec||bytes>512ULL*1024*1024-total){if(error)*error="Bake scene exceeds snapshot budget";return false;}total+=bytes;}
    for(auto name:{"source.dmesh","baked.dmesh"}){auto bytes=std::filesystem::file_size(path/name,ec);if(ec||bytes>512ULL*1024*1024-total){if(error)*error="Bake snapshots exceed 512 MiB scene budget";return false;}total+=bytes;}
    std::ofstream out(path/"recipe.txt",std::ios::binary);out.imbue(std::locale::classic());out<<std::setprecision(9)<<"DVE_MATERIAL_BAKE 2\n"<<r.showingSource<<' '<<r.bakedSourceHash<<' '<<r.bakedAssetHash<<' '<<std::quoted(r.outputDirectory.generic_string())<<'\n';
    out<<std::quoted(signature(r.settings))<<'\n'<<std::quoted(signature(r.bakedSettings))<<'\n';out<<bool(r.sourceTopology)<<' '<<bool(r.bakedTopology)<<'\n';
    if(r.sourceTopology)write_modeling_topology(out,*r.sourceTopology);
    if(r.bakedTopology)write_modeling_topology(out,*r.bakedTopology);
    out<<r.scene.size()<<'\n';for(const auto& m:r.scene)out<<m.pose.position.x<<' '<<m.pose.position.y<<' '<<m.pose.position.z<<' '<<m.pose.rotation.x<<' '<<m.pose.rotation.y<<' '<<m.pose.rotation.z<<' '<<m.pose.rotation.w<<'\n';
    out.close();if(!out){if(error)*error="Could not write bake recipe";return false;}return true;
}
std::optional<MaterialBakeRecipe> read_material_bake_recipe(const std::filesystem::path& path,std::string* error){
    auto fail=[&](std::string message)->std::optional<MaterialBakeRecipe>{if(error)*error=std::move(message);return {};};
    std::error_code ec;const auto size=std::filesystem::file_size(path/"recipe.txt",ec);if(ec||size>32ULL*1024*1024)return fail("Missing or oversized bake recipe");
    std::uint64_t total=0;for(auto name:{"source.dmesh","baked.dmesh"}){auto bytes=std::filesystem::file_size(path/name,ec);if(ec||bytes>512ULL*1024*1024-total)return fail("Bake snapshots exceed 512 MiB scene budget");total+=bytes;}
    std::ifstream in(path/"recipe.txt",std::ios::binary);in.imbue(std::locale::classic());MaterialBakeRecipe r;std::string magic,output;unsigned version{},show{},sourceTopology{},bakedTopology{};
    in>>magic>>version>>show>>r.bakedSourceHash>>r.bakedAssetHash>>std::quoted(output);
    if(version==1){settings_io(in,r.settings);settings_io(in,r.bakedSettings);}
    else if(version==2){std::string current,baked;in>>std::quoted(current)>>std::quoted(baked);if(!render::decode_material_bake_settings(current,r.settings)||!render::decode_material_bake_settings(baked,r.bakedSettings))return fail("Invalid extended bake settings");}
    in>>sourceTopology>>bakedTopology;
    if(!in||magic!="DVE_MATERIAL_BAKE"||(version!=1&&version!=2)||show>1||sourceTopology>1||bakedTopology>1)return fail("Malformed bake recipe");
    r.showingSource=show!=0;r.outputDirectory=output;
    auto source=read_dmesh(path/"source.dmesh",512ULL*1024*1024),baked=read_dmesh(path/"baked.dmesh",512ULL*1024*1024);if(!source||!baked)return fail("Could not read bake snapshots");r.source=std::move(source.asset);r.baked=std::move(baked.asset);
    if(sourceTopology){r.sourceTopology=read_modeling_topology(in,error);if(!r.sourceTopology)return {};}
    if(bakedTopology){r.bakedTopology=read_modeling_topology(in,error);if(!r.bakedTopology)return {};}
    if(version==2){unsigned count{};if(!(in>>count)||count>4096)return fail("Invalid bake scene count");r.scene.resize(count);
        for(unsigned n=0;n<count;++n){auto& m=r.scene[n];in>>m.pose.position.x>>m.pose.position.y>>m.pose.position.z>>m.pose.rotation.x>>m.pose.rotation.y>>m.pose.rotation.z>>m.pose.rotation.w;
            auto file=path/("scene-"+std::to_string(n)+".dmesh");const auto bytes=std::filesystem::file_size(file,ec);if(!in||ec||bytes>512ULL*1024*1024-total)return fail("Invalid or oversized bake scene snapshot");total+=bytes;
            auto mesh=read_dmesh(file,512ULL*1024*1024);if(!mesh)return fail("Cannot read bake scene mesh");m.asset=std::move(mesh.asset);
            auto check=r.settings;check.pose=m.pose;if(!render::valid_material_bake_settings(check))return fail("Invalid bake scene pose");
        }
    }
    in>>std::ws;if(!in.eof())return fail("Trailing bake recipe data");if(!validate_material_bake_recipe(r,error))return {};return r;
}
}
