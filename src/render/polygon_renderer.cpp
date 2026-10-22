#include "dve/render/polygon_renderer.hpp"

#include "dve/polygon_bvh.hpp"
#include "dve/render/ambient_occlusion.hpp"
#include "dve/render/surface_shading.hpp"
#include "dve/render/atmosphere.hpp"
#include "dve/render/subsurface.hpp"

#include <algorithm>
#include <bit>
#include <functional>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <tuple>

namespace dve::render {
namespace {
constexpr float kEpsilon = 1.0e-7F;

Float3 cross3(Float3 a, Float3 b) noexcept {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
Float3 mul3(Float3 a, Float3 b) noexcept { return {a.x*b.x,a.y*b.y,a.z*b.z}; }
Float4 mul4(Float4 a, Float4 b) noexcept { return {a.x*b.x,a.y*b.y,a.z*b.z,a.w*b.w}; }
Float4 scale4(Float4 a, float s) noexcept { return {a.x*s,a.y*s,a.z*s,a.w*s}; }
Float4 add4(Float4 a, Float4 b) noexcept { return {a.x+b.x,a.y+b.y,a.z+b.z,a.w+b.w}; }
float fract(float value) noexcept { return value-std::floor(value); }

struct CameraBasis { Float3 right{},up{},forward{}; float tanHalf{},aspect{}; };
CameraBasis make_basis(const PolygonCamera& camera, const PolygonRenderTarget& target) {
    CameraBasis basis;
    basis.forward=normalize(subtract(camera.target,camera.position));
    basis.right=normalize(cross3(basis.forward,camera.up));
    if(length_squared(basis.right)<kEpsilon)basis.right={1,0,0};
    basis.up=normalize(cross3(basis.right,basis.forward));
    basis.tanHalf=std::tan(camera.verticalFieldOfViewRadians*0.5F);
    basis.aspect=static_cast<float>(target.width)/static_cast<float>(target.height);
    return basis;
}

struct ScreenVertex {
    float x{},y{},depth{},cameraZ{},invZ{};
    Float3 normal{};
    Float3 worldPosition{};
    Float3 objectPosition{};
    Float4 tangent{1.0F,0.0F,0.0F,1.0F};
    Float2 uv{};
    Float2 uv1{};
    Float4 color{};
};

bool project_vertex(const PolygonVertex& vertex,const RigidTransform& transform,
                    const PolygonCamera& camera,const CameraBasis& basis,
                    const PolygonRenderTarget& target,ScreenVertex& output) {
    const Float3 world=transform_point(transform,vertex.position);
    const Float3 relative=subtract(world,camera.position);
    const float cx=dot(relative,basis.right),cy=dot(relative,basis.up),cz=dot(relative,basis.forward);
    if(!(cz>camera.nearPlane&&cz<camera.farPlane)||!std::isfinite(cz))return false;
    const float ndcX=cx/(cz*basis.tanHalf*basis.aspect);
    const float ndcY=cy/(cz*basis.tanHalf);
    output.x=(ndcX*0.5F+0.5F)*static_cast<float>(target.width);
    output.y=(0.5F-ndcY*0.5F)*static_cast<float>(target.height);
    output.depth=(cz-camera.nearPlane)/(camera.farPlane-camera.nearPlane);
    output.cameraZ=cz;output.invZ=1.0F/cz;
    output.normal=normalize(transform_vector(transform,vertex.normal));
    output.worldPosition=world;
    output.objectPosition=vertex.position;
    const Float3 tangent=normalize(transform_vector(transform,{vertex.tangent.x,vertex.tangent.y,vertex.tangent.z}));
    output.tangent={tangent.x,tangent.y,tangent.z,vertex.tangent.w};
    output.uv=vertex.texcoord;output.uv1=vertex.texcoord1;output.color=vertex.color;
    return std::isfinite(output.x)&&std::isfinite(output.y)&&std::isfinite(output.depth);
}

float edge(float ax,float ay,float bx,float by,float px,float py) noexcept {
    return (px-ax)*(by-ay)-(py-ay)*(bx-ax);
}

float wrap_coordinate(float value,ImportedWrapMode mode) noexcept {
    if(mode==ImportedWrapMode::ClampToEdge)return std::clamp(value,0.0F,1.0F);
    if(mode==ImportedWrapMode::MirroredRepeat){
        const float base=std::floor(value);const float local=value-base;
        return (static_cast<long long>(base)&1LL)!=0LL?1.0F-local:local;
    }
    return fract(value);
}

float srgb_to_linear(float encoded) noexcept {
    encoded = std::clamp(encoded, 0.0F, 1.0F);
    return encoded <= 0.04045F
        ? encoded / 12.92F
        : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}

Float4 texel(const PolygonTextureMip& mip,std::uint32_t x,std::uint32_t y,
             PolygonTextureColorSpace colorSpace) noexcept {
    x=std::min(x,mip.width-1U);y=std::min(y,mip.height-1U);
    const std::size_t o=(static_cast<std::size_t>(y)*mip.width+x)*4U;
    const float red = static_cast<float>(mip.rgba8[o]) / 255.0F;
    const float green = static_cast<float>(mip.rgba8[o + 1U]) / 255.0F;
    const float blue = static_cast<float>(mip.rgba8[o + 2U]) / 255.0F;
    const float alpha = static_cast<float>(mip.rgba8[o + 3U]) / 255.0F;
    if(colorSpace==PolygonTextureColorSpace::Srgb)
        return {srgb_to_linear(red), srgb_to_linear(green), srgb_to_linear(blue), alpha};
    return {red,green,blue,alpha};
}

Float4 sample_mip(const PolygonTextureMip& mip,const PolygonSampler& sampler,Float2 uv,
                  PolygonTextureColorSpace colorSpace) noexcept {
    const float u=wrap_coordinate(uv.x,sampler.wrapS),v=wrap_coordinate(uv.y,sampler.wrapT);
    const float x=u*static_cast<float>(mip.width-1U),y=v*static_cast<float>(mip.height-1U);
    if(sampler.magFilter==ImportedTextureFilter::Nearest){return texel(mip,static_cast<std::uint32_t>(std::round(x)),static_cast<std::uint32_t>(std::round(y)),colorSpace);}
    const auto x0=static_cast<std::uint32_t>(std::floor(x)),y0=static_cast<std::uint32_t>(std::floor(y));
    const auto x1=std::min(x0+1U,mip.width-1U),y1=std::min(y0+1U,mip.height-1U);
    const float tx=x-static_cast<float>(x0),ty=y-static_cast<float>(y0);
    const Float4 a=add4(scale4(texel(mip,x0,y0,colorSpace),1.0F-tx),scale4(texel(mip,x1,y0,colorSpace),tx));
    const Float4 b=add4(scale4(texel(mip,x0,y1,colorSpace),1.0F-tx),scale4(texel(mip,x1,y1,colorSpace),tx));
    return add4(scale4(a,1.0F-ty),scale4(b,ty));
}

Float2 select_uv(const PolygonTextureBinding& binding, Float2 uv0, Float2 uv1) noexcept {
    return binding.texcoord == 1U ? uv1 : uv0;
}

Float4 sample_binding_uv(const CookedPolygonAsset& asset,const PolygonTextureBinding& binding,
                         const std::vector<PolygonTextureMipChain>& chains,Float2 uv,
                         float lod,PolygonRenderStats& stats,Float4 fallback) noexcept {
    if(!binding.texture||*binding.texture>=asset.textures.size())return fallback;
    const PolygonTexture& texture=asset.textures[*binding.texture];
    if(texture.imageIndex>=chains.size()||chains[texture.imageIndex].levels.empty())return fallback;
    const PolygonSampler fallbackSampler{};
    const PolygonSampler& sampler=texture.samplerIndex&&*texture.samplerIndex<asset.samplers.size()?asset.samplers[*texture.samplerIndex]:fallbackSampler;
    const auto& levels=chains[texture.imageIndex].levels;
    const std::size_t level=std::min<std::size_t>(static_cast<std::size_t>(std::max(0.0F,std::floor(lod+0.5F))),levels.size()-1U);
    ++stats.textureSamples;
    return sample_mip(levels[level],sampler,uv,binding.colorSpace);
}

Float4 sample_binding(const CookedPolygonAsset& asset,const PolygonTextureBinding& binding,
                      const std::vector<PolygonTextureMipChain>& chains,Float2 uv0,Float2 uv1,
                      float lod,PolygonRenderStats& stats,Float4 fallback) noexcept {
    return sample_binding_uv(asset,binding,chains,select_uv(binding,uv0,uv1),lod,stats,fallback);
}

Float4 weighted4(Float4 a,Float4 b,Float4 c,Float3 weights) noexcept {
    return {a.x*weights.x+b.x*weights.y+c.x*weights.z,
            a.y*weights.x+b.y*weights.y+c.y*weights.z,
            a.z*weights.x+b.z*weights.y+c.z*weights.z,
            a.w*weights.x+b.w*weights.y+c.w*weights.z};
}

Float4 sample_triplanar_binding(const CookedPolygonAsset& asset,const PolygonTextureBinding& binding,
                                const std::vector<PolygonTextureMipChain>& chains,
                                Float3 position,Float3 normal,const TextureTransform2D& transform,
                                float scale,float sharpness,float lod,PolygonRenderStats& stats,
                                Float4 fallback) noexcept {
    if(!binding.texture)return fallback;
    const TriplanarCoordinates coordinates=make_triplanar_coordinates(position,normal,scale,sharpness);
    const Float4 x=sample_binding_uv(asset,binding,chains,transform_texture_coordinates(coordinates.xProjection,transform),lod,stats,fallback);
    const Float4 y=sample_binding_uv(asset,binding,chains,transform_texture_coordinates(coordinates.yProjection,transform),lod,stats,fallback);
    const Float4 z=sample_binding_uv(asset,binding,chains,transform_texture_coordinates(coordinates.zProjection,transform),lod,stats,fallback);
    stats.triplanarSamples+=3U;
    return weighted4(x,y,z,coordinates.weights);
}

Float4 sample_mapped_binding(const CookedPolygonAsset& asset,const PolygonTextureBinding& binding,
                             const std::vector<PolygonTextureMipChain>& chains,
                             const MaterialMappingSettings& mapping,Float2 mappedUv,
                             Float3 worldPosition,Float3 objectPosition,Float3 normal,
                             const TextureTransform2D& transform,float scaleMultiplier,float lod,
                             PolygonRenderStats& stats,Float4 fallback) noexcept {
    if(mapping.mappingMode==MaterialMappingMode::WorldTriplanar)
        return sample_triplanar_binding(asset,binding,chains,worldPosition,normal,transform,
            mapping.triplanarScale*scaleMultiplier,mapping.triplanarBlendSharpness,lod,stats,fallback);
    if(mapping.mappingMode==MaterialMappingMode::ObjectTriplanar)
        return sample_triplanar_binding(asset,binding,chains,objectPosition,normal,transform,
            mapping.triplanarScale*scaleMultiplier,mapping.triplanarBlendSharpness,lod,stats,fallback);
    return sample_binding_uv(asset,binding,chains,mappedUv,lod,stats,fallback);
}

MaterialSurfaceSample sample_layer_surface(
    const CookedPolygonAsset& asset,
    std::uint32_t materialIndex,
    const std::vector<PolygonTextureMipChain>& chains,
    Float2 uv0,
    Float2 uv1,
    Float3 worldPosition,
    Float3 objectPosition,
    Float3 geometricNormal,
    Float3 tangent,
    Float3 bitangent,
    float lod,
    bool enableTextures,
    PolygonRenderStats& stats) noexcept {
    MaterialSurfaceSample sample;
    if (materialIndex >= asset.materials.size() || materialIndex >= asset.materialBindings.size())
        return sample;
    ++stats.layerSourceSamples;
    const VoxelMaterialDefinition& material = asset.materials[materialIndex];
    const PolygonMaterialBinding& binding = asset.materialBindings[materialIndex];
    const Float2 selected = binding.mapping.mappingMode == MaterialMappingMode::UV1 ? uv1 : uv0;
    const Float2 mapped = transform_texture_coordinates(selected, binding.mapping.baseTransform);
    const Float4 baseMap = enableTextures
        ? sample_mapped_binding(asset,binding.baseColor,chains,binding.mapping,mapped,
                                worldPosition,objectPosition,geometricNormal,
                                binding.mapping.baseTransform,1.0F,lod,stats,{1,1,1,1})
        : Float4{1,1,1,1};
    const Float4 mr = enableTextures
        ? sample_mapped_binding(asset,binding.metallicRoughness,chains,binding.mapping,mapped,
                                worldPosition,objectPosition,geometricNormal,
                                binding.mapping.baseTransform,1.0F,lod,stats,{1,1,1,1})
        : Float4{1,1,1,1};
    const Float4 emissiveMap = enableTextures
        ? sample_mapped_binding(asset,binding.emissive,chains,binding.mapping,mapped,
                                worldPosition,objectPosition,geometricNormal,
                                binding.mapping.baseTransform,1.0F,lod,stats,{1,1,1,1})
        : Float4{1,1,1,1};
    const Float4 opacityMap = enableTextures
        ? sample_mapped_binding(asset,binding.opacity,chains,binding.mapping,mapped,
                                worldPosition,objectPosition,geometricNormal,
                                binding.mapping.baseTransform,1.0F,lod,stats,{1,1,1,1})
        : Float4{1,1,1,1};
    sample.baseColor = mul4(material.baseColor,baseMap);
    sample.opacity = sample.baseColor.w * (binding.opacity.texture ? opacityMap.x : 1.0F);
    sample.baseColor.w = sample.opacity;
    sample.metallic = std::clamp(material.metallic *
        (binding.metallicRoughness.texture ? mr.z : 1.0F),0.0F,1.0F);
    sample.roughness = std::clamp(material.roughness *
        (binding.metallicRoughness.texture ? mr.y : 1.0F),0.0F,1.0F);
    sample.emissive = binding.emissive.texture
        ? mul3(material.emissive,{emissiveMap.x,emissiveMap.y,emissiveMap.z})
        : material.emissive;
    sample.height = binding.height.texture && enableTextures
        ? sample_mapped_binding(asset,binding.height,chains,binding.mapping,mapped,
                                worldPosition,objectPosition,geometricNormal,
                                binding.mapping.baseTransform,1.0F,lod,stats,{0.5F,0.5F,0.5F,1.0F}).x
        : 0.5F;
    sample.normal = {0.0F,0.0F,1.0F};
    if (binding.normal.texture && enableTextures) {
        if (binding.mapping.mappingMode==MaterialMappingMode::WorldTriplanar ||
            binding.mapping.mappingMode==MaterialMappingMode::ObjectTriplanar) {
            const Float3 position = binding.mapping.mappingMode==MaterialMappingMode::WorldTriplanar
                ? worldPosition : objectPosition;
            const TriplanarCoordinates tc = make_triplanar_coordinates(
                position,geometricNormal,binding.mapping.triplanarScale,
                binding.mapping.triplanarBlendSharpness);
            const Float4 sx=sample_binding_uv(asset,binding.normal,chains,
                transform_texture_coordinates(tc.xProjection,binding.mapping.baseTransform),lod,stats,{0.5F,0.5F,1.0F,1.0F});
            const Float4 sy=sample_binding_uv(asset,binding.normal,chains,
                transform_texture_coordinates(tc.yProjection,binding.mapping.baseTransform),lod,stats,{0.5F,0.5F,1.0F,1.0F});
            const Float4 sz=sample_binding_uv(asset,binding.normal,chains,
                transform_texture_coordinates(tc.zProjection,binding.mapping.baseTransform),lod,stats,{0.5F,0.5F,1.0F,1.0F});
            const Float3 nx=normalize(Float3{(sx.z*2.0F-1.0F)*(geometricNormal.x<0?-1.0F:1.0F),(sx.y*2.0F-1.0F)*binding.normalScale,(sx.x*2.0F-1.0F)*binding.normalScale});
            const Float3 ny=normalize(Float3{(sy.x*2.0F-1.0F)*binding.normalScale,(sy.z*2.0F-1.0F)*(geometricNormal.y<0?-1.0F:1.0F),(sy.y*2.0F-1.0F)*binding.normalScale});
            const Float3 nz=normalize(Float3{(sz.x*2.0F-1.0F)*binding.normalScale,(sz.y*2.0F-1.0F)*binding.normalScale,(sz.z*2.0F-1.0F)*(geometricNormal.z<0?-1.0F:1.0F)});
            const Float3 worldLayer=normalize(add(add(multiply(nx,tc.weights.x),multiply(ny,tc.weights.y)),multiply(nz,tc.weights.z)));
            sample.normal={dot(worldLayer,tangent),dot(worldLayer,bitangent),dot(worldLayer,geometricNormal)};
            stats.triplanarSamples+=3U;
        } else {
            const Float4 nm=sample_binding_uv(asset,binding.normal,chains,mapped,lod,stats,{0.5F,0.5F,1.0F,1.0F});
            sample.normal=normalize(Float3{(nm.x*2.0F-1.0F)*binding.normalScale,
                                           (nm.y*2.0F-1.0F)*binding.normalScale,
                                           nm.z*2.0F-1.0F});
        }
    }
    return sample;
}

Float4 beauty(const LightPasses& light,float alpha) noexcept{const Float3 s=light.sum();return {s.x,s.y,s.z,alpha};}

// The polygon reference's shading as light passes (surface_shading.hpp; the same model as the
// voxel reference): the sun's diffuse and specular lobes (no sun shadows on polygons), the sky
// hemisphere's diffuse light and specular reflection (AO-occluded), foliage and subsurface
// transmission, and emission. Their sum is the image.
LightPasses shade(const VoxelMaterialDefinition& material,Float4 base,Float3 normal,Float3 view,
                  const RenderEnvironment& environment,bool frontFacing,Float3 transmitted={},
                  float diffusedSun=-1.0F,float ambientOcclusion=1.0F) noexcept {
    LightPasses out;
    const Float3 rgb={base.x,base.y,base.z};
    if(material.shadingModel==MaterialShadingModel::Unlit){out.emission=rgb;return out;}
    if(material.shadingModel==MaterialShadingModel::Emissive){out.emission=add(rgb,material.emissive);return out;}
    normal=normalize(normal);
    if(material.shadingModel==MaterialShadingModel::TwoSidedFoliage&&!frontFacing)normal=multiply(normal,-1.0F);
    view=normalize(view);
    const Float3 light=normalize(environment.sunDirection);
    const float ndotl=dot(normal,light);
    const Float3 sunIrradiance=sun_irradiance(environment);
    // Diffuse: base x (1 - metallic) x (1 - F) x sun x cos (or its subsurface-diffused average).
    const Float3 f0=specular_f0(rgb,material.metallic,material.specular);
    const Float3 halfVector=normalize(add(view,light));
    const Float3 fresnel=fresnel_schlick(std::clamp(dot(view,halfVector),0.0F,1.0F),f0);
    const Float3 diffuseColor=mul3(multiply(rgb,1.0F-material.metallic),Float3{1.0F-fresnel.x,1.0F-fresnel.y,1.0F-fresnel.z});
    const float sunTerm=diffusedSun>=0.0F?diffusedSun:std::max(0.0F,ndotl);
    out.directDiffuse=multiply(mul3(diffuseColor,sunIrradiance),sunTerm);
    const ShadingLobes lobes=material.shadingModel==MaterialShadingModel::ClearCoat
        ? shade_clear_coat_direct(normal,view,light,sunIrradiance,rgb,material.metallic,material.roughness,material.specular,
                                  material.clearCoat,material.clearCoatRoughness)
        : shade_direct_light(normal,view,light,sunIrradiance,rgb,material.metallic,material.roughness,material.specular);
    out.directSpecular=multiply(lobes.specular,kCpuRadianceScale);
    if(material.shadingModel==MaterialShadingModel::ClearCoat&&material.clearCoat>0.0F){
        const Float3 coat=fresnel_schlick(std::clamp(dot(view,halfVector),0.0F,1.0F),{0.04F,0.04F,0.04F});
        const float c=std::clamp(material.clearCoat,0.0F,1.0F);
        const Float3 through{1.0F-c*coat.x,1.0F-c*coat.y,1.0F-c*coat.z};
        out.directDiffuse=mul3(out.directDiffuse,mul3(through,through));
    }
    if(environment.globalIlluminationMode!=GlobalIlluminationMode::Off){
        // No secondary rays here beyond AO: the sky hemisphere around the normal, occluded.
        const float intensity=environment.globalIlluminationIntensity;
        const Float3 irradiance=multiply(sky_irradiance(environment,normal),intensity*ambientOcclusion);
        const Float3 reflection=subtract(multiply(normal,2.0F*dot(normal,view)),view);
        const ShadingLobes ambient=shade_ambient_light(normal,view,irradiance,multiply(sky_radiance(environment,reflection),intensity),
                                                       rgb,material.metallic,material.roughness,material.specular,ambientOcclusion);
        out.indirectDiffuse=ambient.diffuse;out.indirectSpecular=ambient.specular;
    }
    if(material.shadingModel==MaterialShadingModel::TwoSidedFoliage){
        const float wrap=std::clamp(((-ndotl)+material.foliageWrap)/(1.0F+material.foliageWrap),0.0F,1.0F);
        out.transmission=multiply(mul3(material.foliageColor,sunIrradiance),wrap*material.foliageTransmittance);
    }
    out.transmission=add(out.transmission,transmitted);   // subsurface translucency (its own colour)
    out.emission=material.emissive;
    return out;
}

Float4 material_debug_output(PolygonMaterialDebugView view, Float4 base, Float3 normal,
                             const VoxelMaterialDefinition& material, Float2 uv0, Float2 uv1,
                             Float3 worldPosition, Float3 objectPosition,
                             const MaterialMappingSettings& mapping, float detailFade,
                             std::uint32_t parallaxSamples, float layerMask,
                             float layerCoverage) noexcept {
    switch (view) {
        case PolygonMaterialDebugView::Lit: break;
        case PolygonMaterialDebugView::BaseColor:
            return {base.x, base.y, base.z, 1.0F};
        case PolygonMaterialDebugView::WorldNormal:
            return {normal.x * 0.5F + 0.5F, normal.y * 0.5F + 0.5F,
                    normal.z * 0.5F + 0.5F, 1.0F};
        case PolygonMaterialDebugView::Metallic:
            return {material.metallic, material.metallic, material.metallic, 1.0F};
        case PolygonMaterialDebugView::Roughness:
            return {material.roughness, material.roughness, material.roughness, 1.0F};
        case PolygonMaterialDebugView::Emissive:
            return {material.emissive.x, material.emissive.y, material.emissive.z, 1.0F};
        case PolygonMaterialDebugView::Opacity:
            return {base.w, base.w, base.w, 1.0F};
        case PolygonMaterialDebugView::Uv0:
            return {fract(uv0.x), fract(uv0.y), 0.0F, 1.0F};
        case PolygonMaterialDebugView::Uv1:
            return {fract(uv1.x), fract(uv1.y), 0.0F, 1.0F};
        case PolygonMaterialDebugView::TriplanarWeights: {
            if (mapping.mappingMode != MaterialMappingMode::WorldTriplanar &&
                mapping.mappingMode != MaterialMappingMode::ObjectTriplanar) {
                return {0.0F, 0.0F, 0.0F, 1.0F};
            }
            const Float3 position = mapping.mappingMode == MaterialMappingMode::WorldTriplanar
                ? worldPosition : objectPosition;
            const TriplanarCoordinates coordinates = make_triplanar_coordinates(
                position, normal, mapping.triplanarScale, mapping.triplanarBlendSharpness);
            return {coordinates.weights.x, coordinates.weights.y, coordinates.weights.z, 1.0F};
        }
        case PolygonMaterialDebugView::DetailFade:
            return {detailFade, detailFade, detailFade, 1.0F};
        case PolygonMaterialDebugView::ParallaxSampleCount: {
            const std::uint32_t maximum = mapping.heightMode == HeightMappingMode::ParallaxOcclusion
                ? mapping.maximumHeightSteps + mapping.refinementSteps
                : (mapping.heightMode == HeightMappingMode::SteepParallax
                    ? mapping.maximumHeightSteps
                    : (mapping.heightMode == HeightMappingMode::OffsetParallax ? 1U : 0U));
            const float normalized = maximum == 0U ? 0.0F
                : std::clamp(static_cast<float>(parallaxSamples) / static_cast<float>(maximum),
                             0.0F, 1.0F);
            return {normalized, normalized, normalized, 1.0F};
        }
        case PolygonMaterialDebugView::LayerMask:
            return {layerMask, layerMask, layerMask, 1.0F};
        case PolygonMaterialDebugView::LayerCoverage:
            return {layerCoverage, layerCoverage, layerCoverage, 1.0F};
    }
    return base;
}

struct DeferredTriangle {
    const CookedPolygonAsset* asset{};const PolygonSubmesh* submesh{};const PolygonMaterialBinding* binding{};
    const VoxelMaterialDefinition* material{};std::array<ScreenVertex,3> v{};std::uint64_t objectId{};Float4 tint{};
    Float3 cameraPosition{};
    float sortDepth{};bool frontFacing{};std::shared_ptr<const std::vector<PolygonTextureMipChain>> mips;
    // Subsurface translucency: the mesh's BVH (only for assets with a Subsurface material) and pose.
    const PolygonBvh* subsurfaceBvh{};RigidTransform transform{};
};

float projected_diameter(const PolygonBounds& bounds,const RigidTransform& transform,
                         const PolygonCamera& camera,const CameraBasis& basis,
                         const PolygonRenderTarget& target) noexcept {
    const Float3 center=multiply(add(bounds.minimum,bounds.maximum),0.5F);
    const Float3 half=multiply(subtract(bounds.maximum,bounds.minimum),0.5F);
    const float radius=length(half);const Float3 world=transform_point(transform,center);
    const float z=dot(subtract(world,camera.position),basis.forward);
    if(z<=camera.nearPlane)return std::numeric_limits<float>::infinity();
    return 2.0F*radius/(z*basis.tanHalf)*static_cast<float>(target.height)*0.5F;
}

struct LodChoice { const CookedPolygonAsset* asset{}; float threshold{-1.0F}; };
LodChoice select_lod(const PolygonRenderInstance& instance,float diameter) noexcept {
    LodChoice selected{instance.asset};
    float lowest=std::numeric_limits<float>::infinity();
    const CookedPolygonAsset* coarsest{};
    for(const PolygonLodLevel& lod:instance.lods){
        const float threshold=lod.minimumProjectedDiameterPixels;
        if(!lod.asset || !std::isfinite(threshold) || threshold<0.0F) continue;
        if(threshold<lowest){lowest=threshold;coarsest=lod.asset;}
        if(diameter>=threshold && threshold>=selected.threshold) selected={lod.asset,threshold};
    }
    // Below every threshold, retain the coarsest available mesh rather than
    // unexpectedly jumping back to the full-detail base asset.
    if(selected.threshold<0.0F && coarsest) selected={coarsest,lowest};
    return selected;
}

PolygonBounds vertex_bounds(const CookedPolygonAsset& asset) noexcept {
    PolygonBounds bounds{asset.vertices.front().position,asset.vertices.front().position};
    for(const auto& v:asset.vertices){
        bounds.minimum={std::min(bounds.minimum.x,v.position.x),std::min(bounds.minimum.y,v.position.y),std::min(bounds.minimum.z,v.position.z)};
        bounds.maximum={std::max(bounds.maximum.x,v.position.x),std::max(bounds.maximum.y,v.position.y),std::max(bounds.maximum.z,v.position.z)};
    }
    return bounds;
}

bool outside_frustum(const PolygonBounds& bounds,const RigidTransform& transform,
                     const PolygonCamera& camera,const CameraBasis& basis) noexcept {
    const Float3 center=subtract(transform_point(transform,multiply(add(bounds.minimum,bounds.maximum),0.5F)),camera.position);
    const Float3 half=multiply(subtract(bounds.maximum,bounds.minimum),0.5F);
    const Float3 x=transform_vector(transform,{half.x,0,0}),y=transform_vector(transform,{0,half.y,0}),z=transform_vector(transform,{0,0,half.z});
    const auto outside=[&](Float3 normal,float offset){
        const float distance=dot(center,normal)+offset;
        const float radius=std::abs(dot(x,normal))+std::abs(dot(y,normal))+std::abs(dot(z,normal));
        // Non-finite/uncertain bounds remain visible. A tolerance preserves
        // touching and near-plane-intersecting geometry through float rounding.
        const float margin=1.0e-5F*(1.0F+std::abs(distance)+radius);
        return std::isfinite(distance) && std::isfinite(radius) && distance+radius < -margin;
    };
    const Float3 horizontal=multiply(basis.forward,basis.tanHalf*basis.aspect);
    const Float3 vertical=multiply(basis.forward,basis.tanHalf);
    return outside(basis.forward,-camera.nearPlane) || outside(multiply(basis.forward,-1),camera.farPlane) ||
        outside(add(horizontal,basis.right),0) || outside(subtract(horizontal,basis.right),0) ||
        outside(add(vertical,basis.up),0) || outside(subtract(vertical,basis.up),0);
}

// Sunlight scattered through a Subsurface mesh to its shaded side (see subsurface.hpp). The
// polygon reference has no sun shadows, so the sunlit far side is taken as unshadowed, as the
// near side's direct term is.
Float3 polygon_subsurface_light(const DeferredTriangle& tri,const VoxelMaterialDefinition& material,Float3 worldPosition,
                                Float3 normal,const RenderEnvironment& environment,const PolygonRenderOptions& options,
                                PolygonRenderStats& stats){
    if(!tri.subsurfaceBvh||!material_has_subsurface(material))return {};
    const float metersPerUnit=options.metersPerUnit>0.0F&&std::isfinite(options.metersPerUnit)?options.metersPerUnit:kDefaultMetersPerVoxel;
    const float maximum=environment.subsurfaceMaxDistanceMeters/metersPerUnit;
    if(!(maximum>0.0F))return {};
    const Float3 sun=normalize(environment.sunDirection);
    normal=normalize(normal);
    if(!(dot(normal,sun)<0.0F))return {};
    ++stats.subsurfaceMarches;
    const SubsurfaceExit exit=march_polygon_subsurface(*tri.subsurfaceBvh,*tri.asset,tri.transform,
                                                       add(worldPosition,multiply(normal,-1.0e-3F)),sun,maximum);
    if(!exit.found)return {};
    ++stats.subsurfaceExits;
    return subsurface_transmitted_radiance(material,exit,sun,metersPerUnit,1.0F,environment);
}

// Front-lit subsurface diffusion (see subsurface.hpp and the voxel reference's
// diffused_sun_term): cos(normal, sun) averaged over the mesh around the point with Burley's
// profile. Samples are projected onto the mesh along the normal; samples landing on another
// material or on nothing are left out. No sun shadows here, as for the rest of the polygon path.
float polygon_diffused_sun(const DeferredTriangle& tri,const VoxelMaterialDefinition& material,Float3 worldPosition,
                           Float3 normal,const RenderEnvironment& environment,const PolygonRenderOptions& options,
                           std::size_t pixel,PolygonRenderStats& stats){
    const float metersPerUnit=options.metersPerUnit>0.0F&&std::isfinite(options.metersPerUnit)?options.metersPerUnit:kDefaultMetersPerVoxel;
    const float d=material.subsurfaceScatterDistanceMeters/metersPerUnit;
    const Float3 sun=normalize(environment.sunDirection);
    normal=normalize(normal);
    const Float3 helper=std::abs(normal.y)<0.9F?Float3{0.0F,1.0F,0.0F}:Float3{1.0F,0.0F,0.0F};
    const Float3 tangent=normalize(cross3(helper,normal)),bitangent=cross3(normal,tangent);
    const std::uint32_t samples=options.subsurfaceDiffusionSamples;
    const std::uint32_t seed=static_cast<std::uint32_t>(pixel)*747796405U+2891336453U;
    const float rotation=static_cast<float>(seed>>8U)*(1.0F/16777216.0F);
    float sum=0.0F;std::uint32_t valid=0;
    for(std::uint32_t i=0;i<samples;++i){
        const float u=(static_cast<float>(i)+0.5F)/static_cast<float>(samples);
        const float v=std::fmod(static_cast<float>(i)*0.6180339887F+rotation,1.0F);
        const float r=burley_sample_radius(u,d);
        Float3 qn=normal;
        if(r>1.0e-4F){
            const float angle=6.2831853F*v;
            const Float3 offset=add(multiply(tangent,r*std::cos(angle)),multiply(bitangent,r*std::sin(angle)));
            const float h=std::max(r,1.0e-2F);
            ++stats.subsurfaceDiffusionProbes;
            const auto hit=probe_polygon_surface(*tri.subsurfaceBvh,*tri.asset,tri.transform,
                                                 add(add(worldPosition,offset),multiply(normal,h)),multiply(normal,-1.0F),2.0F*h);
            if(!hit||hit->materialIndex!=tri.submesh->materialIndex)continue;
            qn=hit->normal;
        }
        ++valid;
        sum+=std::max(0.0F,dot(qn,sun));
    }
    return valid>0U?sum/static_cast<float>(valid):0.0F;
}

bool polygon_ambient_occlusion_active(const RenderEnvironment& environment) noexcept {
    return environment.globalIlluminationMode!=GlobalIlluminationMode::Off&&environment.ambientOcclusionSamples>0U&&
           environment.ambientOcclusionMaxDistanceMeters>0.0F;
}

// The ambient's occlusion at a fragment (reference_ambient_occlusion: the GPU passes' rays).
float polygon_ambient_occlusion(Float3 worldPosition,Float3 normal,const RenderEnvironment& environment,
                                const PolygonRenderOptions& options,std::size_t pixel,PolygonRenderStats& stats){
    if(!options.occlusionRay||!polygon_ambient_occlusion_active(environment))return 1.0F;
    const float metersPerUnit=options.metersPerUnit>0.0F&&std::isfinite(options.metersPerUnit)?options.metersPerUnit:kDefaultMetersPerVoxel;
    return reference_ambient_occlusion(worldPosition,normalize(normal),static_cast<std::uint32_t>(pixel),0U,environment,metersPerUnit,
        [&](Float3 origin,Float3 direction,float maximum){++stats.ambientOcclusionRays;return options.occlusionRay(origin,direction,maximum);});
}

void raster_triangle(const DeferredTriangle& tri,PolygonRenderTarget& target,const RenderEnvironment& environment,
                     const PolygonRenderOptions& options,bool transparent,PolygonRenderStats& stats) {
    const float area=edge(tri.v[0].x,tri.v[0].y,tri.v[1].x,tri.v[1].y,tri.v[2].x,tri.v[2].y);
    if(!std::isfinite(area)||std::abs(area)<kEpsilon)return;
    const float left=std::floor(std::min({tri.v[0].x,tri.v[1].x,tri.v[2].x})),right=std::ceil(std::max({tri.v[0].x,tri.v[1].x,tri.v[2].x}));
    const float top=std::floor(std::min({tri.v[0].y,tri.v[1].y,tri.v[2].y})),bottom=std::ceil(std::max({tri.v[0].y,tri.v[1].y,tri.v[2].y}));
    if(right<0 || bottom<0 || left>=static_cast<float>(target.width) || top>=static_cast<float>(target.height))return;
    // Clip floating bounds before narrowing; offscreen coordinates can exceed int.
    const int minX=static_cast<int>(std::max(0.0F,left));
    const int maxX=static_cast<int>(std::min(static_cast<float>(target.width-1U),right));
    const int minY=static_cast<int>(std::max(0.0F,top));
    const int maxY=static_cast<int>(std::min(static_cast<float>(target.height-1U),bottom));
    if(minX>maxX||minY>maxY)return;
    ++stats.rasterizedTriangles;

    float textureLod = 0.0F;
    const std::array<const PolygonTextureBinding*,9> textureBindings{{
        &tri.binding->baseColor,&tri.binding->metallicRoughness,&tri.binding->normal,
        &tri.binding->emissive,&tri.binding->opacity,&tri.binding->height,
        &tri.binding->detailBaseColor,&tri.binding->detailNormal,&tri.binding->detailRoughness}};
    const auto accumulate_texture_lod = [&](const PolygonTextureBinding& textureBinding) {
        if(!textureBinding.texture||*textureBinding.texture>=tri.asset->textures.size())return;
        const PolygonTexture& texture=tri.asset->textures[*textureBinding.texture];
        if(texture.imageIndex>=tri.asset->images.size())return;
        const PolygonImage& image=tri.asset->images[texture.imageIndex];
        const Float2 uvA=textureBinding.texcoord==1U?tri.v[0].uv1:tri.v[0].uv;
        const Float2 uvB=textureBinding.texcoord==1U?tri.v[1].uv1:tri.v[1].uv;
        const Float2 uvC=textureBinding.texcoord==1U?tri.v[2].uv1:tri.v[2].uv;
        const float uvArea=std::abs((uvB.x-uvA.x)*(uvC.y-uvA.y)-(uvB.y-uvA.y)*(uvC.x-uvA.x));
        const float screenArea=std::abs(area);
        if(screenArea>kEpsilon&&uvArea>0.0F){
            const float coveredTexels=uvArea*static_cast<float>(image.width)*static_cast<float>(image.height);
            textureLod=std::max(textureLod,std::log2(std::max(1.0F,std::sqrt(coveredTexels/screenArea))));
        }
    };
    for(const PolygonTextureBinding* textureBinding:textureBindings) accumulate_texture_lod(*textureBinding);
    for(const PolygonMaterialLayerBinding& layerBinding:tri.binding->layers){
        accumulate_texture_lod(layerBinding.mask);
        accumulate_texture_lod(layerBinding.height);
    }

    for(int y=minY;y<=maxY;++y)for(int x=minX;x<=maxX;++x){
        const float px=static_cast<float>(x)+0.5F,py=static_cast<float>(y)+0.5F;
        float w0=edge(tri.v[1].x,tri.v[1].y,tri.v[2].x,tri.v[2].y,px,py)/area;
        float w1=edge(tri.v[2].x,tri.v[2].y,tri.v[0].x,tri.v[0].y,px,py)/area;
        float w2=1.0F-w0-w1;if(w0<-kEpsilon||w1<-kEpsilon||w2<-kEpsilon)continue;
        const float invZ=w0*tri.v[0].invZ+w1*tri.v[1].invZ+w2*tri.v[2].invZ;if(invZ<=0)continue;
        w0=w0*tri.v[0].invZ/invZ;w1=w1*tri.v[1].invZ/invZ;w2=1.0F-w0-w1;
        const float depth=w0*tri.v[0].depth+w1*tri.v[1].depth+w2*tri.v[2].depth;
        const std::size_t index=static_cast<std::size_t>(y)*target.width+static_cast<std::size_t>(x);
        if(depth>=target.depth[index]){++stats.depthRejectedFragments;continue;}
        Float2 uv{w0*tri.v[0].uv.x+w1*tri.v[1].uv.x+w2*tri.v[2].uv.x,
                  w0*tri.v[0].uv.y+w1*tri.v[1].uv.y+w2*tri.v[2].uv.y};
        Float2 uv1{w0*tri.v[0].uv1.x+w1*tri.v[1].uv1.x+w2*tri.v[2].uv1.x,
                   w0*tri.v[0].uv1.y+w1*tri.v[1].uv1.y+w2*tri.v[2].uv1.y};
        const Float3 worldPosition{
            w0*tri.v[0].worldPosition.x+w1*tri.v[1].worldPosition.x+w2*tri.v[2].worldPosition.x,
            w0*tri.v[0].worldPosition.y+w1*tri.v[1].worldPosition.y+w2*tri.v[2].worldPosition.y,
            w0*tri.v[0].worldPosition.z+w1*tri.v[1].worldPosition.z+w2*tri.v[2].worldPosition.z};
        const Float3 objectPosition{
            w0*tri.v[0].objectPosition.x+w1*tri.v[1].objectPosition.x+w2*tri.v[2].objectPosition.x,
            w0*tri.v[0].objectPosition.y+w1*tri.v[1].objectPosition.y+w2*tri.v[2].objectPosition.y,
            w0*tri.v[0].objectPosition.z+w1*tri.v[1].objectPosition.z+w2*tri.v[2].objectPosition.z};
        Float4 vertexColor{w0*tri.v[0].color.x+w1*tri.v[1].color.x+w2*tri.v[2].color.x,
                           w0*tri.v[0].color.y+w1*tri.v[1].color.y+w2*tri.v[2].color.y,
                           w0*tri.v[0].color.z+w1*tri.v[1].color.z+w2*tri.v[2].color.z,
                           w0*tri.v[0].color.w+w1*tri.v[1].color.w+w2*tri.v[2].color.w};
        const auto& chains=*tri.mips;
        const Float3 interpolatedNormal{
            w0 * tri.v[0].normal.x + w1 * tri.v[1].normal.x + w2 * tri.v[2].normal.x,
            w0 * tri.v[0].normal.y + w1 * tri.v[1].normal.y + w2 * tri.v[2].normal.y,
            w0 * tri.v[0].normal.z + w1 * tri.v[1].normal.z + w2 * tri.v[2].normal.z};
        Float3 normal=normalize(interpolatedNormal);
        Float3 tangent=normalize(Float3{w0*tri.v[0].tangent.x+w1*tri.v[1].tangent.x+w2*tri.v[2].tangent.x,
                                      w0*tri.v[0].tangent.y+w1*tri.v[1].tangent.y+w2*tri.v[2].tangent.y,
                                      w0*tri.v[0].tangent.z+w1*tri.v[1].tangent.z+w2*tri.v[2].tangent.z});
        tangent=normalize(subtract(tangent,multiply(normal,dot(normal,tangent))));
        const float handed=(w0*tri.v[0].tangent.w+w1*tri.v[1].tangent.w+w2*tri.v[2].tangent.w)<0.0F?-1.0F:1.0F;
        const Float3 bitangent=multiply(cross3(normal,tangent),handed);
        const Float3 viewWorld=normalize(subtract(tri.cameraPosition,worldPosition));
        const Float3 viewTangent{dot(viewWorld,tangent),dot(viewWorld,bitangent),dot(viewWorld,normal)};
        const float cameraDistance=length(subtract(tri.cameraPosition,worldPosition));
        Float2 selectedUv=tri.binding->mapping.mappingMode==MaterialMappingMode::UV1?uv1:uv;
        Float2 mappedUv=transform_texture_coordinates(selectedUv,tri.binding->mapping.baseTransform);
        std::uint32_t fragmentParallaxSamples = 0U;
        if(options.enableTextures&&tri.binding->height.texture&&tri.binding->mapping.heightMode!=HeightMappingMode::Off){
            const auto heightSampler=[&](Float2 sampleUv){return sample_binding_uv(*tri.asset,tri.binding->height,chains,sampleUv,textureLod,stats,{0.5F,0.5F,0.5F,1.0F}).x;};
            const ParallaxMappingResult parallax=apply_parallax_mapping(mappedUv,viewTangent,cameraDistance,tri.binding->mapping,heightSampler);
            mappedUv=parallax.uv;fragmentParallaxSamples=parallax.samples;stats.parallaxSamples+=parallax.samples;
        }
        const Float4 textureColor=options.enableTextures?sample_mapped_binding(*tri.asset,tri.binding->baseColor,chains,tri.binding->mapping,mappedUv,worldPosition,objectPosition,normal,tri.binding->mapping.baseTransform,1.0F,textureLod,stats,{1,1,1,1}):Float4{1,1,1,1};
        const Float4 mr=options.enableTextures?sample_mapped_binding(*tri.asset,tri.binding->metallicRoughness,chains,tri.binding->mapping,mappedUv,worldPosition,objectPosition,normal,tri.binding->mapping.baseTransform,1.0F,textureLod,stats,{1,1,1,1}):Float4{1,1,1,1};
        const Float4 emissiveMap=options.enableTextures?sample_mapped_binding(*tri.asset,tri.binding->emissive,chains,tri.binding->mapping,mappedUv,worldPosition,objectPosition,normal,tri.binding->mapping.baseTransform,1.0F,textureLod,stats,{1,1,1,1}):Float4{1,1,1,1};
        const Float4 opacityMap=options.enableTextures?sample_mapped_binding(*tri.asset,tri.binding->opacity,chains,tri.binding->mapping,mappedUv,worldPosition,objectPosition,normal,tri.binding->mapping.baseTransform,1.0F,textureLod,stats,{1,1,1,1}):Float4{1,1,1,1};
        Float4 base=mul4(mul4(mul4(tri.material->baseColor,vertexColor),textureColor),tri.tint);
        if(tri.binding->opacity.texture)base.w*=opacityMap.x;
        VoxelMaterialDefinition effective=*tri.material;
        if(tri.binding->metallicRoughness.texture){effective.roughness=std::clamp(effective.roughness*mr.y,0.0F,1.0F);effective.metallic=std::clamp(effective.metallic*mr.z,0.0F,1.0F);}
        if(tri.binding->emissive.texture)effective.emissive=mul3(effective.emissive,{emissiveMap.x,emissiveMap.y,emissiveMap.z});
        const float detailFade=material_detail_fade(cameraDistance,tri.binding->mapping);
        Float2 detailUv=transform_texture_coordinates(selectedUv,tri.binding->mapping.detailTransform);
        if(options.enableTextures&&detailFade>0.0F&&tri.binding->detailBaseColor.texture){
            const Float4 detail=sample_mapped_binding(*tri.asset,tri.binding->detailBaseColor,chains,tri.binding->mapping,detailUv,worldPosition,objectPosition,normal,tri.binding->mapping.detailTransform,8.0F,textureLod,stats,{0.5F,0.5F,0.5F,1.0F});
            base=blend_detail_color(base,detail,tri.binding->mapping.detailColorStrength*detailFade);++stats.detailSamples;
        }
        if(options.enableTextures&&detailFade>0.0F&&tri.binding->detailRoughness.texture){
            const Float4 detail=sample_mapped_binding(*tri.asset,tri.binding->detailRoughness,chains,tri.binding->mapping,detailUv,worldPosition,objectPosition,normal,tri.binding->mapping.detailTransform,8.0F,textureLod,stats,{0.5F,0.5F,0.5F,1.0F});
            effective.roughness=blend_detail_roughness(effective.roughness,detail.x,tri.binding->mapping.detailRoughnessStrength*detailFade);++stats.detailSamples;
        }
        if(options.enableTextures&&tri.binding->normal.texture){
            if(tri.binding->mapping.mappingMode==MaterialMappingMode::WorldTriplanar||tri.binding->mapping.mappingMode==MaterialMappingMode::ObjectTriplanar){
                const Float3 position=tri.binding->mapping.mappingMode==MaterialMappingMode::WorldTriplanar?worldPosition:objectPosition;
                const TriplanarCoordinates tc=make_triplanar_coordinates(position,normal,tri.binding->mapping.triplanarScale,tri.binding->mapping.triplanarBlendSharpness);
                const Float4 sx=sample_binding_uv(*tri.asset,tri.binding->normal,chains,transform_texture_coordinates(tc.xProjection,tri.binding->mapping.baseTransform),textureLod,stats,{0.5F,0.5F,1.0F,1.0F});
                const Float4 sy=sample_binding_uv(*tri.asset,tri.binding->normal,chains,transform_texture_coordinates(tc.yProjection,tri.binding->mapping.baseTransform),textureLod,stats,{0.5F,0.5F,1.0F,1.0F});
                const Float4 sz=sample_binding_uv(*tri.asset,tri.binding->normal,chains,transform_texture_coordinates(tc.zProjection,tri.binding->mapping.baseTransform),textureLod,stats,{0.5F,0.5F,1.0F,1.0F});
                const Float3 nx=normalize(Float3{(sx.z*2.0F-1.0F)*(normal.x<0?-1.0F:1.0F),(sx.y*2.0F-1.0F)*tri.binding->normalScale,(sx.x*2.0F-1.0F)*tri.binding->normalScale});
                const Float3 ny=normalize(Float3{(sy.x*2.0F-1.0F)*tri.binding->normalScale,(sy.z*2.0F-1.0F)*(normal.y<0?-1.0F:1.0F),(sy.y*2.0F-1.0F)*tri.binding->normalScale});
                const Float3 nz=normalize(Float3{(sz.x*2.0F-1.0F)*tri.binding->normalScale,(sz.y*2.0F-1.0F)*tri.binding->normalScale,(sz.z*2.0F-1.0F)*(normal.z<0?-1.0F:1.0F)});
                normal=normalize(add(add(multiply(nx,tc.weights.x),multiply(ny,tc.weights.y)),multiply(nz,tc.weights.z)));stats.triplanarSamples+=3U;
            }else{
                const Float4 nm=sample_binding_uv(*tri.asset,tri.binding->normal,chains,mappedUv,textureLod,stats,{0.5F,0.5F,1.0F,1.0F});
                const Float3 mapNormal=normalize(Float3{(nm.x*2.0F-1.0F)*tri.binding->normalScale,(nm.y*2.0F-1.0F)*tri.binding->normalScale,nm.z*2.0F-1.0F});
                normal=normalize(add(add(multiply(tangent,mapNormal.x),multiply(bitangent,mapNormal.y)),multiply(normal,mapNormal.z)));
            }
        }
        if(options.enableTextures&&detailFade>0.0F&&tri.binding->detailNormal.texture){
            const Float4 dn=sample_mapped_binding(*tri.asset,tri.binding->detailNormal,chains,tri.binding->mapping,detailUv,worldPosition,objectPosition,normal,tri.binding->mapping.detailTransform,8.0F,textureLod,stats,{0.5F,0.5F,1.0F,1.0F});
            const Float3 detailTangent=normalize(Float3{(dn.x*2.0F-1.0F)*tri.binding->detailNormalScale,(dn.y*2.0F-1.0F)*tri.binding->detailNormalScale,dn.z*2.0F-1.0F});
            const Float3 detailWorld=normalize(add(add(multiply(tangent,detailTangent.x),multiply(bitangent,detailTangent.y)),multiply(normal,detailTangent.z)));
            normal=blend_detail_normal(normal,detailWorld,tri.binding->mapping.detailNormalStrength*detailFade);++stats.detailSamples;
        }

        float layerDebugMask=0.0F;
        float layerDebugCoverage=0.0F;
        bool appliedMaterialLayer=false;
        if(!tri.material->layers.empty()){
            const float baseHeight=(options.enableTextures&&tri.binding->height.texture)
                ? sample_mapped_binding(*tri.asset,tri.binding->height,chains,tri.binding->mapping,mappedUv,
                    worldPosition,objectPosition,normal,tri.binding->mapping.baseTransform,1.0F,
                    textureLod,stats,{0.5F,0.5F,0.5F,1.0F}).x
                : 0.5F;
            MaterialSurfaceSample surface;
            surface.baseColor=base;surface.normal=normal;surface.metallic=effective.metallic;
            surface.roughness=effective.roughness;surface.emissive=effective.emissive;
            surface.opacity=base.w;surface.height=baseHeight;
            const Float3 layerTangent=normalize(subtract(tangent,multiply(normal,dot(normal,tangent))));
            const Float3 layerBitangent=multiply(cross3(normal,layerTangent),handed);
            const std::size_t layerCount=std::min<std::size_t>(tri.material->layers.size(),kMaximumVoxelMaterialLayers);
            for(std::size_t layerIndex=0U;layerIndex<layerCount;++layerIndex){
                const VoxelMaterialLayer& authored=tri.material->layers[layerIndex];
                if(!authored.enabled||authored.weight<=0.0F||authored.sourceMaterial==kAirMaterial||
                   authored.sourceMaterial>=tri.asset->materials.size())continue;
                MaterialSurfaceSample source=sample_layer_surface(*tri.asset,authored.sourceMaterial,chains,
                    uv,uv1,worldPosition,objectPosition,normal,layerTangent,layerBitangent,
                    textureLod,options.enableTextures,stats);
                MaterialLayerSample layer;
                layer.surface=source;layer.baseColorBlend=authored.blendMode;
                layer.authoredWeight=std::clamp(authored.weight,0.0F,1.0F);
                layer.mask=1.0F;layer.heightBlendStrength=0.0F;
                if(layerIndex<tri.binding->layers.size()){
                    const PolygonMaterialLayerBinding& binding=tri.binding->layers[layerIndex];
                    if(!binding.enabled)continue;
                    layer.semantic=binding.semantic;layer.opacityPolicy=binding.opacityPolicy;
                    layer.heightBlendStrength=binding.heightBlendStrength;
                    layer.heightBlendBias=binding.heightBlendBias;
                    layer.heightBlendTransition=binding.heightBlendTransition;
                    if(options.enableTextures&&binding.mask.texture){
                        const Float2 maskSelected=select_uv(binding.mask,uv,uv1);
                        const Float2 maskUv=transform_texture_coordinates(maskSelected,tri.binding->mapping.baseTransform);
                        const Float4 mask=sample_mapped_binding(*tri.asset,binding.mask,chains,tri.binding->mapping,maskUv,
                            worldPosition,objectPosition,normal,tri.binding->mapping.baseTransform,1.0F,textureLod,stats,{1,1,1,1});
                        layer.mask=std::clamp(mask.x*binding.maskScale+binding.maskBias,0.0F,1.0F);
                        ++stats.layerMaskSamples;
                    }else{
                        layer.mask=std::clamp(binding.maskScale+binding.maskBias,0.0F,1.0F);
                    }
                    if(options.enableTextures&&binding.height.texture){
                        const Float2 heightSelected=select_uv(binding.height,uv,uv1);
                        const Float2 heightUv=transform_texture_coordinates(heightSelected,tri.binding->mapping.baseTransform);
                        layer.surface.height=sample_mapped_binding(*tri.asset,binding.height,chains,tri.binding->mapping,heightUv,
                            worldPosition,objectPosition,normal,tri.binding->mapping.baseTransform,1.0F,textureLod,stats,{0.5F,0.5F,0.5F,1.0F}).x;
                    }
                }
                const Float3 tangentLayerNormal=layer.surface.normal;
                layer.surface.normal={0.0F,0.0F,1.0F};
                const float coverage=height_aware_material_layer_coverage(surface.height,layer);
                float ignoredCoverage{};
                surface=blend_material_surface_layer(surface,layer,&ignoredCoverage);
                surface.normal=apply_reoriented_tangent_material_normal(
                    normal,layerTangent,layerBitangent,tangentLayerNormal,coverage);
                normal=surface.normal;
                appliedMaterialLayer=appliedMaterialLayer||coverage>0.0F;
                if(layerIndex==options.materialLayerDebugIndex){
                    layerDebugMask=std::clamp(layer.mask*layer.authoredWeight,0.0F,1.0F);
                    layerDebugCoverage=coverage;
                }
            }
            if(appliedMaterialLayer)++stats.layeredFragments;
            base=surface.baseColor;base.w=surface.opacity;
            effective.metallic=surface.metallic;effective.roughness=surface.roughness;
            effective.emissive=surface.emissive;normal=surface.normal;
        }
        if(!options.decals.empty()){
            MaterialSurfaceSample decalSurface;
            decalSurface.baseColor=base;decalSurface.normal=normal;
            decalSurface.metallic=effective.metallic;decalSurface.roughness=effective.roughness;
            decalSurface.emissive=effective.emissive;decalSurface.opacity=base.w;
            MaterialDecalCompositeStats decalStats;
            decalSurface=composite_material_decals(decalSurface,worldPosition,normal,options.decals,&decalStats);
            stats.decalCandidates+=decalStats.candidates;stats.decalProjected+=decalStats.projected;
            if(decalStats.applied>0U)++stats.decalFragments;
            base=decalSurface.baseColor;base.w=decalSurface.opacity;normal=decalSurface.normal;
            effective.metallic=decalSurface.metallic;effective.roughness=decalSurface.roughness;
            effective.emissive=decalSurface.emissive;
        }
        if(effective.blendMode==MaterialBlendMode::Masked&&base.w<tri.binding->alphaCutoff){
            ++stats.alphaRejectedFragments;continue;
        }
        const bool debugView = options.materialDebugView != PolygonMaterialDebugView::Lit;
        RenderPassSample passes;
        Float4 shaded{};
        if(debugView){
            shaded=material_debug_output(options.materialDebugView, base, normal, effective, uv, uv1,
                                         worldPosition, objectPosition, tri.binding->mapping, detailFade,
                                         fragmentParallaxSamples, layerDebugMask, layerDebugCoverage);
            passes.light.emission={shaded.x,shaded.y,shaded.z};   // debug colours: all in one pass
        }else{
            passes.light=shade(effective,base,normal,viewWorld,environment,tri.frontFacing,
                    polygon_subsurface_light(tri,effective,worldPosition,normal,environment,options,stats),
                    tri.subsurfaceBvh&&material_has_subsurface(effective)&&options.subsurfaceDiffusionSamples>0U
                        ? polygon_diffused_sun(tri,effective,worldPosition,normal,environment,options,index,stats) : -1.0F,
                    polygon_ambient_occlusion(worldPosition,normal,environment,options,index,stats));
            shaded=beauty(passes.light,base.w);
        }
        if(target.passes.enabled){
            passes.normal=normal;
            const float cameraZ=w0*tri.v[0].cameraZ+w1*tri.v[1].cameraZ+w2*tri.v[2].cameraZ;
            passes.depthMeters=cameraZ*(options.metersPerUnit>0.0F?options.metersPerUnit:kDefaultMetersPerVoxel);
        }
        if(!debugView&&(transparent||tri.material->blendMode==MaterialBlendMode::Translucent||base.w<0.999F)){
            const float alpha=std::clamp(shaded.w,0.0F,1.0F);const Float4 old=target.hdrColor[index];
            target.hdrColor[index]={shaded.x*alpha+old.x*(1-alpha),shaded.y*alpha+old.y*(1-alpha),shaded.z*alpha+old.z*(1-alpha),1};
            target.write_passes(index,passes,alpha,false);
            ++stats.transparentFragments;
        }else{
            target.hdrColor[index]=shaded;target.depth[index]=depth;
            target.write_passes(index,passes,1.0F,true);
            if(options.writeObjectIds){target.objectId[index]=tri.objectId;target.materialIndex[index]=tri.submesh->materialIndex;}
        }
        ++stats.shadedFragments;
    }
}

float aces(float x) noexcept {const float a=2.51F,b=0.03F,c=2.43F,d=0.59F,e=0.14F;return std::clamp((x*(a*x+b))/(x*(c*x+d)+e),0.0F,1.0F);}
float reinhard(float x) noexcept {return x/(1.0F+x);}
float tonemap_curve(float x, TonemapOperator op) noexcept {
    switch (op) {
        case TonemapOperator::Reinhard: return std::clamp(reinhard(x),0.0F,1.0F);
        case TonemapOperator::Clamp: return std::clamp(x,0.0F,1.0F);
        case TonemapOperator::ACES: default: return aces(x);
    }
}
std::uint8_t srgb(float linear) noexcept {linear=std::clamp(linear,0.0F,1.0F);const float encoded=linear<=0.0031308F?12.92F*linear:1.055F*std::pow(linear,1.0F/2.4F)-0.055F;return static_cast<std::uint8_t>(std::clamp(encoded*255.0F+0.5F,0.0F,255.0F));}
}

namespace {
template <class T> void size_pass(std::vector<T>& pass, bool enabled, std::size_t count, T value) {
    if (enabled) pass.assign(count, value); else std::vector<T>().swap(pass);
}
void reset_passes(RenderPassBuffers& p, std::size_t count, Float3 background) {
    const bool on = p.enabled;
    for (std::vector<Float3>* light : {&p.directDiffuse, &p.directSpecular, &p.indirectDiffuse, &p.indirectSpecular,
                                       &p.emission, &p.transmission, &p.fog, &p.shadow, &p.normal})
        size_pass(*light, on, count, Float3{});
    size_pass(p.background, on, count, background);
    size_pass(p.sunVisibility, on, count, 1.0F);
    size_pass(p.depthMeters, on, count, std::numeric_limits<float>::infinity());
}
Float3 lerp_pass(Float3 over, Float3 under, float alpha) noexcept {
    return {over.x * alpha + under.x * (1.0F - alpha), over.y * alpha + under.y * (1.0F - alpha),
            over.z * alpha + under.z * (1.0F - alpha)};
}
} // namespace

Float3 RenderPassBuffers::light_sum(std::size_t i) const noexcept {
    Float3 sum{};
    for (const std::vector<Float3>* pass : {&directDiffuse, &directSpecular, &indirectDiffuse, &indirectSpecular, &emission,
                                            &transmission, &background, &fog})
        sum = add(sum, (*pass)[i]);
    return sum;
}

void PolygonRenderTarget::resize(std::uint32_t newWidth,std::uint32_t newHeight){
    width=newWidth;height=newHeight;const std::size_t count=static_cast<std::size_t>(width)*height;
    hdrColor.assign(count,{});depth.assign(count,1.0F);objectId.assign(count,0U);materialIndex.assign(count,0U);
    reset_passes(passes,count,{});
}
void PolygonRenderTarget::clear(Float4 color,float depthValue){
    std::fill(hdrColor.begin(),hdrColor.end(),color);std::fill(depth.begin(),depth.end(),depthValue);
    std::fill(objectId.begin(),objectId.end(),0U);std::fill(materialIndex.begin(),materialIndex.end(),0U);
    reset_passes(passes,hdrColor.size(),{color.x,color.y,color.z});
}
void PolygonRenderTarget::enable_passes(bool enabled){
    passes.enabled=enabled;
    reset_passes(passes,hdrColor.size(),{});
    if(enabled)sync_background_pass();
}
void PolygonRenderTarget::sync_background_pass(){
    if(!passes.enabled)return;
    for(std::size_t i=0;i<hdrColor.size()&&i<passes.background.size();++i)
        if(!(depth[i]<1.0F))passes.background[i]={hdrColor[i].x,hdrColor[i].y,hdrColor[i].z};
}
bool PolygonRenderTarget::valid() const noexcept{
    const std::size_t count=static_cast<std::size_t>(width)*height;
    if(!(width>0&&height>0&&hdrColor.size()==count&&depth.size()==count&&objectId.size()==count&&materialIndex.size()==count))return false;
    if(!passes.enabled)return true;
    const auto sized=[count](const auto& v){return v.size()==count;};
    return sized(passes.directDiffuse)&&sized(passes.directSpecular)&&sized(passes.indirectDiffuse)&&sized(passes.indirectSpecular)&&
           sized(passes.emission)&&sized(passes.transmission)&&sized(passes.background)&&sized(passes.fog)&&sized(passes.shadow)&&
           sized(passes.sunVisibility)&&sized(passes.normal)&&sized(passes.depthMeters);
}
void PolygonRenderTarget::write_passes(std::size_t i,const RenderPassSample& sample,float alpha,bool opaque) noexcept{
    if(!passes.enabled||i>=passes.directDiffuse.size())return;
    RenderPassBuffers& p=passes;
    const LightPasses& l=sample.light;
    if(opaque){
        p.directDiffuse[i]=l.directDiffuse;p.directSpecular[i]=l.directSpecular;p.indirectDiffuse[i]=l.indirectDiffuse;
        p.indirectSpecular[i]=l.indirectSpecular;p.emission[i]=l.emission;p.transmission[i]=l.transmission;
        p.background[i]={};p.fog[i]={};p.shadow[i]=sample.shadow;p.sunVisibility[i]=sample.sunVisibility;
        p.normal[i]=sample.normal;p.depthMeters[i]=sample.depthMeters;
        return;
    }
    alpha=std::clamp(alpha,0.0F,1.0F);
    p.directDiffuse[i]=lerp_pass(l.directDiffuse,p.directDiffuse[i],alpha);
    p.directSpecular[i]=lerp_pass(l.directSpecular,p.directSpecular[i],alpha);
    p.indirectDiffuse[i]=lerp_pass(l.indirectDiffuse,p.indirectDiffuse[i],alpha);
    p.indirectSpecular[i]=lerp_pass(l.indirectSpecular,p.indirectSpecular[i],alpha);
    p.emission[i]=lerp_pass(l.emission,p.emission[i],alpha);
    p.transmission[i]=lerp_pass(l.transmission,p.transmission[i],alpha);
    p.background[i]=lerp_pass({},p.background[i],alpha);
    p.fog[i]=lerp_pass({},p.fog[i],alpha);
    p.shadow[i]=lerp_pass(sample.shadow,p.shadow[i],alpha);
}

std::uint64_t apply_distance_fog(PolygonRenderTarget& target,const PolygonCamera& camera,const RenderEnvironment& environment,
                                 float metersPerUnit){
    const float density=environment.fogDensityPerMeter;
    if(!(density>0.0F)||!target.valid()||!(camera.farPlane>camera.nearPlane))return 0U;
    metersPerUnit=metersPerUnit>0.0F&&std::isfinite(metersPerUnit)?metersPerUnit:kDefaultMetersPerVoxel;
    const float tanHalf=std::tan(camera.verticalFieldOfViewRadians*0.5F);
    const float aspect=static_cast<float>(target.width)/static_cast<float>(target.height);
    const Float3 fogColor=environment.fogColor;
    std::uint64_t fogged=0U;
    RenderPassBuffers& p=target.passes;
    for(std::uint32_t y=0;y<target.height;++y){
        const float ndcY=(1.0F-2.0F*((static_cast<float>(y)+0.5F)/static_cast<float>(target.height)))*tanHalf;
        for(std::uint32_t x=0;x<target.width;++x){
            const std::size_t i=static_cast<std::size_t>(y)*target.width+x;
            if(!(target.depth[i]<1.0F))continue;
            const float ndcX=(2.0F*((static_cast<float>(x)+0.5F)/static_cast<float>(target.width))-1.0F)*tanHalf*aspect;
            const float cameraZ=camera.nearPlane+target.depth[i]*(camera.farPlane-camera.nearPlane);
            const float meters=cameraZ*std::sqrt(1.0F+ndcX*ndcX+ndcY*ndcY)*metersPerUnit;
            const float t=std::exp(-density*std::max(0.0F,meters-environment.fogStartMeters));
            const Float3 add_fog=multiply(fogColor,1.0F-t);
            Float4& c=target.hdrColor[i];
            c={c.x*t+add_fog.x,c.y*t+add_fog.y,c.z*t+add_fog.z,c.w};
            if(p.enabled){
                for(std::vector<Float3>* pass:{&p.directDiffuse,&p.directSpecular,&p.indirectDiffuse,&p.indirectSpecular,
                                               &p.emission,&p.transmission,&p.background,&p.fog})
                    (*pass)[i]=multiply((*pass)[i],t);
                p.fog[i]=add(p.fog[i],add_fog);
            }
            ++fogged;
        }
    }
    return fogged;
}

namespace {
struct PixelRays {
    Float3 forward{}, right{}, up{};
    float tanHalf{}, aspect{};
    [[nodiscard]] Float3 direction(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) const noexcept {
        const float nx = (2.0F * ((static_cast<float>(x) + 0.5F) / static_cast<float>(w)) - 1.0F) * tanHalf * aspect;
        const float ny = (1.0F - 2.0F * ((static_cast<float>(y) + 0.5F) / static_cast<float>(h))) * tanHalf;
        return normalize(add(forward, add(multiply(right, nx), multiply(up, ny))));
    }
    // Distance along the pixel's ray per unit of camera-space depth.
    [[nodiscard]] float stretch(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) const noexcept {
        const float nx = (2.0F * ((static_cast<float>(x) + 0.5F) / static_cast<float>(w)) - 1.0F) * tanHalf * aspect;
        const float ny = (1.0F - 2.0F * ((static_cast<float>(y) + 0.5F) / static_cast<float>(h))) * tanHalf;
        return std::sqrt(1.0F + nx * nx + ny * ny);
    }
};
PixelRays pixel_rays(const PolygonCamera& camera, const PolygonRenderTarget& target) noexcept {
    PixelRays rays;
    rays.forward = normalize(subtract(camera.target, camera.position));
    rays.right = normalize(cross3(rays.forward, camera.up));
    if (length_squared(rays.right) < 1.0e-12F) rays.right = {1.0F, 0.0F, 0.0F};
    rays.up = normalize(cross3(rays.right, rays.forward));
    rays.tanHalf = std::tan(camera.verticalFieldOfViewRadians * 0.5F);
    rays.aspect = static_cast<float>(target.width) / static_cast<float>(target.height);
    return rays;
}
} // namespace

std::uint64_t paint_sky_background(PolygonRenderTarget& target, const PolygonCamera& camera, const RenderEnvironment& environment) {
    if (environment.skyMode != SkyMode::Physical || !target.valid()) return 0U;
    const PixelRays rays = pixel_rays(camera, target);
    std::uint64_t painted = 0U;
    for (std::uint32_t y = 0; y < target.height; ++y)
        for (std::uint32_t x = 0; x < target.width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * target.width + x;
            if (target.depth[i] < 1.0F) continue;
            const Float3 sky = background_radiance(environment, rays.direction(x, y, target.width, target.height));
            target.hdrColor[i] = {sky.x, sky.y, sky.z, 1.0F};
            if (target.passes.enabled) target.passes.background[i] = sky;
            ++painted;
        }
    return painted;
}

std::uint64_t apply_aerial_perspective(PolygonRenderTarget& target, const PolygonCamera& camera, const RenderEnvironment& environment,
                                       float metersPerUnit) {
    if (environment.skyMode != SkyMode::Physical || !(environment.aerialPerspectiveScale > 0.0F) || !target.valid() ||
        !(camera.farPlane > camera.nearPlane))
        return 0U;
    const auto model = atmosphere_model(environment);
    metersPerUnit = metersPerUnit > 0.0F && std::isfinite(metersPerUnit) ? metersPerUnit : kDefaultMetersPerVoxel;
    const PixelRays rays = pixel_rays(camera, target);
    const float altitude = environment.observerAltitudeMeters + camera.position.y * metersPerUnit;
    const Float3 sunTop = multiply(environment.sunColor, environment.sunIntensity * 3.14159265358979323846F);
    RenderPassBuffers& p = target.passes;
    std::uint64_t changed = 0U;
    for (std::uint32_t y = 0; y < target.height; ++y)
        for (std::uint32_t x = 0; x < target.width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * target.width + x;
            if (!(target.depth[i] < 1.0F)) continue;
            const float cameraZ = camera.nearPlane + target.depth[i] * (camera.farPlane - camera.nearPlane);
            const float meters = cameraZ * rays.stretch(x, y, target.width, target.height) * metersPerUnit * environment.aerialPerspectiveScale;
            const AtmosphereModel::Segment air = model->segment(altitude, rays.direction(x, y, target.width, target.height), meters, 12U);
            const Float3 t = air.transmittance;
            const Float3 added{air.inscattered.x * sunTop.x, air.inscattered.y * sunTop.y, air.inscattered.z * sunTop.z};
            Float4& c = target.hdrColor[i];
            c = {c.x * t.x + added.x, c.y * t.y + added.y, c.z * t.z + added.z, c.w};
            if (p.enabled) {
                for (std::vector<Float3>* pass : {&p.directDiffuse, &p.directSpecular, &p.indirectDiffuse, &p.indirectSpecular,
                                                  &p.emission, &p.transmission, &p.background, &p.fog})
                    (*pass)[i] = {(*pass)[i].x * t.x, (*pass)[i].y * t.y, (*pass)[i].z * t.z};
                p.fog[i] = add(p.fog[i], added);
            }
            ++changed;
        }
    return changed;
}

namespace {
bool write_pfm(const std::filesystem::path& path,std::uint32_t width,std::uint32_t height,int channels,
               const std::function<float(std::size_t,int)>& value,std::string* error){
    std::ofstream out(path,std::ios::binary);
    if(!out){if(error)*error="cannot write "+path.string();return false;}
    out<<(channels==3?"PF":"Pf")<<"\n"<<width<<" "<<height<<"\n-1.0\n";   // negative scale: little-endian
    std::vector<float> row(static_cast<std::size_t>(width)*channels);
    for(std::uint32_t y=height;y-->0;){   // PFM rows run bottom to top
        for(std::uint32_t x=0;x<width;++x)
            for(int c=0;c<channels;++c)row[static_cast<std::size_t>(x)*channels+c]=value(static_cast<std::size_t>(y)*width+x,c);
        for(const float f:row){
            const auto bits=std::bit_cast<std::uint32_t>(f);
            const char bytes[4]{static_cast<char>(bits&0xFFU),static_cast<char>((bits>>8U)&0xFFU),
                                static_cast<char>((bits>>16U)&0xFFU),static_cast<char>((bits>>24U)&0xFFU)};
            out.write(bytes,4);
        }
    }
    if(!out){if(error)*error="failed writing "+path.string();return false;}
    return true;
}
float component(Float3 v,int c) noexcept{return c==0?v.x:c==1?v.y:v.z;}
} // namespace

bool write_render_passes_pfm(const PolygonRenderTarget& target,const std::filesystem::path& directory,const std::string& prefix,
                             float farPlaneMeters,std::string* error){
    if(!target.valid()||!target.passes.enabled){if(error)*error="render passes are not enabled on this target";return false;}
    std::error_code ec;
    std::filesystem::create_directories(directory,ec);
    const RenderPassBuffers& p=target.passes;
    const auto colour=[&](const char* name,const std::vector<Float3>& pass){
        return write_pfm(directory/(prefix+name+".pfm"),target.width,target.height,3,
                         [&](std::size_t i,int c){return component(pass[i],c);},error);
    };
    if(!write_pfm(directory/(prefix+"beauty.pfm"),target.width,target.height,3,
                  [&](std::size_t i,int c){const Float4& v=target.hdrColor[i];return c==0?v.x:c==1?v.y:v.z;},error))return false;
    const std::pair<const char*,const std::vector<Float3>*> light[]{
        {"direct_diffuse",&p.directDiffuse},{"direct_specular",&p.directSpecular},{"indirect_diffuse",&p.indirectDiffuse},
        {"indirect_specular",&p.indirectSpecular},{"emission",&p.emission},{"transmission",&p.transmission},
        {"background",&p.background},{"fog",&p.fog},{"shadow",&p.shadow},{"normal",&p.normal}};
    for(const auto& [name,pass]:light)if(!colour(name,*pass))return false;
    if(!write_pfm(directory/(prefix+"sun_visibility.pfm"),target.width,target.height,1,
                  [&](std::size_t i,int){return p.sunVisibility[i];},error))return false;
    return write_pfm(directory/(prefix+"depth.pfm"),target.width,target.height,1,
                     [&](std::size_t i,int){return std::isfinite(p.depthMeters[i])?p.depthMeters[i]:farPlaneMeters;},error);
}

float polygon_hit_opacity(const CookedPolygonAsset& asset, const PolygonBvhHit& hit, Float3 worldPosition,
                          Float3 worldNormal, const std::vector<PolygonTextureMipChain>& mips, float tintAlpha) noexcept {
    if (hit.materialIndex >= asset.materials.size() || hit.materialIndex >= asset.materialBindings.size()) return 1.0F;
    const VoxelMaterialDefinition& material = asset.materials[hit.materialIndex];
    const PolygonMaterialBinding& binding = asset.materialBindings[hit.materialIndex];
    const std::size_t t = hit.firstIndex;
    if (t + 2U >= asset.indices.size()) return material.baseColor.w * tintAlpha;
    const PolygonVertex& a = asset.vertices[asset.indices[t]];
    const PolygonVertex& b = asset.vertices[asset.indices[t + 1U]];
    const PolygonVertex& c = asset.vertices[asset.indices[t + 2U]];
    // Barycentric weights of the hit in the triangle (object space).
    const Float3 e1 = subtract(b.position, a.position), e2 = subtract(c.position, a.position), q = subtract(hit.position, a.position);
    const float d00 = dot(e1, e1), d01 = dot(e1, e2), d11 = dot(e2, e2), d20 = dot(q, e1), d21 = dot(q, e2);
    const float denominator = d00 * d11 - d01 * d01;
    float w1 = 0.0F, w2 = 0.0F;
    if (std::abs(denominator) > 1.0e-20F) { w1 = (d11 * d20 - d01 * d21) / denominator; w2 = (d00 * d21 - d01 * d20) / denominator; }
    const float w0 = 1.0F - w1 - w2;
    const Float2 uv0{a.texcoord.x * w0 + b.texcoord.x * w1 + c.texcoord.x * w2, a.texcoord.y * w0 + b.texcoord.y * w1 + c.texcoord.y * w2};
    const Float2 uv1{a.texcoord1.x * w0 + b.texcoord1.x * w1 + c.texcoord1.x * w2,
                     a.texcoord1.y * w0 + b.texcoord1.y * w1 + c.texcoord1.y * w2};
    const float vertexAlpha = a.color.w * w0 + b.color.w * w1 + c.color.w * w2;
    const Float2 selected = binding.mapping.mappingMode == MaterialMappingMode::UV1 ? uv1 : uv0;
    const Float2 mapped = transform_texture_coordinates(selected, binding.mapping.baseTransform);
    PolygonRenderStats ignored;
    const Float4 texture = sample_mapped_binding(asset, binding.baseColor, mips, binding.mapping, mapped, worldPosition,
                                                 hit.position, worldNormal, binding.mapping.baseTransform, 1.0F, 0.0F,
                                                 ignored, {1, 1, 1, 1});
    float opacity = material.baseColor.w * vertexAlpha * texture.w * tintAlpha;
    if (binding.opacity.texture)
        opacity *= sample_mapped_binding(asset, binding.opacity, mips, binding.mapping, mapped, worldPosition, hit.position,
                                         worldNormal, binding.mapping.baseTransform, 1.0F, 0.0F, ignored, {1, 1, 1, 1}).x;
    return opacity;
}

PolygonTextureMipChain generate_texture_mips(const PolygonImage& image) {
    PolygonTextureMipChain chain;
    const std::size_t requiredBytes =
        static_cast<std::size_t>(image.width) * image.height * 4U;
    if (image.width == 0 || image.height == 0 || image.rgba8.size() != requiredBytes) {
        return chain;
    }

    chain.levels.push_back({image.width, image.height, image.rgba8});
    while (chain.levels.back().width > 1U || chain.levels.back().height > 1U) {
        const auto& source = chain.levels.back();
        PolygonTextureMip next;
        next.width = std::max(1U, source.width / 2U);
        next.height = std::max(1U, source.height / 2U);
        next.rgba8.resize(static_cast<std::size_t>(next.width) * next.height * 4U);

        for (std::uint32_t y = 0; y < next.height; ++y) {
            for (std::uint32_t x = 0; x < next.width; ++x) {
                for (std::uint32_t channel = 0; channel < 4U; ++channel) {
                    std::uint32_t sum = 0;
                    std::uint32_t sampleCount = 0;
                    for (std::uint32_t offsetY = 0; offsetY < 2U; ++offsetY) {
                        for (std::uint32_t offsetX = 0; offsetX < 2U; ++offsetX) {
                            const std::uint32_t sourceX =
                                std::min(source.width - 1U, x * 2U + offsetX);
                            const std::uint32_t sourceY =
                                std::min(source.height - 1U, y * 2U + offsetY);
                            sum += source.rgba8[
                                (static_cast<std::size_t>(sourceY) * source.width + sourceX) * 4U +
                                channel];
                            ++sampleCount;
                        }
                    }
                    next.rgba8[
                        (static_cast<std::size_t>(y) * next.width + x) * 4U + channel] =
                        static_cast<std::uint8_t>((sum + sampleCount / 2U) / sampleCount);
                }
            }
        }
        chain.levels.push_back(std::move(next));
    }
    return chain;
}

void PolygonRenderCache::clear() noexcept {
    mips_.clear(); lods_.clear(); residentMipBytes_=0; frame_=0;
}

PolygonRenderStats ReferencePolygonRenderer::render(
    std::span<const PolygonRenderInstance> instances,
    const PolygonCamera& camera,
    const RenderEnvironment& environment,
    PolygonRenderTarget& target,
    const PolygonRenderOptions& options) const {
    PolygonRenderStats stats;
    if (!target.valid() || !(camera.nearPlane > 0.0F) ||
        !(camera.farPlane > camera.nearPlane)) {
        return stats;
    }
    if (!options.preserveExistingDepth) {
        target.clear();
    }

    const CameraBasis basis = make_basis(camera, target);
    PolygonRenderCache transientCache;
    auto& cache = options.cache ? *options.cache : transientCache;
    ++cache.frame_;
    struct FrameAsset {
        bool valid{};
        PolygonBounds bounds{};
        std::uint64_t hash{};
        std::shared_ptr<const std::vector<PolygonTextureMipChain>> mips;
    };
    std::unordered_map<const CookedPolygonAsset*, FrameAsset> prepared;
    const auto prepare = [&](const CookedPolygonAsset* asset) -> FrameAsset& {
        auto [it, inserted] = prepared.try_emplace(asset);
        if(inserted){
            ++stats.assetValidations;
            it->second.valid=asset && static_cast<bool>(validate_polygon_asset(*asset));
            if(it->second.valid){
                it->second.bounds=vertex_bounds(*asset);
                it->second.hash=asset->contentHash ? asset->contentHash : polygon_asset_content_hash(*asset);
            }
        }
        return it->second;
    };
    static const auto emptyMips=std::make_shared<const std::vector<PolygonTextureMipChain>>();
    const auto texture_mips = [&](const CookedPolygonAsset& asset,FrameAsset& frameAsset){
        if(!options.enableTextures || asset.images.empty())return emptyMips;
        if(frameAsset.mips){++stats.mipCacheHits;return frameAsset.mips;}
        if(const auto it=cache.mips_.find(frameAsset.hash);it!=cache.mips_.end()){
            ++stats.mipCacheHits; it->second.lastUse=cache.frame_;
            return frameAsset.mips=it->second.chains;
        }
        ++stats.mipCacheMisses;
        auto chains=std::make_shared<std::vector<PolygonTextureMipChain>>();
        chains->reserve(asset.images.size());
        std::size_t bytes=0;
        for(const auto& image:asset.images){
            chains->push_back(generate_texture_mips(image)); ++stats.mipChainsBuilt;
            for(const auto& mip:chains->back().levels)bytes+=mip.rgba8.size();
        }
        frameAsset.mips=chains;
        if(bytes<=cache.maximumMipBytes_){
            while(!cache.mips_.empty() && (cache.residentMipBytes_>cache.maximumMipBytes_-bytes || cache.mips_.size()>=128U)){
                const auto oldest=std::min_element(cache.mips_.begin(),cache.mips_.end(),[](const auto& a,const auto& b){return a.second.lastUse<b.second.lastUse;});
                cache.residentMipBytes_-=oldest->second.bytes; cache.mips_.erase(oldest);
            }
            cache.mips_.emplace(frameAsset.hash,PolygonRenderCache::MipEntry{chains,bytes,cache.frame_});
            cache.residentMipBytes_+=bytes;
        }
        return frameAsset.mips;
    };
    const float bias=std::isfinite(options.lodBias) ? std::clamp(options.lodBias,-4.0F,4.0F) : 0.0F;
    const float hysteresis=std::isfinite(options.lodHysteresis) ? std::clamp(options.lodHysteresis,0.0F,0.49F) : 0.0F;
    std::vector<DeferredTriangle> transparent;
    // Ambient occlusion: without a caller's tracer, AO rays see the polygons being rendered.
    PolygonRenderOptions frameOptions = options;
    std::vector<std::pair<std::shared_ptr<const PolygonBvh>, RigidTransform>> aoScene;
    if (!options.occlusionRay && polygon_ambient_occlusion_active(environment) &&
        options.materialDebugView == PolygonMaterialDebugView::Lit) {
        std::unordered_map<const CookedPolygonAsset*, std::shared_ptr<const PolygonBvh>> bvhs;
        for (const PolygonRenderInstance& instance : instances) {
            if (!instance.visible || !instance.asset || !prepare(instance.asset).valid) continue;
            auto& bvh = bvhs[instance.asset];
            if (!bvh) {
                auto fresh = std::make_shared<PolygonBvh>();
                if (!fresh->build(*instance.asset)) continue;
                bvh = std::move(fresh);
            }
            aoScene.emplace_back(bvh, instance.transform);
        }
        frameOptions.occlusionRay = [&aoScene](Float3 origin, Float3 direction, float maximum) -> std::optional<float> {
            std::optional<float> best;
            for (const auto& [bvh, pose] : aoScene)
                if (const auto hit = bvh->raycast(inverse_transform_point(pose, origin), inverse_transform_vector(pose, direction),
                                                  best ? *best : maximum))
                    best = hit->distance;
            return best;
        };
    }
    // BVHs for meshes with a Subsurface material, built once per asset per render.
    std::unordered_map<const CookedPolygonAsset*, std::unique_ptr<PolygonBvh>> subsurfaceBvhs;
    const auto subsurface_bvh = [&](const CookedPolygonAsset& asset) -> const PolygonBvh* {
        if (!(environment.subsurfaceMaxDistanceMeters > 0.0F) ||
            options.materialDebugView != PolygonMaterialDebugView::Lit) return nullptr;
        auto [it, inserted] = subsurfaceBvhs.try_emplace(&asset);
        if (inserted && std::any_of(asset.materials.begin(), asset.materials.end(),
                                    [](const VoxelMaterialDefinition& m) { return material_has_subsurface(m); })) {
            auto bvh = std::make_unique<PolygonBvh>();
            if (bvh->build(asset)) it->second = std::move(bvh);
        }
        return it->second.get();
    };
    for (const PolygonRenderInstance& instance : instances) {
        ++stats.submittedInstances;
        if (!instance.visible || instance.asset == nullptr) {
            ++stats.culledInstances;
            continue;
        }
        const auto& base=prepare(instance.asset);
        if(!base.valid){++stats.culledInstances;continue;}
        const float diameter =
            projected_diameter(base.bounds, instance.transform, camera, basis, target);
        const float effectiveDiameter=diameter*std::exp2(-bias);
        auto choice=select_lod(instance,effectiveDiameter);
        if(options.cache && choice.threshold>=0.0F){
            if(const auto it=cache.lods_.find(instance.objectId);it!=cache.lods_.end()){
                const auto& old=it->second;
                const bool levelStillPresent=std::any_of(instance.lods.begin(),instance.lods.end(),[&](const auto& level){return level.asset==old.selected && level.minimumProjectedDiameterPixels==old.threshold;});
                if(old.base==instance.asset && old.baseHash==base.hash && old.bias==bias && levelStillPresent && choice.asset!=old.selected){
                    const bool up=choice.threshold>old.threshold;
                    if((up && effectiveDiameter<choice.threshold*(1.0F+hysteresis)) ||
                       (!up && effectiveDiameter>=old.threshold*(1.0F-hysteresis)))choice={old.selected,old.threshold};
                }
            }
            if(cache.lods_.contains(instance.objectId) || cache.lods_.size()<4096U)
                cache.lods_.insert_or_assign(instance.objectId,PolygonRenderCache::LodEntry{instance.asset,base.hash,choice.asset,choice.threshold,bias,cache.frame_});
        }
        const CookedPolygonAsset* asset=choice.asset;
        if (asset == nullptr || diameter < 0.5F) {
            ++stats.culledInstances;
            continue;
        }

        auto& selected=prepare(asset);
        if(!selected.valid){++stats.culledInstances;continue;}
        if(options.enableFrustumCulling && outside_frustum(selected.bounds,instance.transform,camera,basis)){
            ++stats.culledInstances; ++stats.frustumCulledInstances; continue;
        }
        if(asset!=instance.asset)++stats.lodSelections;
        const auto mips=texture_mips(*asset,selected);
        const PolygonBvh* subsurfaceBvh=subsurface_bvh(*asset);

        for (const PolygonSubmesh& submesh : asset->submeshes) {
            if (submesh.materialIndex >= asset->materials.size()) {
                continue;
            }
            const auto& material = asset->materials[submesh.materialIndex];
            const auto& binding = asset->materialBindings[submesh.materialIndex];
            for (std::uint32_t i = 0; i < submesh.indexCount; i += 3U) {
                ++stats.submittedTriangles;
                std::array<ScreenVertex, 3> vertices{};
                bool projected = true;
                for (std::uint32_t corner = 0; corner < 3U; ++corner) {
                    const std::uint32_t index =
                        asset->indices[submesh.firstIndex + i + corner];
                    projected = projected &&
                        project_vertex(asset->vertices[index], instance.transform, camera, basis,
                                       target, vertices[corner]);
                }
                if (!projected) {
                    ++stats.clippedTriangles;
                    continue;
                }

                const float signedArea =
                    edge(vertices[0].x, vertices[0].y, vertices[1].x, vertices[1].y,
                         vertices[2].x, vertices[2].y);
                const bool frontFacing = signedArea > 0.0F;
                if (!binding.doubleSided && !frontFacing) {
                    ++stats.backfaceCulledTriangles;
                    continue;
                }

                DeferredTriangle triangle{
                    asset,
                    &submesh,
                    &binding,
                    &material,
                    vertices,
                    instance.objectId,
                    instance.tint,
                    camera.position,
                    (vertices[0].cameraZ + vertices[1].cameraZ + vertices[2].cameraZ) / 3.0F,
                    frontFacing,
                    mips,
                    subsurfaceBvh,
                    instance.transform};
                if (material.blendMode == MaterialBlendMode::Translucent ||
                    material.baseColor.w < 0.999F) {
                    transparent.push_back(std::move(triangle));
                } else {
                    raster_triangle(triangle, target, environment, frameOptions, false, stats);
                }
            }
        }
    }

    if (options.enableTransparentSorting) {
        std::sort(transparent.begin(), transparent.end(),
                  [](const DeferredTriangle& left, const DeferredTriangle& right) {
                      return left.sortDepth > right.sortDepth;
                  });
    }
    for (const DeferredTriangle& triangle : transparent) {
        raster_triangle(triangle, target, environment, frameOptions, true, stats);
    }
    for(auto it=cache.lods_.begin();it!=cache.lods_.end();){
        it=it->second.lastUse==cache.frame_ ? std::next(it) : cache.lods_.erase(it);
    }
    stats.mipCacheBytes=cache.residentMipBytes_;
    return stats;
}

bool composite_hybrid_layers(const PolygonRenderTarget& voxelLayer,const PolygonRenderTarget& polygonLayer,PolygonRenderTarget& output,std::string* error){if(!voxelLayer.valid()||!polygonLayer.valid()||voxelLayer.width!=polygonLayer.width||voxelLayer.height!=polygonLayer.height){if(error)*error="hybrid layers must be valid and have identical dimensions";return false;}output.resize(voxelLayer.width,voxelLayer.height);for(std::size_t i=0;i<output.depth.size();++i){const bool polygon=polygonLayer.depth[i]<voxelLayer.depth[i];output.depth[i]=polygon?polygonLayer.depth[i]:voxelLayer.depth[i];output.hdrColor[i]=polygon?polygonLayer.hdrColor[i]:voxelLayer.hdrColor[i];output.objectId[i]=polygon?polygonLayer.objectId[i]:voxelLayer.objectId[i];output.materialIndex[i]=polygon?polygonLayer.materialIndex[i]:voxelLayer.materialIndex[i];}
    // Render passes follow the layer that owns each pixel (when both layers have them).
    if(output.passes.enabled&&voxelLayer.passes.enabled&&polygonLayer.passes.enabled){
        auto pick=[&](auto member){
            for(std::size_t i=0;i<output.depth.size();++i)
                (output.passes.*member)[i]=((polygonLayer.depth[i]<voxelLayer.depth[i]?polygonLayer.passes:voxelLayer.passes).*member)[i];
        };
        pick(&RenderPassBuffers::directDiffuse);pick(&RenderPassBuffers::directSpecular);pick(&RenderPassBuffers::indirectDiffuse);
        pick(&RenderPassBuffers::indirectSpecular);pick(&RenderPassBuffers::emission);pick(&RenderPassBuffers::transmission);
        pick(&RenderPassBuffers::background);pick(&RenderPassBuffers::fog);pick(&RenderPassBuffers::shadow);
        pick(&RenderPassBuffers::sunVisibility);pick(&RenderPassBuffers::normal);pick(&RenderPassBuffers::depthMeters);
    }
    return true;}

bool resolve_polygon_render_rgba8(const PolygonRenderTarget& target, float exposure,
                                  std::vector<std::uint8_t>& rgba, std::string* error) {
    return resolve_polygon_render_rgba8(target, exposure, TonemapOperator::ACES, rgba, error);
}

bool resolve_polygon_render_rgba8(const PolygonRenderTarget& target, float exposure,
                                  TonemapOperator tonemapOperator,
                                  std::vector<std::uint8_t>& rgba, std::string* error) {
    if (!target.valid() || !(exposure > 0) || !std::isfinite(exposure)) {
        if (error) *error = "invalid render target or exposure";
        return false;
    }
    rgba.resize(target.hdrColor.size() * 4U);
    std::uint8_t* out = rgba.data();
    for (const Float4& c : target.hdrColor) {
        out[0] = srgb(tonemap_curve(c.x * exposure, tonemapOperator));
        out[1] = srgb(tonemap_curve(c.y * exposure, tonemapOperator));
        out[2] = srgb(tonemap_curve(c.z * exposure, tonemapOperator));
        out[3] = 255U;
        out += 4;
    }
    return true;
}

bool write_polygon_render_ppm(const std::filesystem::path& path, const PolygonRenderTarget& target,
                              float exposure, std::string* error) {
    std::vector<std::uint8_t> rgba;
    if (!resolve_polygon_render_rgba8(target, exposure, rgba, error)) return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "failed to open PPM output";
        return false;
    }
    out << "P6\n" << target.width << ' ' << target.height << "\n255\n";
    std::vector<char> rgb(rgba.size() / 4U * 3U);
    for (std::size_t i = 0, j = 0; i < rgba.size(); i += 4U, j += 3U) {
        rgb[j] = static_cast<char>(rgba[i]);
        rgb[j + 1U] = static_cast<char>(rgba[i + 1U]);
        rgb[j + 2U] = static_cast<char>(rgba[i + 2U]);
    }
    out.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    if (!out) {
        if (error) *error = "failed to write PPM output";
        return false;
    }
    return true;
}

} // namespace dve::render
