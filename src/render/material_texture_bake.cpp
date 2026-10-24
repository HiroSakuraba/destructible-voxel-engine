#include "dve/render/material_texture_bake.hpp"
#include "dve/render/layered_surface.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/polygon_bvh.hpp"
#include "dve/rgba8_png.hpp"
#include "dve/transform.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <tuple>
#include <cerrno>
#if defined(__linux__)
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif

namespace dve::render {
namespace {
struct Point { double x{},y{}; };
struct Triangle {
    std::array<Point,3> uv;
    std::uint32_t first{},left{},right{},top{},bottom{};
};
Float3 cross(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
double edge(Point a,Point b,Point p){return (b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x);}
Float2 uv(const PolygonVertex& v,unsigned set){return set==1?v.texcoord1:v.texcoord;}
bool finite(Float3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
bool selected(const MaterialTextureBakeSettings& s,MaterialBakeMap m){return (s.maps&unsigned(m))!=0;}
std::string hex(std::uint64_t h){std::ostringstream o;o<<std::hex<<std::setfill('0')<<std::setw(16)<<h;return o.str();}
void hash_word(std::uint64_t& h,std::uint64_t value){for(unsigned n=0;n<8;++n){h^=(value>>(n*8))&255U;h*=1099511628211ULL;}}
std::uint64_t recipe_key(std::uint64_t source,const MaterialTextureBakeSettings& s){
    std::uint64_t h=14695981039346656037ULL;hash_word(h,1);hash_word(h,source);
    for(auto word:{s.materialIndex,s.targetUv,s.resolution,s.samplesPerAxis,s.paddingPixels,s.maps})hash_word(h,word);
    const float values[]{s.metersPerUnit,s.pose.position.x,s.pose.position.y,s.pose.position.z,
        s.pose.rotation.x,s.pose.rotation.y,s.pose.rotation.z,s.pose.rotation.w};
    for(float value:values)hash_word(h,std::bit_cast<std::uint32_t>(value));
    return h;
}
bool cancelled(const MaterialBakeControl& c){return c.cancelled();}
void report(const MaterialBakeControl& c,float value,std::string_view phase){if(c.progress)c.progress(value,phase);}

// Continuous triangle intersection catches subpixel and mirrored overlaps, independently
// of the chosen raster resolution. Shared edges have zero area and are allowed.
double intersection_area(const Triangle& a,const Triangle& b){
    std::vector<Point> polygon(a.uv.begin(),a.uv.end());
    const double sign=edge(b.uv[0],b.uv[1],b.uv[2])>0?1:-1;
    for(unsigned n=0;n<3&&!polygon.empty();++n){
        std::vector<Point> next;Point previous=polygon.back();
        double previousSide=edge(b.uv[n],b.uv[(n+1)%3],previous)*sign;
        for(Point current:polygon){
            const double side=edge(b.uv[n],b.uv[(n+1)%3],current)*sign;
            if((side>=0)!=(previousSide>=0)){
                const double t=previousSide/(previousSide-side);
                next.push_back({previous.x+(current.x-previous.x)*t,previous.y+(current.y-previous.y)*t});
            }
            if(side>=0)next.push_back(current);
            previous=current;previousSide=side;
        }
        polygon=std::move(next);
    }
    double twice=0;for(std::size_t n=0;n<polygon.size();++n){const auto a0=polygon[n],b0=polygon[(n+1)%polygon.size()];twice+=a0.x*b0.y-a0.y*b0.x;}
    return std::abs(twice)*.5;
}
using VertexKey=std::array<std::uint32_t,8>;
VertexKey vertex_key(const PolygonVertex& v,unsigned set){
    const auto t=uv(v,set);const float values[]{v.position.x,v.position.y,v.position.z,t.x,t.y,v.normal.x,v.normal.y,v.normal.z};
    VertexKey key{};for(unsigned n=0;n<8;++n)key[n]=values[n]==0?0:std::bit_cast<std::uint32_t>(values[n]);return key;
}
std::vector<Triangle> triangles(const CookedPolygonAsset& a,const MaterialTextureBakeSettings& s,const std::vector<std::uint32_t>& first){
    std::vector<Triangle> out;out.reserve(first.size());
    for(auto index:first){Triangle t;t.first=index;for(unsigned n=0;n<3;++n){const auto v=uv(a.vertices[a.indices[index+n]],s.targetUv);t.uv[n]={v.x,v.y};}out.push_back(t);}return out;
}
struct Accumulator {
    Float4 base{0,0,0,0};Float3 normal{},emission{};float metallic{},roughness{};std::uint32_t count{};
};
bool unit_range(float value){return std::isfinite(value)&&value>=-.00001F&&value<=1.00001F;}
std::uint8_t byte(float value){return std::uint8_t(std::lround(std::clamp(value,0.F,1.F)*255));}
float srgb(float value){value=std::clamp(value,0.F,1.F);return value<=.0031308F?value*12.92F:1.055F*std::pow(value,1/2.4F)-.055F;}
bool top_left(Point a,Point b){return b.y<a.y||(b.y==a.y&&b.x>a.x);}
}

const char* material_bake_map_name(MaterialBakeMap map) noexcept {
    switch(map){case MaterialBakeMap::BaseColor:return "base-color";case MaterialBakeMap::Normal:return "normal";
        case MaterialBakeMap::MetallicRoughness:return "metallic-roughness";case MaterialBakeMap::Emission:return "emission";case MaterialBakeMap::Opacity:return "opacity";}
    return "unknown";
}

MaterialBakeValidation validate_material_bake(const CookedPolygonAsset& a,const MaterialTextureBakeSettings& s,const MaterialBakeControl& control){
    MaterialBakeValidation result;
    auto fail=[&](std::string error,unsigned triangle=~0U,unsigned other=~0U){
        if(triangle!=~0U){error+="; triangle "+std::to_string(triangle);result.triangle=triangle;}
        if(other!=~0U){error+=" and "+std::to_string(other);result.otherTriangle=other;}
        result.error=std::move(error);return result;
    };
    if(cancelled(control))return fail("Bake cancelled");
    if(s.targetUv>1||s.resolution<8||s.resolution>4096||s.samplesPerAxis<1||s.samplesPerAxis>4||s.paddingPixels>64||s.paddingPixels>=s.resolution/2||!s.maps||(s.maps&~31U))
        return fail("Invalid bake settings: UV0/UV1, resolution 8..4096, 1..4 samples/axis, padding <=64 and valid maps required");
    if(!std::isfinite(s.metersPerUnit)||s.metersPerUnit<=0||!finite(s.pose.position))return fail("Invalid frozen coordinate scale/pose");
    const auto q=s.pose.rotation;const float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    if(!std::isfinite(norm)||std::abs(norm-1)>1e-3F)return fail("Bake pose must contain a unit quaternion");
    const auto valid=validate_polygon_asset(a);if(!valid)return fail(valid.message);
    if(s.materialIndex>=a.materials.size())return fail("Bake material slot does not exist");
    const auto& m=a.materials[s.materialIndex];const auto& binding=a.materialBindings[s.materialIndex];
    if(m.transmission>0||m.blendMode==MaterialBlendMode::Translucent||
       (m.shadingModel!=MaterialShadingModel::StandardPBR&&m.shadingModel!=MaterialShadingModel::ClearCoat))return fail("Bake supports opaque/masked PBR surfaces and an existing uniform coat");
    if(binding.mapping.heightMode!=HeightMappingMode::Off||binding.detailBaseColor.texture||binding.detailNormal.texture||binding.detailRoughness.texture)
        return fail("Parallax and distance-faded detail maps must be removed before this static bake");
    if(!validate_layered_surface(a,s.materialIndex,&result.error))return result;
    std::uint64_t imageBytes=0;for(const auto& image:a.images)imageBytes+=image.rgba8.size();
    const auto faces=a.indices.size()/3;
    result.estimatedBytes=std::uint64_t(s.resolution)*s.resolution*96+imageBytes*8+
        std::uint64_t(a.vertices.size())*sizeof(PolygonVertex)*2+faces*768;
    if(result.estimatedBytes>s.memoryBudgetBytes)return fail("Bake exceeds memory budget (reduce resolution or increase the explicit budget)");
    for(const auto& sub:a.submeshes)if(sub.materialIndex==s.materialIndex)
        for(unsigned first=sub.firstIndex;first<sub.firstIndex+sub.indexCount;first+=3)result.firstIndices.push_back(first);
    if(result.firstIndices.empty())return fail("Selected material is not assigned to any triangles");
    if(result.firstIndices.size()>100000)return fail("Bake target exceeds 100,000 triangles");
    std::sort(result.firstIndices.begin(),result.firstIndices.end());
    if(std::adjacent_find(result.firstIndices.begin(),result.firstIndices.end())!=result.firstIndices.end())return fail("Target submeshes overlap in index-buffer ranges");
    auto ts=triangles(a,s,result.firstIndices);const unsigned grid=(std::min)(128U,(std::max)(4U,unsigned(std::sqrt(double(ts.size())))));
    std::vector<std::vector<unsigned>> bins(grid*grid);std::uint64_t references=0,pairs=0;
    std::vector<unsigned> parent(ts.size());std::iota(parent.begin(),parent.end(),0);
    auto root=[&](unsigned v){while(parent[v]!=v){parent[v]=parent[parent[v]];v=parent[v];}return v;};
    std::map<std::tuple<VertexKey,VertexKey,bool>,unsigned> edges;
    for(unsigned n=0;n<ts.size();++n){
        if(cancelled(control))return fail("Bake cancelled");
        auto& t=ts[n];for(Point p:t.uv)if(!std::isfinite(p.x)||!std::isfinite(p.y)||p.x<0||p.x>1||p.y<0||p.y>1)return fail("Target UVs must be unique within [0,1]; source wrapping is unaffected",t.first/3);
        if(std::abs(edge(t.uv[0],t.uv[1],t.uv[2]))<1e-12)return fail("Target triangle has degenerate UVs",t.first/3);
        const auto& v0=a.vertices[a.indices[t.first]];const auto& v1=a.vertices[a.indices[t.first+1]];const auto& v2=a.vertices[a.indices[t.first+2]];
        if(length_squared(v0.normal)<1e-12F||length_squared(v1.normal)<1e-12F||length_squared(v2.normal)<1e-12F)return fail("Target normals must define a valid tangent frame",t.first/3);
        if(length_squared(cross(subtract(v1.position,v0.position),subtract(v2.position,v0.position)))<1e-20F)return fail("Target triangle has degenerate geometry",t.first/3);
        const auto bounds=[&](bool y,bool maximum){double value=y?t.uv[0].y:t.uv[0].x;for(unsigned k=1;k<3;++k)value=maximum?std::max(value,y?t.uv[k].y:t.uv[k].x):std::min(value,y?t.uv[k].y:t.uv[k].x);return (std::min)(grid-1,unsigned(value*grid));};
        t.left=bounds(false,false);t.right=bounds(false,true);t.top=bounds(true,false);t.bottom=bounds(true,true);
        for(unsigned y=t.top;y<=t.bottom;++y)for(unsigned x=t.left;x<=t.right;++x){
            if(++references>1000000)return fail("UV validation cell-reference budget exceeded");
            auto& bin=bins[y*grid+x];for(unsigned previous:bin){
                if(++pairs>8000000)return fail("UV validation pair budget exceeded; simplify the atlas");
                const auto& b=ts[previous];if(x!=std::max(t.left,b.left)||y!=std::max(t.top,b.top))continue;
                if(intersection_area(t,b)>1e-13)return fail("Target UV triangles overlap (including mirrored or subpixel overlaps)",t.first/3,b.first/3);
            }
            bin.push_back(n);
        }
        for(unsigned k=0;k<3;++k){auto va=vertex_key(a.vertices[a.indices[t.first+k]],s.targetUv),vb=vertex_key(a.vertices[a.indices[t.first+(k+1)%3]],s.targetUv);if(vb<va)std::swap(va,vb);
            const auto [it,inserted]=edges.emplace(std::tuple{va,vb,dot(cross(subtract(v1.position,v0.position),subtract(v2.position,v0.position)),v0.normal)*float(edge(t.uv[0],t.uv[1],t.uv[2]))<0},n);if(!inserted)parent[root(n)]=root(it->second);}
    }
    std::map<unsigned,unsigned> chartIds;result.charts.reserve(ts.size());
    for(unsigned n=0;n<ts.size();++n){const auto r=root(n);const auto [it,inserted]=chartIds.emplace(r,unsigned(chartIds.size()));(void)inserted;result.charts.push_back(it->second);}
    result.valid=true;return result;
}

MaterialTextureBakeResult bake_material_textures(const CookedPolygonAsset& a,const MaterialTextureBakeSettings& settings,const MaterialBakeControl& control){
    MaterialTextureBakeResult result;result.settings=settings;
    auto fail=[&](std::string error){result.error=std::move(error);result.cancelled=cancelled(control);result.images.clear();return result;};
    report(control,0,"Validating UVs and material dependencies");
    const auto validation=validate_material_bake(a,settings,control);if(!validation.valid)return fail(validation.error);
    result.sourceHash=polygon_asset_content_hash(a);result.recipeKey=recipe_key(result.sourceHash,settings);
    result.chartCount=*std::max_element(validation.charts.begin(),validation.charts.end())+1;
    const unsigned size=settings.resolution,samples=settings.samplesPerAxis;const std::size_t pixels=std::size_t(size)*size;
    std::vector<Accumulator> sums(pixels);std::vector<unsigned> owners(pixels,~0U),seeds(pixels,~0U),queue;queue.reserve(pixels);
    std::vector<std::uint16_t> distance(pixels,std::numeric_limits<std::uint16_t>::max());
    std::vector<PolygonTextureMipChain> mips;mips.reserve(a.images.size());
    for(const auto& image:a.images){if(cancelled(control))return fail("Bake cancelled");auto chain=generate_texture_mips(image);chain.normalLevels=generate_texture_mips(image,true).levels;chain.srgbLevels=generate_texture_mips(image,false,true).levels;mips.push_back(std::move(chain));}
    auto ts=triangles(a,settings,validation.firstIndices);
    for(unsigned n=0;n<ts.size();++n){
        if(cancelled(control))return fail("Bake cancelled");
        const auto& triangle=ts[n];const auto& v0=a.vertices[a.indices[triangle.first]],&v1=a.vertices[a.indices[triangle.first+1]],&v2=a.vertices[a.indices[triangle.first+2]];
        const auto e1=subtract(v1.position,v0.position),e2=subtract(v2.position,v0.position);
        const double du1=triangle.uv[1].x-triangle.uv[0].x,dv1=triangle.uv[1].y-triangle.uv[0].y,du2=triangle.uv[2].x-triangle.uv[0].x,dv2=triangle.uv[2].y-triangle.uv[0].y;
        const double determinant=du1*dv2-dv1*du2;
        const auto dpdu=multiply(subtract(multiply(e1,float(dv2)),multiply(e2,float(dv1))),float(1/determinant));
        const auto dpdv=multiply(subtract(multiply(e2,float(du1)),multiply(e1,float(du2))),float(1/determinant));
        const PolygonSurfaceDifferentials differentials{multiply(transform_vector(settings.pose,dpdu),1.F/float(size)),multiply(transform_vector(settings.pose,dpdv),1.F/float(size))};
        auto points=triangle.uv;for(auto& p:points){p.x*=size;p.y*=size;}
        std::array<unsigned,3> order{0,1,2};if(edge(points[0],points[1],points[2])<0)std::swap(order[1],order[2]);
        const auto pa=points[order[0]],pb=points[order[1]],pc=points[order[2]];const double area=edge(pa,pb,pc);
        const int left=std::max(0,int(std::floor(std::min({pa.x,pb.x,pc.x})))),right=std::min(int(size)-1,int(std::ceil(std::max({pa.x,pb.x,pc.x}))));
        const int top=std::max(0,int(std::floor(std::min({pa.y,pb.y,pc.y})))),bottom=std::min(int(size)-1,int(std::ceil(std::max({pa.y,pb.y,pc.y}))));
        bool any=false;
        for(int y=top;y<=bottom;++y){
            if(cancelled(control))return fail("Bake cancelled");
            for(int x=left;x<=right;++x)for(unsigned sy=0;sy<samples;++sy)for(unsigned sx=0;sx<samples;++sx){
                const Point p{double(x)+(sx+.5)/samples,double(y)+(sy+.5)/samples};
                const double ea=edge(pb,pc,p),eb=edge(pc,pa,p),ec=edge(pa,pb,p);
                const auto inside=[](double e,bool include){return e>1e-10||(e>=-1e-10&&include);};
                if(!inside(ea,top_left(pb,pc))||!inside(eb,top_left(pc,pa))||!inside(ec,top_left(pa,pb)))continue;
                any=true;const auto index=std::size_t(y)*size+unsigned(x);const auto chart=validation.charts[n];
                if(owners[index]!=~0U&&owners[index]!=chart)return fail("Distinct UV charts share a destination texel; increase spacing/resolution");
                owners[index]=chart;std::array<float,3> weights{};weights[order[0]]=float(ea/area);weights[order[1]]=float(eb/area);weights[order[2]]=float(ec/area);
                const auto blend=[&](Float3 a0,Float3 b0,Float3 c0){return add(add(multiply(a0,weights[0]),multiply(b0,weights[1])),multiply(c0,weights[2]));};
                const auto position=blend(v0.position,v1.position,v2.position);
                const auto interpolated=transform_vector(settings.pose,blend(v0.normal,v1.normal,v2.normal));
                if(length_squared(interpolated)<1e-12F)return fail("Interpolated target normal is singular");
                const auto normal=normalize(interpolated);
                auto tangent=subtract(transform_vector(settings.pose,dpdu),multiply(normal,dot(normal,transform_vector(settings.pose,dpdu))));
                if(length_squared(tangent)<1e-20F)return fail("Destination tangent frame is singular");
                tangent=normalize(tangent);const auto perpendicular=cross(normal,tangent);
                const auto bitangent=multiply(perpendicular,dot(perpendicular,transform_vector(settings.pose,dpdv))<0?-1.F:1.F);
                PolygonBvhHit hit;hit.position=position;hit.normal=normal;hit.materialIndex=settings.materialIndex;hit.firstIndex=triangle.first;
                const auto surface=sample_polygon_hit_material(a,hit,settings.pose,normal,mips,settings.metersPerUnit,0,0,&differentials);
                const auto& m=surface.material;
                if(selected(settings,MaterialBakeMap::BaseColor)&&(!unit_range(m.baseColor.x)||!unit_range(m.baseColor.y)||!unit_range(m.baseColor.z)||!unit_range(m.baseColor.w)))return fail("Base color exceeds PNG8 range; remove HDR/additive overflow or choose other maps");
                if(selected(settings,MaterialBakeMap::Emission)&&(!unit_range(m.emissive.x)||!unit_range(m.emissive.y)||!unit_range(m.emissive.z)))return fail("Emission exceeds LDR PNG8 range; HDR emission baking needs a precision upgrade");
                if(!finite(surface.normal)||!unit_range(m.metallic)||!unit_range(m.roughness))return fail("Material evaluation produced invalid surface values");
                auto& sum=sums[index];sum.base.x+=m.baseColor.x;sum.base.y+=m.baseColor.y;sum.base.z+=m.baseColor.z;sum.base.w+=m.baseColor.w;
                sum.metallic+=m.metallic;sum.roughness+=m.roughness;sum.emission=add(sum.emission,m.emissive);
                sum.normal=add(sum.normal,{dot(surface.normal,tangent),dot(surface.normal,bitangent),dot(surface.normal,normal)});++sum.count;++result.surfaceSamples;
            }
        }
        if(!any)return fail("A target UV triangle has no covered supersample; increase resolution or repack");
        report(control,.05F+.75F*float(n+1)/float(ts.size()),"Evaluating unlit material channels");
    }
    for(unsigned index=0;index<pixels;++index)if(sums[index].count){distance[index]=0;seeds[index]=index;queue.push_back(index);++result.coveredPixels;}
    // Multi-source dilation retains chart identity and rejects insufficient gutters.
    for(std::size_t next=0;next<queue.size();++next){
        if((next&4095U)==0&&cancelled(control))return fail("Bake cancelled");
        const auto index=queue[next];if(distance[index]>=settings.paddingPixels)continue;
        const int x=int(index%size),y=int(index/size);
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){if(!dx&&!dy)continue;const int nx=x+dx,ny=y+dy;if(nx<0||ny<0||nx>=int(size)||ny>=int(size))continue;
            const auto other=unsigned(ny)*size+unsigned(nx);
            if(owners[other]==~0U){owners[other]=owners[index];distance[other]=std::uint16_t(distance[index]+1);seeds[other]=seeds[index];queue.push_back(other);++result.dilatedPixels;}
            else if(owners[other]!=owners[index]&&unsigned(distance[index])+distance[other]+1<settings.paddingPixels*2)
                return fail("UV charts have insufficient pixel padding; repack with larger gutters");
        }
    }
    result.safeMipLevels=1;for(unsigned gap=settings.paddingPixels;gap>=4;gap/=2)++result.safeMipLevels;
    for(auto map:{MaterialBakeMap::BaseColor,MaterialBakeMap::Normal,MaterialBakeMap::MetallicRoughness,MaterialBakeMap::Emission,MaterialBakeMap::Opacity})if(selected(settings,map)){
        PolygonImage image;image.name=material_bake_map_name(map);image.mimeType="image/png";image.width=image.height=size;image.rgba8.resize(pixels*4);
        for(std::size_t index=0;index<pixels;++index){
            if((index&4095U)==0&&cancelled(control))return fail("Bake cancelled");
            const auto seed=seeds[index];Float4 value{0,0,0,map==MaterialBakeMap::BaseColor?0.F:1.F};
            if(map==MaterialBakeMap::Normal)value={.5F,.5F,1,1};
            if(map==MaterialBakeMap::MetallicRoughness)value={1,1,0,1};
            if(seed!=~0U){const auto& sum=sums[seed];const float inverse=1.F/float(sum.count);
                if(map==MaterialBakeMap::BaseColor)value={srgb(sum.base.x*inverse),srgb(sum.base.y*inverse),srgb(sum.base.z*inverse),sum.base.w*inverse};
                else if(map==MaterialBakeMap::Normal){const auto n0=normalize(sum.normal);value={n0.x*.5F+.5F,n0.y*.5F+.5F,n0.z*.5F+.5F,1};}
                else if(map==MaterialBakeMap::MetallicRoughness)value={1,sum.roughness*inverse,sum.metallic*inverse,1};
                else if(map==MaterialBakeMap::Emission)value={srgb(sum.emission.x*inverse),srgb(sum.emission.y*inverse),srgb(sum.emission.z*inverse),1};
                else value={sum.base.w*inverse,sum.base.w*inverse,sum.base.w*inverse,1};
            }
            const auto out=index*4;image.rgba8[out]=byte(value.x);image.rgba8[out+1]=byte(value.y);image.rgba8[out+2]=byte(value.z);image.rgba8[out+3]=byte(value.w);
        }
        result.images.emplace(map,std::move(image));
    }
    result.success=true;report(control,1,"Material maps ready");return result;
}

std::optional<CookedPolygonAsset> make_baked_material_asset(const CookedPolygonAsset& source,const MaterialTextureBakeResult& bake,std::string* error){
    auto fail=[&](std::string message)->std::optional<CookedPolygonAsset>{if(error)*error=std::move(message);return {};};
    if(!bake.success||bake.sourceHash!=polygon_asset_content_hash(source)||bake.recipeKey!=recipe_key(bake.sourceHash,bake.settings))return fail("Bake is incomplete or stale; rebake from the current source");
    if(bake.settings.materialIndex>=source.materials.size()||bake.settings.materialIndex>=source.materialBindings.size()||bake.settings.targetUv>1||!validate_polygon_asset(source))return fail("Invalid source or bake target");
    if((bake.settings.maps&kMaterialBakeDefaultMaps)!=kMaterialBakeDefaultMaps)return fail("Applying a bake requires base color, normal, metallic/roughness and emission maps");
    if(source.materials.size()>=256)return fail("No free derived material slot");
    auto asset=source;auto material=source.materials[bake.settings.materialIndex];material.name+=" (baked)";material.preset={};material.layers.clear();material.surfaceDetail={};material.normalMap.reset();
    material.baseColor={1,1,1,1};material.metallic=material.roughness=1;material.emissive={1,1,1};
    PolygonMaterialBinding binding;const auto& old=source.materialBindings[bake.settings.materialIndex];binding.doubleSided=old.doubleSided;binding.alphaCutoff=old.alphaCutoff;
    binding.mapping.mappingMode=bake.settings.targetUv==1?MaterialMappingMode::UV1:MaterialMappingMode::UV0;
    const auto sampler=unsigned(asset.samplers.size());asset.samplers.push_back({ImportedWrapMode::ClampToEdge,ImportedWrapMode::ClampToEdge,ImportedTextureFilter::Linear,ImportedTextureFilter::Linear});
    for(auto map:{MaterialBakeMap::BaseColor,MaterialBakeMap::Normal,MaterialBakeMap::MetallicRoughness,MaterialBakeMap::Emission}){
        const auto found=bake.images.find(map);if(found==bake.images.end())return fail("Required baked map is missing");
        const auto& image=found->second;if(image.width!=bake.settings.resolution||image.height!=image.width||image.rgba8.size()!=std::size_t(image.width)*image.height*4)return fail("Baked map dimensions are invalid");
        const auto imageIndex=unsigned(asset.images.size());asset.images.push_back(image);const auto texture=unsigned(asset.textures.size());asset.textures.push_back({image.name,imageIndex,sampler});
        PolygonTextureBinding value{texture,bake.settings.targetUv,(map==MaterialBakeMap::BaseColor||map==MaterialBakeMap::Emission)?PolygonTextureColorSpace::Srgb:PolygonTextureColorSpace::Linear};
        if(map==MaterialBakeMap::BaseColor)binding.baseColor=value;else if(map==MaterialBakeMap::Normal)binding.normal=value;else if(map==MaterialBakeMap::MetallicRoughness)binding.metallicRoughness=value;else binding.emissive=value;
    }
    const auto slot=unsigned(asset.materials.size());asset.materials.push_back(std::move(material));asset.materialBindings.push_back(binding);
    for(auto& sub:asset.submeshes)if(sub.materialIndex==bake.settings.materialIndex){sub.materialIndex=slot;for(unsigned n=sub.firstIndex;n<sub.firstIndex+sub.indexCount;++n){
        auto vertex=source.vertices[source.indices[n]];vertex.color={1,1,1,1};asset.indices[n]=unsigned(asset.vertices.size());asset.vertices.push_back(vertex);
    }}
    asset.contentHash=polygon_asset_content_hash(asset);const auto valid=validate_polygon_asset(asset);if(!valid)return fail(valid.message);return asset;
}

std::string material_bake_manifest(const MaterialTextureBakeResult& bake){
    const auto& s=bake.settings;std::ostringstream out;out<<std::setprecision(9);
    out<<"{\n  \"schema\":1,\n  \"recipe_version\":1,\n  \"hash_algorithm\":\"DVE FNV-1a content hash (not a security digest)\",\n  \"source_hash\":\""<<hex(bake.sourceHash)<<"\",\n  \"recipe_key\":\""<<hex(bake.recipeKey)<<"\",\n  \"source_mesh\":\"source.dmesh\",\n  \"derived_mesh\":"<<((s.maps&kMaterialBakeDefaultMaps)==kMaterialBakeDefaultMaps?"\"baked.dmesh\"":"null")<<",\n  \"material_slot\":"<<s.materialIndex<<",\n  \"target_uv\":"<<s.targetUv<<",\n  \"resolution\":"<<s.resolution<<",\n  \"samples_per_axis\":"<<s.samplesPerAxis<<",\n  \"padding_pixels\":"<<s.paddingPixels<<",\n  \"safe_mip_levels\":"<<bake.safeMipLevels<<",\n  \"normal_convention\":\"DVE per-fragment derivative tangent frame, +Y; not an external MikkTSpace bake\",\n  \"meters_per_unit\":"<<s.metersPerUnit<<",\n  \"frozen_position\":["<<s.pose.position.x<<","<<s.pose.position.y<<","<<s.pose.position.z<<"],\n  \"frozen_rotation_xyzw\":["<<s.pose.rotation.x<<","<<s.pose.rotation.y<<","<<s.pose.rotation.z<<","<<s.pose.rotation.w<<"],\n  \"maps\":[";
    bool first=true;for(const auto& [map,image]:bake.images){(void)image;if(!first)out<<",";first=false;out<<"{\"file\":\""<<material_bake_map_name(map)<<".png\",\"rgb_space\":\""<<((map==MaterialBakeMap::BaseColor||map==MaterialBakeMap::Emission)?"sRGB":"linear")<<"\",\"alpha_space\":\"linear\"}";}
    out<<"],\n  \"metallic_roughness_channels\":\"R=1 (no AO), G=perceptual roughness, B=metallic, A=1\",\n  \"rebake_policy\":\"Any source/dependency/geometry/UV/pose change makes this derived result stale. Original stack and vertex colors are preserved in source.dmesh. No lighting is baked.\"\n}\n";return out.str();
}

bool write_material_bake_bundle(const std::filesystem::path& directory,const CookedPolygonAsset& source,const MaterialTextureBakeResult& bake,const MaterialBakeControl& control,std::string* error){
    auto fail=[&](std::string message){if(error)*error=std::move(message);return false;};
    if(!bake.success||bake.sourceHash!=polygon_asset_content_hash(source))return fail("Cannot export incomplete or stale bake");
    if(directory.empty()||directory.filename().empty()||directory.filename()=="."||directory.filename()=="..")return fail("Choose a new output bundle directory");
    std::error_code ec;if(std::filesystem::exists(directory,ec)||ec)return fail("Output bundle already exists or cannot be inspected");
    const bool complete=(bake.settings.maps&kMaterialBakeDefaultMaps)==kMaterialBakeDefaultMaps;
    std::optional<CookedPolygonAsset> derived;
    if(complete){derived=make_baked_material_asset(source,bake,error);if(!derived)return false;}
    else if(bake.recipeKey!=recipe_key(bake.sourceHash,bake.settings))return fail("Bake settings changed; rebake first");
    for(auto map:{MaterialBakeMap::BaseColor,MaterialBakeMap::Normal,MaterialBakeMap::MetallicRoughness,MaterialBakeMap::Emission,MaterialBakeMap::Opacity})
        if(selected(bake.settings,map)&&!bake.images.contains(map))return fail("Selected baked map is missing");
    for(const auto& [map,image]:bake.images)if(!selected(bake.settings,map)||image.width!=bake.settings.resolution||image.height!=image.width||image.rgba8.size()!=std::size_t(image.width)*image.height*4)return fail("Invalid baked map");
    const auto parent=directory.parent_path().empty()?std::filesystem::path("."):directory.parent_path();std::filesystem::create_directories(parent,ec);if(ec)return fail(ec.message());
    static std::atomic<std::uint64_t> serial{};std::filesystem::path stage;bool created=false;
    for(unsigned attempt=0;attempt<64&&!created;++attempt){stage=parent/(directory.filename().string()+".tmp-"+std::to_string(serial.fetch_add(1)));created=std::filesystem::create_directory(stage,ec);if(ec&&ec!=std::errc::file_exists)return fail(ec.message());ec.clear();}
    if(!created)return fail("Could not reserve a staging directory");
    struct Cleanup {std::filesystem::path path;~Cleanup(){if(!path.empty()){std::error_code ignored;std::filesystem::remove_all(path,ignored);}}} cleanup{stage};
    if(cancelled(control))return fail("Bake cancelled");
    if(!write_dmesh(stage/"source.dmesh",source,error))return false;
    if(derived&&!write_dmesh(stage/"baked.dmesh",*derived,error))return false;
    for(const auto& [map,image]:bake.images){if(cancelled(control))return fail("Bake cancelled");if(!write_rgba8_png(stage/(std::string(material_bake_map_name(map))+".png"),image.width,image.height,image.rgba8,error))return false;}
    std::ofstream manifest(stage/"provenance.json",std::ios::binary);manifest<<material_bake_manifest(bake);manifest.close();if(!manifest)return fail("Could not write bake provenance");
    if(cancelled(control))return fail("Bake cancelled");
    // A regular rename can replace an empty directory created after preflight.
    // Use the platform's atomic no-replace operation to protect concurrent writers.
#if defined(__linux__)
    if(syscall(SYS_renameat2,AT_FDCWD,stage.c_str(),AT_FDCWD,directory.c_str(),1U)!=0)return fail(std::generic_category().message(errno));
#elif defined(_WIN32)
    if(!MoveFileExW(stage.c_str(),directory.c_str(),MOVEFILE_WRITE_THROUGH))return fail("Cannot publish bake bundle without replacing an existing directory");
#elif defined(__APPLE__)
    if(renamex_np(stage.c_str(),directory.c_str(),RENAME_EXCL)!=0)return fail(std::generic_category().message(errno));
#else
    return fail("Atomic no-replace bundle publication is unavailable on this platform");
#endif
    cleanup.path.clear();return true;
}
}
