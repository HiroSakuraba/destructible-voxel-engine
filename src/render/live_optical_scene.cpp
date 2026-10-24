#include "dve/render/live_optical_scene.hpp"
#include "dve/transform.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

namespace dve::render {
namespace {
bool fail(std::string* e,const char* s){if(e)*e=s;return false;}
Float3 add3(Float3 a,Float3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Float3 sub(Float3 a,Float3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
Float3 cross3(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
Float3 point(const std::array<float,16>& m,Float3 p){return {m[0]*p.x+m[4]*p.y+m[8]*p.z+m[12],m[1]*p.x+m[5]*p.y+m[9]*p.z+m[13],m[2]*p.x+m[6]*p.y+m[10]*p.z+m[14]};}
Float3 vector(const std::array<float,16>& m,Float3 p){auto n=m;n[12]=n[13]=n[14]=0;return point(n,p);}
bool rigid(const std::array<float,16>& m){
    const Float3 x{m[0],m[1],m[2]},y{m[4],m[5],m[6]},z{m[8],m[9],m[10]};
    return std::all_of(m.begin(),m.end(),[](float f){return std::isfinite(f);}) &&
        std::abs(m[3])+std::abs(m[7])+std::abs(m[11])+std::abs(m[15]-1)<1e-5F &&
        std::abs(dot(x,x)-1)<1e-4F && std::abs(dot(y,y)-1)<1e-4F && std::abs(dot(z,z)-1)<1e-4F &&
        std::abs(dot(x,y))+std::abs(dot(x,z))+std::abs(dot(y,z))<1e-4F && dot(x,cross3(y,z))>.9999F;
}
bool has_maps(const PolygonMaterialBinding& b){return b.baseColor.texture||b.metallicRoughness.texture||b.normal.texture||b.emissive.texture||b.opacity.texture||b.height.texture||b.detailBaseColor.texture||b.detailNormal.texture||b.detailRoughness.texture||!b.layers.empty();}
float component(Float3 p,unsigned axis){return axis==0?p.x:axis==1?p.y:p.z;}
void bounds(Float3& lo,Float3& hi,Float3 p){lo={(std::min)(lo.x,p.x),(std::min)(lo.y,p.y),(std::min)(lo.z,p.z)};hi={(std::max)(hi.x,p.x),(std::max)(hi.y,p.y),(std::max)(hi.z,p.z)};}
// Double precision, independent of the shader traversal. Used once per camera update.
double intersect(const GpuOpticalTriangle& t,Float3 o,Float3 d){
    const auto a=sub(t.p1,t.p0),b=sub(t.p2,t.p0),q=cross3(d,b);const double det=dot(a,q);
    if(std::abs(det)<1e-14)return -1;
    const auto s=sub(o,t.p0),r=cross3(s,a);const double u=dot(s,q)/det,v=dot(d,r)/det;
    if(u<0||v<0||u+v>1)return -1;
    return dot(b,r)/det;
}
// Position welding accepts duplicated normal/UV seam vertices. Quantisation is
// relative to this volume's bounds, so translating the instance does not change closure.
bool closed_volume(std::span<const GpuOpticalTriangle> triangles,double& volume){
    if(triangles.empty())return false;
    Float3 lo=triangles[0].p0,hi=lo;
    for(const auto& t:triangles)for(auto p:{t.p0,t.p1,t.p2})bounds(lo,hi,p);
    const auto extent=sub(hi,lo);const double span=(std::max)({double(extent.x),double(extent.y),double(extent.z)});
    if(span<=0)return false;
    const double step=span*1e-6;
    using Position=std::array<long long,3>;using Edge=std::pair<Position,Position>;
    const auto weld=[&](Float3 p)->Position{return {std::llround((double(p.x)-lo.x)/step),std::llround((double(p.y)-lo.y)/step),std::llround((double(p.z)-lo.z)/step)};};
    std::map<Edge,std::pair<unsigned,int>> edges;volume=0;
    for(const auto& t:triangles){
        const std::array<Position,3> v{weld(t.p0),weld(t.p1),weld(t.p2)};
        for(unsigned j=0;j<3;++j){auto a=v[j],b=v[(j+1)%3];if(a==b)return false;const bool forward=a<b;
            auto& edge=edges[forward?Edge{a,b}:Edge{b,a}];++edge.first;edge.second+=forward?1:-1;}
        const auto a=sub(t.p0,lo),b=sub(t.p1,lo),c=sub(t.p2,lo);
        volume+=(double(a.x)*(double(b.y)*c.z-double(b.z)*c.y)+double(a.y)*(double(b.z)*c.x-double(b.x)*c.z)+double(a.z)*(double(b.x)*c.y-double(b.y)*c.x))/6;
    }
    return volume>span*span*span*1e-10 && std::all_of(edges.begin(),edges.end(),[](const auto& e){return e.second.first==2 && e.second.second==0;});
}
}

namespace {
// Splits triangles into connected components by shared welded positions.
std::vector<std::vector<GpuOpticalTriangle>> connected_components(std::span<const GpuOpticalTriangle> triangles){
    Float3 lo=triangles[0].p0,hi=lo;
    for(const auto& t:triangles)for(auto p:{t.p0,t.p1,t.p2})bounds(lo,hi,p);
    const auto extent=sub(hi,lo);const double span=(std::max)({double(extent.x),double(extent.y),double(extent.z),1e-30});
    const double step=span*1e-6;
    using Position=std::array<long long,3>;
    const auto weld=[&](Float3 p)->Position{return {std::llround((double(p.x)-lo.x)/step),std::llround((double(p.y)-lo.y)/step),std::llround((double(p.z)-lo.z)/step)};};
    std::vector<std::size_t> parent(triangles.size());for(std::size_t i=0;i<parent.size();++i)parent[i]=i;
    const auto find=[&](std::size_t x){while(parent[x]!=x){parent[x]=parent[parent[x]];x=parent[x];}return x;};
    std::map<Position,std::size_t> owner;
    for(std::size_t i=0;i<triangles.size();++i)for(auto p:{triangles[i].p0,triangles[i].p1,triangles[i].p2}){
        const auto [it,inserted]=owner.emplace(weld(p),i);if(!inserted)parent[find(i)]=find(it->second);}
    std::map<std::size_t,std::size_t> slot;std::vector<std::vector<GpuOpticalTriangle>> out;
    for(std::size_t i=0;i<triangles.size();++i){const auto r=find(i);const auto [it,inserted]=slot.emplace(r,out.size());
        if(inserted)out.emplace_back();out[it->second].push_back(triangles[i]);}
    return out;
}
}

bool build_live_optical_scene(std::span<const LiveOpticalInstance> instances,LiveOpticalSceneData& out,std::string* error){
    LiveOpticalSceneData scene;std::size_t dielectricMaterials{};
    for(const auto& instance:instances){
        if(!instance.asset||!rigid(instance.objectToWorld))return fail(error,"GPU clear glass requires rigid polygon poses; bake scale into geometry");
        const auto& a=*instance.asset;
        if(!validate_polygon_asset(a))return fail(error,"GPU optical scene contains an invalid polygon asset");
        if(a.materials.size()>4096-scene.materials.size())return fail(error,"GPU optical scene exceeds 4096 materials");
        const auto materialOffset=static_cast<std::uint32_t>(scene.materials.size());
        for(const auto& m:a.materials)scene.materials.push_back(pack_gpu_material_record(m));
        for(std::uint32_t mi=0;mi<a.materials.size();++mi){
            const auto& m=a.materials[mi];const auto& binding=a.materialBindings[mi];
            const bool used=std::any_of(a.submeshes.begin(),a.submeshes.end(),[&](const auto& s){return s.materialIndex==mi&&s.indexCount;});
            if(!used)continue;
            if(has_maps(binding)||!m.layers.empty()||m.surfaceDetail.pattern!=SurfacePattern::NoPattern||binding.mapping.heightMode!=HeightMappingMode::Off)
                return fail(error,"GPU optical secondary hits currently require untextured materials without layers, height or surface recipes");
            if(m.blendMode!=MaterialBlendMode::Opaque||m.baseColor.w!=1)
                return fail(error,"GPU optical secondary hits require full opaque coverage; alpha masks/translucency are not supported");
            if(m.transmission>0 && (m.transmission!=1||m.roughness!=0||m.metallic!=0||m.clearCoat!=0||m.emissive.x!=0||m.emissive.y!=0||m.emissive.z!=0))
                return fail(error,"GPU clear glass requires transmission=1, roughness=0, metallic=0, clearCoat=0 and no interface emission");
            if(m.transmission<=0 && m.shadingModel!=MaterialShadingModel::StandardPBR)
                return fail(error,"GPU optical receivers currently require standard PBR with constant/vertex color and optional emission");
            if(m.transmission<=0 && m.clearCoat!=0)return fail(error,"GPU optical receiver clear coat is not implemented");
            const auto first=scene.triangles.size();
            for(const auto& s:a.submeshes)if(s.materialIndex==mi)for(std::uint32_t i=0;i<s.indexCount;i+=3){
                if(scene.triangles.size()>=1048576)return fail(error,"GPU optical scene exceeds 1048576 triangles");
                GpuOpticalTriangle t;t.material=materialOffset+mi;
                std::array<Float3*,3> positions{&t.p0,&t.p1,&t.p2},normals{&t.n0,&t.n1,&t.n2},colors{&t.c0,&t.c1,&t.c2};
                for(unsigned j=0;j<3;++j){const auto& v=a.vertices[a.indices[s.firstIndex+i+j]];
                    if(v.color.w!=1)return fail(error,"GPU optical scene requires full vertex coverage");
                    *positions[j]=point(instance.objectToWorld,v.position);*normals[j]=normalize(vector(instance.objectToWorld,v.normal));*colors[j]={v.color.x,v.color.y,v.color.z};
                    const auto p=*positions[j],n=*normals[j];
                    if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||!std::isfinite(n.x)||!std::isfinite(n.y)||!std::isfinite(n.z))
                        return fail(error,"GPU optical scene has a non-finite transformed vertex");
                }
                const float areaSquared=length_squared(cross3(sub(t.p1,t.p0),sub(t.p2,t.p0)));
                if(!std::isfinite(areaSquared)||areaSquared<1e-20F)return fail(error,"GPU optical scene contains a degenerate or out-of-range triangle");
                scene.triangles.push_back(t);
            }
            if(m.transmission>0){
                const auto mine=std::span<const GpuOpticalTriangle>(scene.triangles).subspan(first);
                double total{};
                if(!closed_volume(mine,total))return fail(error,"GPU solid glass requires closed, consistently outward-wound geometry");
                for(auto& component:connected_components(mine)){double volume{};
                    if(!closed_volume(component,volume))return fail(error,"GPU solid glass requires each connected component to be closed and outward-wound");
                    scene.volumes.push_back({materialOffset+mi,volume,std::move(component)});}
                ++dielectricMaterials;
                if(dielectricMaterials>8)return fail(error,"GPU optical scene supports at most eight dielectric media");
            }
        }
    }
    if(!scene.triangles.empty()){
        std::function<void(std::uint32_t,std::uint32_t)> build=[&](std::uint32_t first,std::uint32_t count){
            const auto index=static_cast<std::uint32_t>(scene.nodes.size());scene.nodes.emplace_back();
            GpuOpticalNode node;node.minimum=node.maximum=scene.triangles[first].p0;
            for(auto i=first;i<first+count;++i)for(auto p:{scene.triangles[i].p0,scene.triangles[i].p1,scene.triangles[i].p2})bounds(node.minimum,node.maximum,p);
            if(count<=4){node.firstTriangle=first;node.triangleCount=count;}
            else{const auto extent=sub(node.maximum,node.minimum);const unsigned axis=extent.x>extent.y?(extent.x>extent.z?0U:2U):(extent.y>extent.z?1U:2U);
                const auto middle=first+count/2;
                std::nth_element(scene.triangles.begin()+first,scene.triangles.begin()+middle,scene.triangles.begin()+first+count,[&](const auto& a,const auto& b){return component(add3(add3(a.p0,a.p1),a.p2),axis)<component(add3(add3(b.p0,b.p1),b.p2),axis);});
                build(first,count/2);build(middle,count-count/2);}
            node.escape=static_cast<std::uint32_t>(scene.nodes.size());scene.nodes[index]=node;
        };build(0,static_cast<std::uint32_t>(scene.triangles.size()));
    }
    scene.settings.nodeCount=static_cast<std::uint32_t>(scene.nodes.size());scene.settings.triangleCount=static_cast<std::uint32_t>(scene.triangles.size());scene.settings.materialCount=static_cast<std::uint32_t>(scene.materials.size());
    out=std::move(scene);return true;
}

bool set_live_optical_camera(LiveOpticalSceneData& scene,Float3 camera,std::string* error){
    if(!std::isfinite(camera.x)||!std::isfinite(camera.y)||!std::isfinite(camera.z))return fail(error,"GPU optical camera must be finite");
    const auto d=normalize(Float3{.317F,.713F,.991F});
    // Containment is decided per closed connected component; nesting order uses the
    // volume of the component that actually contains the camera.
    std::vector<double> containing(scene.materials.size(),-1);
    for(const auto& v:scene.volumes){double nearest=std::numeric_limits<double>::infinity();bool inside{};
        for(const auto& t:v.triangles){const double hit=intersect(t,camera,d);
            if(hit>1e-7&&hit<nearest){nearest=hit;inside=dot(d,cross3(sub(t.p1,t.p0),sub(t.p2,t.p0)))>0;}}
        if(!inside)continue;
        if(containing[v.material]>=0)return fail(error,"GPU optical camera lies inside nested components of the same dielectric material; this topology is not supported");
        containing[v.material]=v.volume;}
    std::vector<std::pair<double,std::uint32_t>> order;
    for(std::uint32_t m=0;m<containing.size();++m)if(containing[m]>=0)order.push_back({containing[m],m});
    std::sort(order.begin(),order.end(),[](const auto& a,const auto& b){return a.first!=b.first?a.first>b.first:a.second<b.second;});
    if(order.size()>scene.settings.media.size())return fail(error,"GPU optical camera is inside too many dielectric media");
    scene.settings.mediumCount=0;scene.settings.media.fill(0);
    for(const auto& o:order)scene.settings.media[scene.settings.mediumCount++]=o.second+1;
    return true;
}

LiveOpticalSceneGpu::~LiveOpticalSceneGpu(){clear();}
void LiveOpticalSceneGpu::clear() noexcept{
    device_.wait_idle();if(group_)(void)device_.destroy_bind_group(group_,nullptr);group_={};
    for(auto& b:buffers_){if(b)(void)device_.destroy_buffer(b,nullptr);b={};}
    if(layout_)(void)device_.destroy_bind_group_layout(layout_,nullptr);
    layout_={};key_.clear();data_={};cameraValid_=false;
}
bool LiveOpticalSceneGpu::initialize(std::string* error){
    if(layout_)return true;
    rhi::BindGroupLayoutDesc desc;desc.debugName="Live optical scene layout";
    for(unsigned i=0;i<4;++i)desc.bindings.push_back({i,rhi::BindingType::StorageBufferReadOnly,rhi::ShaderStage::Fragment});
    layout_=device_.create_bind_group_layout(desc,error);return bool(layout_);
}
bool LiveOpticalSceneGpu::prepare(std::span<const LiveOpticalInstance> instances,Float3 camera,std::string* error){
    if(!initialize(error))return false;
    std::vector<std::uint64_t> key;
    for(const auto& instance:instances){if(!instance.asset)return fail(error,"Missing GPU optical asset");key.push_back(instance.asset->contentHash);
        for(float f:instance.objectToWorld)key.push_back(std::bit_cast<std::uint32_t>(f));}
    const bool rebuild=key!=key_||!group_;
    if(!rebuild){
        if(cameraValid_&&camera.x==camera_.x&&camera.y==camera_.y&&camera.z==camera_.z)return true;
        const auto previous=data_.settings;
        if(!set_live_optical_camera(data_,camera,error))return false;
        device_.wait_idle();
        if(!device_.write_buffer(buffers_[3],0,std::as_bytes(std::span(&data_.settings,1)),error)){data_.settings=previous;return false;}
        camera_=camera;cameraValid_=true;return true;
    }
    LiveOpticalSceneData next;
    if(!build_live_optical_scene(instances,next,error))return false;
    if(!set_live_optical_camera(next,camera,error))return false;
    // The live renderer already serializes uniform reuse between frames. Keep this
    // resource owner safe independently too: no in-flight descriptors are overwritten.
    device_.wait_idle();
    std::array<rhi::BufferHandle,4> created{};rhi::BindGroupHandle group;
    const auto cleanup=[&](){if(group)(void)device_.destroy_bind_group(group,nullptr);for(auto b:created)if(b)(void)device_.destroy_buffer(b,nullptr);};
    const auto upload=[&](unsigned index,std::span<const std::byte> bytes){
        std::array<std::byte,116> zero{};if(bytes.empty())bytes=zero;
        rhi::BufferDesc desc;desc.bytes=bytes.size();desc.usage=rhi::BufferUsage::Storage|rhi::BufferUsage::CopyDestination;desc.memory=rhi::MemoryDomain::Upload;desc.initialState=rhi::ResourceState::ShaderRead;desc.debugName="Live optical scene buffer";
        created[index]=device_.create_buffer(desc,error);return created[index]&&device_.write_buffer(created[index],0,bytes,error);};
    if(!upload(0,std::as_bytes(std::span(next.nodes)))||!upload(1,std::as_bytes(std::span(next.triangles)))||!upload(2,std::as_bytes(std::span(next.materials)))||!upload(3,std::as_bytes(std::span(&next.settings,1)))){cleanup();return false;}
    rhi::BindGroupDesc desc;desc.layout=layout_;desc.debugName="Live optical scene";
    // Descriptor ranges exactly match each allocated payload (the empty fallback is 116 bytes).
    for(unsigned i=0;i<4;++i){std::size_t bytes=i==0?next.nodes.size()*sizeof(GpuOpticalNode):i==1?next.triangles.size()*sizeof(GpuOpticalTriangle):i==2?next.materials.size()*sizeof(GpuMaterialRecord):sizeof(GpuOpticalSettings);if(!bytes)bytes=116;desc.entries.push_back({i,created[i],{},0,bytes,{}});}
    group=device_.create_bind_group(desc,error);if(!group){cleanup();return false;}
    if(group_)(void)device_.destroy_bind_group(group_,nullptr);
    for(auto b:buffers_)if(b)(void)device_.destroy_buffer(b,nullptr);
    group_=group;buffers_=created;data_=std::move(next);key_=std::move(key);camera_=camera;cameraValid_=true;++builds_;return true;
}
} // namespace dve::render
