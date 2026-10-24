#include "dve/render/polygon_renderer.hpp"
#include "dve/render/scene_lighting.hpp"
#include "dve/voxel_normal_map.hpp"
#include "dve/triplanar_normal.hpp"

#include "dve/polygon_bvh.hpp"
#include "dve/render/ambient_occlusion.hpp"
#include "dve/render/surface_shading.hpp"
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


float srgb_to_linear(float encoded) noexcept {
    encoded = std::clamp(encoded, 0.0F, 1.0F);
    return encoded <= 0.04045F
        ? encoded / 12.92F
        : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}

Float4 texel(const PolygonTextureMip& mip,std::uint32_t x,std::uint32_t y,
             PolygonTextureColorSpace colorSpace) noexcept {
    x=(std::min)(x,mip.width-1U);y=(std::min)(y,mip.height-1U);
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
    auto address=[](int v,int n,ImportedWrapMode mode){
        if(mode==ImportedWrapMode::ClampToEdge)return std::clamp(v,0,n-1);
        const int period=mode==ImportedWrapMode::MirroredRepeat?n*2:n;
        const int q=(v%period+period)%period;return q>=n?period-1-q:q;
    };
    const float u=sampler.wrapS==ImportedWrapMode::ClampToEdge?std::clamp(uv.x,0.F,1.F):std::fmod(uv.x,2.F);
    const float v=sampler.wrapT==ImportedWrapMode::ClampToEdge?std::clamp(uv.y,0.F,1.F):std::fmod(uv.y,2.F);
    const float x=u*float(mip.width)-.5F,y=v*float(mip.height)-.5F;
    auto at=[&](int px,int py){return texel(mip,unsigned(address(px,int(mip.width),sampler.wrapS)),unsigned(address(py,int(mip.height),sampler.wrapT)),colorSpace);};
    if(sampler.magFilter==ImportedTextureFilter::Nearest)return at(int(std::floor(x+.5F)),int(std::floor(y+.5F)));
    const int x0=int(std::floor(x)),y0=int(std::floor(y));const float tx=x-float(x0),ty=y-float(y0);
    const auto a=add4(scale4(at(x0,y0),1-tx),scale4(at(x0+1,y0),tx));
    const auto b=add4(scale4(at(x0,y0+1),1-tx),scale4(at(x0+1,y0+1),tx));
    return add4(scale4(a,1-ty),scale4(b,ty));
}

Float2 select_uv(const PolygonTextureBinding& binding, Float2 uv0, Float2 uv1) noexcept {
    return binding.texcoord == 1U ? uv1 : uv0;
}

// Perspective-correct per-fragment gradients; constant LOD remains available to
// ray-based callers which do not have a pixel footprint.
struct TextureLod {
    float value{};
    Float2 dx{},dy{};
    Float3 worldDx{},worldDy{},objectDx{},objectDy{};
    bool gradients{},hasUvSets{};
    float recipeFootprintScale{1};
    Float2 uv0Dx{},uv0Dy{},uv1Dx{},uv1Dy{};
    TextureLod selected(bool second) const {auto out=*this;if(hasUvSets){out.dx=second?uv1Dx:uv0Dx;out.dy=second?uv1Dy:uv0Dy;}return out;}
    TextureLod(float v=0):value(v){}
    TextureLod transformed(const TextureTransform2D& t) const {
        auto out=*this;auto linear=t;linear.offset={};
        out.dx=transform_texture_coordinates(dx,linear);out.dy=transform_texture_coordinates(dy,linear);
        return out;
    }
};
Float4 sample_binding_uv(const CookedPolygonAsset& asset,const PolygonTextureBinding& binding,
                         const std::vector<PolygonTextureMipChain>& chains,Float2 uv,
                         TextureLod lod,PolygonRenderStats& stats,Float4 fallback,bool normalMap=false) noexcept {
    if(!binding.texture||*binding.texture>=asset.textures.size())return fallback;
    const PolygonTexture& texture=asset.textures[*binding.texture];
    if(texture.imageIndex>=chains.size()||chains[texture.imageIndex].levels.empty())return fallback;
    const PolygonSampler fallbackSampler{};
    const PolygonSampler& sampler=texture.samplerIndex&&*texture.samplerIndex<asset.samplers.size()?asset.samplers[*texture.samplerIndex]:fallbackSampler;
    const auto& chain=chains[texture.imageIndex];
    const auto& levels=normalMap&&!chain.normalLevels.empty()?chain.normalLevels:
        (binding.colorSpace==PolygonTextureColorSpace::Srgb&&!chain.srgbLevels.empty()?chain.srgbLevels:chain.levels);
    float levelValue=lod.value;
    if(lod.gradients){
        const float w=static_cast<float>(levels[0].width),h=static_cast<float>(levels[0].height);
        const float a=lod.dx.x*lod.dx.x*w*w+lod.dx.y*lod.dx.y*h*h;
        const float b=lod.dy.x*lod.dy.x*w*w+lod.dy.y*lod.dy.y*h*h;
        levelValue=std::log2(std::max(1.0F,std::sqrt(std::max(a,b))));
    }
    levelValue=std::clamp(levelValue,0.0F,static_cast<float>(levels.size()-1U));
    const auto level=static_cast<std::size_t>(std::floor(levelValue+(sampler.minFilter==ImportedTextureFilter::Nearest?.5F:0.F)));
    ++stats.textureSamples;
    const auto colorSpace=normalMap?PolygonTextureColorSpace::Linear:binding.colorSpace;
    PolygonSampler filtered=sampler;
    filtered.magFilter=levelValue>0?sampler.minFilter:sampler.magFilter;
    const Float4 a=sample_mip(levels[level],filtered,uv,colorSpace);
    if(sampler.minFilter==ImportedTextureFilter::Nearest||level+1U>=levels.size())return a;
    const float fraction=levelValue-static_cast<float>(level);
    if(fraction<=0)return a;
    ++stats.textureSamples;
    return add4(scale4(a,1-fraction),scale4(sample_mip(levels[level+1],filtered,uv,colorSpace),fraction));
}

Float4 weighted4(Float4 a,Float4 b,Float4 c,Float3 weights) noexcept {
    return {a.x*weights.x+b.x*weights.y+c.x*weights.z,
            a.y*weights.x+b.y*weights.y+c.y*weights.z,
            a.z*weights.x+b.z*weights.y+c.z*weights.z,
            a.w*weights.x+b.w*weights.y+c.w*weights.z};
}

TextureLod normal_triplanar_lod(TextureLod lod,const MaterialMappingSettings& mapping,int axis) {
    const auto project=[&](Float3 p){const float scale=mapping.triplanarScale;return axis==0?Float2{p.z*scale,p.y*scale}:axis==1?Float2{p.x*scale,p.z*scale}:Float2{p.x*scale,p.y*scale};};
    lod.dx=project(mapping.mappingMode==MaterialMappingMode::ObjectTriplanar?lod.objectDx:lod.worldDx);
    lod.dy=project(mapping.mappingMode==MaterialMappingMode::ObjectTriplanar?lod.objectDy:lod.worldDy);
    return lod.transformed(mapping.baseTransform);
}

// Triplanar normal maps decode each projection's sample and blend with the shared whiteout
// helper (dve/triplanar_normal.hpp), so a neutral map keeps the geometric normal. Mirrors
// SampleLiveMaterialNormal in shaders/common/live_material_sampling.hlsli (CPU/GPU parity).
Float3 triplanar_whiteout_normal(Float3 n,Float4 sx,Float4 sy,Float4 sz,float normalScale,Float3 weights) {
    const auto decode=[&](Float4 s){return Float3{(s.x*2.0F-1.0F)*normalScale,(s.y*2.0F-1.0F)*normalScale,s.z*2.0F-1.0F};};
    return dve::triplanar_whiteout_normal(n,decode(sx),decode(sy),decode(sz),weights);
}

Float4 sample_triplanar_binding(const CookedPolygonAsset& asset,const PolygonTextureBinding& binding,
                                const std::vector<PolygonTextureMipChain>& chains,
                                Float3 position,Float3 normal,const TextureTransform2D& transform,
                                float scale,float sharpness,TextureLod lod,PolygonRenderStats& stats,
                                Float4 fallback,bool normalMap=false) noexcept {
    if(!binding.texture)return fallback;
    const TriplanarCoordinates coordinates=make_triplanar_coordinates(position,normal,scale,sharpness);
    const auto plane=[&](int axis){
        auto out=lod;
        const auto uv=[&](Float3 v){return axis==0?Float2{v.z*scale,v.y*scale}:
            (axis==1?Float2{v.x*scale,v.z*scale}:Float2{v.x*scale,v.y*scale});};
        out.dx=uv(lod.worldDx);out.dy=uv(lod.worldDy);return out.transformed(transform);
    };
    const Float4 x=sample_binding_uv(asset,binding,chains,transform_texture_coordinates(coordinates.xProjection,transform),plane(0),stats,fallback,normalMap);
    const Float4 y=sample_binding_uv(asset,binding,chains,transform_texture_coordinates(coordinates.yProjection,transform),plane(1),stats,fallback,normalMap);
    const Float4 z=sample_binding_uv(asset,binding,chains,transform_texture_coordinates(coordinates.zProjection,transform),plane(2),stats,fallback,normalMap);
    stats.triplanarSamples+=3U;
    return weighted4(x,y,z,coordinates.weights);
}

