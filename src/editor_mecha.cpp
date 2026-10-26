#include "dve/editor_prototype.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/editor_polygon.hpp"
#include <stdexcept>
#include <string>
namespace dve::editor {
namespace {
Component role(const char* name,std::string key){
 auto c=make_component_from_schema(std::string("dve.prototype.")+name,1);
 if(!c)throw std::runtime_error("Mecha component schema unavailable");
 c->properties["key"]=std::move(key);return *c;
}
void mesh_part(CookedPolygonAsset& out,Float3 pos,Float3 size,unsigned mat){
 auto cube=make_editor_polygon_cube(1);
 const auto base=unsigned(out.vertices.size()),first=unsigned(out.indices.size());
 for(auto v:cube.vertices){
  v.position={pos.x+(v.position.x-.5F)*size.x,
              pos.y+(v.position.y-.5F)*size.y,
              pos.z+(v.position.z-.5F)*size.z};
  out.vertices.push_back(v);
 }
 for(auto n:cube.indices)out.indices.push_back(n+base);
 out.submeshes.push_back({"Part",first,unsigned(cube.indices.size()),mat});
}
}
EditorDocument make_mecha_starter(){
 EditorDocument d("Iron Yard - Mecha Combat M1-M4");
 auto box=[&](std::string label,Float3 pos,Float3 size,Float4 color,bool collide)->EditorObject&{
  EditorObject o(d.allocate_object_id(),std::move(label));
  o.flags.anchored=true;o.flags.collisionEnabled=collide;o.transform.position=pos;
  o.polygon=make_editor_polygon_cube(o.id);
  for(auto& v:o.polygon->vertices)v.position={(v.position.x-.5F)*size.x,
      (v.position.y-.5F)*size.y,(v.position.z-.5F)*size.z};
  o.polygon->materials[0].baseColor=color;refresh_editor_polygon(*o.polygon,true);
  return d.add_object(std::move(o));
 };
 box("Industrial ground",{0,12,-.4F},{78,95,.8F},{.18F,.21F,.25F,1},true);
 box("Left boundary",{-38,12,2},{1,95,4},{.31F,.33F,.36F,1},true);
 box("Right boundary",{38,12,2},{1,95,4},{.31F,.33F,.36F,1},true);
 box("Rear boundary",{0,59,2},{76,1,4},{.31F,.33F,.36F,1},true);
 box("Freight container A",{-12,11,2},{5,8,4},{.56F,.28F,.13F,1},true);
 box("Freight container B",{12,22,2},{8,4,4},{.22F,.38F,.56F,1},true);
 box("Warehouse wall",{-11,37,3},{14,1.8F,6},{.43F,.43F,.42F,1},true);
 // M3: exported voxel cover rather than unmodifiable polygon facades.
 auto voxelWall=[&](std::string name,Float3 pos,int halfWidth){
  EditorObject o(d.allocate_object_id(),std::move(name));
  o.flags.anchored=true;o.flags.collisionEnabled=true;o.flags.structural=true;
  o.transform.position=pos;o.voxelSizeMeters=.65F;
  for(int x=-halfWidth;x<=halfWidth;++x)
   for(int y=-1;y<=1;++y)
    for(int z=0;z<7;++z)
     (void)o.voxels->set_voxel({x,y,z},MaterialId{2});
  d.add_object(std::move(o));
 };
 voxelWall("Destructible Front Barricade",{0,-3,.03F},8);
 voxelWall("Destructible Warehouse Cover",{7,39,.03F},6);

 // Kinematic player: model has no rigid body, the runtime moves a swept capsule.
 auto& chassis=box("Player Mech Chassis",{0,-23,.05F},{2.7F,3.5F,2.1F},
   {.13F,.48F,.52F,1},false);
 chassis.components.push_back(role("mech_player","player"));
 // Replace the placeholder body with a readable two-leg silhouette.
 CookedPolygonAsset legs;legs.objectId=chassis.id;legs.materials.resize(2);legs.materialBindings.resize(2);
 legs.materials[0].baseColor={.14F,.44F,.51F,1};
 legs.materials[1].baseColor={.13F,.15F,.20F,1};
 mesh_part(legs,{-1,0,1.F},{.85F,1.4F,2.F},0);
 mesh_part(legs,{1,0,1.F},{.85F,1.4F,2.F},0);
 mesh_part(legs,{0,0,2.0F},{2.7F,1.4F,.8F},1);
 refresh_editor_polygon(legs,true);chassis.polygon=std::move(legs);

 auto& torso=box("Swiveling Cannon Turret",{0,-23,2.05F},{3,2,1.4F},
   {.18F,.65F,.70F,1},false);
 torso.components.push_back(role("mech_torso","turret"));
 CookedPolygonAsset upper;upper.objectId=torso.id;upper.materials.resize(3);
 upper.materialBindings.resize(3);
 upper.materials[0].baseColor={.17F,.63F,.70F,1};
 upper.materials[1].baseColor={.10F,.13F,.16F,1};
 upper.materials[2].baseColor={.92F,.68F,.21F,1};
 mesh_part(upper,{0,0,0},{2.7F,2.2F,1.25F},0);
 mesh_part(upper,{-1.1F,2.1F,.2F},{.55F,3.2F,.55F},1);
 mesh_part(upper,{1.2F,1.1F,.1F},{1.F,2.6F,.9F},2);
 refresh_editor_polygon(upper,true);torso.polygon=std::move(upper);

 const Float3 positions[]={{-16,14,1.6F},{15,34,1.6F},{0,50,1.6F}};
 for(int i=0;i<3;++i){
  auto& target=box("Target Walker "+std::to_string(i+1),positions[i],
   {2.4F,2.2F,3.2F},{.74F,.28F,.24F,1},false);
  target.components.push_back(role("mech_target","target"+std::to_string(i+1)));
  target.components.back().properties["health"]=i==2?160.0:90.0;
  target.components.back().properties["hit_radius"]=1.7;
   target.components.back().properties["ai_kind"]=std::int64_t(i==0?1:i==1?2:3);
   target.components.back().properties["move_speed"]=i==0?5.0:i==1?2.8:0.0;
   target.components.back().properties["attack_range"]=i==2?38.0:30.0;
   target.components.back().properties["damage_per_shot"]=i==1?8.0:4.0;
   target.components.back().properties["shot_interval"]=i==1?2.0:1.4;
 }
 EditorObject rules(d.allocate_object_id(),"Mecha Rules");
 rules.flags.anchored=true;rules.flags.collisionEnabled=false;
 rules.components.push_back(role("mech_rules","rules"));
 rules.components.back().properties["target_count"]=std::int64_t{3};
 d.add_object(std::move(rules));
 return d;
}
} // namespace dve::editor
