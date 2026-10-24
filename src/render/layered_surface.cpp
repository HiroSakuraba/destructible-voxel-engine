#include "dve/render/layered_surface.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/gpu_material_buffer.hpp"
#include "dve/gpu_material_mapping.hpp"
#include "dve/gpu_material_surface_detail.hpp"
#include <bit>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace dve::render {
static_assert(sizeof(GpuMaterialRecord)==kLayerMaterialWords*4);
static_assert(sizeof(GpuPolygonMaterialMappingRecord)==kLayerMappingWords*4);
static_assert(sizeof(GpuMaterialSurfaceDetail)==kLayerRecipeWords*4);
namespace {
bool fail(std::string* e,const char* s){if(e)*e=s;return false;}
template<class T> std::uint32_t append(std::vector<std::uint32_t>& w,const T& t){
    static_assert(sizeof(T)%4==0);const auto at=std::uint32_t(w.size());
    w.resize(w.size()+sizeof(T)/4);std::memcpy(w.data()+at,&t,sizeof(T));return at;
}
}
std::optional<LayeredSurfacePayload> build_layered_surface_payload(const CookedPolygonAsset& a,std::uint32_t index,std::string* e){
    if(!validate_layered_surface(a,index,e))return {};
    LayeredSurfacePayload result;auto& w=result.words;
    const auto& material=a.materials[index];if(material.layers.empty())return result;
    const auto& binding=a.materialBindings[index];
    w[1]=append(w,pack_gpu_polygon_material_mapping(binding));
    using Key=std::tuple<unsigned,bool,bool,unsigned,unsigned>;
    std::map<Key,std::uint32_t> maps;
    std::map<std::tuple<unsigned,bool,bool>,std::uint32_t> imageData;
    bool valid=true;
    auto map=[&](const PolygonTextureBinding& b,bool normal=false){
        if(!b.texture)return 0U;
        if(*b.texture>=a.textures.size()||b.texcoord>1){valid=fail(e,"Invalid layer texture binding");return 0U;}
        const auto& t=a.textures[*b.texture];
        if(t.imageIndex>=a.images.size()||(t.samplerIndex&&*t.samplerIndex>=a.samplers.size())){valid=fail(e,"Invalid layer image or sampler");return 0U;}
        const bool srgb=!normal&&b.colorSpace==PolygonTextureColorSpace::Srgb;
        const Key key{t.imageIndex,normal,srgb,t.samplerIndex.value_or(~0U),b.texcoord};
        if(auto it=maps.find(key);it!=maps.end())return it->second;
        const auto dataKey=std::tuple{t.imageIndex,normal,srgb};
        const PolygonSampler sampler=t.samplerIndex?a.samplers[*t.samplerIndex]:PolygonSampler{};
        if(auto it=imageData.find(dataKey);it!=imageData.end()){
            // Coordinate/sampler metadata may differ; the typed image texels do not.
            const auto begin=it->second,count=9+w[begin]*3;
            const std::vector<std::uint32_t> metadata(w.begin()+begin,w.begin()+begin+count);
            const auto offset=std::uint32_t(w.size());w.insert(w.end(),metadata.begin(),metadata.end());
            w[offset+1]=unsigned(sampler.wrapS);w[offset+2]=unsigned(sampler.wrapT);
            w[offset+3]=unsigned(sampler.minFilter);w[offset+4]=unsigned(sampler.magFilter);w[offset+6]=b.texcoord;
            maps.emplace(key,offset);return offset;
        }
        const auto& image=a.images[t.imageIndex];
        std::size_t bytes=36,levels=0;
        for(auto width=image.width,height=image.height;width&&height;){
            const auto pixels=std::uint64_t(width)*height;
            if(pixels>kMaximumLayerPayloadBytes/4||bytes>kMaximumLayerPayloadBytes-pixels*4){valid=fail(e,"Layer image payload exceeds 128 MiB budget");return 0U;}
            bytes+=std::size_t(pixels)*4;++levels;
            if(width==1&&height==1)break;
            width=(std::max)(1U,width/2);height=(std::max)(1U,height/2);
        }
        bytes+=levels*12;
        if(bytes>kMaximumLayerPayloadBytes||w.size()*4>kMaximumLayerPayloadBytes-bytes){valid=fail(e,"Layer image payload exceeds 128 MiB budget");return 0U;}
        auto chain=generate_texture_mips(image,normal,srgb);
        if(chain.levels.empty()){valid=fail(e,"Layer images must contain decoded RGBA8 pixels");return 0U;}
        const auto offset=std::uint32_t(w.size());
        w.insert(w.end(),{std::uint32_t(chain.levels.size()),unsigned(sampler.wrapS),unsigned(sampler.wrapT),unsigned(sampler.minFilter),unsigned(sampler.magFilter),unsigned(srgb),b.texcoord,image.width,image.height});
        w.resize(w.size()+chain.levels.size()*3);
        for(std::size_t level=0;level<chain.levels.size();++level){const auto& mip=chain.levels[level];
            const auto table=offset+9+level*3;w[table]=std::uint32_t(w.size());w[table+1]=mip.width;w[table+2]=mip.height;
            for(std::size_t p=0;p<mip.rgba8.size();p+=4)w.push_back(unsigned(mip.rgba8[p])|(unsigned(mip.rgba8[p+1])<<8U)|(unsigned(mip.rgba8[p+2])<<16U)|(unsigned(mip.rgba8[p+3])<<24U));
        }
        maps.emplace(key,offset);imageData.emplace(dataKey,offset);return offset;
    };
    // Evaluate the RHS before indexing: appending may reallocate the vector.
    const auto baseHeight=map(binding.height);w[2]=baseHeight;
    for(std::size_t i=0;i<material.layers.size();++i){
        const auto& authored=material.layers[i];
        const PolygonMaterialLayerBinding* lb=i<binding.layers.size()?&binding.layers[i]:nullptr;
        if(!authored.enabled||authored.weight<=0||(lb&&!lb->enabled))continue;
        const auto& source=a.materials[authored.sourceMaterial];const auto& sb=a.materialBindings[authored.sourceMaterial];
        const auto at=append(w,pack_gpu_material_record(source));w[4+w[0]++]=at;
        append(w,pack_gpu_polygon_material_mapping(sb));append(w,pack_gpu_material_surface_detail(source.surfaceDetail));
        const auto controls=w.size();w.resize(w.size()+16);
        w[controls]=std::bit_cast<std::uint32_t>(authored.weight);w[controls+1]=unsigned(authored.blendMode);
        w[controls+2]=lb?unsigned(lb->opacityPolicy):0U;
        const float values[]{lb?lb->maskScale:1,lb?lb->maskBias:0,lb?lb->heightBlendStrength:0,lb?lb->heightBlendBias:0,lb?lb->heightBlendTransition:.1F};
        for(unsigned n=0;n<5;++n)w[controls+3+n]=std::bit_cast<std::uint32_t>(values[n]);
        const PolygonTextureBinding empty;
        const PolygonTextureBinding* channels[]{lb?&lb->mask:&empty,lb?&lb->height:&empty,&sb.baseColor,&sb.metallicRoughness,&sb.normal,&sb.emissive,&sb.opacity,&sb.height};
        for(unsigned n=0;n<8;++n){const auto value=map(*channels[n],n==4);w[controls+8+n]=value;}
    }
    if(!valid||w.size()*4>kMaximumLayerPayloadBytes)return {};
    return result;
}
}