Float4 sample_mapped_binding(const CookedPolygonAsset& asset,const PolygonTextureBinding& binding,
                             const std::vector<PolygonTextureMipChain>& chains,
                             const MaterialMappingSettings& mapping,Float2 mappedUv,
                             Float3 worldPosition,Float3 objectPosition,Float3 normal,
                             const TextureTransform2D& transform,float scaleMultiplier,TextureLod lod,
                             PolygonRenderStats& stats,Float4 fallback,bool normalMap=false) noexcept {
    if(mapping.mappingMode==MaterialMappingMode::WorldTriplanar)
        return sample_triplanar_binding(asset,binding,chains,worldPosition,normal,transform,
            mapping.triplanarScale*scaleMultiplier,mapping.triplanarBlendSharpness,lod,stats,fallback,normalMap);
    if(mapping.mappingMode==MaterialMappingMode::ObjectTriplanar){
        lod.worldDx=lod.objectDx;lod.worldDy=lod.objectDy;
        return sample_triplanar_binding(asset,binding,chains,objectPosition,normal,transform,
            mapping.triplanarScale*scaleMultiplier,mapping.triplanarBlendSharpness,lod,stats,fallback,normalMap);
    }
    return sample_binding_uv(asset,binding,chains,mappedUv,lod.transformed(transform),stats,fallback,normalMap);
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
    TextureLod lod,
    bool enableTextures,
    PolygonRenderStats& stats, const RigidTransform& pose = {},float units=1,bool normalOnly=false,
    Float2 normalUvOffset={0,0}) noexcept {
    MaterialSurfaceSample sample;
    if (materialIndex >= asset.materials.size() || materialIndex >= asset.materialBindings.size())
        return sample;
    if(!normalOnly)++stats.layerSourceSamples;
    const VoxelMaterialDefinition& material = asset.materials[materialIndex];
    const PolygonMaterialBinding& binding = asset.materialBindings[materialIndex];
    const auto mappedUv=[&](const PolygonTextureBinding& texture){return transform_texture_coordinates(
        (binding.mapping.mappingMode==MaterialMappingMode::UV1||texture.texcoord==1)?uv1:uv0,binding.mapping.baseTransform);};
    const auto footprint=[&](const PolygonTextureBinding& texture){return lod.selected(binding.mapping.mappingMode==MaterialMappingMode::UV1||texture.texcoord==1);};
    const auto channel=[&](const PolygonTextureBinding& texture,Float4 fallback){return enableTextures?
        sample_mapped_binding(asset,texture,chains,binding.mapping,mappedUv(texture),worldPosition,objectPosition,
            binding.mapping.mappingMode==MaterialMappingMode::ObjectTriplanar?inverse_transform_vector(pose,geometricNormal):geometricNormal,
            binding.mapping.baseTransform,1.F,footprint(texture),stats,fallback):fallback;};
    if(!normalOnly){
        const auto baseMap=channel(binding.baseColor,{1,1,1,1});
        const auto mr=channel(binding.metallicRoughness,{1,1,1,1});
        const auto emissiveMap=channel(binding.emissive,{1,1,1,1});
        const auto opacityMap=channel(binding.opacity,{1,1,1,1});
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
        sample.height=channel(binding.height,{.5F,.5F,.5F,1}).x;
    }
    sample.normal = {0.0F,0.0F,1.0F};
    if (binding.normal.texture && enableTextures) {
        if (binding.mapping.mappingMode==MaterialMappingMode::WorldTriplanar ||
            binding.mapping.mappingMode==MaterialMappingMode::ObjectTriplanar) {
            const Float3 position = binding.mapping.mappingMode==MaterialMappingMode::WorldTriplanar
                ? worldPosition : objectPosition;
            const TriplanarCoordinates tc = make_triplanar_coordinates(
                position,binding.mapping.mappingMode==MaterialMappingMode::ObjectTriplanar?inverse_transform_vector(pose,geometricNormal):geometricNormal,binding.mapping.triplanarScale,
                binding.mapping.triplanarBlendSharpness);
            const Float4 sx=sample_binding_uv(asset,binding.normal,chains,
                transform_texture_coordinates(tc.xProjection,binding.mapping.baseTransform),normal_triplanar_lod(lod,binding.mapping,0),stats,{0.5F,0.5F,1.0F,1.0F},true);
            const Float4 sy=sample_binding_uv(asset,binding.normal,chains,
                transform_texture_coordinates(tc.yProjection,binding.mapping.baseTransform),normal_triplanar_lod(lod,binding.mapping,1),stats,{0.5F,0.5F,1.0F,1.0F},true);
            const Float4 sz=sample_binding_uv(asset,binding.normal,chains,
                transform_texture_coordinates(tc.zProjection,binding.mapping.baseTransform),normal_triplanar_lod(lod,binding.mapping,2),stats,{0.5F,0.5F,1.0F,1.0F},true);
            Float3 worldLayer=triplanar_whiteout_normal(binding.mapping.mappingMode==MaterialMappingMode::ObjectTriplanar?inverse_transform_vector(pose,geometricNormal):geometricNormal,sx,sy,sz,binding.normalScale,tc.weights);
            if(binding.mapping.mappingMode==MaterialMappingMode::ObjectTriplanar)worldLayer=transform_vector(pose,worldLayer);
            sample.normal={dot(worldLayer,tangent),dot(worldLayer,bitangent),dot(worldLayer,geometricNormal)};
            stats.triplanarSamples+=3U;
        } else {
            const auto normalUv=mappedUv(binding.normal);
            const Float4 nm=sample_binding_uv(asset,binding.normal,chains,{normalUv.x+normalUvOffset.x,normalUv.y+normalUvOffset.y},footprint(binding.normal).transformed(binding.mapping.baseTransform),stats,{0.5F,0.5F,1.0F,1.0F},true);
            sample.normal=normalize(Float3{(nm.x*2.0F-1.0F)*binding.normalScale,
                                           (nm.y*2.0F-1.0F)*binding.normalScale,
                                           nm.z*2.0F-1.0F});
            auto gradients=footprint(binding.normal).transformed(binding.mapping.baseTransform);
            const float basisScale=(std::max)({std::abs(gradients.dx.x),std::abs(gradients.dx.y),std::abs(gradients.dy.x),std::abs(gradients.dy.y),1e-30F});
            gradients.dx={gradients.dx.x/basisScale,gradients.dx.y/basisScale};gradients.dy={gradients.dy.x/basisScale,gradients.dy.y/basisScale};
            const auto basisDx=multiply(lod.worldDx,1/basisScale),basisDy=multiply(lod.worldDy,1/basisScale);
            const float determinant=gradients.dx.x*gradients.dy.y-gradients.dx.y*gradients.dy.x;
            if((gradients.gradients||gradients.hasUvSets)&&std::abs(determinant)>1e-8F){
                auto t=multiply(subtract(multiply(basisDx,gradients.dy.y),multiply(basisDy,gradients.dx.y)),1/determinant);
                const auto rawB=multiply(subtract(multiply(basisDy,gradients.dx.x),multiply(basisDx,gradients.dy.x)),1/determinant);
                t=subtract(t,multiply(geometricNormal,dot(geometricNormal,t)));
                if(length(t)>1e-6F){t=normalize(t);const auto b=multiply(cross3(geometricNormal,t),dot(cross3(geometricNormal,t),rawB)<0?-1.F:1.F);
                    const auto world=normalize(add(add(multiply(t,sample.normal.x),multiply(b,sample.normal.y)),multiply(geometricNormal,sample.normal.z)));
                    sample.normal={dot(world,tangent),dot(world,bitangent),dot(world,geometricNormal)};}
            }
        }
    }
    if(material.surfaceDetail.pattern!=SurfacePattern::NoPattern && units>0){
        auto evaluated=material;evaluated.baseColor=sample.baseColor;evaluated.roughness=sample.roughness;
        auto world=normalize(add(add(multiply(tangent,sample.normal.x),multiply(bitangent,sample.normal.y)),multiply(geometricNormal,sample.normal.z)));
        float width=(std::max)(length(lod.worldDx),length(lod.worldDy))*units*lod.recipeFootprintScale;
        if(unsigned(material.surfaceDetail.space)>=unsigned(SurfaceDetailSpace::UV0)){
            const auto gradients=lod.selected(material.surfaceDetail.space==SurfaceDetailSpace::UV1);
            width=(std::max)(std::hypot(gradients.dx.x,gradients.dx.y),std::hypot(gradients.dy.x,gradients.dy.y))*material.surfaceDetail.uvTileMeters*lod.recipeFootprintScale;
        }
        apply_material_surface_detail(evaluated,world,{multiply(objectPosition,units),multiply(worldPosition,units),uv0,uv1,true,pose,tangent,bitangent,width});
        sample.baseColor=evaluated.baseColor;sample.roughness=evaluated.roughness;
        sample.normal={dot(world,tangent),dot(world,bitangent),dot(world,geometricNormal)};
    }
    return sample;
}

