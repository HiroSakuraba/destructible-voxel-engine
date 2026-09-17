#include "dve/render/polygon_renderer.hpp"

#include <algorithm>
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

Float4 shade(const VoxelMaterialDefinition& material,Float4 base,Float3 normal,
             const RenderEnvironment& environment,bool frontFacing) noexcept {
    normal=normalize(normal);
    if(material.shadingModel==MaterialShadingModel::TwoSidedFoliage&&!frontFacing)normal=multiply(normal,-1.0F);
    const Float3 light=normalize(environment.sunDirection);
    const float ndotl=dot(normal,light);
    const float direct=std::max(0.0F,ndotl)*environment.sunIntensity;
    Float3 ambient{};
    if (environment.globalIlluminationMode != GlobalIlluminationMode::Off) {
        // The CPU polygon oracle has no scene acceleration structure for secondary rays, so
        // VoxelOneBounce intentionally falls back to the shared hemisphere term here. The
        // production voxel path and hybrid shader contract perform the bounded scene trace.
        ambient={environment.skyColor.x+environment.groundColor.x,
                 environment.skyColor.y+environment.groundColor.y,
                 environment.skyColor.z+environment.groundColor.z};
        ambient=multiply(ambient,0.5F*environment.globalIlluminationIntensity);
    }
    Float3 rgb={base.x,base.y,base.z};
    if(material.shadingModel==MaterialShadingModel::Unlit)return {rgb.x,rgb.y,rgb.z,base.w};
    Float3 lit=mul3(rgb,add(ambient,multiply(environment.sunColor,direct)));
    if(material.shadingModel==MaterialShadingModel::TwoSidedFoliage){
        const float wrap=std::clamp(((-ndotl)+material.foliageWrap)/(1.0F+material.foliageWrap),0.0F,1.0F);
        lit=add(lit,multiply(mul3(material.foliageColor,environment.sunColor),wrap*material.foliageTransmittance*environment.sunIntensity));
    }
    if(material.shadingModel==MaterialShadingModel::ClearCoat&&material.clearCoat>0.0F){
        const Float3 view={0,0,1};const Float3 halfVector=normalize(add(light,view));
        const float highlight=std::pow(std::max(0.0F,dot(normal,halfVector)),std::max(2.0F,256.0F*(1.0F-material.clearCoatRoughness)));
        lit=add(lit,multiply(environment.sunColor,highlight*material.clearCoat));
    }
    lit=add(lit,material.emissive);
    if(material.shadingModel==MaterialShadingModel::Emissive)lit=add(rgb,material.emissive);
    return {lit.x,lit.y,lit.z,base.w};
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
};

float projected_diameter(const CookedPolygonAsset& asset,const RigidTransform& transform,
                         const PolygonCamera& camera,const CameraBasis& basis,
                         const PolygonRenderTarget& target) noexcept {
    const Float3 center=multiply(add(asset.bounds.minimum,asset.bounds.maximum),0.5F);
    const Float3 half=multiply(subtract(asset.bounds.maximum,asset.bounds.minimum),0.5F);
    const float radius=length(half);const Float3 world=transform_point(transform,center);
    const float z=dot(subtract(world,camera.position),basis.forward);
    if(z<=camera.nearPlane)return std::numeric_limits<float>::infinity();
    return 2.0F*radius/(z*basis.tanHalf)*static_cast<float>(target.height)*0.5F;
}

const CookedPolygonAsset* select_lod(const PolygonRenderInstance& instance,float diameter,PolygonRenderStats& stats) noexcept {
    const CookedPolygonAsset* selected=instance.asset;float best=-1.0F;
    for(const PolygonLodLevel& lod:instance.lods){
        if(lod.asset&&diameter>=lod.minimumProjectedDiameterPixels&&lod.minimumProjectedDiameterPixels>=best){selected=lod.asset;best=lod.minimumProjectedDiameterPixels;}
    }
    if (selected != instance.asset) {
        ++stats.lodSelections;
    }
    return selected;
}

void raster_triangle(const DeferredTriangle& tri,PolygonRenderTarget& target,const RenderEnvironment& environment,
                     const PolygonRenderOptions& options,bool transparent,PolygonRenderStats& stats) {
    const float area=edge(tri.v[0].x,tri.v[0].y,tri.v[1].x,tri.v[1].y,tri.v[2].x,tri.v[2].y);
    if(std::abs(area)<kEpsilon)return;
    const int minX=std::max(0,static_cast<int>(std::floor(std::min({tri.v[0].x,tri.v[1].x,tri.v[2].x}))));
    const int maxX=std::min(static_cast<int>(target.width)-1,static_cast<int>(std::ceil(std::max({tri.v[0].x,tri.v[1].x,tri.v[2].x}))));
    const int minY=std::max(0,static_cast<int>(std::floor(std::min({tri.v[0].y,tri.v[1].y,tri.v[2].y}))));
    const int maxY=std::min(static_cast<int>(target.height)-1,static_cast<int>(std::ceil(std::max({tri.v[0].y,tri.v[1].y,tri.v[2].y}))));
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
        const Float4 shaded = debugView
            ? material_debug_output(options.materialDebugView, base, normal, effective, uv, uv1,
                                    worldPosition, objectPosition, tri.binding->mapping, detailFade,
                                    fragmentParallaxSamples, layerDebugMask, layerDebugCoverage)
            : shade(effective,base,normal,environment,tri.frontFacing);
        if(!debugView&&(transparent||tri.material->blendMode==MaterialBlendMode::Translucent||base.w<0.999F)){
            const float alpha=std::clamp(shaded.w,0.0F,1.0F);const Float4 old=target.hdrColor[index];
            target.hdrColor[index]={shaded.x*alpha+old.x*(1-alpha),shaded.y*alpha+old.y*(1-alpha),shaded.z*alpha+old.z*(1-alpha),1};
            ++stats.transparentFragments;
        }else{
            target.hdrColor[index]=shaded;target.depth[index]=depth;
            if(options.writeObjectIds){target.objectId[index]=tri.objectId;target.materialIndex[index]=tri.submesh->materialIndex;}
        }
        ++stats.shadedFragments;
    }
}

