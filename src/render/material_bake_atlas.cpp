#include "dve/render/material_texture_bake.hpp"
#include "dve/transform.hpp"
#include <algorithm>
#include <utility>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <numeric>
namespace dve::render {
namespace {
struct Chart {std::uint32_t first{};std::array<Float2,3> p{};double width{},height{},area{};unsigned x{},y{};};
Float3 cross(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
}
MaterialBakeValidation prepare_material_bake_atlas(const CookedPolygonAsset& a,const MaterialTextureBakeSettings& s,const MaterialBakeControl& control){
    MaterialBakeValidation r;auto fail=[&](std::string e){r.error=std::move(e);r.valid=false;return r;};
    if(!valid_material_bake_settings(s)||!s.generateAtlas)return fail("Atlas requires valid settings and UV1 destination");
    if(auto v=validate_polygon_asset(a);!v)return fail(v.message);
    std::vector<Chart> charts;
    for(const auto& sub:a.submeshes)if(material_bake_selects(s,sub.materialIndex))for(unsigned first=sub.firstIndex;first<sub.firstIndex+sub.indexCount;first+=3){
        if(control.cancelled())return fail("Bake cancelled");
        if(charts.size()>=100000)return fail("Atlas target exceeds 100,000 triangles");
        Chart c;c.first=first;std::array<Float3,3> v;for(unsigned k=0;k<3;++k){const auto p=a.vertices[a.indices[first+k]].position;v[k]=multiply(Float3{p.x*s.scale.x,p.y*s.scale.y,p.z*s.scale.z},s.metersPerUnit);}
        unsigned base=0;for(unsigned k=1;k<3;++k)if(length_squared(subtract(v[(k+1)%3],v[k]))>length_squared(subtract(v[(base+1)%3],v[base])))base=k;
        const unsigned b=(base+1)%3,t=(base+2)%3;const auto e=subtract(v[b],v[base]),other=subtract(v[t],v[base]);
        const double width=std::sqrt(double(length_squared(e))),twice=std::sqrt(double(length_squared(cross(e,other))));
        if(!(std::isfinite(width)&&std::isfinite(twice)&&width>1e-10&&twice>1e-16))return fail("Cannot atlas degenerate geometry at triangle "+std::to_string(first/3));
        c.width=width;c.height=twice/width;c.area=twice*.5;
        c.p[base]={0,0};c.p[b]={float(width),0};c.p[t]={float(dot(e,other)/width),float(c.height)};charts.push_back(c);
    }
    if(charts.empty())return fail("No assigned triangles in selected slots");
    std::stable_sort(charts.begin(),charts.end(),[](const Chart& a0,const Chart& b0){return a0.height>b0.height;});
    const unsigned gutter=s.paddingPixels+1;
    auto pack=[&](double density,unsigned resolution){unsigned x=0,y=0,row=0;for(auto& c:charts){
        const double pw=std::ceil(c.width*density)+2*gutter,ph=std::ceil(c.height*density)+2*gutter;
        if(pw>resolution||ph>resolution)return false;
        const auto w=unsigned(pw),h=unsigned(ph);
        if(x+w>resolution){y+=row;x=0;row=0;}if(y+h>resolution)return false;
        c.x=x+gutter;c.y=y+gutter;x+=w;row=(std::max)(row,h);
    }return true;};
    unsigned resolution=s.resolution;double best=0;
    for(;;){
        if(control.cancelled())return fail("Bake cancelled");
        double lo=0,hi=double(resolution)/(std::max)(charts[0].height,1e-12);
        if(!pack(0,resolution)){if(resolution<4096){resolution=(std::min)(4096U,resolution*2);continue;}return fail("Atlas gutters alone exceed 4096; reduce triangles or padding");}
        for(unsigned it=0;it<48;++it){double mid=(lo+hi)*.5;if(pack(mid,resolution))lo=mid;else hi=mid;}
        best=lo*.999999;
        if(best>=s.texelsPerMeter||s.texelsPerMeter==0)break;
        if(resolution==4096)return fail("Requested minimum texel density exceeds a 4096 atlas; reduce density or split the mesh");
        resolution=(std::min)(4096U,resolution*2);
    }
    pack(best,resolution);r.resolution=resolution;r.minimumTexelsPerMeter=float(best);r.targetUvs.resize(a.indices.size());double area=0;
    for(unsigned n=0;n<charts.size();++n){const auto& c=charts[n];area+=c.area;for(unsigned k=0;k<3;++k)r.targetUvs[c.first+k]={float((c.x+c.p[k].x*best)/resolution),float((c.y+c.p[k].y*best)/resolution)};r.firstIndices.push_back(c.first);r.charts.push_back(n);}
    r.utilization=float(area*best*best/(double(resolution)*resolution));r.valid=true;
    r.warnings.push_back("Conservative per-triangle charts; no seam editing or distortion optimization. Source UV0/UV1 are evaluated before destination remapping.");
    if(r.utilization<.25F)r.warnings.push_back("Low atlas utilization; reduce padding or split the object into fewer/larger charts.");
    return r;
}
}
