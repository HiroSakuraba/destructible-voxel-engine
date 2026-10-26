#include "dve/nurbs_trimming.hpp"
#include "nurbs_evaluation_internal.hpp"
#include <algorithm>
#include <cmath>
#include <set>
namespace dve {
namespace {
using P=NurbsUV;
bool fail(std::string* e,const char* text){if(e)*e=text;return false;}
double cross(P a,P b,P c){return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);}
bool finite(P p){return std::isfinite(p[0])&&std::isfinite(p[1])&&p[0]>=0&&p[0]<=1&&p[1]>=0&&p[1]<=1;}
bool on(P a,P b,P p){return std::abs(cross(a,b,p))<=1e-13&&p[0]>=std::min(a[0],b[0])-1e-13&&p[0]<=std::max(a[0],b[0])+1e-13&&p[1]>=std::min(a[1],b[1])-1e-13&&p[1]<=std::max(a[1],b[1])+1e-13;}
bool intersects(P a,P b,P c,P d){const double x=cross(a,b,c),y=cross(a,b,d),u=cross(c,d,a),v=cross(c,d,b);return (x*y<0&&u*v<0)||on(a,b,c)||on(a,b,d)||on(c,d,a)||on(c,d,b);}
bool inside(const NurbsTrimLoop& loop,P p){bool result=false;for(std::size_t i=0,j=loop.points.size()-1;i<loop.points.size();j=i++){
    auto a=loop.points[i],b=loop.points[j];if((a[1]>p[1])!=(b[1]>p[1])&&p[0]<(b[0]-a[0])*(p[1]-a[1])/(b[1]-a[1])+a[0])result=!result;}return result;}
std::uint32_t signature(const NurbsTrimRegion& trim,P p){std::uint32_t bits=0;for(std::size_t i=0;i<trim.loops.size();++i)if(inside(trim.loops[i],p))bits|=1U<<i;return bits;}
struct Edge{P a,b;double y(double x)const{return std::lerp(a[1],b[1],(x-a[0])/(b[0]-a[0]));}};
std::vector<P> clip(std::vector<P> p,double y,bool lower){std::vector<P> out;if(p.empty())return out;auto previous=p.back();bool was=lower?previous[1]>=y:previous[1]<=y;
    for(auto q:p){bool in=lower?q[1]>=y:q[1]<=y;if(in!=was){const double t=(y-previous[1])/(q[1]-previous[1]);out.push_back({std::lerp(previous[0],q[0],t),y});}if(in)out.push_back(q);previous=q;was=in;}
    out.erase(std::unique(out.begin(),out.end()),out.end());if(out.size()>1&&out.front()==out.back())out.pop_back();return out;}
}
bool validate_nurbs_trim(const NurbsTrimRegion& trim,std::string* error){
    if(trim.loops.empty()||trim.loops.size()>32||trim.seeds.size()>32)return fail(error,"Trimming requires 1..32 loops and at most 32 region seeds");
    std::size_t total=0;std::set<std::uint64_t> ids;
    for(const auto& loop:trim.loops){total+=loop.points.size();if(!loop.id||!ids.insert(loop.id).second||loop.points.size()<3||total>2048)return fail(error,"Trim loops require unique IDs, at least 3 vertices, and at most 2048 total vertices");
        double area=0;for(std::size_t i=0;i<loop.points.size();++i){auto a=loop.points[i],b=loop.points[(i+1)%loop.points.size()];if(!finite(a)||a==b)return fail(error,"Trim vertices must be finite normalized UVs with nonzero edges");area+=a[0]*b[1]-a[1]*b[0];}
        if(std::abs(area)<1e-12)return fail(error,"Trim loop area is degenerate");}
    for(std::size_t l=0;l<trim.loops.size();++l)for(std::size_t m=l;m<trim.loops.size();++m){const auto& a=trim.loops[l].points;const auto& b=trim.loops[m].points;
        for(std::size_t i=0;i<a.size();++i)for(std::size_t j=(l==m?i+1:0);j<b.size();++j){if(l==m&&(j==i+1||(i==0&&j+1==a.size())))continue;
            if(intersects(a[i],a[(i+1)%a.size()],b[j],b[(j+1)%b.size()]))return fail(error,"Trim loops must be simple and must not touch or cross each other");}}
    for(auto p:trim.seeds){if(!finite(p))return fail(error,"Invalid trim region seed");for(const auto& l:trim.loops)for(std::size_t i=0;i<l.points.size();++i)if(on(l.points[i],l.points[(i+1)%l.points.size()],p))return fail(error,"Region seed lies on a trim boundary");}
    return true;
}
bool nurbs_trim_contains(const NurbsTrimRegion& trim,P p){if(trim.loops.empty()||trim.loops.size()>32)return false;const auto bits=signature(trim,p);bool retained=false;
    if(trim.seeds.empty()){for(unsigned i=0;i<32;++i)if(bits&(1U<<i))retained=!retained;}
    else for(auto seed:trim.seeds)if(signature(trim,seed)==bits){retained=true;break;}
    return retained!=trim.inverted;
}
std::optional<NurbsDisplayGrid> nurbs_trim_grid(const NurbsSource& s,const NurbsDisplayGrid& base,const NurbsTrimRegion& trim,std::string* error){
    if(!validate_nurbs(s,error)||!validate_nurbs_display_grid(s,base,error)||!validate_nurbs_trim(trim,error))return {};
    auto grid=base;const auto actual=[&](double x){return std::lerp(s.u.knots.front(),s.u.knots.back(),x);};
    for(const auto& l:trim.loops)for(std::size_t i=0;i<l.points.size();++i){auto a=l.points[i],b=l.points[(i+1)%l.points.size()];grid.u.push_back(actual(a[0]));
        if(a[1]==b[1])continue;
        for(double v:base.v){const double y=(v-s.v.knots.front())/(s.v.knots.back()-s.v.knots.front());if(y>std::min(a[1],b[1])&&y<std::max(a[1],b[1]))grid.u.push_back(actual(std::lerp(a[0],b[0],(y-a[1])/(b[1]-a[1]))));}}
    std::sort(grid.u.begin(),grid.u.end());grid.u.erase(std::unique(grid.u.begin(),grid.u.end()),grid.u.end());
    if(!validate_nurbs_display_grid(s,grid,error))return {};return grid;
}
std::optional<CookedPolygonAsset> tessellate_nurbs_trimmed_grid(const NurbsSource& s,std::uint64_t id,const NurbsDisplayGrid& grid,const NurbsTrimRegion& trim,std::string* error,std::stop_token stop,std::vector<P>* parameters){
    const auto refuse=[&](const char* text)->std::optional<CookedPolygonAsset>{fail(error,text);return {};};
    if(!validate_nurbs(s,error)||!validate_nurbs_display_grid(s,grid,error)||!validate_nurbs_trim(trim,error))return {};
    std::vector<double> u,v;for(double x:grid.u)u.push_back((x-s.u.knots.front())/(s.u.knots.back()-s.u.knots.front()));for(double y:grid.v)v.push_back((y-s.v.knots.front())/(s.v.knots.back()-s.v.knots.front()));
    // The supplied grid must already contain every loop's vertical event. Network
    // synchronization can add more events, but tessellation cannot change it.
    for(const auto& l:trim.loops)for(auto p:l.points){const double a=std::lerp(s.u.knots.front(),s.u.knots.back(),p[0]);if(!std::binary_search(grid.u.begin(),grid.u.end(),a))return refuse("Trim events are missing from the synchronized display grid");}
    CookedPolygonAsset mesh;mesh.objectId=id;VoxelMaterialDefinition material;material.name="Trimmed NURBS";mesh.materials.push_back(material);mesh.materialBindings.resize(1);mesh.materialBindings[0].doubleSided=true;
    std::vector<P> uv;double weight=0;for(auto p:s.controls)weight=std::max(weight,p.weight);
    for(std::size_t i=0;i+1<u.size();++i){if(stop.stop_requested())return refuse("Trim tessellation cancelled");const double a=u[i],b=u[i+1],mid=(a+b)*0.5;
        std::vector<Edge> edges{{{a,0},{b,0}},{{a,1},{b,1}}};
        for(const auto& l:trim.loops)for(std::size_t k=0;k<l.points.size();++k){Edge e{l.points[k],l.points[(k+1)%l.points.size()]};if(mid>std::min(e.a[0],e.b[0])&&mid<std::max(e.a[0],e.b[0]))edges.push_back(e);}
        std::sort(edges.begin(),edges.end(),[&](const Edge& x,const Edge& y){return x.y(mid)<y.y(mid);});
        for(std::size_t k=1;k<edges.size();++k){const auto& low=edges[k-1];const auto& high=edges[k];const double bottom=low.y(mid),top=high.y(mid);
            if(top-bottom<=1e-14||!nurbs_trim_contains(trim,{mid,(bottom+top)*0.5}))continue;
            for(std::size_t j=0;j+1<v.size();++j){auto polygon=clip(clip({{a,low.y(a)},{b,low.y(b)},{b,high.y(b)},{a,high.y(a)}},v[j],true),v[j+1],false);if(polygon.size()<3)continue;
                double area=0;for(std::size_t n=0;n<polygon.size();++n){auto x=polygon[n],y=polygon[(n+1)%polygon.size()];area+=x[0]*y[1]-x[1]*y[0];}if(area<1e-16)continue;
                if(mesh.vertices.size()+polygon.size()>65536)return refuse("Trimmed NURBS exceeds 65536 display vertices");
                const auto first=static_cast<unsigned>(mesh.vertices.size());
                for(auto q:polygon){q[0]=std::clamp(q[0],a,b);q[1]=std::clamp(q[1],v[j],v[j+1]);const double x=q[0]==u[i]?grid.u[i]:q[0]==u[i+1]?grid.u[i+1]:std::lerp(s.u.knots.front(),s.u.knots.back(),q[0]),y=q[1]==v[j]?grid.v[j]:q[1]==v[j+1]?grid.v[j+1]:std::lerp(s.v.knots.front(),s.v.knots.back(),q[1]);
                    auto point=detail::evaluate_validated_nurbs(s,x,y,weight);const double sideX=std::clamp(x,std::nextafter(grid.u[i],grid.u[i+1]),std::nextafter(grid.u[i+1],grid.u[i]));const double sideY=std::clamp(y,std::nextafter(grid.v[j],grid.v[j+1]),std::nextafter(grid.v[j+1],grid.v[j]));auto side=detail::evaluate_validated_nurbs(s,sideX,sideY,weight);if(!point||!side)return refuse("Trim surface evaluation failed");
                    const auto d=side->du,e=side->dv;const std::array<double,3> n{d[1]*e[2]-d[2]*e[1],d[2]*e[0]-d[0]*e[2],d[0]*e[1]-d[1]*e[0]};const double len=std::hypot(n[0],n[1],n[2]),speed=std::hypot(d[0],d[1],d[2]);if(!(len>1e-14&&speed>1e-14))return refuse("Trim surface is singular");
                    PolygonVertex vertex;vertex.position={float(point->position[0]),float(point->position[1]),float(point->position[2])};vertex.normal={float(n[0]/len),float(n[1]/len),float(n[2]/len)};vertex.tangent={float(d[0]/speed),float(d[1]/speed),float(d[2]/speed),1};vertex.texcoord={float(std::lerp(s.textureU[0],s.textureU[1],q[0])),float(std::lerp(s.textureV[0],s.textureV[1],q[1]))};vertex.texcoord1=vertex.texcoord;mesh.vertices.push_back(vertex);uv.push_back({x,y});}
                for(unsigned n=1;n+1<polygon.size();++n)if(cross(polygon[0],polygon[n],polygon[n+1])>1e-16)mesh.indices.insert(mesh.indices.end(),{first,first+n,first+n+1});
            }
        }
    }
    if(mesh.indices.empty())return refuse("Selected trim region has no display area");
    mesh.submeshes.push_back({"Trimmed NURBS",0,unsigned(mesh.indices.size()),0});mesh.bounds.minimum=mesh.bounds.maximum=mesh.vertices.front().position;
    for(const auto& vertex:mesh.vertices){auto p=vertex.position;mesh.bounds.minimum={std::min(mesh.bounds.minimum.x,p.x),std::min(mesh.bounds.minimum.y,p.y),std::min(mesh.bounds.minimum.z,p.z)};mesh.bounds.maximum={std::max(mesh.bounds.maximum.x,p.x),std::max(mesh.bounds.maximum.y,p.y),std::max(mesh.bounds.maximum.z,p.z)};}
    for(std::size_t i=0;i<mesh.indices.size();i+=3){const auto a=mesh.vertices[mesh.indices[i]].position,b=mesh.vertices[mesh.indices[i+1]].position,c=mesh.vertices[mesh.indices[i+2]].position;
        const double x=(double(b.y)-a.y)*(double(c.z)-a.z)-(double(b.z)-a.z)*(double(c.y)-a.y),y=(double(b.z)-a.z)*(double(c.x)-a.x)-(double(b.x)-a.x)*(double(c.z)-a.z),z=(double(b.x)-a.x)*(double(c.y)-a.y)-(double(b.y)-a.y)*(double(c.x)-a.x);
        if(!(std::hypot(x,y,z)>0))return refuse("Trim display triangles collapse at float precision");}
    mesh.contentHash=polygon_asset_content_hash(mesh);auto valid=validate_polygon_asset(mesh);if(!valid)return refuse(valid.message.c_str());if(parameters)*parameters=std::move(uv);return mesh;
}
}
