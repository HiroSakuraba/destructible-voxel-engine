#include "dve/editor_prototype.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/editor_polygon.hpp"
#include <array>
#include <stdexcept>
namespace dve::editor {
namespace {
Component component(const char* role,std::string key){
 auto c=make_component_from_schema(std::string("dve.prototype.")+role,1);
 if(!c)throw std::runtime_error("Flight component schema unavailable");
 c->properties["key"]=std::move(key);return *c;
}
void append_box(CookedPolygonAsset& mesh,Float3 center,Float3 size,unsigned material){
 const auto cube=make_editor_polygon_cube(1);
 const auto offset=unsigned(mesh.vertices.size()),first=unsigned(mesh.indices.size());
 for(auto v:cube.vertices){
  v.position=add(center,Float3{(v.position.x-.5F)*size.x,(v.position.y-.5F)*size.y,(v.position.z-.5F)*size.z});
  mesh.vertices.push_back(v);
 }
 for(auto i:cube.indices)mesh.indices.push_back(i+offset);
 mesh.submeshes.push_back({"Part",first,unsigned(cube.indices.size()),material});
}
}
EditorDocument make_flying_starter(){
 EditorDocument d("Wind Garden - Six Form Flight");
 auto box=[&](std::string label,Float3 pos,Float3 size,Float4 color,bool collision=true)->EditorObject&{
  EditorObject o(d.allocate_object_id(),std::move(label));
  o.flags.anchored=true;o.flags.collisionEnabled=collision;o.transform.position=pos;
  o.polygon=make_editor_polygon_cube(o.id);
  for(auto& v:o.polygon->vertices)v.position={(v.position.x-.5F)*size.x,(v.position.y-.5F)*size.y,(v.position.z-.5F)*size.z};
  o.polygon->materials[0].baseColor=color;refresh_editor_polygon(*o.polygon,true);
  return d.add_object(std::move(o));
 };
 box("Wind Garden valley floor",{0,275,-2},{650,800,4},{.17F,.26F,.18F,1});
 for(int i=0;i<7;++i){
   const float y=50.F+100.F*float(i);
   box("Left canyon wall",{-170.F,y,26.F},{16,64,52},{.34F,.33F,.30F,1});
   box("Right canyon wall",{170.F,y,26.F},{16,64,52},{.36F,.32F,.29F,1});
 }
 for(unsigned n=0;n<6;++n){
  const float y=55.F+100.F*float(n);
  const std::array<float,6> gateHeight{{90.F,80.F,100.F,120.F,100.F,120.F}};
  const float z=gateHeight[n];
  const std::array<Float4,6> colors{{{.65F,.84F,.96F,1},{.44F,.86F,.54F,1},{.99F,.72F,.30F,1},{.95F,.30F,.22F,1},{.66F,.52F,.90F,1},{.95F,.88F,.48F,1}}};
  auto& ring=box("Flight ring "+std::to_string(n+1),{0,y,z},{50,.3F,.8F},colors[n],false);
  ring.components.push_back(component("flight_gate","flight-gate-"+std::to_string(n)));
  ring.components.back().properties["order"]=std::int64_t(n);
  ring.components.back().properties["mode"]=std::int64_t(n);
  ring.components.back().properties["half_width"]=24.0;
  ring.components.back().properties["half_height"]=22.0;
  box("Gate left pylon",{-25.F,y,z},{1.4F,2,43},colors[n],false);
  box("Gate right pylon",{25.F,y,z},{1.4F,2,43},colors[n],false);
  box("Gate top",{0,y,z+22.F},{51,2,1.4F},colors[n],false);
 }
 auto& pad=box("Launch pad",{0,-35,0.1F},{28,25,.2F},{.12F,.35F,.52F,1});
 (void)pad;
 box("Thermal marker", {50,280,8},{8,8,16},{.83F,.46F,.12F,1},false);
 EditorObject flyer(d.allocate_object_id(),"Morphing Aircraft");
 flyer.flags.anchored=true;flyer.flags.collisionEnabled=false;
 flyer.transform.position={0,0,90};
 flyer.components.push_back(component("flight_vehicle","flyer"));
 CookedPolygonAsset mesh;mesh.objectId=flyer.id;
 mesh.materials.resize(4);mesh.materialBindings.resize(4);
 mesh.materials[0].baseColor={.14F,.52F,.84F,1};
 mesh.materials[1].baseColor={.9F,.83F,.26F,1};
 mesh.materials[2].baseColor={.18F,.21F,.28F,1};
 mesh.materials[3].baseColor={.9F,.94F,.99F,1};
 append_box(mesh,{0,0,0},{1.2F,4.F,.7F},0);  // fuselage, +Y forward
 append_box(mesh,{0,-.3F,.1F},{8.6F,1.1F,.20F},1); // shared wing silhouette
 append_box(mesh,{0,-1.7F,.7F},{2.6F,.7F,.13F},3);
 append_box(mesh,{0,-1.8F,.6F},{.14F,.8F,1.3F},2);
 append_box(mesh,{0,2.2F,0},{.65F,.4F,.65F},2);
 refresh_editor_polygon(mesh,true);flyer.polygon=std::move(mesh);
 d.add_object(std::move(flyer));
 EditorObject rules(d.allocate_object_id(),"Flight Rules");
 rules.flags.anchored=true;rules.flags.collisionEnabled=false;
 rules.components.push_back(component("flight_rules","flight"));
 d.add_object(std::move(rules));
 return d;
}
}
