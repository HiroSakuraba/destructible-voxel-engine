#include "dve/polygon_asset.hpp"
#include "dve/master_material.hpp"
#include "dve/transform.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace dve {
namespace {

constexpr std::array<char, 8> kMagic{'D','V','E','M','E','S','H','1'};
constexpr std::uint16_t kMajor = 1;
constexpr std::uint16_t kMinor = 3;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::uint32_t kMaximumFileMaterials = 256U;
constexpr std::uint32_t kMaximumFileStrings = 1U << 20U;
constexpr std::uint64_t kMaximumFileVertices = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumFileIndices = 192ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumImageBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;

bool finite(float value) noexcept { return std::isfinite(value); }
bool finite(Float2 value) noexcept { return finite(value.x) && finite(value.y); }
bool finite(Float3 value) noexcept { return finite(value.x) && finite(value.y) && finite(value.z); }
bool finite(Float4 value) noexcept { return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w); }

Float3 add3(Float3 a, Float3 b) noexcept { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Float3 sub3(Float3 a, Float3 b) noexcept { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Float3 mul3(Float3 a, float s) noexcept { return {a.x*s, a.y*s, a.z*s}; }
Float3 cross3(Float3 a, Float3 b) noexcept {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
float dot3(Float3 a, Float3 b) noexcept { return a.x*b.x+a.y*b.y+a.z*b.z; }
float length_sq(Float3 a) noexcept { return dot3(a,a); }
Float3 normalized(Float3 a, Float3 fallback = {0.0F, 1.0F, 0.0F}) noexcept {
    const float sq = length_sq(a);
    if (!(sq > 1.0e-20F) || !finite(sq)) return fallback;
    return mul3(a, 1.0F/std::sqrt(sq));
}

Float3 transform_normal_inverse_transpose(const Matrix4& m, Float3 n) noexcept {
    const float a00=m.values[0], a01=m.values[4], a02=m.values[8];
    const float a10=m.values[1], a11=m.values[5], a12=m.values[9];
    const float a20=m.values[2], a21=m.values[6], a22=m.values[10];
    const float c00 = a11*a22-a12*a21;
    const float c01 = a12*a20-a10*a22;
    const float c02 = a10*a21-a11*a20;
    const float c10 = a02*a21-a01*a22;
    const float c11 = a00*a22-a02*a20;
    const float c12 = a01*a20-a00*a21;
    const float c20 = a01*a12-a02*a11;
    const float c21 = a02*a10-a00*a12;
    const float c22 = a00*a11-a01*a10;
    const float det = a00*c00+a01*c01+a02*c02;
    if (!(std::abs(det) > 1.0e-12F) || !finite(det)) return normalized(transform_vector(m,n));
    const float invDet=1.0F/det;
    // inverse-transpose is cofactor matrix divided by determinant.
    return normalized({
        (c00*n.x+c01*n.y+c02*n.z)*invDet,
        (c10*n.x+c11*n.y+c12*n.z)*invDet,
        (c20*n.x+c21*n.y+c22*n.z)*invDet});
}

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept { hash=(hash^value)*kFnvPrime; }
template<class T> void hash_scalar(std::uint64_t& hash, T value) noexcept {
    const auto* p=reinterpret_cast<const std::uint8_t*>(&value);
    for(std::size_t i=0;i<sizeof(T);++i) hash_byte(hash,p[i]);
}
void hash_string(std::uint64_t& hash, std::string_view text) noexcept {
    hash_scalar(hash, static_cast<std::uint64_t>(text.size()));
    for(unsigned char c:text) hash_byte(hash,c);
}
void hash_float3(std::uint64_t& h, Float3 v) noexcept { hash_scalar(h,v.x);hash_scalar(h,v.y);hash_scalar(h,v.z); }
void hash_float4(std::uint64_t& h, Float4 v) noexcept { hash_scalar(h,v.x);hash_scalar(h,v.y);hash_scalar(h,v.z);hash_scalar(h,v.w); }
void hash_material(std::uint64_t& h, const VoxelMaterialDefinition& m) noexcept {
    hash_string(h,m.name); hash_float4(h,m.baseColor); hash_float3(h,m.emissive);
    hash_scalar(h,m.metallic); hash_scalar(h,m.roughness); hash_scalar(h,m.specular);
    hash_scalar(h,static_cast<std::uint8_t>(m.shadingModel)); hash_scalar(h,static_cast<std::uint8_t>(m.blendMode));
    hash_scalar(h,m.subsurfaceScatterDistanceMeters); hash_float3(h,m.subsurfaceColor);
    hash_scalar(h,m.clearCoat); hash_scalar(h,m.clearCoatRoughness); hash_float3(h,m.foliageColor);
    hash_scalar(h,m.foliageTransmittance); hash_scalar(h,m.foliageWrap);
    hash_scalar(h,m.densityKilogramsPerCubicMeter); hash_scalar(h,m.structuralStrength);
    hash_scalar(h,m.fractureResistance); hash_scalar(h,m.flammability); hash_scalar(h,m.thermalConductivity);
    hash_scalar(h,m.transparent); hash_scalar(h,m.structural);
    hash_scalar(h,static_cast<std::uint64_t>(m.layers.size()));
    for(const auto& l:m.layers){hash_scalar(h,l.sourceMaterial);hash_scalar(h,l.weight);hash_scalar(h,static_cast<std::uint8_t>(l.blendMode));hash_scalar(h,l.enabled);}
}

class Writer {
public:
    void u8(std::uint8_t v){data.push_back(static_cast<std::byte>(v));}
    void u16(std::uint16_t v){u8(static_cast<std::uint8_t>(v));u8(static_cast<std::uint8_t>(v>>8U));}
    void u32(std::uint32_t v){for(unsigned s=0;s<32;s+=8)u8(static_cast<std::uint8_t>(v>>s));}
    void u64(std::uint64_t v){for(unsigned s=0;s<64;s+=8)u8(static_cast<std::uint8_t>(v>>s));}
    void f32(float v){u32(std::bit_cast<std::uint32_t>(v));}
    void bytes(const void* p,std::size_t n){const auto* b=static_cast<const std::byte*>(p);data.insert(data.end(),b,b+static_cast<std::ptrdiff_t>(n));}
    void string(std::string_view s){if(s.size()>kMaximumFileStrings)throw std::runtime_error("DMESH string exceeds limit");u32(static_cast<std::uint32_t>(s.size()));bytes(s.data(),s.size());}
    std::vector<std::byte> data;
};

class Reader {
public:
    explicit Reader(const std::vector<std::byte>& d):data(d){}
    void require(std::size_t n)const{if(n>data.size()-pos)throw std::runtime_error("truncated DMESH file");}
    std::uint8_t u8(){require(1);return std::to_integer<std::uint8_t>(data[pos++]);}
    std::uint16_t u16(){const auto a=u8();const auto b=u8();return static_cast<std::uint16_t>(a|(static_cast<std::uint16_t>(b)<<8U));}
    std::uint32_t u32(){std::uint32_t v=0;for(unsigned s=0;s<32;s+=8)v|=static_cast<std::uint32_t>(u8())<<s;return v;}
    std::uint64_t u64(){std::uint64_t v=0;for(unsigned s=0;s<64;s+=8)v|=static_cast<std::uint64_t>(u8())<<s;return v;}
    float f32(){const float v=std::bit_cast<float>(u32());if(!finite(v))throw std::runtime_error("DMESH contains non-finite float");return v;}
    std::string string(){const auto n=u32();if(n>kMaximumFileStrings)throw std::runtime_error("DMESH string exceeds limit");require(n);std::string s(reinterpret_cast<const char*>(data.data()+pos),n);pos+=n;return s;}
    void bytes(void* out,std::size_t n){require(n);std::memcpy(out,data.data()+pos,n);pos+=n;}
    [[nodiscard]] std::size_t remaining()const noexcept{return data.size()-pos;}
    const std::vector<std::byte>& data; std::size_t pos{};
};

std::vector<std::byte> read_all(const std::filesystem::path& path,
                                std::uint64_t maximumBytes) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("unable to open DMESH file");
    }
    const auto size = in.tellg();
    if (size < 0) {
        throw std::runtime_error("unable to determine DMESH size");
    }
    if (static_cast<std::uint64_t>(size) > maximumBytes) {
        throw std::runtime_error("DMESH exceeds configured size limit");
    }
    in.seekg(0);
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    if (!data.empty() &&
        !in.read(reinterpret_cast<char*>(data.data()), size)) {
        throw std::runtime_error("unable to read DMESH file");
    }
    return data;
}

void write_material(Writer& w,const VoxelMaterialDefinition& m){
    w.string(m.name);w.f32(m.baseColor.x);w.f32(m.baseColor.y);w.f32(m.baseColor.z);w.f32(m.baseColor.w);
    w.f32(m.emissive.x);w.f32(m.emissive.y);w.f32(m.emissive.z);w.f32(m.metallic);w.f32(m.roughness);w.f32(m.specular);
    w.u8(static_cast<std::uint8_t>(m.shadingModel));w.u8(static_cast<std::uint8_t>(m.blendMode));w.u16(0);
    w.f32(m.subsurfaceScatterDistanceMeters);w.f32(m.subsurfaceColor.x);w.f32(m.subsurfaceColor.y);w.f32(m.subsurfaceColor.z);
    w.f32(m.clearCoat);w.f32(m.clearCoatRoughness);w.f32(m.foliageColor.x);w.f32(m.foliageColor.y);w.f32(m.foliageColor.z);w.f32(m.foliageTransmittance);w.f32(m.foliageWrap);
    w.f32(m.densityKilogramsPerCubicMeter);w.f32(m.structuralStrength);w.f32(m.fractureResistance);w.f32(m.flammability);w.f32(m.thermalConductivity);
    w.u8(m.transparent?1U:0U);w.u8(m.structural?1U:0U);w.u8(static_cast<std::uint8_t>(m.layers.size()));w.u8(0);
    for(const auto& l:m.layers){w.u8(l.sourceMaterial);w.u8(static_cast<std::uint8_t>(l.blendMode));w.u8(l.enabled?1U:0U);w.u8(0);w.f32(l.weight);}
}
VoxelMaterialDefinition read_material(Reader& r){
    VoxelMaterialDefinition m;m.name=r.string();m.baseColor={r.f32(),r.f32(),r.f32(),r.f32()};m.emissive={r.f32(),r.f32(),r.f32()};m.metallic=r.f32();m.roughness=r.f32();m.specular=r.f32();
    const auto shading=r.u8(),blend=r.u8();(void)r.u16();
    if(shading>static_cast<std::uint8_t>(MaterialShadingModel::ClearCoat)||blend>static_cast<std::uint8_t>(MaterialBlendMode::Translucent))throw std::runtime_error("DMESH material enum invalid");
    m.shadingModel=static_cast<MaterialShadingModel>(shading);m.blendMode=static_cast<MaterialBlendMode>(blend);
    m.subsurfaceScatterDistanceMeters=r.f32();m.subsurfaceColor={r.f32(),r.f32(),r.f32()};m.clearCoat=r.f32();m.clearCoatRoughness=r.f32();m.foliageColor={r.f32(),r.f32(),r.f32()};m.foliageTransmittance=r.f32();m.foliageWrap=r.f32();
    m.densityKilogramsPerCubicMeter=r.f32();m.structuralStrength=r.f32();m.fractureResistance=r.f32();m.flammability=r.f32();m.thermalConductivity=r.f32();m.transparent=r.u8()!=0;m.structural=r.u8()!=0;const auto layers=r.u8();(void)r.u8();
    if(layers>kMaximumVoxelMaterialLayers)throw std::runtime_error("DMESH material layer limit exceeded");
    for(std::uint8_t i=0;i<layers;++i){VoxelMaterialLayer l;l.sourceMaterial=r.u8();const auto mode=r.u8();l.enabled=r.u8()!=0;(void)r.u8();l.weight=r.f32();if(mode>static_cast<std::uint8_t>(MaterialLayerBlendMode::Additive))throw std::runtime_error("DMESH layer mode invalid");l.blendMode=static_cast<MaterialLayerBlendMode>(mode);m.layers.push_back(l);}return m;
}


void hash_texture_binding(std::uint64_t& h, const PolygonTextureBinding& binding) noexcept {
    hash_scalar(h, binding.texture.has_value());
    if (binding.texture) hash_scalar(h, *binding.texture);
    hash_scalar(h, binding.texcoord);
    hash_scalar(h, static_cast<std::uint8_t>(binding.colorSpace));
}

void write_texture_binding(Writer& w, const PolygonTextureBinding& binding) {
    w.u32(binding.texture.value_or(0U));
    w.u8(binding.texture ? 1U : 0U);
    w.u8(static_cast<std::uint8_t>(binding.colorSpace));
    w.u16(static_cast<std::uint16_t>(binding.texcoord));
}

PolygonTextureBinding read_texture_binding(Reader& r) {
    PolygonTextureBinding binding;
    const std::uint32_t texture = r.u32();
    const bool hasTexture = r.u8() != 0U;
    const std::uint8_t colorSpace = r.u8();
    binding.texcoord = r.u16();
    if (colorSpace > static_cast<std::uint8_t>(PolygonTextureColorSpace::Srgb)) {
        throw std::runtime_error("DMESH texture color space is invalid");
    }
    binding.colorSpace = static_cast<PolygonTextureColorSpace>(colorSpace);
    if (hasTexture) binding.texture = texture;
    return binding;
}

void hash_material_layer_binding(std::uint64_t& h,
                                 const PolygonMaterialLayerBinding& binding) noexcept {
    hash_scalar(h, static_cast<std::uint8_t>(binding.semantic));
    hash_texture_binding(h, binding.mask);
    hash_texture_binding(h, binding.height);
    hash_scalar(h, binding.maskScale); hash_scalar(h, binding.maskBias);
    hash_scalar(h, binding.heightBlendStrength); hash_scalar(h, binding.heightBlendBias);
    hash_scalar(h, binding.heightBlendTransition);
    hash_scalar(h, static_cast<std::uint8_t>(binding.opacityPolicy));
    hash_scalar(h, binding.enabled);
}

void write_material_layer_binding(Writer& w, const PolygonMaterialLayerBinding& binding) {
    w.u8(static_cast<std::uint8_t>(binding.semantic));
    w.u8(static_cast<std::uint8_t>(binding.opacityPolicy));
    w.u8(binding.enabled ? 1U : 0U); w.u8(0U);
    write_texture_binding(w, binding.mask);
    write_texture_binding(w, binding.height);
    w.f32(binding.maskScale); w.f32(binding.maskBias);
    w.f32(binding.heightBlendStrength); w.f32(binding.heightBlendBias);
    w.f32(binding.heightBlendTransition);
}

PolygonMaterialLayerBinding read_material_layer_binding(Reader& r) {
    PolygonMaterialLayerBinding binding;
    const auto semantic = r.u8();
    const auto opacityPolicy = r.u8();
    binding.enabled = r.u8() != 0U; (void)r.u8();
    if (semantic > static_cast<std::uint8_t>(MaterialLayerSemantic::FractureExposure) ||
        opacityPolicy > static_cast<std::uint8_t>(MaterialLayerOpacityPolicy::Replace))
        throw std::runtime_error("DMESH material layer binding enum invalid");
    binding.semantic = static_cast<MaterialLayerSemantic>(semantic);
    binding.opacityPolicy = static_cast<MaterialLayerOpacityPolicy>(opacityPolicy);
    binding.mask = read_texture_binding(r);
    binding.height = read_texture_binding(r);
    binding.maskScale = r.f32(); binding.maskBias = r.f32();
    binding.heightBlendStrength = r.f32(); binding.heightBlendBias = r.f32();
    binding.heightBlendTransition = r.f32();
    return binding;
}

void hash_texture_transform(std::uint64_t& h, const TextureTransform2D& transform) noexcept {
    hash_scalar(h, transform.scale.x); hash_scalar(h, transform.scale.y);
    hash_scalar(h, transform.offset.x); hash_scalar(h, transform.offset.y);
    hash_scalar(h, transform.rotationRadians);
}

void hash_mapping_settings(std::uint64_t& h, const MaterialMappingSettings& mapping) noexcept {
    hash_scalar(h, static_cast<std::uint8_t>(mapping.mappingMode));
    hash_texture_transform(h, mapping.baseTransform);
    hash_scalar(h, mapping.triplanarScale); hash_scalar(h, mapping.triplanarBlendSharpness);
    hash_texture_transform(h, mapping.detailTransform);
    hash_scalar(h, mapping.detailColorStrength); hash_scalar(h, mapping.detailNormalStrength);
    hash_scalar(h, mapping.detailRoughnessStrength);
    hash_scalar(h, mapping.detailFadeStartMeters); hash_scalar(h, mapping.detailFadeEndMeters);
    hash_scalar(h, static_cast<std::uint8_t>(mapping.heightMode));
    hash_scalar(h, mapping.heightScale); hash_scalar(h, mapping.heightReferencePlane);
    hash_scalar(h, mapping.minimumHeightSteps); hash_scalar(h, mapping.maximumHeightSteps);
    hash_scalar(h, mapping.refinementSteps); hash_scalar(h, mapping.maximumParallaxDistanceMeters);
}

void write_texture_transform(Writer& w, const TextureTransform2D& transform) {
    w.f32(transform.scale.x); w.f32(transform.scale.y);
    w.f32(transform.offset.x); w.f32(transform.offset.y);
    w.f32(transform.rotationRadians);
}

TextureTransform2D read_texture_transform(Reader& r) {
    TextureTransform2D transform;
    transform.scale = {r.f32(), r.f32()};
    transform.offset = {r.f32(), r.f32()};
    transform.rotationRadians = r.f32();
    return transform;
}

void write_mapping_settings(Writer& w, const MaterialMappingSettings& mapping) {
    w.u8(static_cast<std::uint8_t>(mapping.mappingMode));
    w.u8(static_cast<std::uint8_t>(mapping.heightMode));
    w.u16(0U);
    write_texture_transform(w, mapping.baseTransform);
    w.f32(mapping.triplanarScale); w.f32(mapping.triplanarBlendSharpness);
    write_texture_transform(w, mapping.detailTransform);
    w.f32(mapping.detailColorStrength); w.f32(mapping.detailNormalStrength);
    w.f32(mapping.detailRoughnessStrength);
    w.f32(mapping.detailFadeStartMeters); w.f32(mapping.detailFadeEndMeters);
    w.f32(mapping.heightScale); w.f32(mapping.heightReferencePlane);
    w.u32(mapping.minimumHeightSteps); w.u32(mapping.maximumHeightSteps);
    w.u32(mapping.refinementSteps); w.f32(mapping.maximumParallaxDistanceMeters);
}

MaterialMappingSettings read_mapping_settings(Reader& r) {
    MaterialMappingSettings mapping;
    const auto mode = r.u8();
    const auto heightMode = r.u8();
    (void)r.u16();
    if (mode > static_cast<std::uint8_t>(MaterialMappingMode::ObjectTriplanar) ||
        heightMode > static_cast<std::uint8_t>(HeightMappingMode::ParallaxOcclusion)) {
        throw std::runtime_error("DMESH material mapping enum invalid");
    }
    mapping.mappingMode = static_cast<MaterialMappingMode>(mode);
    mapping.heightMode = static_cast<HeightMappingMode>(heightMode);
    mapping.baseTransform = read_texture_transform(r);
    mapping.triplanarScale = r.f32(); mapping.triplanarBlendSharpness = r.f32();
    mapping.detailTransform = read_texture_transform(r);
    mapping.detailColorStrength = r.f32(); mapping.detailNormalStrength = r.f32();
    mapping.detailRoughnessStrength = r.f32();
    mapping.detailFadeStartMeters = r.f32(); mapping.detailFadeEndMeters = r.f32();
    mapping.heightScale = r.f32(); mapping.heightReferencePlane = r.f32();
    mapping.minimumHeightSteps = r.u32(); mapping.maximumHeightSteps = r.u32();
    mapping.refinementSteps = r.u32(); mapping.maximumParallaxDistanceMeters = r.f32();
    return mapping;
}

bool valid_texture_binding(const PolygonTextureBinding& binding,
                           const CookedPolygonAsset& asset) noexcept {
    return binding.texcoord <= 1U &&
           (!binding.texture || *binding.texture < asset.textures.size());
}

VoxelMaterialDefinition convert_material(const ImportedMaterial& in){
    VoxelMaterialDefinition out;out.name=in.name.empty()?"PolygonMaterial":in.name;out.baseColor=in.baseColorFactor;out.emissive=in.emissiveFactor;out.metallic=std::clamp(in.metallicFactor,0.0F,1.0F);out.roughness=std::clamp(in.roughnessFactor,0.0F,1.0F);
    switch(in.alphaMode){case ImportedAlphaMode::Opaque:out.blendMode=MaterialBlendMode::Opaque;break;case ImportedAlphaMode::Mask:out.blendMode=MaterialBlendMode::Masked;break;case ImportedAlphaMode::Blend:out.blendMode=MaterialBlendMode::Translucent;out.transparent=true;break;}
    return out;
}

} // namespace

