#include "dve/render/material_texture_bake.hpp"
#include <algorithm>
#include <utility>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <iomanip>
#include <locale>
#include <sstream>
#include "dve/transform.hpp"
namespace dve::render {
bool material_bake_selects(const MaterialTextureBakeSettings& s,std::uint32_t slot){
    return s.allAssignedSlots|| (s.materialSlots.empty()?slot==s.materialIndex:std::find(s.materialSlots.begin(),s.materialSlots.end(),slot)!=s.materialSlots.end());
}
namespace {
void vector_io(std::ostream& out,Float3 a){out<<a.x<<' '<<a.y<<' '<<a.z<<' ';}
void vector_io(std::istream& in,Float3& a){in>>a.x>>a.y>>a.z;}
bool finite(Float3 a){return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z);}
}
bool valid_material_bake_settings(const MaterialTextureBakeSettings& s){
    const auto q=s.pose.rotation;const float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    if(s.materialIndex>=256||s.materialSlots.size()>256||!std::is_sorted(s.materialSlots.begin(),s.materialSlots.end())||std::adjacent_find(s.materialSlots.begin(),s.materialSlots.end())!=s.materialSlots.end()||(!s.materialSlots.empty()&&s.materialSlots.back()>=256))return false;
    if(s.targetUv>1||(s.generateAtlas&&s.targetUv!=1)||s.resolution<8||s.resolution>4096||s.samplesPerAxis<1||s.samplesPerAxis>4||s.paddingPixels>64||s.paddingPixels>=s.resolution/2||!s.maps||(s.maps&~127U)||!s.memoryBudgetBytes)return false;
    if(!finite(s.scale)||s.scale.x<=0||s.scale.y<=0||s.scale.z<=0)return false;
    if(!std::isfinite(s.texelsPerMeter)||s.texelsPerMeter<0||s.texelsPerMeter>100000||(!s.generateAtlas&&s.texelsPerMeter>0)||s.raySamples<1||s.raySamples>256)return false;
    if(!(s.metersPerUnit>0)||!std::isfinite(s.metersPerUnit)||!finite(s.pose.position)||!std::isfinite(norm)||std::abs(norm-1)>.001F)return false;
    if(!(s.aoDistanceMeters>0)||!std::isfinite(s.aoDistanceMeters)||!(s.rayBiasMeters>0)||!std::isfinite(s.rayBiasMeters)||s.rayBiasMeters>=s.aoDistanceMeters||!(s.lightingRange>0)||!std::isfinite(s.lightingRange))return false;
    return encode_scene_lighting(s.lighting).size()<=2U*1024*1024&&s.lighting.validate()&&s.lighting.pathTraceBounces<=8&&s.lighting.pathTraceSamples<=64&&s.lighting.localLightSamples<=16;
}
std::string encode_material_bake_settings(const MaterialTextureBakeSettings& s){
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17);
    out<<s.materialIndex<<' '<<s.targetUv<<' '<<s.resolution<<' '<<s.samplesPerAxis<<' '<<s.paddingPixels<<' '<<s.maps<<' '<<s.memoryBudgetBytes<<' '<<s.metersPerUnit<<' ';
    vector_io(out,s.pose.position);out<<s.pose.rotation.x<<' '<<s.pose.rotation.y<<' '<<s.pose.rotation.z<<' '<<s.pose.rotation.w<<' ';
    vector_io(out,s.scale);out<<s.allAssignedSlots<<' '<<s.generateAtlas<<' '<<s.texelsPerMeter<<' '<<s.raySamples<<' '<<s.seed<<' '<<s.aoDistanceMeters<<' '<<s.rayBiasMeters<<' '<<s.lightingRange<<' '<<s.materialSlots.size()<<' ';
    for(auto slot:s.materialSlots)out<<slot<<' ';
    const auto& e=s.lighting;vector_io(out,e.sunDirection);vector_io(out,e.sunColor);vector_io(out,e.skyColor);vector_io(out,e.groundColor);
    out<<e.sunIntensity<<' '<<unsigned(e.shadowMode)<<' '<<e.shadowStrength<<' '<<e.shadowSoftnessRadians<<' '<<e.shadowSamples<<' '<<e.shadowMaxDistanceMeters<<' '<<e.contactShadowDistanceMeters<<' '<<e.shadowBiasMeters<<' '<<unsigned(e.globalIlluminationMode)<<' '<<e.globalIlluminationIntensity<<' '<<e.globalIlluminationMaxDistanceMeters<<' '<<std::quoted(encode_scene_lighting(e));
    return out.str();
}
bool decode_material_bake_settings(std::string_view text,MaterialTextureBakeSettings& settings){
    if(text.size()>3U*1024*1024)return false;
    MaterialTextureBakeSettings s;std::istringstream in{std::string(text)};in.imbue(std::locale::classic());
    in>>s.materialIndex>>s.targetUv>>s.resolution>>s.samplesPerAxis>>s.paddingPixels>>s.maps>>s.memoryBudgetBytes>>s.metersPerUnit;
    vector_io(in,s.pose.position);in>>s.pose.rotation.x>>s.pose.rotation.y>>s.pose.rotation.z>>s.pose.rotation.w;
    unsigned all{},generated{},count{},shadow{},gi{};vector_io(in,s.scale);in>>all>>generated>>s.texelsPerMeter>>s.raySamples>>s.seed>>s.aoDistanceMeters>>s.rayBiasMeters>>s.lightingRange>>count;
    if(!in||all>1||generated>1||count>256)return false;
    s.allAssignedSlots=all!=0;s.generateAtlas=generated!=0;s.materialSlots.resize(count);for(auto& slot:s.materialSlots)in>>slot;
    auto& e=s.lighting;vector_io(in,e.sunDirection);vector_io(in,e.sunColor);vector_io(in,e.skyColor);vector_io(in,e.groundColor);
    std::string lights;in>>e.sunIntensity>>shadow>>e.shadowStrength>>e.shadowSoftnessRadians>>e.shadowSamples>>e.shadowMaxDistanceMeters>>e.contactShadowDistanceMeters>>e.shadowBiasMeters>>gi>>e.globalIlluminationIntensity>>e.globalIlluminationMaxDistanceMeters>>std::quoted(lights);
    if(!in||shadow>4||gi>static_cast<unsigned>(GlobalIlluminationMode::RadianceCascades))return false;
    e.shadowMode=ShadowMode(shadow);e.globalIlluminationMode=GlobalIlluminationMode(gi);
    if(!decode_scene_lighting(lights,e)||!valid_material_bake_settings(s))return false;
    in>>std::ws;if(!in.eof())return false;settings=std::move(s);return true;
}
CookedPolygonAsset material_bake_scaled_mesh(const CookedPolygonAsset& source,Float3 scale){
    auto a=source;for(auto& v:a.vertices){v.position={v.position.x*scale.x,v.position.y*scale.y,v.position.z*scale.z};v.normal=normalize(Float3{v.normal.x/scale.x,v.normal.y/scale.y,v.normal.z/scale.z});}
    if(!a.vertices.empty()){a.bounds.minimum=a.bounds.maximum=a.vertices[0].position;for(const auto& v:a.vertices){a.bounds.minimum={ (std::min)(a.bounds.minimum.x,v.position.x),(std::min)(a.bounds.minimum.y,v.position.y),(std::min)(a.bounds.minimum.z,v.position.z)};a.bounds.maximum={ (std::max)(a.bounds.maximum.x,v.position.x),(std::max)(a.bounds.maximum.y,v.position.y),(std::max)(a.bounds.maximum.z,v.position.z)};}}
    a.contentHash=polygon_asset_content_hash(a);return a;
}
std::uint64_t material_bake_scene_hash(std::span<const MaterialBakeSceneMesh> scene){
    if(scene.empty())return 0;
    std::uint64_t h=14695981039346656037ULL;auto word=[&](std::uint64_t v){for(unsigned n=0;n<8;++n){h^=(v>>(8*n))&255U;h*=1099511628211ULL;}};
    word(scene.size());for(const auto& m:scene){word(polygon_asset_content_hash(m.asset));for(float v:{m.pose.position.x,m.pose.position.y,m.pose.position.z,m.pose.rotation.x,m.pose.rotation.y,m.pose.rotation.z,m.pose.rotation.w})word(std::bit_cast<std::uint32_t>(v));}return h;
}
}
