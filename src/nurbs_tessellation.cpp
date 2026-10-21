#include "dve/nurbs_tessellation.hpp"
#include "dve/nurbs_modeling.hpp"
#include "nurbs_evaluation_internal.hpp"
#include <algorithm>
#include <cmath>
namespace dve {
namespace {
bool refuse(std::string* error,const char* text){if(error)*error=text;return false;}
void merge(std::vector<double>& values,const std::vector<double>& extra){values.insert(values.end(),extra.begin(),extra.end());std::sort(values.begin(),values.end());values.erase(std::unique(values.begin(),values.end()),values.end());}
}
bool validate_nurbs_display_grid(const NurbsSource& source,const NurbsDisplayGrid& grid,std::string* error){
    if(source.is_curve())return refuse(error,"Shared display requires a surface");
    for(auto [axis,values]:{std::pair{&source.u,&grid.u},std::pair{&source.v,&grid.v}}){
        if(values->size()<2||values->size()>4096||values->front()!=axis->knots.front()||values->back()!=axis->knots.back())return refuse(error,"Shared grid has invalid endpoints or sample count");
        for(std::size_t i=0;i<values->size();++i)if(!std::isfinite((*values)[i])||(i&&!((*values)[i]>(*values)[i-1])))return refuse(error,"Shared grid parameters must be finite and strictly increasing");
        for(std::size_t i=1;i<values->size();++i)if(std::nextafter((*values)[i-1],(*values)[i])>std::nextafter((*values)[i],(*values)[i-1]))return refuse(error,"Shared grid spans have no representable interior parameter");
        for(double knot:axis->knots)if(!std::binary_search(values->begin(),values->end(),knot))return refuse(error,"Shared grid must contain every source knot");
    }
    if((grid.u.size()-1)*(grid.v.size()-1)>16384)return refuse(error,"Shared grid exceeds 65536 cell-corner vertices");
    return true;
}
std::optional<NurbsDisplayGrid> nurbs_display_grid(const NurbsSource& source,const NurbsDisplayGrid* seed,std::string* error,std::stop_token stop){
    if(!validate_nurbs(source,error)||source.is_curve()){if(error&&error->empty())*error="Shared display requires a surface";return {};}
    if(stop.stop_requested())return {};
    if(seed&&!validate_nurbs_display_grid(source,*seed,error))return {};
    if(source.adaptiveDisplay){NurbsDisplayGrid grid;auto mesh=tessellate_nurbs_adaptive(source,1,source.chordTolerance,source.normalAngle,error,stop,&grid,seed);if(!mesh)return {};return grid;}
    NurbsDisplayGrid grid;
    for(auto [axis,values]:{std::pair{&source.u,&grid.u},std::pair{&source.v,&grid.v}}){
        for(std::size_t i=axis->degree;i+1<axis->knots.size()-axis->degree;++i)if(axis->knots[i+1]>axis->knots[i]){
            for(unsigned j=0;j<source.samplesPerSpan;++j)values->push_back(std::lerp(axis->knots[i],axis->knots[i+1],double(j)/source.samplesPerSpan));
        }
        values->push_back(axis->knots.back());
    }
    if(seed){merge(grid.u,seed->u);merge(grid.v,seed->v);}
    if(!validate_nurbs_display_grid(source,grid,error))return {};return grid;
}
std::optional<CookedPolygonAsset> tessellate_nurbs_grid(const NurbsSource& source,std::uint64_t id,const NurbsDisplayGrid& grid,std::string* error,std::stop_token stop){
    if(!validate_nurbs(source,error)||!validate_nurbs_display_grid(source,grid,error))return {};
    double weightScale=0;for(auto p:source.controls)weightScale=(std::max)(weightScale,p.weight);
    auto fail=[&](const char* text)->std::optional<CookedPolygonAsset>{if(error)*error=text;return {};};
    CookedPolygonAsset mesh;mesh.objectId=id;VoxelMaterialDefinition material;material.name="Shared NURBS";mesh.materials.push_back(material);mesh.materialBindings.resize(1);mesh.materialBindings[0].doubleSided=true;
    for(std::size_t j=0;j+1<grid.v.size();++j)for(std::size_t i=0;i+1<grid.u.size();++i){
        if(stop.stop_requested())return fail("Shared NURBS tessellation cancelled");
        const double a=grid.u[i],b=grid.u[i+1],c=grid.v[j],d=grid.v[j+1];const auto first=unsigned(mesh.vertices.size());
        for(auto uv:std::array<std::array<double,2>,4>{{{a,c},{b,c},{b,d},{a,d}}}){
            auto point=detail::evaluate_validated_nurbs(source,uv[0],uv[1],weightScale);
            auto side=detail::evaluate_validated_nurbs(source,std::clamp(uv[0],std::nextafter(a,b),std::nextafter(b,a)),std::clamp(uv[1],std::nextafter(c,d),std::nextafter(d,c)),weightScale);
            if(!point||!side)return fail("Shared NURBS evaluation failed");
            const auto u=side->du,v=side->dv;std::array<double,3> n{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};const double length=std::hypot(n[0],n[1],n[2]),speed=std::hypot(u[0],u[1],u[2]);
            if(!(length>1e-14&&speed>1e-14))return fail("Shared NURBS surface is singular");
            PolygonVertex vertex;vertex.position={float(point->position[0]),float(point->position[1]),float(point->position[2])};vertex.normal={float(n[0]/length),float(n[1]/length),float(n[2]/length)};vertex.tangent={float(u[0]/speed),float(u[1]/speed),float(u[2]/speed),1};
            vertex.texcoord={float(std::lerp(source.textureU[0],source.textureU[1],(uv[0]-grid.u.front())/(grid.u.back()-grid.u.front()))),float(std::lerp(source.textureV[0],source.textureV[1],(uv[1]-grid.v.front())/(grid.v.back()-grid.v.front())))};vertex.texcoord1=vertex.texcoord;mesh.vertices.push_back(vertex);
        }
        mesh.indices.insert(mesh.indices.end(),{first,first+1,first+2,first,first+2,first+3});
    }
    mesh.submeshes.push_back({"Shared NURBS",0,unsigned(mesh.indices.size()),0});mesh.bounds.minimum=mesh.bounds.maximum=mesh.vertices.front().position;
    for(const auto& v:mesh.vertices){auto p=v.position;mesh.bounds.minimum={(std::min)(mesh.bounds.minimum.x,p.x),(std::min)(mesh.bounds.minimum.y,p.y),(std::min)(mesh.bounds.minimum.z,p.z)};mesh.bounds.maximum={(std::max)(mesh.bounds.maximum.x,p.x),(std::max)(mesh.bounds.maximum.y,p.y),(std::max)(mesh.bounds.maximum.z,p.z)};}
    for(std::size_t i=0;i<mesh.indices.size();i+=3){auto a=mesh.vertices[mesh.indices[i]].position,b=mesh.vertices[mesh.indices[i+1]].position,c=mesh.vertices[mesh.indices[i+2]].position;const double x=(double(b.y)-a.y)*(double(c.z)-a.z)-(double(b.z)-a.z)*(double(c.y)-a.y),y=(double(b.z)-a.z)*(double(c.x)-a.x)-(double(b.x)-a.x)*(double(c.z)-a.z),z=(double(b.x)-a.x)*(double(c.y)-a.y)-(double(b.y)-a.y)*(double(c.x)-a.x);if(!(std::hypot(x,y,z)>0))return fail("Shared NURBS grid collapses at float precision");}
    mesh.contentHash=polygon_asset_content_hash(mesh);auto valid=validate_polygon_asset(mesh);if(!valid)return fail(valid.message.c_str());return mesh;
}
}