std::uint64_t polygon_asset_content_hash(const CookedPolygonAsset& a) noexcept {
    std::uint64_t h=kFnvOffset;hash_scalar(h,a.objectId);hash_scalar(h,static_cast<std::uint64_t>(a.materials.size()));
    for(const auto& m:a.materials)hash_material(h,m);
    hash_scalar(h,static_cast<std::uint64_t>(a.materialBindings.size()));
    for(const auto& b:a.materialBindings){
        hash_scalar(h,b.doubleSided);hash_scalar(h,b.alphaCutoff);hash_scalar(h,b.normalScale);
        hash_texture_binding(h,b.baseColor);hash_texture_binding(h,b.metallicRoughness);
        hash_texture_binding(h,b.normal);hash_texture_binding(h,b.emissive);hash_texture_binding(h,b.opacity);
        hash_mapping_settings(h,b.mapping); hash_texture_binding(h,b.height);
        hash_texture_binding(h,b.detailBaseColor); hash_texture_binding(h,b.detailNormal);
        hash_texture_binding(h,b.detailRoughness); hash_scalar(h,b.detailNormalScale);
        // Preserve v1.0-v1.2 content hashes when no v1.3 layer bindings are authored.
        // The marker makes non-empty extension data unambiguous without invalidating legacy assets.
        if (!b.layers.empty()) {
            hash_scalar(h, std::uint64_t{0x4456454C41594552ULL});
            hash_scalar(h, static_cast<std::uint64_t>(b.layers.size()));
            for (const auto& layer : b.layers) hash_material_layer_binding(h, layer);
        }
    }
    hash_scalar(h,static_cast<std::uint64_t>(a.images.size()));for(const auto& im:a.images){hash_string(h,im.name);hash_string(h,im.mimeType);hash_scalar(h,im.width);hash_scalar(h,im.height);hash_scalar(h,static_cast<std::uint64_t>(im.rgba8.size()));for(auto v:im.rgba8)hash_byte(h,v);}
    hash_scalar(h,static_cast<std::uint64_t>(a.samplers.size()));for(const auto& s:a.samplers){hash_scalar(h,static_cast<std::uint16_t>(s.wrapS));hash_scalar(h,static_cast<std::uint16_t>(s.wrapT));hash_scalar(h,static_cast<std::uint16_t>(s.minFilter));hash_scalar(h,static_cast<std::uint16_t>(s.magFilter));}
    hash_scalar(h,static_cast<std::uint64_t>(a.textures.size()));for(const auto& t:a.textures){hash_string(h,t.name);hash_scalar(h,t.imageIndex);hash_scalar(h,t.samplerIndex.has_value());if(t.samplerIndex)hash_scalar(h,*t.samplerIndex);}
    hash_scalar(h,static_cast<std::uint64_t>(a.vertices.size()));for(const auto& v:a.vertices){hash_float3(h,v.position);hash_float3(h,v.normal);hash_float4(h,v.tangent);hash_scalar(h,v.texcoord.x);hash_scalar(h,v.texcoord.y);hash_scalar(h,v.texcoord1.x);hash_scalar(h,v.texcoord1.y);hash_float4(h,v.color);}
    hash_scalar(h,static_cast<std::uint64_t>(a.indices.size()));for(auto i:a.indices)hash_scalar(h,i);
    hash_scalar(h,static_cast<std::uint64_t>(a.submeshes.size()));for(const auto& s:a.submeshes){hash_string(h,s.name);hash_scalar(h,s.firstIndex);hash_scalar(h,s.indexCount);hash_scalar(h,s.materialIndex);}
    hash_float3(h,a.bounds.minimum);hash_float3(h,a.bounds.maximum);return h;
}

