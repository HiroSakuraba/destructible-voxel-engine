#include "dve/nurbs_trimming.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace dve;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+":"+std::to_string(__LINE__));}while(false)
double area(const CookedPolygonAsset& mesh){double sum=0;for(std::size_t i=0;i<mesh.indices.size();i+=3){auto a=mesh.vertices[mesh.indices[i]].texcoord,b=mesh.vertices[mesh.indices[i+1]].texcoord,c=mesh.vertices[mesh.indices[i+2]].texcoord;sum+=std::abs((double(b.x)-a.x)*(double(c.y)-a.y)-(double(b.y)-a.y)*(double(c.x)-a.x))*0.5;}return sum;}
NurbsTrimLoop rectangle(std::uint64_t id,double a,double b){return {id,{{a,a},{b,a},{b,b},{a,b}}};}
void tests(){
    auto s=make_nurbs_patch(2);s.samplesPerSpan=4;auto base=nurbs_display_grid(s);CHECK(base);
    NurbsTrimRegion trim;trim.loops.push_back(rectangle(1,0.1,0.9));trim.loops.push_back(rectangle(2,0.3,0.7));
    std::string error;CHECK(validate_nurbs_trim(trim,&error));CHECK(nurbs_trim_contains(trim,{0.2,0.2}));CHECK(!nurbs_trim_contains(trim,{0.5,0.5}));
    auto grid=nurbs_trim_grid(s,*base,trim,&error);CHECK(grid);auto mesh=tessellate_nurbs_trimmed_grid(s,1,*grid,trim,&error);if(!mesh)throw std::runtime_error(error);CHECK(std::abs(area(*mesh)-0.48)<1e-6);
    trim.seeds={{0.5,0.5}};mesh=tessellate_nurbs_trimmed_grid(s,1,*grid,trim,&error);CHECK(mesh);CHECK(std::abs(area(*mesh)-0.16)<1e-6);
    trim.seeds={{0.05,0.05}};mesh=tessellate_nurbs_trimmed_grid(s,1,*grid,trim,&error);CHECK(mesh);CHECK(std::abs(area(*mesh)-0.36)<1e-6);
    trim.seeds.clear();trim.inverted=true;mesh=tessellate_nurbs_trimmed_grid(s,1,*grid,trim,&error);CHECK(mesh);CHECK(std::abs(area(*mesh)-0.52)<1e-6);
    NurbsTrimRegion concave;concave.loops.push_back({1,{{0.1,0.1},{0.9,0.1},{0.9,0.4},{0.4,0.4},{0.4,0.9},{0.1,0.9}}});grid=nurbs_trim_grid(s,*base,concave,&error);CHECK(grid);mesh=tessellate_nurbs_trimmed_grid(s,1,*grid,concave,&error);CHECK(mesh);CHECK(std::abs(area(*mesh)-0.39)<1e-6);
    NurbsTrimRegion triangle;triangle.loops.push_back({1,{{0.1,0.1},{0.9,0.2},{0.4,0.9}}});grid=nurbs_trim_grid(s,*base,triangle,&error);if(!grid)throw std::runtime_error(error);mesh=tessellate_nurbs_trimmed_grid(s,1,*grid,triangle,&error);if(!mesh)throw std::runtime_error(error);CHECK(std::abs(area(*mesh)-0.305)<1e-6);
    // Texture transforms do not change parameter-space trimming.
    s.textureU={4,8};s.textureV={-1,2};grid=nurbs_trim_grid(s,*base,triangle,&error);mesh=tessellate_nurbs_trimmed_grid(s,1,*grid,triangle,&error);CHECK(mesh);
    s.u.knots={-2,-2,-2,-2,3,3,3,3};s.v.knots={10,10,10,10,12,12,12,12};s.controls[5].y=0.4;s.controls[6].weight=1.15;
    base=nurbs_display_grid(s);CHECK(base);grid=nurbs_trim_grid(s,*base,triangle,&error);if(!grid)throw std::runtime_error(error);
    std::vector<NurbsUV> params;mesh=tessellate_nurbs_trimmed_grid(s,1,*grid,triangle,&error,{},&params);if(!mesh)throw std::runtime_error(error);
    CHECK(params.size()==mesh->vertices.size());for(std::size_t i=0;i<params.size();++i){auto p=evaluate_nurbs(s,params[i][0],params[i][1]);CHECK(p);CHECK(mesh->vertices[i].position.x==float(p->position[0]));CHECK(mesh->vertices[i].position.y==float(p->position[1]));}
    auto crossed=triangle;crossed.loops[0].points={{0,0},{1,1},{0,1},{1,0}};CHECK(!validate_nurbs_trim(crossed));
    auto touching=trim;touching.loops[1]=rectangle(2,0.1,0.5);CHECK(!validate_nurbs_trim(touching));
    auto bad=triangle;bad.loops[0].points[0][0]=-0.1;CHECK(!validate_nurbs_trim(bad));
    bad=triangle;bad.seeds={{0.1,0.1}};CHECK(!validate_nurbs_trim(bad));
    std::stop_source stop;stop.request_stop();CHECK(!tessellate_nurbs_trimmed_grid(s,1,*grid,triangle,&error,stop.get_token()));
}
int main(){try{tests();std::cout<<"NURBS trim tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