float aces(float x) noexcept {const float a=2.51F,b=0.03F,c=2.43F,d=0.59F,e=0.14F;return std::clamp((x*(a*x+b))/(x*(c*x+d)+e),0.0F,1.0F);}
std::uint8_t srgb(float linear) noexcept {linear=std::clamp(linear,0.0F,1.0F);const float encoded=linear<=0.0031308F?12.92F*linear:1.055F*std::pow(linear,1.0F/2.4F)-0.055F;return static_cast<std::uint8_t>(std::clamp(encoded*255.0F+0.5F,0.0F,255.0F));}
}

void PolygonRenderTarget::resize(std::uint32_t newWidth,std::uint32_t newHeight){
    width=newWidth;height=newHeight;const std::size_t count=static_cast<std::size_t>(width)*height;
    hdrColor.assign(count,{});depth.assign(count,1.0F);objectId.assign(count,0U);materialIndex.assign(count,0U);
}
void PolygonRenderTarget::clear(Float4 color,float depthValue){std::fill(hdrColor.begin(),hdrColor.end(),color);std::fill(depth.begin(),depth.end(),depthValue);std::fill(objectId.begin(),objectId.end(),0U);std::fill(materialIndex.begin(),materialIndex.end(),0U);}
bool PolygonRenderTarget::valid() const noexcept{const std::size_t count=static_cast<std::size_t>(width)*height;return width>0&&height>0&&hdrColor.size()==count&&depth.size()==count&&objectId.size()==count&&materialIndex.size()==count;}

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
    std::vector<DeferredTriangle> transparent;
    for (const PolygonRenderInstance& instance : instances) {
        ++stats.submittedInstances;
        if (!instance.visible || instance.asset == nullptr ||
            !validate_polygon_asset(*instance.asset)) {
            ++stats.culledInstances;
            continue;
        }

        const float diameter =
            projected_diameter(*instance.asset, instance.transform, camera, basis, target);
        const CookedPolygonAsset* asset = select_lod(instance, diameter, stats);
        if (asset == nullptr || diameter < 0.5F) {
            ++stats.culledInstances;
            continue;
        }

        auto mips = std::make_shared<std::vector<PolygonTextureMipChain>>();
        mips->reserve(asset->images.size());
        for (const auto& image : asset->images) {
            mips->push_back(generate_texture_mips(image));
        }

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
                    mips};
                if (material.blendMode == MaterialBlendMode::Translucent ||
                    material.baseColor.w < 0.999F) {
                    transparent.push_back(std::move(triangle));
                } else {
                    raster_triangle(triangle, target, environment, options, false, stats);
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
        raster_triangle(triangle, target, environment, options, true, stats);
    }
    return stats;
}

bool composite_hybrid_layers(const PolygonRenderTarget& voxelLayer,const PolygonRenderTarget& polygonLayer,PolygonRenderTarget& output,std::string* error){if(!voxelLayer.valid()||!polygonLayer.valid()||voxelLayer.width!=polygonLayer.width||voxelLayer.height!=polygonLayer.height){if(error)*error="hybrid layers must be valid and have identical dimensions";return false;}output.resize(voxelLayer.width,voxelLayer.height);for(std::size_t i=0;i<output.depth.size();++i){const bool polygon=polygonLayer.depth[i]<voxelLayer.depth[i];output.depth[i]=polygon?polygonLayer.depth[i]:voxelLayer.depth[i];output.hdrColor[i]=polygon?polygonLayer.hdrColor[i]:voxelLayer.hdrColor[i];output.objectId[i]=polygon?polygonLayer.objectId[i]:voxelLayer.objectId[i];output.materialIndex[i]=polygon?polygonLayer.materialIndex[i]:voxelLayer.materialIndex[i];}return true;}

bool write_polygon_render_ppm(const std::filesystem::path& path,const PolygonRenderTarget& target,float exposure,std::string* error){if(!target.valid()||!(exposure>0)||!std::isfinite(exposure)){if(error)*error="invalid render target or exposure";return false;}std::ofstream out(path,std::ios::binary|std::ios::trunc);if(!out){if(error)*error="failed to open PPM output";return false;}out<<"P6\n"<<target.width<<' '<<target.height<<"\n255\n";for(const Float4& c:target.hdrColor){const std::array<char,3> pixel{static_cast<char>(srgb(aces(c.x*exposure))),static_cast<char>(srgb(aces(c.y*exposure))),static_cast<char>(srgb(aces(c.z*exposure)))};out.write(pixel.data(),3);}if(!out){if(error)*error="failed to write PPM output";return false;}return true;}

} // namespace dve::render
