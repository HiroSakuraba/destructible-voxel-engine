#include "dve/editor_prototype.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/editor_polygon.hpp"
#include <stdexcept>
namespace dve::editor {
namespace {
Component role(std::string type,std::string key){
 auto c=make_component_from_schema("dve.prototype."+type,1);
 if(!c)throw std::runtime_error("S1 submarine prototype component not registered");
 c->properties["key"]=std::move(key);
 return *c;
}
void box_part(CookedPolygonAsset& mesh,Float3 center,Float3 size,unsigned material){
 const auto cube=make_editor_polygon_cube(1);
 const unsigned first=unsigned(mesh.indices.size()),base=unsigned(mesh.vertices.size());
 for(auto vertex:cube.vertices){
  vertex.position=add(center,{(vertex.position.x-.5F)*size.x,
      (vertex.position.y-.5F)*size.y,(vertex.position.z-.5F)*size.z});
  mesh.vertices.push_back(vertex);
 }
 for(auto index:cube.indices)mesh.indices.push_back(index+base);
 mesh.submeshes.push_back({"Part",first,unsigned(cube.indices.size()),material});
}
}
EditorDocument make_submarine_starter(){
 EditorDocument d("Deep Explorer — Mini-Submarine");
 auto terrain=[&](std::string name,Float3 position,Float3 extent,Float4 color,bool solid){
  EditorObject obj(d.allocate_object_id(),std::move(name));
  obj.flags.anchored=true;obj.flags.collisionEnabled=solid;obj.transform.position=position;
  obj.polygon=make_editor_polygon_cube(obj.id);
  for(auto& vertex:obj.polygon->vertices){
   vertex.position={(vertex.position.x-.5F)*extent.x,
      (vertex.position.y-.5F)*extent.y,(vertex.position.z-.5F)*extent.z};
  }
  obj.polygon->materials[0].baseColor=color;
  refresh_editor_polygon(*obj.polygon,true);
  d.add_object(std::move(obj));
 };
 // Z-up flat ocean with a collision floor at z=-40; ocean itself is analytic.
 terrain("Seafloor", {0,0,-45},{280,280,10},{.27F,.34F,.36F,1},true);
 terrain("Reef outcrop",{-24,52,-35},{16,25,11},{.35F,.37F,.32F,1},true);
 terrain("Wreck hull",{0,80,-30},{8,17,4},{.33F,.23F,.13F,1},true);
 terrain("Wreck tower",{0,84,-26},{3,3,5},{.27F,.29F,.26F,1},true);
 terrain("Red survey marker",{-7,78,-27},{1.2F,1.2F,1.2F},{.95F,.12F,.09F,1},false);
 terrain("Green survey marker",{7,78,-27},{1.2F,1.2F,1.2F},{.1F,.92F,.35F,1},false);
 terrain("Blue survey marker",{0,66,-24},{1.2F,1.2F,1.2F},{.14F,.30F,.98F,1},false);
 EditorObject sub(d.allocate_object_id(),"Deep Explorer Mini-Sub");
 sub.flags.anchored=true;sub.flags.collisionEnabled=false;
 sub.transform.position={0,-20,-1.7F};
 CookedPolygonAsset mesh;mesh.objectId=sub.id;
 mesh.materials.resize(4);mesh.materialBindings.resize(4);
 mesh.materials[0].baseColor={.95F,.69F,.10F,1}; // yellow pressure hull
 mesh.materials[1].baseColor={.16F,.23F,.26F,1}; // dark viewport
 mesh.materials[2].baseColor={.66F,.68F,.71F,1}; // fins/metal
 mesh.materials[3].baseColor={.98F,.97F,.84F,1}; // forward lamps
 box_part(mesh,{0,0,0},{1.85F,4.2F,1.65F},0);
 box_part(mesh,{0,1.95F,.05F},{1.3F,.4F,1.18F},1);
 box_part(mesh,{0,0,.95F},{.8F,1.25F,.46F},0);
 box_part(mesh,{-1.07F,-1.5F,-.15F},{.35F,1.0F,.3F},2);
 box_part(mesh,{1.07F,-1.5F,-.15F},{.35F,1.0F,.3F},2);
 box_part(mesh,{-0.55F,2.18F,.3F},{.25F,.2F,.25F},3);
 box_part(mesh,{0.55F,2.18F,.3F},{.25F,.2F,.25F},3);
 refresh_editor_polygon(mesh,true);sub.polygon=std::move(mesh);
 auto vessel=role("water_vessel","0-mini-sub");
 vessel.properties["craft_type"]=std::int64_t{6};
 vessel.properties["mass_kg"]=2410.0;
 vessel.properties["sub_dry_mass_kg"]=2350.0;
 vessel.properties["displacement_m3"]=2.35;
 vessel.properties["length_m"]=4.2;vessel.properties["beam_m"]=1.85;
 vessel.properties["draft_m"]=1.8;
 vessel.properties["linear_damping"]=2000.0;
 vessel.properties["angular_damping"]=900.0;
 vessel.properties["probes_length"]=std::int64_t{3};
 vessel.properties["probes_beam"]=std::int64_t{3};
 vessel.properties["thrust_newtons"]=0.0;
 vessel.properties["rudder_torque"]=0.0;
 vessel.properties["sub_ballast_kg"]=60.0;
 vessel.properties["sub_ballast_capacity_kg"]=260.0;
 vessel.properties["sub_forward_thrust_n"]=2200.0;
 vessel.properties["sub_vertical_thrust_n"]=3200.0;
 vessel.properties["sub_depth_rating_m"]=80.0;
 sub.components.push_back(std::move(vessel));d.add_object(std::move(sub));
 EditorObject ocean(d.allocate_object_id(),"Ocean Surface");
 ocean.flags.anchored=true;ocean.flags.collisionEnabled=false;
 auto settings=role("water_ocean","ocean");
 settings.properties["sea_state"]=std::int64_t{1};
 settings.properties["current_x"]=.35;settings.properties["current_y"]=.15;
 settings.properties["seafloor_z"]=-40.0;
 settings.properties["water_clarity"]=std::int64_t{0};
 ocean.components.push_back(std::move(settings));d.add_object(std::move(ocean));
 EditorObject rules(d.allocate_object_id(),"Submarine Rules");
 rules.flags.anchored=true;rules.flags.collisionEnabled=false;
 rules.components.push_back(role("water_rules","rules"));
 d.add_object(std::move(rules));
 return d;
}
} // namespace dve::editor