PolygonAssetValidationResult validate_polygon_asset(const CookedPolygonAsset& a) noexcept {
    const auto fail=[](PolygonAssetErrorCode c,std::string m){return PolygonAssetValidationResult{c,std::move(m)};};
    if(a.vertices.empty()||a.indices.empty()||a.submeshes.empty())return fail(PolygonAssetErrorCode::Empty,"polygon asset has no renderable geometry");
    if(a.vertices.size()>kMaximumFileVertices||a.indices.size()>kMaximumFileIndices||a.materials.empty()||a.materials.size()>kMaximumFileMaterials)return fail(PolygonAssetErrorCode::LimitExceeded,"polygon asset exceeds count limits");
    if(a.materialBindings.size()!=a.materials.size())return fail(PolygonAssetErrorCode::InvalidMaterial,"material binding count does not match material count");
    std::string layerError;if(!resolve_voxel_material_layers(a.materials,&layerError))return fail(PolygonAssetErrorCode::InvalidMaterial,"material layers are invalid: "+layerError);
    for(const auto& v:a.vertices)if(!finite(v.position)||!finite(v.normal)||!finite(v.tangent)||!finite(v.texcoord)||!finite(v.texcoord1)||!finite(v.color))return fail(PolygonAssetErrorCode::InvalidVertex,"polygon vertex contains a non-finite value");
    for(auto i:a.indices)if(i>=a.vertices.size())return fail(PolygonAssetErrorCode::InvalidIndex,"polygon index is outside the vertex array");
    if((a.indices.size()%3U)!=0U)return fail(PolygonAssetErrorCode::InvalidIndex,"polygon index count is not triangle-aligned");
    for(const auto& s:a.submeshes){if(s.indexCount==0U||(s.indexCount%3U)!=0U||s.firstIndex>a.indices.size()||s.indexCount>a.indices.size()-s.firstIndex||s.materialIndex>=a.materials.size())return fail(PolygonAssetErrorCode::InvalidSubmesh,"submesh range or material is invalid");}
    for(const auto& b:a.materialBindings){
        if(!finite(b.alphaCutoff)||b.alphaCutoff<0.0F||b.alphaCutoff>1.0F||
           !finite(b.normalScale)||b.normalScale<0.0F||b.normalScale>8.0F||
           !finite(b.detailNormalScale)||b.detailNormalScale<0.0F||b.detailNormalScale>8.0F)
            return fail(PolygonAssetErrorCode::InvalidMaterial,"material texture controls are invalid");
        std::string mappingError;
        if(!validate_material_mapping_settings(b.mapping,&mappingError))
            return fail(PolygonAssetErrorCode::InvalidMaterial,"material mapping is invalid: "+mappingError);
        if(!valid_texture_binding(b.baseColor,a)||!valid_texture_binding(b.metallicRoughness,a)||
           !valid_texture_binding(b.normal,a)||!valid_texture_binding(b.emissive,a)||
           !valid_texture_binding(b.opacity,a)||!valid_texture_binding(b.height,a)||
           !valid_texture_binding(b.detailBaseColor,a)||!valid_texture_binding(b.detailNormal,a)||
           !valid_texture_binding(b.detailRoughness,a))
            return fail(PolygonAssetErrorCode::InvalidTexture,"material references an unknown texture or UV set");
        if(b.mapping.heightMode!=HeightMappingMode::Off&&!b.height.texture)
            return fail(PolygonAssetErrorCode::InvalidMaterial,"height mapping mode requires a height texture");
        if (b.layers.size() > kMaximumVoxelMaterialLayers)
            return fail(PolygonAssetErrorCode::InvalidMaterial,"material has too many per-pixel layer bindings");
        for (const PolygonMaterialLayerBinding& layer : b.layers) {
            if (!valid_texture_binding(layer.mask, a) || !valid_texture_binding(layer.height, a))
                return fail(PolygonAssetErrorCode::InvalidTexture,
                            "material layer references an unknown texture or UV set");
            if (static_cast<unsigned>(layer.semantic) >
                    static_cast<unsigned>(MaterialLayerSemantic::FractureExposure) ||
                static_cast<unsigned>(layer.opacityPolicy) >
                    static_cast<unsigned>(MaterialLayerOpacityPolicy::Replace) ||
                !finite(layer.maskScale) || !finite(layer.maskBias) ||
                !finite(layer.heightBlendStrength) || layer.heightBlendStrength < 0.0F ||
                !finite(layer.heightBlendBias) || !finite(layer.heightBlendTransition) ||
                layer.heightBlendTransition < 0.0F)
                return fail(PolygonAssetErrorCode::InvalidMaterial,
                            "material per-pixel layer controls are invalid");
        }
    }
    std::uint64_t imageBytes=0;for(const auto& im:a.images){const std::uint64_t expected=static_cast<std::uint64_t>(im.width)*im.height*4ULL;if(im.width==0||im.height==0||expected!=im.rgba8.size())return fail(PolygonAssetErrorCode::InvalidTexture,"image dimensions do not match RGBA payload");if(expected>kMaximumImageBytes-imageBytes)return fail(PolygonAssetErrorCode::LimitExceeded,"image payload exceeds limit");imageBytes+=expected;}
    for(const auto& t:a.textures){if(t.imageIndex>=a.images.size()||(t.samplerIndex&&*t.samplerIndex>=a.samplers.size()))return fail(PolygonAssetErrorCode::InvalidTexture,"texture references an unknown image or sampler");}
    if(!finite(a.bounds.minimum)||!finite(a.bounds.maximum)||a.bounds.minimum.x>a.bounds.maximum.x||a.bounds.minimum.y>a.bounds.maximum.y||a.bounds.minimum.z>a.bounds.maximum.z)return fail(PolygonAssetErrorCode::InvalidBounds,"polygon bounds are invalid");
    if(a.contentHash!=0&&a.contentHash!=polygon_asset_content_hash(a))return fail(PolygonAssetErrorCode::HashMismatch,"polygon content hash does not match");
    return {};
}

