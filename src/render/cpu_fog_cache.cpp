#include "dve/render/cpu_fog_cache.hpp"
#include "dve/render/cpu_fog.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>

namespace dve::render {
namespace {
struct Bounds{Float3 lo{},hi{};};
Bounds unite(Bounds a,Bounds b){return {{std::min(a.lo.x,b.lo.x),std::min(a.lo.y,b.lo.y),std::min(a.lo.z,b.lo.z)},
    {std::max(a.hi.x,b.hi.x),std::max(a.hi.y,b.hi.y),std::max(a.hi.z,b.hi.z)}};}
bool contains(Bounds b,Float3 p){return p.x>=b.lo.x&&p.x<=b.hi.x&&p.y>=b.lo.y&&p.y<=b.hi.y&&p.z>=b.lo.z&&p.z<=b.hi.z;}
Bounds bounds(const GaborVolumePrimitive& p){
    const auto a=rotate(p.rotation,{p.scale.x*p.extent,0,0}),b=rotate(p.rotation,{0,p.scale.y*p.extent,0}),c=rotate(p.rotation,{0,0,p.scale.z*p.extent});
    // Outward padding accommodates floating point rotations at a support edge.
    const Float3 h{std::abs(a.x)+std::abs(b.x)+std::abs(c.x)+1e-5F,std::abs(a.y)+std::abs(b.y)+std::abs(c.y)+1e-5F,std::abs(a.z)+std::abs(b.z)+std::abs(c.z)+1e-5F};
    return {subtract(p.center,h),add(p.center,h)};
}
bool intersect(Bounds b,Float3 o,Float3 d,float& lo,float& hi){
    const float origin[]{o.x,o.y,o.z},direction[]{d.x,d.y,d.z},a[]{b.lo.x,b.lo.y,b.lo.z},z[]{b.hi.x,b.hi.y,b.hi.z};
    for(int i=0;i<3;++i){if(std::abs(direction[i])<1e-20F){if(origin[i]<a[i]||origin[i]>z[i])return false;continue;}
        float x=(a[i]-origin[i])/direction[i],y=(z[i]-origin[i])/direction[i];if(x>y)std::swap(x,y);
        lo=std::max(lo,x);hi=std::min(hi,y);if(hi<=lo)return false;}
    return true;
}
struct Node{Bounds box;std::uint32_t left{},right{},first{},count{};};
struct Entry{
    std::vector<GaborVolumePrimitive> primitives;
    std::vector<Bounds> boxes;
    std::vector<std::uint32_t> order;
    std::vector<Node> nodes;
    std::uint32_t build(std::uint32_t first,std::uint32_t count){
        const auto index=static_cast<std::uint32_t>(nodes.size());nodes.push_back({});
        auto box=boxes[order[first]];for(auto i=first+1;i<first+count;++i)box=unite(box,boxes[order[i]]);
        nodes[index].box=box;
        if(count<=8){nodes[index].first=first;nodes[index].count=count;return index;}
        const auto extent=subtract(box.hi,box.lo);const int axis=extent.x>extent.y?(extent.x>extent.z?0:2):(extent.y>extent.z?1:2);
        const auto coord=[&](std::uint32_t id){const auto& p=primitives[id].center;return axis==0?p.x:(axis==1?p.y:p.z);};
        const auto half=count/2;
        std::nth_element(order.begin()+first,order.begin()+first+half,order.begin()+first+count,[&](auto a,auto b){return coord(a)<coord(b);});
        const auto left=build(first,half),right=build(first+half,count-half);
        nodes[index].left=left;nodes[index].right=right;return index;
    }
};
}
struct CpuFogDensityCache::Impl{
    std::size_t maximum{},resident{};
    std::map<std::uint64_t,Entry> entries;
};
CpuFogDensityCache::CpuFogDensityCache(std::size_t maximum):impl_(std::make_unique<Impl>()){impl_->maximum=maximum;}
CpuFogDensityCache::~CpuFogDensityCache()=default;
void CpuFogDensityCache::clear(){impl_->entries.clear();impl_->resident=0;}
std::size_t CpuFogDensityCache::resident_primitives()const noexcept{return impl_->resident;}
bool CpuFogDensityCache::prepare(const GaborVolumeAsset& asset){
    if(asset.contentHash==0||asset.primitives.empty()||asset.primitives.size()>impl_->maximum)return false;
    if(impl_->entries.contains(asset.contentHash))return true;
    // Deterministic FIFO-by-hash eviction, with no borrowed pointers retained.
    while(impl_->resident+asset.primitives.size()>impl_->maximum&&!impl_->entries.empty()){
        auto it=impl_->entries.begin();impl_->resident-=it->second.primitives.size();impl_->entries.erase(it);
    }
    Entry entry;entry.primitives=asset.primitives;entry.boxes.reserve(entry.primitives.size());
    for(const auto& p:entry.primitives)entry.boxes.push_back(bounds(p));
    entry.order.resize(entry.primitives.size());std::iota(entry.order.begin(),entry.order.end(),0U);
    entry.build(0,static_cast<std::uint32_t>(entry.order.size()));
    impl_->resident+=entry.primitives.size();impl_->entries.emplace(asset.contentHash,std::move(entry));return true;
}
float CpuFogDensityCache::density(const GaborVolumeAsset& asset,Float3 p,CpuFogStats* stats)const{
    const auto found=impl_->entries.find(asset.contentHash);
    if(found==impl_->entries.end()){
        if(stats)stats->primitiveEvaluations+=asset.primitives.size();
        return evaluate_gabor_density(asset,p);
    }
    const auto& e=found->second;float density=0;
    std::array<std::uint32_t,64> stack{};std::size_t size=1;
    while(size){const auto& n=e.nodes[stack[--size]];if(stats)++stats->densityNodes;
        if(!contains(n.box,p))continue;
        if(n.count){for(auto i=n.first;i<n.first+n.count;++i){
            const auto id=e.order[i];if(!contains(e.boxes[id],p))continue;
            if(stats)++stats->primitiveEvaluations;
            const auto& primitive=e.primitives[id];
            density+=gabor_lod_weight(primitive,256,asset.material.lodBias)*evaluate_gabor_primitive_density(primitive,p);
        }}else{stack[size++]=n.left;stack[size++]=n.right;}
    }
    return std::max(0.0F,density*asset.material.densityMultiplier);
}
std::vector<Float2> CpuFogDensityCache::occupied(const GaborVolumeAsset& asset,Float3 o,Float3 d,float lo,float hi)const{
    const auto found=impl_->entries.find(asset.contentHash);if(found==impl_->entries.end())return {{lo,hi}};
    const auto& e=found->second;std::vector<Float2> intervals;
    std::array<std::uint32_t,64> stack{};std::size_t size=1;
    while(size){const auto& n=e.nodes[stack[--size]];float a=lo,b=hi;if(!intersect(n.box,o,d,a,b))continue;
        if(n.count){intervals.push_back({a,b});if(intervals.size()>64)return {{lo,hi}};}
        else{stack[size++]=n.left;stack[size++]=n.right;}}
    std::sort(intervals.begin(),intervals.end(),[](Float2 a,Float2 b){return a.x<b.x;});
    std::vector<Float2> merged;for(auto q:intervals){if(!merged.empty()&&q.x<=merged.back().y)merged.back().y=std::max(merged.back().y,q.y);else merged.push_back(q);}
    return merged;
}
} // namespace dve::render
