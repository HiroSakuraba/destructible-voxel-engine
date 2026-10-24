#include "dve/editor_material_inspection.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/transform.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>
namespace dve::editor {
const char* material_inspection_channel_label(MaterialInspectionChannel c) noexcept {
    constexpr const char* labels[]{"Beauty","Base color","World normal","Roughness","Metallic","Emission (LDR)","Opacity","UV coverage / padding"};
    return unsigned(c)<8?labels[unsigned(c)]:"Invalid channel";
}
MaterialPreviewImage render_material_inspection(const CookedPolygonAsset& source,const CookedPolygonAsset& baked,
    const render::MaterialTextureBakeSettings& settings,MaterialInspectionChannel channel,MaterialInspectionView view,const std::function<bool()>& cancelled){
    MaterialPreviewImage out;out.rgb.resize(out.size*out.size);
    if(unsigned(channel)>7||unsigned(view)>2){out.error="Invalid inspection mode";return out;}
#if !defined(DVE_EDITOR_MATERIAL_PREVIEW_RENDERER)
    (void)source;(void)baked;(void)settings;(void)cancelled;out.error="Inspection requires a renderer-enabled editor build";return out;
#else
    if(settings.targetUv>1||settings.resolution<8||settings.resolution>4096||settings.paddingPixels>64){out.error="Invalid inspection recipe";return out;}
    const auto q=settings.pose.rotation;const float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    if(!std::isfinite(norm)||std::abs(norm-1)>.001F||!std::isfinite(settings.pose.position.x)||!std::isfinite(settings.pose.position.y)||!std::isfinite(settings.pose.position.z)||!std::isfinite(settings.metersPerUnit)||settings.metersPerUnit<=0){out.error="Invalid inspection pose or scale";return out;}
    if(source.indices.size()>300000||baked.indices.size()>300000){out.error="Inspection exceeds 100,000 triangles";return out;}
    if(!validate_polygon_asset(source)||!validate_polygon_asset(baked)||source.indices.empty()){out.error="Invalid inspection snapshots";return out;}
    auto stopped=[&]{return cancelled&&cancelled();};
    const auto center=multiply(add(source.bounds.minimum,source.bounds.maximum),.5F);const float radius=(std::max)(length(subtract(source.bounds.maximum,source.bounds.minimum))*.5F,.001F);
    auto normal=normalize(source.vertices[source.indices[0]].normal);auto axis=std::abs(normal.y)<.9F?Float3{0,1,0}:Float3{0,0,1};
    render::PolygonCamera camera;camera.target=transform_point(settings.pose,center);camera.position=transform_point(settings.pose,add(center,multiply(normal,radius*2.5F)));camera.up=transform_vector(settings.pose,axis);camera.nearPlane=radius*.01F;camera.farPlane=radius*10;
    auto one=[&](const CookedPolygonAsset& asset,unsigned slot){
        std::vector<std::uint32_t> rgb(out.size*out.size,0x172133);
        if(channel==MaterialInspectionChannel::UvCoverage){
            std::vector<unsigned> counts(rgb.size());std::vector<unsigned> distance(rgb.size(),~0U),queue;
            for(const auto& sub:asset.submeshes)if(sub.materialIndex==slot)for(unsigned first=sub.firstIndex;first<sub.firstIndex+sub.indexCount;first+=3){
                if(stopped())return rgb;
                std::array<Float2,3> uv;bool finite=true;for(unsigned n=0;n<3;++n){const auto& v=asset.vertices[asset.indices[first+n]];uv[n]=settings.targetUv?v.texcoord1:v.texcoord;finite=finite&&std::isfinite(uv[n].x)&&std::isfinite(uv[n].y);if(!finite)break;uv[n].x=std::clamp(uv[n].x,-1.F,2.F)*float(out.size);uv[n].y=std::clamp(uv[n].y,-1.F,2.F)*float(out.size);}
                if(!finite)continue; // NaN/Inf UVs would make the float-to-int bounds below undefined
                auto edge=[](Float2 a,Float2 b,Float2 p){return (b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x);};
                if(edge(uv[0],uv[1],uv[2])<0)std::swap(uv[1],uv[2]);
                if(edge(uv[0],uv[1],uv[2])<1e-8F)continue;
                const int left=(std::max)(0,int(std::floor((std::min)({uv[0].x,uv[1].x,uv[2].x})))),right=(std::min)(int(out.size)-1,int(std::ceil((std::max)({uv[0].x,uv[1].x,uv[2].x}))));
                const int top=(std::max)(0,int(std::floor((std::min)({uv[0].y,uv[1].y,uv[2].y})))),bottom=(std::min)(int(out.size)-1,int(std::ceil((std::max)({uv[0].y,uv[1].y,uv[2].y}))));
                for(int y=top;y<=bottom;++y)for(int x=left;x<=right;++x){bool inside=true;for(unsigned n=0;n<3;++n){auto a=uv[n],b=uv[(n+1)%3];auto e=edge(a,b,{float(x)+.5F,float(y)+.5F});const bool include=b.y<a.y||(b.y==a.y&&b.x>a.x);if(e<0||(e==0&&!include)){inside=false;break;}}if(inside)++counts[unsigned(y)*out.size+unsigned(x)];}
            }
            const unsigned padding=unsigned(std::ceil(float(settings.paddingPixels)*float(out.size)/float(settings.resolution)));
            for(unsigned i=0;i<rgb.size();++i)if(counts[i]){rgb[i]=counts[i]>1?0xff5555:0x66cddd;distance[i]=0;queue.push_back(i);}
            for(std::size_t n=0;n<queue.size();++n){auto i=queue[n];if(distance[i]>=padding)continue;const int x=int(i%out.size),y=int(i/out.size);for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const int xx=x+dx,yy=y+dy;if(xx<0||yy<0||xx>=int(out.size)||yy>=int(out.size))continue;auto at=unsigned(yy)*out.size+unsigned(xx);if(distance[at]!=~0U)continue;distance[at]=distance[i]+1;rgb[at]=0x56667b;queue.push_back(at);}}
            return rgb;
        }
        render::PolygonRenderTarget target;target.resize(out.size,out.size);target.clear();
        render::PolygonRenderOptions options;options.preserveExistingDepth=false;options.metersPerUnit=settings.metersPerUnit;
        constexpr render::PolygonMaterialDebugView views[]{render::PolygonMaterialDebugView::Lit,render::PolygonMaterialDebugView::BaseColor,render::PolygonMaterialDebugView::WorldNormal,render::PolygonMaterialDebugView::Roughness,render::PolygonMaterialDebugView::Metallic,render::PolygonMaterialDebugView::Emissive,render::PolygonMaterialDebugView::Opacity};options.materialDebugView=views[unsigned(channel)];
        render::PolygonRenderInstance instance{1,&asset,settings.pose};render::ReferencePolygonRenderer renderer;RenderEnvironment env;env.sunIntensity=2;env.sunDirection=transform_vector(settings.pose,normalize(add(normal,multiply(axis,.4F))));
        (void)renderer.render(std::span(&instance,1),camera,env,target,options);
        auto encode=[&](float value){value=(std::max)(0.F,value);if(channel==MaterialInspectionChannel::Beauty)value=value/(1+value);if(channel==MaterialInspectionChannel::Beauty||channel==MaterialInspectionChannel::BaseColor||channel==MaterialInspectionChannel::Emission)value=value<=.0031308F?12.92F*value:1.055F*std::pow(value,1/2.4F)-.055F;return unsigned(std::lround(std::clamp(value,0.F,1.F)*255));};
        for(unsigned y=0;y<out.size;++y){if(stopped())return rgb;for(unsigned x=0;x<out.size;++x){auto i=y*out.size+x;auto c=target.hdrColor[i];rgb[i]=target.objectId[i]?(encode(c.x)<<16)|(encode(c.y)<<8)|encode(c.z):(((x/8+y/8)%2)?0x293447:0x172133);}}
        return rgb;
    };
    if(stopped()){out.error="Inspection cancelled";return out;}
    auto left=one(view==MaterialInspectionView::Baked?baked:source,view==MaterialInspectionView::Baked?unsigned(baked.materials.size()-1):settings.materialIndex);
    if(view==MaterialInspectionView::Split){auto right=one(baked,unsigned(baked.materials.size()-1));for(unsigned y=0;y<out.size;++y)for(unsigned x=out.size/2;x<out.size;++x)left[y*out.size+x]=right[y*out.size+x];for(unsigned y=0;y<out.size;++y)left[y*out.size+out.size/2]=0xffffff;}
    out.rgb=std::move(left);if(stopped())out.error="Inspection cancelled";return out;
#endif
}
}