CookedPolygonAsset cook_polygon_scene(const ImportedScene& scene,const PolygonCookOptions& options,std::vector<ImportDiagnostic>* diagnostics){
    if(options.maximumMaterials==0||options.maximumMaterials>256U)throw std::invalid_argument("maximumMaterials must be in [1,256]");
    CookedPolygonAsset out;out.objectId=options.objectId;
    if(scene.materials.empty()){ImportedMaterial fallback;fallback.name="Default";out.materials.push_back(convert_material(fallback));out.materialBindings.push_back({});}
    else{
        if(scene.materials.size()>options.maximumMaterials)throw std::runtime_error("polygon material limit exceeded");
        for(const auto& m:scene.materials){
            out.materials.push_back(convert_material(m));
            PolygonMaterialBinding binding;
            binding.doubleSided=m.doubleSided;binding.alphaCutoff=std::clamp(m.alphaCutoff,0.0F,1.0F);
            binding.baseColor={m.baseColorTexture,m.baseColorTexcoord,PolygonTextureColorSpace::Srgb};
            binding.metallicRoughness={m.metallicRoughnessTexture,m.metallicRoughnessTexcoord,PolygonTextureColorSpace::Linear};
            binding.normal={m.normalTexture,m.normalTexcoord,PolygonTextureColorSpace::Linear};binding.normalScale=std::clamp(m.normalScale,0.0F,8.0F);
            binding.emissive={m.emissiveTexture,m.emissiveTexcoord,PolygonTextureColorSpace::Srgb};
            binding.opacity={m.opacityTexture,m.opacityTexcoord,PolygonTextureColorSpace::Linear};
            out.materialBindings.push_back(binding);
        }
    }
    if(options.preserveTextures){
        out.images.reserve(scene.images.size());for(const auto& im:scene.images)out.images.push_back({im.name,im.mimeType,im.width,im.height,im.rgba8});
        out.samplers.reserve(scene.samplers.size());for(const auto& s:scene.samplers)out.samplers.push_back({s.wrapS,s.wrapT,s.minFilter,s.magFilter});
        out.textures.reserve(scene.textures.size());for(const auto& t:scene.textures)out.textures.push_back({t.name,t.imageIndex,t.samplerIndex});
    } else { for(auto& b:out.materialBindings){b.baseColor.texture.reset();b.metallicRoughness.texture.reset();b.normal.texture.reset();b.emissive.texture.reset();b.opacity.texture.reset();b.height.texture.reset();b.detailBaseColor.texture.reset();b.detailNormal.texture.reset();b.detailRoughness.texture.reset();b.mapping.heightMode=HeightMappingMode::Off;} }

    bool haveBounds=false;std::vector<bool> suppliedNormal;std::vector<Float3> normalAccum;std::vector<Float3> tangentAccum;std::vector<Float3> bitangentAccum;
    for(std::size_t nodeIndex=0;nodeIndex<scene.nodes.size();++nodeIndex){const auto& node=scene.nodes[nodeIndex];if(!node.mesh||*node.mesh>=scene.meshes.size())continue;const auto& mesh=scene.meshes[*node.mesh];
        if(out.vertices.size()+mesh.vertices.size()>options.maximumVertices)throw std::runtime_error("polygon vertex limit exceeded");
        const std::uint32_t base=static_cast<std::uint32_t>(out.vertices.size());
        for(const auto& v:mesh.vertices){PolygonVertex p;p.position=transform_point(node.worldTransform,v.position);p.normal=length_sq(v.normal)>1.0e-20F?transform_normal_inverse_transpose(node.worldTransform,v.normal):Float3{};p.texcoord=v.texcoord;p.texcoord1=v.texcoord1;p.color=v.color;out.vertices.push_back(p);suppliedNormal.push_back(length_sq(p.normal)>1.0e-20F);normalAccum.push_back({});tangentAccum.push_back({});bitangentAccum.push_back({});
            if(!haveBounds){out.bounds={p.position,p.position};haveBounds=true;}else{out.bounds.minimum={std::min(out.bounds.minimum.x,p.position.x),std::min(out.bounds.minimum.y,p.position.y),std::min(out.bounds.minimum.z,p.position.z)};out.bounds.maximum={std::max(out.bounds.maximum.x,p.position.x),std::max(out.bounds.maximum.y,p.position.y),std::max(out.bounds.maximum.z,p.position.z)};}}
        std::map<std::uint32_t,std::vector<std::uint32_t>> groups;
        for(const auto& tri:mesh.triangles){if(tri.indices[0]>=mesh.vertices.size()||tri.indices[1]>=mesh.vertices.size()||tri.indices[2]>=mesh.vertices.size())continue;const std::uint32_t mat=tri.materialIndex<out.materials.size()?tri.materialIndex:0U;auto& group=groups[mat];const std::array<std::uint32_t,3> ids{base+tri.indices[0],base+tri.indices[1],base+tri.indices[2]};group.insert(group.end(),ids.begin(),ids.end());
            const Float3 p0=out.vertices[ids[0]].position,p1=out.vertices[ids[1]].position,p2=out.vertices[ids[2]].position;const Float3 e1=sub3(p1,p0),e2=sub3(p2,p0);const Float3 fn=cross3(e1,e2);for(auto id:ids)normalAccum[id]=add3(normalAccum[id],fn);
            const Float2 uv0=out.vertices[ids[0]].texcoord,uv1=out.vertices[ids[1]].texcoord,uv2=out.vertices[ids[2]].texcoord;const float du1=uv1.x-uv0.x,dv1=uv1.y-uv0.y,du2=uv2.x-uv0.x,dv2=uv2.y-uv0.y;const float denom=du1*dv2-du2*dv1;if(std::abs(denom)>1.0e-12F){const float inv=1.0F/denom;const Float3 t=mul3(sub3(mul3(e1,dv2),mul3(e2,dv1)),inv);const Float3 b=mul3(sub3(mul3(e2,du1),mul3(e1,du2)),inv);for(auto id:ids){tangentAccum[id]=add3(tangentAccum[id],t);bitangentAccum[id]=add3(bitangentAccum[id],b);}}}
        for(auto& [mat,idx]:groups){if(out.indices.size()+idx.size()>options.maximumIndices)throw std::runtime_error("polygon index limit exceeded");PolygonSubmesh sub;sub.name=(node.name.empty()?mesh.name:node.name)+"_material_"+std::to_string(mat);sub.firstIndex=static_cast<std::uint32_t>(out.indices.size());sub.indexCount=static_cast<std::uint32_t>(idx.size());sub.materialIndex=mat;out.indices.insert(out.indices.end(),idx.begin(),idx.end());out.submeshes.push_back(std::move(sub));}
    }
    if(options.generateMissingNormals){for(std::size_t i=0;i<out.vertices.size();++i)if(!suppliedNormal[i])out.vertices[i].normal=normalized(normalAccum[i]);}
    for(std::size_t i=0;i<out.vertices.size();++i){out.vertices[i].normal=normalized(out.vertices[i].normal);if(options.generateTangents){Float3 t=sub3(tangentAccum[i],mul3(out.vertices[i].normal,dot3(out.vertices[i].normal,tangentAccum[i])));t=normalized(t,{1.0F,0.0F,0.0F});const float handed=dot3(cross3(out.vertices[i].normal,t),bitangentAccum[i])<0.0F?-1.0F:1.0F;out.vertices[i].tangent={t.x,t.y,t.z,handed};}}
    out.contentHash=polygon_asset_content_hash(out);const auto valid=validate_polygon_asset(out);if(!valid){if(diagnostics)diagnostics->push_back({ImportDiagnostic::Severity::Error,"POLYGON_ASSET_INVALID",valid.message});throw std::runtime_error(valid.message);}if(diagnostics)diagnostics->push_back({ImportDiagnostic::Severity::Info,"POLYGON_COOKED","Preserved polygon mesh with shared DVE materials, UVs, vertex colors, submeshes, and optional textures."});return out;
}

