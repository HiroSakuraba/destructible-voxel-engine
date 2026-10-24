#include "dve/editor_prototype.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/editor_polygon.hpp"
#include <array>
#include <cmath>
#include <stdexcept>
namespace dve::editor {
namespace {
Component component(const char* role,std::string key){auto c=make_component_from_schema(std::string("dve.prototype.")+role,1);if(!c)throw std::runtime_error("Racing schema missing");c->properties["key"]=std::move(key);return *c;}
void append_box(CookedPolygonAsset& mesh,Float3 center,Float3 size,unsigned material){const auto cube=make_editor_polygon_cube(1);const auto offset=unsigned(mesh.vertices.size()),first=unsigned(mesh.indices.size());for(auto v:cube.vertices){v.position=add(center,Float3{(v.position.x-.5F)*size.x,(v.position.y-.5F)*size.y,(v.position.z-.5F)*size.z});mesh.vertices.push_back(v);}for(auto i:cube.indices)mesh.indices.push_back(i+offset);mesh.submeshes.push_back({"Box",first,unsigned(cube.indices.size()),material});}
}
EditorDocument make_racing_starter(){
 EditorDocument d("Racing Time Trial");
 auto box=[&](std::string name,Float3 position,Float3 size,Float4 color,bool collision=true)->EditorObject&{EditorObject o(d.allocate_object_id(),std::move(name));o.flags.anchored=true;o.flags.collisionEnabled=collision;o.transform.position=position;o.polygon=make_editor_polygon_cube(o.id);for(auto& v:o.polygon->vertices)v.position={(v.position.x-.5F)*size.x,(v.position.y-.5F)*size.y,(v.position.z-.5F)*size.z};o.polygon->materials[0].baseColor=color;refresh_editor_polygon(*o.polygon,true);return d.add_object(std::move(o));};
 box("Asphalt",{0,0,-.25F},{100,96,.5F},{.13F,.15F,.18F,1});
 box("Infield grass",{0,0,.01F},{46,40,.02F},{.12F,.3F,.1F,1},false);
 for(float x:{-24.F,24.F})box("Inner side barrier",{x,0,.7F},{.6F,42,1.4F},{.7F,.27F,.12F,1});
 for(float y:{-21.F,21.F})box("Inner end barrier",{0,y,.7F},{48,.6F,1.4F},{.7F,.27F,.12F,1});
 for(float x:{-46.F,46.F})box("Outer side barrier",{x,0,.8F},{.6F,88,1.6F},{.65F,.68F,.72F,1});
 for(float y:{-44.F,44.F})box("Outer end barrier",{0,y,.8F},{92,.6F,1.6F},{.65F,.68F,.72F,1});
 for(int n=-20;n<=20;n+=5){for(float y:{-32.F,32.F})box("Lane dash",{float(n),y,.012F},{2,.18F,.02F},{.85F,.85F,.8F,1},false);for(float x:{-35.F,35.F})box("Lane dash",{x,float(n),.012F},{.18F,2,.02F},{.85F,.85F,.8F,1},false);}
 EditorObject car(d.allocate_object_id(),"Race Car");car.flags.anchored=true;car.flags.collisionEnabled=false;car.transform.position={2,-32,.04F};car.transform.rotation=quaternion_from_axis_angle({0,0,1},-1.570796327F);car.components.push_back(component("race_car","car"));car.components.back().properties["handling_profile"]=std::string{"street"};
 CookedPolygonAsset mesh;mesh.objectId=car.id;mesh.materials.resize(3);mesh.materialBindings.resize(3);mesh.materials[0].baseColor={.65F,.025F,.015F,1};mesh.materials[0].roughness=.2F;mesh.materials[0].shadingModel=MaterialShadingModel::ClearCoat;mesh.materials[0].clearCoat=1;mesh.materials[1].baseColor={.015F,.02F,.025F,1};mesh.materials[2].baseColor={.05F,.15F,.22F,1};
 append_box(mesh,{0,0,.65F},{1.5F,3.2F,.55F},0);append_box(mesh,{0,-.2F,1.12F},{1.2F,1.5F,.5F},2);for(float x:{-.83F,.83F})for(float y:{-1.F,1.F})append_box(mesh,{x,y,.285F},{.25F,.65F,.65F},1);append_box(mesh,{0,1.63F,.65F},{1.1F,.05F,.15F},2);refresh_editor_polygon(mesh,true);car.polygon=std::move(mesh);d.add_object(std::move(car));
 EditorObject rules(d.allocate_object_id(),"Race Rules");rules.flags.anchored=true;rules.flags.collisionEnabled=false;rules.components.push_back(component("race_rules","race"));d.add_object(std::move(rules));
 const std::array<Float3,4> centers{{{0,-32,0},{35,0,0},{0,32,0},{-35,0,0}}};const std::array<float,4> angles{{-1.570796327F,0,1.570796327F,3.141592654F}};
 for(unsigned n=0;n<4;++n){auto& gate=box(n?"Checkpoint "+std::to_string(n):"Finish",centers[n],{20,.15F,.04F},n?Float4{.06F,.65F,.8F,1}:Float4{.95F,.85F,.15F,1},false);gate.transform.rotation=quaternion_from_axis_angle({0,0,1},angles[n]);gate.components.push_back(component("race_gate","gate-"+std::to_string(n)));gate.components[0].properties["order"]=std::int64_t(n);
  // Visible gate pylons and arrows use the gate's metric frame; they have no collision.
  const auto pose=gate.transform;for(float side:{-9.5F,9.5F})box("Gate pylon",transform_point(pose,{side,0,1.5F}),{.35F,.35F,3},{.08F,.7F,.85F,1},false);
  for(unsigned k=0;k<3;++k){EditorObject arrow(d.allocate_object_id(),"Direction arrow");arrow.flags.anchored=true;arrow.flags.collisionEnabled=false;arrow.transform=pose;arrow.transform.position=transform_point(pose,{0,2+1.7F*float(k),.04F});CookedPolygonAsset marking;marking.objectId=arrow.id;marking.materials.resize(1);marking.materialBindings.resize(1);marking.materials[0].baseColor={.95F,.85F,.2F,1};for(auto position:{Float3{-.8F,0,0},Float3{.8F,0,0},Float3{0,1.3F,0}}){PolygonVertex vertex;vertex.position=position;vertex.normal={0,0,1};marking.vertices.push_back(vertex);}marking.indices={0,1,2};marking.submeshes={{"Arrow",0,3,0}};refresh_editor_polygon(marking,true);arrow.polygon=std::move(marking);d.add_object(std::move(arrow));}
 }
 return d;
}
}