// The sky/ground hemisphere around a direction (spwi::environment_radiance, ShadeAmbient).
Float3 hemisphere_light(const RenderEnvironment& environment,Float3 direction) noexcept {
    const float t=std::clamp(0.5F+0.5F*normalize(direction).y,0.0F,1.0F);
    return add(multiply(environment.groundColor,1.0F-t),multiply(environment.skyColor,t));
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
    const Float3 sunIrradiance=multiply(environment.sunColor,environment.sunIntensity);
    // Diffuse: base x (1 - metallic) x (1 - F) x sun x cos (or its subsurface-diffused average).
    const Float3 f0=specular_f0(rgb,material.metallic,material.specular);
    const Float3 halfVector=normalize(add(view,light));
    const Float3 fresnel=fresnel_schlick(std::clamp(dot(view,halfVector),0.0F,1.0F),f0);
    const Float3 diffuseColor=mul3(multiply(rgb,1.0F-material.metallic),Float3{1.0F-fresnel.x,1.0F-fresnel.y,1.0F-fresnel.z});
    const float sunTerm=diffusedSun>=0.0F?diffusedSun:(std::max)(0.0F,ndotl);
    out.directDiffuse=multiply(mul3(diffuseColor,sunIrradiance),sunTerm);
    const ShadingLobes lobes=material.shadingModel==MaterialShadingModel::ClearCoat
        ? shade_clear_coat_direct(normal,view,light,sunIrradiance,rgb,material.metallic,material.roughness,material.specular,
                                  material.clearCoat,material.clearCoatRoughness,material.iridescence,material.iridescenceIor,
                                  material.iridescenceThicknessNm,material.indexOfRefraction)
        : shade_direct_light(normal,view,light,sunIrradiance,rgb,material.metallic,material.roughness,material.specular,
                             material.iridescence,material.iridescenceIor,material.iridescenceThicknessNm,material.indexOfRefraction);
    Float3 specularSum=lobes.specular;
    if(material_has_sheen(material))
        specularSum=add(specularSum,sheen_direct(normal,view,light,sunIrradiance,material.sheenColor,material.sheenRoughness));
    out.directSpecular=multiply(specularSum,kCpuRadianceScale);
    if(material.shadingModel==MaterialShadingModel::ClearCoat&&material.clearCoat>0.0F){
        const Float3 coat=fresnel_schlick(std::clamp(dot(view,halfVector),0.0F,1.0F),{0.04F,0.04F,0.04F});
        const float c=std::clamp(material.clearCoat,0.0F,1.0F);
        const Float3 through{1.0F-c*coat.x,1.0F-c*coat.y,1.0F-c*coat.z};
        out.directDiffuse=mul3(out.directDiffuse,mul3(through,through));
    }
    if(environment.globalIlluminationMode!=GlobalIlluminationMode::Off){
        // No secondary rays here beyond AO: the sky hemisphere around the normal, occluded.
        const float intensity=environment.globalIlluminationIntensity;
        const Float3 irradiance=multiply(hemisphere_light(environment,normal),intensity*ambientOcclusion);
        const Float3 reflection=subtract(multiply(normal,2.0F*dot(normal,view)),view);
        const ShadingLobes ambient=shade_ambient_light(normal,view,irradiance,multiply(hemisphere_light(environment,reflection),intensity),
                                                       rgb,material.metallic,material.roughness,material.specular,ambientOcclusion,
                                                       material.iridescence,material.iridescenceIor,material.iridescenceThicknessNm,material.indexOfRefraction);
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
                ? mapping.maximumHeightSteps + 1U + mapping.refinementSteps
                : (mapping.heightMode == HeightMappingMode::SteepParallax
                    ? mapping.maximumHeightSteps + 1U
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
    float nearPlane{},farPlane{};
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
        bounds.minimum={(std::min)(bounds.minimum.x,v.position.x),(std::min)(bounds.minimum.y,v.position.y),(std::min)(bounds.minimum.z,v.position.z)};
        bounds.maximum={(std::max)(bounds.maximum.x,v.position.x),(std::max)(bounds.maximum.y,v.position.y),(std::max)(bounds.maximum.z,v.position.z)};
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
            const float h=(std::max)(r,1.0e-2F);
            ++stats.subsurfaceDiffusionProbes;
            const auto hit=probe_polygon_surface(*tri.subsurfaceBvh,*tri.asset,tri.transform,
                                                 add(add(worldPosition,offset),multiply(normal,h)),multiply(normal,-1.0F),2.0F*h);
            if(!hit||hit->materialIndex!=tri.submesh->materialIndex)continue;
            qn=hit->normal;
        }
        ++valid;
        sum+=(std::max)(0.0F,dot(qn,sun));
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

    const float dwDx[3]{(tri.v[2].y-tri.v[1].y)/area,(tri.v[0].y-tri.v[2].y)/area,(tri.v[1].y-tri.v[0].y)/area};
    const float dwDy[3]{-(tri.v[2].x-tri.v[1].x)/area,-(tri.v[0].x-tri.v[2].x)/area,-(tri.v[1].x-tri.v[0].x)/area};
    const auto gradient=[&](const float* dw,Float3 value,Float3 a,Float3 b,Float3 c,float invZ){
        Float3 numerator{};float denominator=0;
        const Float3 values[]{a,b,c};
        for(int i=0;i<3;++i){const float w=dw[i]*tri.v[i].invZ;numerator=add(numerator,multiply(values[i],w));denominator+=w;}
        return multiply(subtract(numerator,multiply(value,denominator)),1/invZ);
    };

    for(int y=minY;y<=maxY;++y)for(int x=minX;x<=maxX;++x){
        const float px=static_cast<float>(x)+0.5F,py=static_cast<float>(y)+0.5F;
        float w0=edge(tri.v[1].x,tri.v[1].y,tri.v[2].x,tri.v[2].y,px,py)/area;
        float w1=edge(tri.v[2].x,tri.v[2].y,tri.v[0].x,tri.v[0].y,px,py)/area;
        float w2=1.0F-w0-w1;if(w0<-kEpsilon||w1<-kEpsilon||w2<-kEpsilon)continue;
        const float invZ=w0*tri.v[0].invZ+w1*tri.v[1].invZ+w2*tri.v[2].invZ;if(invZ<=0)continue;
        w0=w0*tri.v[0].invZ/invZ;w1=w1*tri.v[1].invZ/invZ;w2=1.0F-w0-w1;
        float depth=w0*tri.v[0].depth+w1*tri.v[1].depth+w2*tri.v[2].depth;
        const std::size_t index=static_cast<std::size_t>(y)*target.width+static_cast<std::size_t>(x);
        const bool relief=options.surfaceDetail.reliefDepthMeters>0&&options.enableTextures&&tri.binding->height.texture&&
            tri.binding->mapping.heightMode!=HeightMappingMode::Off&&
            (tri.binding->mapping.mappingMode==MaterialMappingMode::UV0||tri.binding->mapping.mappingMode==MaterialMappingMode::UV1);
        if(!relief&&depth>=target.depth[index]){++stats.depthRejectedFragments;continue;}
        Float2 uv{w0*tri.v[0].uv.x+w1*tri.v[1].uv.x+w2*tri.v[2].uv.x,
                  w0*tri.v[0].uv.y+w1*tri.v[1].uv.y+w2*tri.v[2].uv.y};
        Float2 uv1{w0*tri.v[0].uv1.x+w1*tri.v[1].uv1.x+w2*tri.v[2].uv1.x,
                   w0*tri.v[0].uv1.y+w1*tri.v[1].uv1.y+w2*tri.v[2].uv1.y};
        Float3 worldPosition{
            w0*tri.v[0].worldPosition.x+w1*tri.v[1].worldPosition.x+w2*tri.v[2].worldPosition.x,
            w0*tri.v[0].worldPosition.y+w1*tri.v[1].worldPosition.y+w2*tri.v[2].worldPosition.y,
            w0*tri.v[0].worldPosition.z+w1*tri.v[1].worldPosition.z+w2*tri.v[2].worldPosition.z};
        const Float3 objectPosition{
            w0*tri.v[0].objectPosition.x+w1*tri.v[1].objectPosition.x+w2*tri.v[2].objectPosition.x,
            w0*tri.v[0].objectPosition.y+w1*tri.v[1].objectPosition.y+w2*tri.v[2].objectPosition.y,
            w0*tri.v[0].objectPosition.z+w1*tri.v[1].objectPosition.z+w2*tri.v[2].objectPosition.z};
        TextureLod textureLod;
        textureLod.gradients=options.enableMipSelection;
        const bool secondUv=tri.binding->mapping.mappingMode==MaterialMappingMode::UV1;
        const Float2 chosen=secondUv?uv1:uv;
        const auto uv3=[](Float2 q){return Float3{q.x,q.y,0};};
        const Float3 ua=uv3(secondUv?tri.v[0].uv1:tri.v[0].uv),ub=uv3(secondUv?tri.v[1].uv1:tri.v[1].uv),uc=uv3(secondUv?tri.v[2].uv1:tri.v[2].uv);
        const Float3 gx=gradient(dwDx,uv3(chosen),ua,ub,uc,invZ),gy=gradient(dwDy,uv3(chosen),ua,ub,uc,invZ);
        textureLod.dx={gx.x,gx.y};textureLod.dy={gy.x,gy.y};
        textureLod.hasUvSets=true;
        const auto ux0=gradient(dwDx,uv3(uv),uv3(tri.v[0].uv),uv3(tri.v[1].uv),uv3(tri.v[2].uv),invZ),uy0=gradient(dwDy,uv3(uv),uv3(tri.v[0].uv),uv3(tri.v[1].uv),uv3(tri.v[2].uv),invZ);
        const auto ux1=gradient(dwDx,uv3(uv1),uv3(tri.v[0].uv1),uv3(tri.v[1].uv1),uv3(tri.v[2].uv1),invZ),uy1=gradient(dwDy,uv3(uv1),uv3(tri.v[0].uv1),uv3(tri.v[1].uv1),uv3(tri.v[2].uv1),invZ);
        textureLod.uv0Dx={ux0.x,ux0.y};textureLod.uv0Dy={uy0.x,uy0.y};textureLod.uv1Dx={ux1.x,ux1.y};textureLod.uv1Dy={uy1.x,uy1.y};
        textureLod.worldDx=gradient(dwDx,worldPosition,tri.v[0].worldPosition,tri.v[1].worldPosition,tri.v[2].worldPosition,invZ);
        textureLod.worldDy=gradient(dwDy,worldPosition,tri.v[0].worldPosition,tri.v[1].worldPosition,tri.v[2].worldPosition,invZ);
        textureLod.objectDx=gradient(dwDx,objectPosition,tri.v[0].objectPosition,tri.v[1].objectPosition,tri.v[2].objectPosition,invZ);
        textureLod.objectDy=gradient(dwDy,objectPosition,tri.v[0].objectPosition,tri.v[1].objectPosition,tri.v[2].objectPosition,invZ);
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
        const Float2 unshiftedMappedUv=mappedUv;
        std::uint32_t fragmentParallaxSamples = 0U;
        float fragmentHeight=tri.binding->mapping.heightReferencePlane;
        bool heightHit=false;
        float reliefOffset=0;
        const auto heightSampler=[&](Float2 sampleUv){return sample_binding_uv(*tri.asset,tri.binding->height,chains,sampleUv,textureLod.transformed(tri.binding->mapping.baseTransform),stats,{0.5F,0.5F,0.5F,1.0F}).x;};
        if(options.enableTextures&&tri.binding->height.texture&&tri.binding->mapping.heightMode!=HeightMappingMode::Off){
            const auto mappedLod=textureLod.transformed(tri.binding->mapping.baseTransform);
            const float footprint=options.enableMipSelection?(std::max)(std::hypot(mappedLod.dx.x,mappedLod.dx.y),std::hypot(mappedLod.dy.x,mappedLod.dy.y)):0;
            const auto& heightTexture=tri.asset->textures[*tri.binding->height.texture];
            Float2 heightBounds=chains[heightTexture.imageIndex].heightBounds;
            if(tri.binding->height.colorSpace==PolygonTextureColorSpace::Srgb)heightBounds={srgb_to_linear(heightBounds.x),srgb_to_linear(heightBounds.y)};
            const ParallaxMappingResult parallax=apply_parallax_mapping(mappedUv,viewTangent,cameraDistance*resolve_meters_per_voxel(options.metersPerUnit),
                tri.binding->mapping,heightSampler,{std::clamp(options.surfaceDetail.maximumParallaxSteps,1U,128U),footprint,heightBounds});
            if(parallax.valid){mappedUv=parallax.uv;heightHit=parallax.hit;fragmentHeight=parallax.layerDepth;}
            if(!parallax.valid||!parallax.hit)++stats.parallaxFallbacks;
            fragmentParallaxSamples=parallax.samples;stats.parallaxSamples+=parallax.samples;
            if(relief&&heightHit&&viewTangent.z>0.08F){
                const float fade=std::clamp(1-cameraDistance*resolve_meters_per_voxel(options.metersPerUnit)/tri.binding->mapping.maximumParallaxDistanceMeters,0.0F,1.0F);
                reliefOffset=(fragmentHeight-tri.binding->mapping.heightReferencePlane)*options.surfaceDetail.reliefDepthMeters*fade/
                    (resolve_meters_per_voxel(options.metersPerUnit)*viewTangent.z);
                // depth is affine in camera-space Z; scale the original camera Z
                // by the ray-distance ratio. Near/far and surface occlusion use
                // the corrected value before any colour/AOV is written.
                const float cameraZ=w0*tri.v[0].cameraZ+w1*tri.v[1].cameraZ+w2*tri.v[2].cameraZ;
                const float correctedZ=cameraZ*(1+reliefOffset/(std::max)(cameraDistance,1e-6F));
                // The camera near/far span is retained in DeferredTriangle.
                depth=(correctedZ-tri.nearPlane)/(tri.farPlane-tri.nearPlane);
                worldPosition=subtract(worldPosition,multiply(viewWorld,reliefOffset));
            }
        }
        if(depth<0||depth>=1||depth>=target.depth[index]){++stats.depthRejectedFragments;continue;}
        const Float4 textureColor=options.enableTextures?sample_mapped_binding(*tri.asset,tri.binding->baseColor,chains,tri.binding->mapping,mappedUv,worldPosition,objectPosition,normal,tri.binding->mapping.baseTransform,1.0F,textureLod,stats,{1,1,1,1}):Float4{1,1,1,1};
        const Float4 mr=options.enableTextures?sample_mapped_binding(*tri.asset,tri.binding->metallicRoughness,chains,tri.binding->mapping,mappedUv,worldPosition,objectPosition,normal,tri.binding->mapping.baseTransform,1.0F,textureLod,stats,{1,1,1,1}):Float4{1,1,1,1};
        const Float4 emissiveMap=options.enableTextures?sample_mapped_binding(*tri.asset,tri.binding->emissive,chains,tri.binding->mapping,mappedUv,worldPosition,objectPosition,normal,tri.binding->mapping.baseTransform,1.0F,textureLod,stats,{1,1,1,1}):Float4{1,1,1,1};
        const Float4 opacityMap=options.enableTextures?sample_mapped_binding(*tri.asset,tri.binding->opacity,chains,tri.binding->mapping,mappedUv,worldPosition,objectPosition,normal,tri.binding->mapping.baseTransform,1.0F,textureLod,stats,{1,1,1,1}):Float4{1,1,1,1};
        Float4 base=mul4(mul4(mul4(tri.material->baseColor,vertexColor),textureColor),tri.tint);
        if(tri.binding->opacity.texture)base.w*=opacityMap.x;
        VoxelMaterialDefinition effective=*tri.material;
        if(tri.binding->metallicRoughness.texture){effective.roughness=std::clamp(effective.roughness*mr.y,0.0F,1.0F);effective.metallic=std::clamp(effective.metallic*mr.z,0.0F,1.0F);}
        if(tri.binding->emissive.texture)effective.emissive=mul3(effective.emissive,{emissiveMap.x,emissiveMap.y,emissiveMap.z});
        const float detailFade=material_detail_fade(cameraDistance*resolve_meters_per_voxel(options.metersPerUnit),tri.binding->mapping);
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
            const auto geometryNormal=normalize(interpolatedNormal);
            const auto root=sample_layer_surface(*tri.asset,tri.submesh->materialIndex,chains,
                uv,uv1,worldPosition,objectPosition,geometryNormal,tangent,bitangent,textureLod,
                true,stats,tri.transform,0,true,
                {mappedUv.x-unshiftedMappedUv.x,mappedUv.y-unshiftedMappedUv.y});
            normal=normalize(add(add(multiply(tangent,root.normal.x),multiply(bitangent,root.normal.y)),multiply(geometryNormal,root.normal.z)));
        }
        if(options.enableTextures&&tri.binding->height.texture&&options.surfaceDetail.heightBumpStrength!=0&&
           (tri.binding->mapping.mappingMode==MaterialMappingMode::UV0||tri.binding->mapping.mappingMode==MaterialMappingMode::UV1)){
            const auto& texture=tri.asset->textures[*tri.binding->height.texture];
            const auto& image=tri.asset->images[texture.imageIndex];
            const Float2 texelSize{1.0F/static_cast<float>(image.width),1.0F/static_cast<float>(image.height)};
            const auto bump=height_bump_normal(mappedUv,texelSize,options.surfaceDetail.heightBumpStrength,heightSampler);stats.bumpSamples+=4;
            const auto gn=normalize(interpolatedNormal);
            const Float3 baseTangent{dot(normal,tangent),dot(normal,bitangent),dot(normal,gn)};
            const auto combined=blend_detail_normal(baseTangent,bump,1);
            normal=normalize(add(add(multiply(tangent,combined.x),multiply(bitangent,combined.y)),multiply(gn,combined.z)));
        }
        if(options.enableTextures&&detailFade>0.0F&&tri.binding->detailNormal.texture){
            const Float4 dn=sample_mapped_binding(*tri.asset,tri.binding->detailNormal,chains,tri.binding->mapping,detailUv,worldPosition,objectPosition,normal,tri.binding->mapping.detailTransform,8.0F,textureLod,stats,{0.5F,0.5F,1.0F,1.0F},true);
            const Float3 detailTangent=normalize(Float3{(dn.x*2.0F-1.0F)*tri.binding->detailNormalScale,(dn.y*2.0F-1.0F)*tri.binding->detailNormalScale,dn.z*2.0F-1.0F});
            const Float3 geometricNormal=normalize(interpolatedNormal);
            const Float3 baseTangent{dot(normal,tangent),dot(normal,bitangent),dot(normal,geometricNormal)};
            const Float3 combined=blend_detail_normal(baseTangent,detailTangent,tri.binding->mapping.detailNormalStrength*detailFade);
            normal=normalize(add(add(multiply(tangent,combined.x),multiply(bitangent,combined.y)),multiply(geometricNormal,combined.z)));++stats.detailSamples;
        }

        float layerDebugMask=0.0F;
        float layerDebugCoverage=0.0F;
        bool appliedMaterialLayer=false;
        if(!tri.material->layers.empty()){
            // Layered CPU raster and ray/bake callers share the same root map evaluation.
            const auto geometryNormal=normalize(interpolatedNormal);
            const auto root=sample_layer_surface(*tri.asset,tri.submesh->materialIndex,chains,
                uv,uv1,worldPosition,objectPosition,geometryNormal,tangent,bitangent,textureLod,
                options.enableTextures,stats,tri.transform,0);
            base=mul4(mul4(root.baseColor,vertexColor),tri.tint);
            effective.metallic=root.metallic;effective.roughness=root.roughness;effective.emissive=root.emissive;
            normal=normalize(add(add(multiply(tangent,root.normal.x),multiply(bitangent,root.normal.y)),multiply(geometryNormal,root.normal.z)));
            const float baseHeight=root.height;
            MaterialSurfaceSample surface;
            surface.baseColor=base;surface.normal=normal;surface.metallic=effective.metallic;
            surface.roughness=effective.roughness;surface.emissive=effective.emissive;
            surface.opacity=base.w;surface.height=baseHeight;
            const std::size_t layerCount=std::min<std::size_t>(tri.material->layers.size(),kMaximumVoxelMaterialLayers);
            for(std::size_t layerIndex=0U;layerIndex<layerCount;++layerIndex){
                const VoxelMaterialLayer& authored=tri.material->layers[layerIndex];
                if(!authored.enabled||authored.weight<=0.0F||authored.sourceMaterial==kAirMaterial||
                   authored.sourceMaterial>=tri.asset->materials.size())continue;
                MaterialSurfaceSample source=sample_layer_surface(*tri.asset,authored.sourceMaterial,chains,
                    uv,uv1,worldPosition,objectPosition,normalize(interpolatedNormal),tangent,bitangent,
                    textureLod,options.enableTextures,stats,tri.transform,resolve_meters_per_voxel(options.metersPerUnit));
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
                        const Float2 maskSelected=(tri.binding->mapping.mappingMode==MaterialMappingMode::UV1||binding.mask.texcoord==1)?uv1:uv;
                        const Float2 maskUv=transform_texture_coordinates(maskSelected,tri.binding->mapping.baseTransform);
                        const Float4 mask=sample_mapped_binding(*tri.asset,binding.mask,chains,tri.binding->mapping,maskUv,
                            worldPosition,objectPosition,tri.binding->mapping.mappingMode==MaterialMappingMode::ObjectTriplanar?inverse_transform_vector(tri.transform,geometryNormal):geometryNormal,tri.binding->mapping.baseTransform,1.0F,textureLod.selected(tri.binding->mapping.mappingMode==MaterialMappingMode::UV1||binding.mask.texcoord==1),stats,{1,1,1,1});
                        layer.mask=std::clamp(mask.x*binding.maskScale+binding.maskBias,0.0F,1.0F);
                        ++stats.layerMaskSamples;
                    }else{
                        layer.mask=std::clamp(binding.maskScale+binding.maskBias,0.0F,1.0F);
                    }
                    if(options.enableTextures&&binding.height.texture){
                        const Float2 heightSelected=(tri.binding->mapping.mappingMode==MaterialMappingMode::UV1||binding.height.texcoord==1)?uv1:uv;
                        const Float2 heightUv=transform_texture_coordinates(heightSelected,tri.binding->mapping.baseTransform);
                        layer.surface.height=sample_mapped_binding(*tri.asset,binding.height,chains,tri.binding->mapping,heightUv,
                            worldPosition,objectPosition,tri.binding->mapping.mappingMode==MaterialMappingMode::ObjectTriplanar?inverse_transform_vector(tri.transform,geometryNormal):geometryNormal,tri.binding->mapping.baseTransform,1.0F,textureLod.selected(tri.binding->mapping.mappingMode==MaterialMappingMode::UV1||binding.height.texcoord==1),stats,{0.5F,0.5F,0.5F,1.0F}).x;
                    }
                }
                const Float3 tangentLayerNormal=layer.surface.normal;
                layer.surface.normal={0.0F,0.0F,1.0F};
                const float coverage=height_aware_material_layer_coverage(surface.height,layer);
                float ignoredCoverage{};
                surface=blend_material_surface_layer(surface,layer,&ignoredCoverage);
                const auto geom=normalize(interpolatedNormal);
                const auto bn=Float3{dot(normal,tangent),dot(normal,bitangent),dot(normal,geom)};
                const auto combined=blend_reoriented_material_normals(bn,tangentLayerNormal,coverage);
                surface.normal=normalize(add(add(multiply(tangent,combined.x),multiply(bitangent,combined.y)),multiply(geom,combined.z)));
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
        if(effective.normalMap){
            const float units=options.metersPerUnit>0&&std::isfinite(options.metersPerUnit)?options.metersPerUnit:kDefaultMetersPerVoxel;
            normal=normalize(transform_vector(tri.transform,voxel_normal_map_normal(effective,multiply(objectPosition,units),inverse_transform_vector(tri.transform,normal))));
        }
        if(effective.surfaceDetail.pattern!=SurfacePattern::NoPattern){
            const float units=resolve_meters_per_voxel(options.metersPerUnit);
            float footprint=(std::max)(length(textureLod.worldDx),length(textureLod.worldDy))*units;
            if(effective.surfaceDetail.space==SurfaceDetailSpace::UV0||effective.surfaceDetail.space==SurfaceDetailSpace::UV1){
                const bool uvSecond=effective.surfaceDetail.space==SurfaceDetailSpace::UV1;
                const auto dx=gradient(dwDx,uv3(uvSecond?uv1:uv),uv3(uvSecond?tri.v[0].uv1:tri.v[0].uv),uv3(uvSecond?tri.v[1].uv1:tri.v[1].uv),uv3(uvSecond?tri.v[2].uv1:tri.v[2].uv),invZ);
                const auto dy=gradient(dwDy,uv3(uvSecond?uv1:uv),uv3(uvSecond?tri.v[0].uv1:tri.v[0].uv),uv3(uvSecond?tri.v[1].uv1:tri.v[1].uv),uv3(uvSecond?tri.v[2].uv1:tri.v[2].uv),invZ);
                footprint=(std::max)(length(dx),length(dy))*effective.surfaceDetail.uvTileMeters;
            }
            effective.baseColor=base;
            apply_material_surface_detail(effective,normal,{multiply(objectPosition,units),multiply(worldPosition,units),uv,uv1,true,tri.transform,tangent,bitangent,footprint});
            base=effective.baseColor;
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
            if(options.surfaceDetail.parallaxSelfShadow&&heightHit&&tri.binding->height.texture){
                const auto sun=normalize(environment.sunDirection);
                const Float3 tangentSun{dot(sun,tangent),dot(sun,bitangent),dot(sun,normalize(interpolatedNormal))};
                const auto shadow=parallax_height_shadow(mappedUv,fragmentHeight,tangentSun,tri.binding->mapping,heightSampler,
                    std::clamp(options.surfaceDetail.shadowSteps,1U,64U));
                stats.parallaxShadowSamples+=shadow.samples;
                passes.light.directDiffuse=multiply(passes.light.directDiffuse,shadow.visibility);
                passes.light.directSpecular=multiply(passes.light.directSpecular,shadow.visibility);
            }
            if (options.sceneLighting) {
              const float visibility = options.sceneLighting->sun_visibility(
                  worldPosition, normal, static_cast<std::uint32_t>(index));
              passes.sunVisibility = visibility;
              passes.shadow = multiply(
                  add(passes.light.directDiffuse, passes.light.directSpecular),
                  1 - visibility);
              passes.light.directDiffuse =
                  multiply(passes.light.directDiffuse, visibility);
              passes.light.directSpecular =
                  multiply(passes.light.directSpecular, visibility);
              effective.baseColor = base;
              const auto local = options.sceneLighting->direct(
                  worldPosition, normal, viewWorld, effective,
                  static_cast<std::uint32_t>(index));
              passes.light.directDiffuse =
                  add(passes.light.directDiffuse, local.diffuse);
              passes.light.directSpecular =
                  add(passes.light.directSpecular, local.specular);
              if (environment.pathTraceBounces > 0 &&
                  environment.globalIlluminationMode !=
                      GlobalIlluminationMode::Off) {
                const auto bounce = options.sceneLighting->indirect_lobes(
                    worldPosition, normal, viewWorld, effective,
                    static_cast<std::uint32_t>(index));
                passes.light.indirectDiffuse = bounce.diffuse;
                passes.light.indirectSpecular = bounce.specular;
              }
            }
            if (options.sceneLighting && material_uses_camera_transport(effective)) {
                const float cameraZ = w0*tri.v[0].cameraZ + w1*tri.v[1].cameraZ + w2*tri.v[2].cameraZ;
                const float rayLimit = tri.farPlane * cameraDistance / (std::max)(1e-6F, cameraZ);
                // The camera path re-traces from the eye, so this fragment's mapped normal
                // is only valid if the fragment *is* the traced first hit. Pass the raster
                // surface distance along the ray; camera_transport uses the fragment normal
                // (flipped to face the ray, so genuine back faces of double-sided sheets or a
                // camera inside glass keep their mapped normal) only when the traced hit lies
                // at that distance. A different surface (e.g. the far side of an inward-wound
                // closed mesh) falls back to the traced normal instead of bending the
                // refracted ray into total internal reflection.
                passes.light = options.sceneLighting->camera_transport(
                    tri.cameraPosition, normalize(subtract(worldPosition, tri.cameraPosition)),
                    rayLimit, static_cast<std::uint32_t>(index), &effective, normal,
                    cameraDistance);
            }
            shaded=beauty(passes.light,base.w);
        }
        if(target.passes.enabled){
            passes.normal=normal;
            const float cameraZ=w0*tri.v[0].cameraZ+w1*tri.v[1].cameraZ+w2*tri.v[2].cameraZ;
            passes.depthMeters=cameraZ*(1+reliefOffset/(std::max)(cameraDistance,1e-6F))*resolve_meters_per_voxel(options.metersPerUnit);
        }
        if(!debugView&&!material_uses_camera_transport(effective)&&(transparent||tri.material->blendMode==MaterialBlendMode::Translucent||base.w<0.999F)){
            const float alpha=std::clamp(shaded.w,0.0F,1.0F);const Float4 old=target.hdrColor[index];
            target.hdrColor[index]={shaded.x*alpha+old.x*(1-alpha),shaded.y*alpha+old.y*(1-alpha),shaded.z*alpha+old.z*(1-alpha),1};
            target.write_passes(index,passes,alpha,false);
            ++stats.transparentFragments;
        }else{
            target.hdrColor[index]=shaded;target.depth[index]=depth;
            if(relief&&heightHit)++stats.reliefDepthWrites;
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
// Vector tonemap operators (AgX and Khronos PBR Neutral mix channels, so they
// cannot be expressed as per-channel curves). Formulas ported from NVIDIA's
// nvpro_core2 nvshaders/tonemap_functions.h.slang (Apache-2.0), which adapts
// the Khronos Group PBR Neutral reference and Benjamin Wrensch's minimal AgX
// approximation of Troy Sobotka's AgX. The sRGB encode stays a separate step,
// as elsewhere in this pipeline.
Float3 agx_color(Float3 c) noexcept {
    // mul(color, M): row vector times the row-listed matrix.
    auto mul = [](Float3 v, const float (&m)[3][3]) noexcept {
        return Float3{v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0],
                      v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1],
                      v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2]};
    };
    static constexpr float kInset[3][3] = {
        {0.842479062253094F, 0.0423282422610123F, 0.0423756549057051F},
        {0.0784335999999992F, 0.878468636469772F, 0.0784336F},
        {0.0792237451477643F, 0.0791661274605434F, 0.879142973793104F}};
    static constexpr float kOutset[3][3] = {
        {1.19687900512017F, -0.0528968517574562F, -0.0529716355144438F},
        {-0.0980208811401368F, 1.15190312990417F, -0.0980434501171241F},
        {-0.0990297440797205F, -0.0989611768448433F, 1.15107367264116F}};
    Float3 v = mul(c, kInset);
    static constexpr float kMinEv = -12.47393F, kMaxEv = 4.026069F;
    auto encode = [](float x) noexcept {
        const float logged = std::log2((std::max)(x, 1.0e-10F));
        return (std::clamp(logged, kMinEv, kMaxEv) - kMinEv) / (kMaxEv - kMinEv);
    };
    v = Float3{encode(v.x), encode(v.y), encode(v.z)};
    auto sigmoid = [](float x) noexcept {
        float r = 15.5F * x - 40.14F;
        r = r * x + 31.96F; r = r * x - 6.868F; r = r * x + 0.4298F;
        r = r * x + 0.1191F; r = r * x - 0.0023F;
        return r;
    };
    v = Float3{sigmoid(v.x), sigmoid(v.y), sigmoid(v.z)};
    return mul(v, kOutset);
}
Float3 pbr_neutral_color(Float3 c) noexcept {
    constexpr float kStartCompression = 0.8F - 0.04F;
    constexpr float kDesaturation = 0.15F;
    const float x = (std::min)(c.x, (std::min)(c.y, c.z));
    const float peak = (std::max)(c.x, (std::max)(c.y, c.z));
    const float offset = x < 0.08F ? x * (-6.25F * x + 1.0F) : 0.04F;
    Float3 out{c.x - offset, c.y - offset, c.z - offset};
    if (peak >= kStartCompression) {
        const float d = 1.0F - kStartCompression;
        const float newPeak = 1.0F - d * d / (peak + d - kStartCompression);
        const float scale = newPeak / peak;
        out = Float3{out.x * scale, out.y * scale, out.z * scale};
        const float g = 1.0F - 1.0F / (kDesaturation * (peak - newPeak) + 1.0F);
        out = Float3{out.x + (newPeak - out.x) * g, out.y + (newPeak - out.y) * g,
                     out.z + (newPeak - out.z) * g};
    }
    return out;
}
Float3 tonemap_color(Float3 c, TonemapOperator op) noexcept {
    switch (op) {
        case TonemapOperator::AgX: return agx_color(c);
        case TonemapOperator::PBRNeutral: return pbr_neutral_color(c);
        default:
            return {tonemap_curve(c.x, op), tonemap_curve(c.y, op), tonemap_curve(c.z, op)};
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

namespace {
// Optical depth of height-falloff fog along one ray: the integral of
// density x exp(-falloff x max(0, height - base)) over ray metres [from, to], where the
// height varies linearly as height0 + slope x m (both in metres). Closed form, in double:
// at or below the base height the density is the full density; above it the integrand is
// an exponential in m. The above-base integral is factored as
// exp(-falloff x h) x length x (1 - exp(-z)) / z with z = falloff x slope x length, which
// stays accurate as z -> 0 (the ratio tends to 1).
double fog_optical_depth(double from, double to, double height0, double slope, double density,
                         double falloff, double base) {
    if (!(to > from)) return 0.0;
    const double hFrom = height0 + slope * from - base;
    const double hTo = height0 + slope * to - base;
    if (hFrom <= 0.0 && hTo <= 0.0) return density * (to - from);
    const auto above = [&](double a, double b, double ha) {
        const double length = b - a;
        const double z = falloff * slope * length;
        const double ratio = z == 0.0 ? 1.0 : -std::expm1(-z) / z;
        return density * std::exp(-falloff * ha) * length * ratio;
    };
    if (hFrom >= 0.0 && hTo >= 0.0) return above(from, to, hFrom);
    const double cross = from - hFrom / slope;   // height == base here; slope != 0 (signs differ)
    if (slope > 0.0) return density * (cross - from) + above(cross, to, 0.0);
    return above(from, cross, hFrom) + density * (to - cross);
}
} // namespace

std::uint64_t apply_distance_fog(PolygonRenderTarget& target,const PolygonCamera& camera,const RenderEnvironment& environment,
                                 float metersPerUnit){
    const float density=environment.fogDensityPerMeter;
    if(!(density>0.0F)||!target.valid()||!(camera.farPlane>camera.nearPlane))return 0U;
    metersPerUnit=metersPerUnit>0.0F&&std::isfinite(metersPerUnit)?metersPerUnit:kDefaultMetersPerVoxel;
    const float tanHalf=std::tan(camera.verticalFieldOfViewRadians*0.5F);
    const float aspect=static_cast<float>(target.width)/static_cast<float>(target.height);
    const Float3 fogColor=environment.fogColor;
    // Height falloff (opt-in): the world height along each pixel's ray, from the camera basis.
    const bool heightFog=environment.fogHeightFalloffPerMeter>0.0F;
    Float3 basisF{},basisR{},basisU{};
    double cameraHeightMeters=0.0;
    if(heightFog){
        basisF=normalize(subtract(camera.target,camera.position));
        basisR=normalize(cross3(basisF,camera.up));
        basisU=cross3(basisR,basisF);
        cameraHeightMeters=static_cast<double>(camera.position.y)*metersPerUnit;
    }
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
            float t;
            if(!heightFog){
                t=std::exp(-density*(std::max)(0.0F,meters-environment.fogStartMeters));
            }else{
                const double invLength=1.0/std::sqrt(1.0+static_cast<double>(ndcX)*ndcX+static_cast<double>(ndcY)*ndcY);
                const double slope=(basisF.y+basisR.y*ndcX+basisU.y*ndcY)*invLength;   // d(height)/d(ray metre)
                const double tau=fog_optical_depth(environment.fogStartMeters,meters,cameraHeightMeters,slope,
                                                   density,environment.fogHeightFalloffPerMeter,environment.fogBaseHeightMeters);
                t=static_cast<float>(std::exp(-tau));
            }
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

PolygonHitSurface sample_polygon_hit_material(const CookedPolygonAsset& asset,const PolygonBvhHit& hit,
    const RigidTransform& pose,Float3 normal,const std::vector<PolygonTextureMipChain>& chains,
    float metersPerUnit,float footprintMeters,float viewDistanceMeters,const PolygonSurfaceDifferentials* differentials) noexcept {
    PolygonHitSurface out;out.normal=normalize(normal);
    if(hit.materialIndex>=asset.materials.size()||hit.materialIndex>=asset.materialBindings.size()||std::uint64_t(hit.firstIndex)+2>=asset.indices.size())return out;
    out.material=asset.materials[hit.materialIndex];auto& material=out.material;
    const auto& binding=asset.materialBindings[hit.materialIndex];
    const auto ia=asset.indices[hit.firstIndex],ib=asset.indices[hit.firstIndex+1],ic=asset.indices[hit.firstIndex+2];
    if(ia>=asset.vertices.size()||ib>=asset.vertices.size()||ic>=asset.vertices.size())return out;
    const auto& a=asset.vertices[ia];const auto& b=asset.vertices[ib];const auto& c=asset.vertices[ic];
    const auto e1=subtract(b.position,a.position),e2=subtract(c.position,a.position),q=subtract(hit.position,a.position);
    const float d00=dot(e1,e1),d01=dot(e1,e2),d11=dot(e2,e2),det=d00*d11-d01*d01;
    float w1=0,w2=0;if(std::abs(det)>1e-20F){w1=(d11*dot(q,e1)-d01*dot(q,e2))/det;w2=(d00*dot(q,e2)-d01*dot(q,e1))/det;}const float w0=1-w1-w2;
    const Float2 uv{a.texcoord.x*w0+b.texcoord.x*w1+c.texcoord.x*w2,a.texcoord.y*w0+b.texcoord.y*w1+c.texcoord.y*w2};
    const Float2 uv1{a.texcoord1.x*w0+b.texcoord1.x*w1+c.texcoord1.x*w2,a.texcoord1.y*w0+b.texcoord1.y*w1+c.texcoord1.y*w2};
    Float3 tangent=transform_vector(pose,{a.tangent.x*w0+b.tangent.x*w1+c.tangent.x*w2,a.tangent.y*w0+b.tangent.y*w1+c.tangent.y*w2,a.tangent.z*w0+b.tangent.z*w1+c.tangent.z*w2});
    tangent=subtract(tangent,multiply(out.normal,dot(out.normal,tangent)));
    if(length(tangent)<1e-6F)tangent=cross3(std::abs(out.normal.y)<.9F?Float3{0,1,0}:Float3{1,0,0},out.normal);
    tangent=normalize(tangent);const float handed=a.tangent.w*w0+b.tangent.w*w1+c.tangent.w*w2<0?-1.F:1.F;
    const auto bitangent=multiply(cross3(out.normal,tangent),handed);const auto world=transform_point(pose,hit.position);
    TextureLod lod;lod.gradients=true;
    const float units=metersPerUnit>0&&std::isfinite(metersPerUnit)?metersPerUnit:1;
    const float footprint=std::isfinite(footprintMeters)?(std::max)(0.F,footprintMeters)/units:0;
    // Retain a metric frame even for an infinitesimal ray cone. It defines mapped
    // normal orientation without inventing a texture/procedural filtering footprint.
    lod.gradients=footprint>0;lod.recipeFootprintScale=footprint>0?1.F:0.F;
    const auto frameWidth=footprint>0?footprint:1.F;
    lod.worldDx=multiply(tangent,frameWidth);lod.worldDy=multiply(bitangent,frameWidth);
    if(differentials){lod.worldDx=differentials->worldDx;lod.worldDy=differentials->worldDy;lod.gradients=true;lod.recipeFootprintScale=1;}
    lod.objectDx=inverse_transform_vector(pose,lod.worldDx);lod.objectDy=inverse_transform_vector(pose,lod.worldDy);
    const auto selectedA=binding.mapping.mappingMode==MaterialMappingMode::UV1?a.texcoord1:a.texcoord;
    const auto selectedB=binding.mapping.mappingMode==MaterialMappingMode::UV1?b.texcoord1:b.texcoord;
    const auto selectedC=binding.mapping.mappingMode==MaterialMappingMode::UV1?c.texcoord1:c.texcoord;
    if(std::abs(det)>1e-20F){
        const auto gradient=[&](float v1,float v2){return add(multiply(e1,(d11*v1-d01*v2)/det),multiply(e2,(d00*v2-d01*v1)/det));};
        const auto gu=gradient(selectedB.x-selectedA.x,selectedC.x-selectedA.x),gv=gradient(selectedB.y-selectedA.y,selectedC.y-selectedA.y);
        lod.dx={dot(gu,lod.objectDx),dot(gv,lod.objectDx)};lod.dy={dot(gu,lod.objectDy),dot(gv,lod.objectDy)};
    }
    lod.hasUvSets=true;
    if(std::abs(det)>1e-20F){
        const auto gradients=[&](Float2 ua,Float2 ub,Float2 uc,Float2& dx,Float2& dy){
            const auto gu=add(multiply(e1,(d11*(ub.x-ua.x)-d01*(uc.x-ua.x))/det),multiply(e2,(d00*(uc.x-ua.x)-d01*(ub.x-ua.x))/det));
            const auto gv=add(multiply(e1,(d11*(ub.y-ua.y)-d01*(uc.y-ua.y))/det),multiply(e2,(d00*(uc.y-ua.y)-d01*(ub.y-ua.y))/det));
            dx={dot(gu,lod.objectDx),dot(gv,lod.objectDx)};dy={dot(gu,lod.objectDy),dot(gv,lod.objectDy)};
        };
        gradients(a.texcoord,b.texcoord,c.texcoord,lod.uv0Dx,lod.uv0Dy);gradients(a.texcoord1,b.texcoord1,c.texcoord1,lod.uv1Dx,lod.uv1Dy);
    }
    const auto geometricNormal=out.normal;
    PolygonRenderStats stats;
    auto surface=sample_layer_surface(asset,hit.materialIndex,chains,uv,uv1,world,hit.position,out.normal,tangent,bitangent,lod,true,stats,pose,0);
    const Float4 vertex{a.color.x*w0+b.color.x*w1+c.color.x*w2,a.color.y*w0+b.color.y*w1+c.color.y*w2,a.color.z*w0+b.color.z*w1+c.color.z*w2,a.color.w*w0+b.color.w*w1+c.color.w*w2};
    surface.baseColor=mul4(surface.baseColor,vertex);surface.opacity*=vertex.w;
    const Float2 selected=binding.mapping.mappingMode==MaterialMappingMode::UV1?uv1:uv;
    const Float2 detailUv=transform_texture_coordinates(selected,binding.mapping.detailTransform);
    const float fade=material_detail_fade(viewDistanceMeters,binding.mapping);
    const auto map=[&](const PolygonTextureBinding& texture,const TextureTransform2D& transform,Float2 at,float multiplier,Float4 fallback,bool isNormal=false){return sample_mapped_binding(asset,texture,chains,binding.mapping,at,world,hit.position,out.normal,transform,multiplier,lod,stats,fallback,isNormal);};
    if(binding.detailBaseColor.texture)surface.baseColor=blend_detail_color(surface.baseColor,map(binding.detailBaseColor,binding.mapping.detailTransform,detailUv,8,{.5F,.5F,.5F,1}),binding.mapping.detailColorStrength*fade);
    if(binding.detailRoughness.texture)surface.roughness=blend_detail_roughness(surface.roughness,map(binding.detailRoughness,binding.mapping.detailTransform,detailUv,8,{.5F,.5F,.5F,1}).x,binding.mapping.detailRoughnessStrength*fade);
    out.normal=normalize(add(add(multiply(tangent,surface.normal.x),multiply(bitangent,surface.normal.y)),multiply(out.normal,surface.normal.z)));
    surface.normal=out.normal;
    if(binding.detailNormal.texture){const auto v=map(binding.detailNormal,binding.mapping.detailTransform,detailUv,8,{.5F,.5F,1,1},true);
        out.normal=apply_reoriented_tangent_material_normal(out.normal,tangent,bitangent,normalize(Float3{(2*v.x-1)*binding.detailNormalScale,(2*v.y-1)*binding.detailNormalScale,2*v.z-1}),binding.mapping.detailNormalStrength*fade);surface.normal=out.normal;}
    for(std::size_t index=0;index<material.layers.size();++index){const auto& authored=material.layers[index];
        if(!authored.enabled||authored.weight<=0||authored.sourceMaterial==kAirMaterial||authored.sourceMaterial>=asset.materials.size())continue;
        MaterialLayerSample layer;layer.surface=sample_layer_surface(asset,authored.sourceMaterial,chains,uv,uv1,world,hit.position,geometricNormal,tangent,bitangent,lod,true,stats,pose,units);
        layer.baseColorBlend=authored.blendMode;layer.authoredWeight=authored.weight;layer.mask=1;layer.heightBlendStrength=0;
        if(index<binding.layers.size()){const auto& l=binding.layers[index];if(!l.enabled)continue;layer.semantic=l.semantic;layer.opacityPolicy=l.opacityPolicy;layer.heightBlendStrength=l.heightBlendStrength;layer.heightBlendBias=l.heightBlendBias;layer.heightBlendTransition=l.heightBlendTransition;
            const auto at=transform_texture_coordinates(binding.mapping.mappingMode==MaterialMappingMode::UV1?uv1:select_uv(l.mask,uv,uv1),binding.mapping.baseTransform);
            layer.mask=std::clamp((l.mask.texture?sample_mapped_binding(asset,l.mask,chains,binding.mapping,at,world,hit.position,binding.mapping.mappingMode==MaterialMappingMode::ObjectTriplanar?inverse_transform_vector(pose,geometricNormal):geometricNormal,binding.mapping.baseTransform,1,lod.selected(binding.mapping.mappingMode==MaterialMappingMode::UV1||l.mask.texcoord==1),stats,{1,1,1,1}).x:1)*l.maskScale+l.maskBias,0.F,1.F);
            if(l.height.texture){const auto heightUv=transform_texture_coordinates(binding.mapping.mappingMode==MaterialMappingMode::UV1?uv1:select_uv(l.height,uv,uv1),binding.mapping.baseTransform);layer.surface.height=sample_mapped_binding(asset,l.height,chains,binding.mapping,heightUv,world,hit.position,binding.mapping.mappingMode==MaterialMappingMode::ObjectTriplanar?inverse_transform_vector(pose,geometricNormal):geometricNormal,binding.mapping.baseTransform,1,lod.selected(binding.mapping.mappingMode==MaterialMappingMode::UV1||l.height.texcoord==1),stats,{.5F,.5F,.5F,1}).x;}}
        const auto layerNormal=layer.surface.normal;layer.surface.normal={0,0,1};const float coverage=height_aware_material_layer_coverage(surface.height,layer);
        surface=blend_material_surface_layer(surface,layer);
        const auto combined=blend_reoriented_material_normals({dot(out.normal,tangent),dot(out.normal,bitangent),dot(out.normal,geometricNormal)},layerNormal,coverage);
        out.normal=normalize(add(add(multiply(tangent,combined.x),multiply(bitangent,combined.y)),multiply(geometricNormal,combined.z)));surface.normal=out.normal;
    }
    material.baseColor=surface.baseColor;material.baseColor.w=surface.opacity;material.roughness=surface.roughness;material.metallic=surface.metallic;material.emissive=surface.emissive;
    if(material.normalMap)out.normal=normalize(transform_vector(pose,voxel_normal_map_normal(material,multiply(hit.position,units),inverse_transform_vector(pose,out.normal))));
    float recipeFootprint=differentials?(std::max)(length(lod.worldDx),length(lod.worldDy))*units:(std::max)(0.F,footprintMeters);
    if(material.surfaceDetail.space==SurfaceDetailSpace::UV0||material.surfaceDetail.space==SurfaceDetailSpace::UV1){
        recipeFootprint=0;
        if((footprint>0||differentials)&&std::abs(det)>1e-20F){
            const bool second=material.surfaceDetail.space==SurfaceDetailSpace::UV1;
            const auto ua=second?a.texcoord1:a.texcoord,ub=second?b.texcoord1:b.texcoord,uc=second?c.texcoord1:c.texcoord;
            const auto gradient=[&](float v1,float v2){return add(multiply(e1,(d11*v1-d01*v2)/det),multiply(e2,(d00*v2-d01*v1)/det));};
            const auto gu=gradient(ub.x-ua.x,uc.x-ua.x),gv=gradient(ub.y-ua.y,uc.y-ua.y);
            const auto width=[&](Float3 d){return std::hypot(dot(gu,d),dot(gv,d));};
            recipeFootprint=(std::max)(width(lod.objectDx),width(lod.objectDy))*material.surfaceDetail.uvTileMeters;
        }
    }
    apply_material_surface_detail(material,out.normal,{multiply(hit.position,units),multiply(world,units),uv,uv1,true,pose,tangent,bitangent,recipeFootprint});
    return out;
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
    if (material.layers.empty()) return opacity;
    // Material layers composite into the opacity before the rasterizer's alpha test (Lerp,
    // Multiply, Maximum or Replace opacity policies), so the shadow test applies them too.
    const Float3 n = normalize(worldNormal);
    const Float3 helper = std::abs(n.y) < 0.9F ? Float3{0.0F, 1.0F, 0.0F} : Float3{1.0F, 0.0F, 0.0F};
    const Float3 layerTangent = normalize(cross3(helper, n));
    const Float3 layerBitangent = cross3(n, layerTangent);
    MaterialSurfaceSample surface;
    surface.baseColor = {material.baseColor.x * texture.x, material.baseColor.y * texture.y, material.baseColor.z * texture.z, opacity};
    surface.normal = n;
    surface.metallic = material.metallic;
    surface.roughness = material.roughness;
    surface.emissive = material.emissive;
    surface.opacity = opacity;
    surface.height = binding.height.texture
        ? sample_mapped_binding(asset, binding.height, mips, binding.mapping, mapped, worldPosition, hit.position, n,
                                binding.mapping.baseTransform, 1.0F, 0.0F, ignored, {0.5F, 0.5F, 0.5F, 1.0F}).x
        : 0.5F;
    const std::size_t layerCount = std::min<std::size_t>(material.layers.size(), kMaximumVoxelMaterialLayers);
    for (std::size_t layerIndex = 0U; layerIndex < layerCount; ++layerIndex) {
        const VoxelMaterialLayer& authored = material.layers[layerIndex];
        if (!authored.enabled || authored.weight <= 0.0F || authored.sourceMaterial == kAirMaterial ||
            authored.sourceMaterial >= asset.materials.size()) continue;
        MaterialLayerSample layer;
        layer.surface = sample_layer_surface(asset, authored.sourceMaterial, mips, uv0, uv1, worldPosition, hit.position, n,
                                             layerTangent, layerBitangent, 0.0F, true, ignored);
        layer.baseColorBlend = authored.blendMode;
        layer.authoredWeight = std::clamp(authored.weight, 0.0F, 1.0F);
        layer.mask = 1.0F;
        layer.heightBlendStrength = 0.0F;
        if (layerIndex < binding.layers.size()) {
            const PolygonMaterialLayerBinding& layerBinding = binding.layers[layerIndex];
            if (!layerBinding.enabled) continue;
            layer.semantic = layerBinding.semantic;
            layer.opacityPolicy = layerBinding.opacityPolicy;
            layer.heightBlendStrength = layerBinding.heightBlendStrength;
            layer.heightBlendBias = layerBinding.heightBlendBias;
            layer.heightBlendTransition = layerBinding.heightBlendTransition;
            if (layerBinding.mask.texture) {
                const Float2 maskUv = transform_texture_coordinates(select_uv(layerBinding.mask, uv0, uv1), binding.mapping.baseTransform);
                const Float4 mask = sample_mapped_binding(asset, layerBinding.mask, mips, binding.mapping, maskUv, worldPosition,
                                                          hit.position, n, binding.mapping.baseTransform, 1.0F, 0.0F, ignored, {1, 1, 1, 1});
                layer.mask = std::clamp(mask.x * layerBinding.maskScale + layerBinding.maskBias, 0.0F, 1.0F);
            } else {
                layer.mask = std::clamp(layerBinding.maskScale + layerBinding.maskBias, 0.0F, 1.0F);
            }
            if (layerBinding.height.texture) {
                const Float2 heightUv = transform_texture_coordinates(select_uv(layerBinding.height, uv0, uv1), binding.mapping.baseTransform);
                layer.surface.height = sample_mapped_binding(asset, layerBinding.height, mips, binding.mapping, heightUv, worldPosition,
                                                             hit.position, n, binding.mapping.baseTransform, 1.0F, 0.0F, ignored,
                                                             {0.5F, 0.5F, 0.5F, 1.0F}).x;
            }
        }
        layer.surface.normal = {0.0F, 0.0F, 1.0F};
        float ignoredCoverage{};
        surface = blend_material_surface_layer(surface, layer, &ignoredCoverage);
        surface.normal = n;
    }
    return surface.opacity;
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
            auto& chain=chains->back();
            const auto imageIndex=static_cast<std::size_t>(&image-asset.images.data());
            bool normalUse=false,srgbUse=false;
            const auto uses=[&](const PolygonTextureBinding& binding){return binding.texture&&*binding.texture<asset.textures.size()&&asset.textures[*binding.texture].imageIndex==imageIndex;};
            for(const auto& b:asset.materialBindings){
                normalUse=normalUse||uses(b.normal)||uses(b.detailNormal);
                for(const auto* slot:{&b.baseColor,&b.emissive,&b.detailBaseColor})srgbUse=srgbUse||(uses(*slot)&&slot->colorSpace==PolygonTextureColorSpace::Srgb);
            }
            // Variant level sets live inside the image's one chain; mipChainsBuilt counts
            // chains (one per image per cache miss), not variants.
            if(normalUse){chain.normalLevels=generate_texture_mips(image,true).levels;for(const auto& m:chain.normalLevels)bytes+=m.rgba8.size();}
            if(srgbUse){chain.srgbLevels=generate_texture_mips(image,false,true).levels;for(const auto& m:chain.srgbLevels)bytes+=m.rgba8.size();}
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
    std::unique_ptr<SceneLighting> ownedLighting;
    if (!frameOptions.sceneLighting &&
        (!environment.lights.empty() || environment.pathTraceBounces > 0 || scene_uses_camera_transport({},instances))) {
      ownedLighting = std::make_unique<SceneLighting>(
          std::span<const VoxelReferenceInstance>{}, instances, environment,
          options.metersPerUnit);
      frameOptions.sceneLighting = ownedLighting.get();
    }
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
                    camera.nearPlane,camera.farPlane,
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
        const Float3 mapped = tonemap_color({c.x * exposure, c.y * exposure, c.z * exposure},
                                            tonemapOperator);
        out[0] = srgb(mapped.x);
        out[1] = srgb(mapped.y);
        out[2] = srgb(mapped.z);
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