bool write_dmesh(const std::filesystem::path& path,const CookedPolygonAsset& input,std::string* error){
    try{CookedPolygonAsset a=input;a.contentHash=polygon_asset_content_hash(a);const auto valid=validate_polygon_asset(a);if(!valid)throw std::runtime_error(valid.message);Writer w;w.bytes(kMagic.data(),kMagic.size());w.u16(kMajor);w.u16(kMinor);w.u64(a.objectId);w.u64(a.contentHash);w.u32(static_cast<std::uint32_t>(a.materials.size()));w.u32(static_cast<std::uint32_t>(a.images.size()));w.u32(static_cast<std::uint32_t>(a.samplers.size()));w.u32(static_cast<std::uint32_t>(a.textures.size()));w.u64(a.vertices.size());w.u64(a.indices.size());w.u32(static_cast<std::uint32_t>(a.submeshes.size()));w.u32(0);
        for(std::size_t i=0;i<a.materials.size();++i){
            write_material(w,a.materials[i]);const auto& b=a.materialBindings[i];
            w.u8(b.doubleSided?1U:0U);w.u8(0);w.u16(0);w.f32(b.alphaCutoff);w.f32(b.normalScale);
            write_texture_binding(w,b.baseColor);write_texture_binding(w,b.metallicRoughness);
            write_texture_binding(w,b.normal);write_texture_binding(w,b.emissive);write_texture_binding(w,b.opacity);
            write_mapping_settings(w,b.mapping);write_texture_binding(w,b.height);
            write_texture_binding(w,b.detailBaseColor);write_texture_binding(w,b.detailNormal);
            write_texture_binding(w,b.detailRoughness);w.f32(b.detailNormalScale);
            w.u8(static_cast<std::uint8_t>(b.layers.size())); w.u8(0U); w.u16(0U);
            for (const auto& layer : b.layers) write_material_layer_binding(w, layer);
        }
        for(const auto& im:a.images){w.string(im.name);w.string(im.mimeType);w.u32(im.width);w.u32(im.height);w.u64(im.rgba8.size());if(!im.rgba8.empty())w.bytes(im.rgba8.data(),im.rgba8.size());}
        for(const auto& s:a.samplers){w.u16(static_cast<std::uint16_t>(s.wrapS));w.u16(static_cast<std::uint16_t>(s.wrapT));w.u16(static_cast<std::uint16_t>(s.minFilter));w.u16(static_cast<std::uint16_t>(s.magFilter));}
        for(const auto& t:a.textures){w.string(t.name);w.u32(t.imageIndex);w.u8(t.samplerIndex?1U:0U);w.u8(0);w.u16(0);w.u32(t.samplerIndex.value_or(0U));}
        for(const auto& v:a.vertices){w.f32(v.position.x);w.f32(v.position.y);w.f32(v.position.z);w.f32(v.normal.x);w.f32(v.normal.y);w.f32(v.normal.z);w.f32(v.tangent.x);w.f32(v.tangent.y);w.f32(v.tangent.z);w.f32(v.tangent.w);w.f32(v.texcoord.x);w.f32(v.texcoord.y);w.f32(v.texcoord1.x);w.f32(v.texcoord1.y);w.f32(v.color.x);w.f32(v.color.y);w.f32(v.color.z);w.f32(v.color.w);}
        for (const auto index : a.indices) {
            w.u32(index);
        }
        for (const auto& submesh : a.submeshes) {
            w.string(submesh.name);
            w.u32(submesh.firstIndex);
            w.u32(submesh.indexCount);
            w.u32(submesh.materialIndex);
        }
        w.f32(a.bounds.minimum.x);
        w.f32(a.bounds.minimum.y);
        w.f32(a.bounds.minimum.z);
        w.f32(a.bounds.maximum.x);
        w.f32(a.bounds.maximum.y);
        w.f32(a.bounds.maximum.z);
        std::ofstream out(path,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("unable to create DMESH file");if(!w.data.empty()&&!out.write(reinterpret_cast<const char*>(w.data.data()),static_cast<std::streamsize>(w.data.size())))throw std::runtime_error("unable to write DMESH file");return true;
    }catch(const std::exception& e){if(error)*error=e.what();return false;}}

PolygonAssetReadResult read_dmesh(const std::filesystem::path& path,std::uint64_t maximumBytes){PolygonAssetReadResult result;try{const auto data=read_all(path,maximumBytes);Reader r(data);std::array<char,8> magic{};r.bytes(magic.data(),magic.size());if(magic!=kMagic)throw std::runtime_error("DMESH magic mismatch");const auto major=r.u16(),minor=r.u16();if(major!=kMajor||minor>kMinor){result.code=PolygonAssetErrorCode::UnsupportedVersion;result.error="unsupported DMESH version";return result;}result.asset.objectId=r.u64();const auto storedHash=r.u64();const auto materialCount=r.u32(),imageCount=r.u32(),samplerCount=r.u32(),textureCount=r.u32();const auto vertexCount=r.u64(),indexCount=r.u64();const auto submeshCount=r.u32();(void)r.u32();
        if (materialCount == 0 || materialCount > kMaximumFileMaterials ||
            vertexCount > kMaximumFileVertices || indexCount > kMaximumFileIndices ||
            submeshCount > indexCount / 3U + 1U) {
            throw std::runtime_error("DMESH count limit exceeded");
        }
        result.asset.materials.reserve(materialCount);
        result.asset.materialBindings.reserve(materialCount);
        for(std::uint32_t i=0;i<materialCount;++i){
            result.asset.materials.push_back(read_material(r));PolygonMaterialBinding b;
            b.doubleSided=r.u8()!=0;
            if(minor==0U){const bool hasTexture=r.u8()!=0;(void)r.u16();b.alphaCutoff=r.f32();const auto tex=r.u32();b.baseColor.texcoord=r.u32();if(hasTexture)b.baseColor.texture=tex;}
            else{(void)r.u8();(void)r.u16();b.alphaCutoff=r.f32();b.normalScale=r.f32();b.baseColor=read_texture_binding(r);b.metallicRoughness=read_texture_binding(r);b.normal=read_texture_binding(r);b.emissive=read_texture_binding(r);b.opacity=read_texture_binding(r);
                if(minor>=2U){b.mapping=read_mapping_settings(r);b.height=read_texture_binding(r);b.detailBaseColor=read_texture_binding(r);b.detailNormal=read_texture_binding(r);b.detailRoughness=read_texture_binding(r);b.detailNormalScale=r.f32();
                    if (minor >= 3U) { const auto layerCount = r.u8(); (void)r.u8(); (void)r.u16();
                        if (layerCount > kMaximumVoxelMaterialLayers) throw std::runtime_error("DMESH material layer binding limit exceeded");
                        b.layers.reserve(layerCount); for (std::uint8_t layerIndex = 0; layerIndex < layerCount; ++layerIndex) b.layers.push_back(read_material_layer_binding(r)); }}}
            result.asset.materialBindings.push_back(b);
        }
        result.asset.images.reserve(imageCount);std::uint64_t totalImageBytes=0;for(std::uint32_t i=0;i<imageCount;++i){PolygonImage im;im.name=r.string();im.mimeType=r.string();im.width=r.u32();im.height=r.u32();const auto bytes=r.u64();if(bytes>kMaximumImageBytes-totalImageBytes||bytes>r.remaining())throw std::runtime_error("DMESH image payload exceeds limit");totalImageBytes+=bytes;im.rgba8.resize(static_cast<std::size_t>(bytes));if(bytes)r.bytes(im.rgba8.data(),im.rgba8.size());result.asset.images.push_back(std::move(im));}
        result.asset.samplers.reserve(samplerCount);for(std::uint32_t i=0;i<samplerCount;++i)result.asset.samplers.push_back({static_cast<ImportedWrapMode>(r.u16()),static_cast<ImportedWrapMode>(r.u16()),static_cast<ImportedTextureFilter>(r.u16()),static_cast<ImportedTextureFilter>(r.u16())});
        result.asset.textures.reserve(textureCount);for(std::uint32_t i=0;i<textureCount;++i){PolygonTexture t;t.name=r.string();t.imageIndex=r.u32();const bool has=r.u8()!=0;(void)r.u8();(void)r.u16();const auto sampler=r.u32();if(has)t.samplerIndex=sampler;result.asset.textures.push_back(std::move(t));}
        result.asset.vertices.resize(static_cast<std::size_t>(vertexCount));for(auto& v:result.asset.vertices){v.position={r.f32(),r.f32(),r.f32()};v.normal={r.f32(),r.f32(),r.f32()};v.tangent={r.f32(),r.f32(),r.f32(),r.f32()};v.texcoord={r.f32(),r.f32()};if(minor>=1U)v.texcoord1={r.f32(),r.f32()};v.color={r.f32(),r.f32(),r.f32(),r.f32()};}
        result.asset.indices.resize(static_cast<std::size_t>(indexCount));for(auto& i:result.asset.indices)i=r.u32();result.asset.submeshes.reserve(submeshCount);for(std::uint32_t i=0;i<submeshCount;++i)result.asset.submeshes.push_back({r.string(),r.u32(),r.u32(),r.u32()});result.asset.bounds.minimum={r.f32(),r.f32(),r.f32()};result.asset.bounds.maximum={r.f32(),r.f32(),r.f32()};if(r.remaining()!=0)throw std::runtime_error("DMESH has trailing data");result.asset.contentHash=storedHash;const auto valid=validate_polygon_asset(result.asset);if(!valid){result.code=valid.code;result.error=valid.message;return result;}result.code=PolygonAssetErrorCode::NoError;return result;
    }catch(const std::exception& e){result.code=PolygonAssetErrorCode::Corrupt;result.error=e.what();return result;}}

} // namespace dve
