#include "dve/editor_prototype.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/editor_polygon.hpp"
#include <stdexcept>
namespace dve::editor {
namespace {
Component role(const char* name,std::string key){
 auto c=make_component_from_schema(std::string("dve.prototype.")+name,1);
 if(!c)throw std::runtime_error("W1 water component registration missing");
 c->properties["key"]=std::move(key);return *c;
}
void part(CookedPolygonAsset& mesh,Float3 at,Float3 size,unsigned material){
 const auto cube=make_editor_polygon_cube(1);
 const auto first=unsigned(mesh.indices.size()),base=unsigned(mesh.vertices.size());
 for(auto v:cube.vertices){
    v.position=add(at,{(v.position.x-.5F)*size.x,(v.position.y-.5F)*size.y,(v.position.z-.5F)*size.z});
    mesh.vertices.push_back(v);
 }
 for(auto i:cube.indices)mesh.indices.push_back(i+base);
 mesh.submeshes.push_back({"Part",first,unsigned(cube.indices.size()),material});
}
CookedPolygonAsset hull(EditorObjectId id,bool pirate){
 CookedPolygonAsset m;m.objectId=id;m.materials.resize(4);m.materialBindings.resize(4);
 m.materials[0].baseColor=pirate?Float4{.31F,.16F,.08F,1}:Float4{.11F,.32F,.45F,1};
 m.materials[1].baseColor={.52F,.33F,.18F,1};
 m.materials[2].baseColor={.83F,.77F,.60F,1};
 m.materials[3].baseColor={.13F,.15F,.16F,1};
 if(pirate){
   part(m,{0,0,-.62F},{7,26,2.9F},0);
   part(m,{0,0,.95F},{6.8F,24.8F,.38F},1);
   for(float x:{-3.2F,3.2F})part(m,{x,0,.56F},{.35F,26.F,1.3F},1);
   part(m,{0,-6.5F,2.1F},{5.8F,6,2.7F},0);
   part(m,{0,7.2F,1.45F},{5.6F,4,1.6F},0);
   for(float y:{-6.5F,2.6F,8.0F}){
      part(m,{0,y,7.2F},{.43F,.43F,13.F},3);
      part(m,{0,y,8.5F},{6.5F,.16F,5.0F},2);
   }
   part(m,{0,13.6F,.2F},{1.2F,3.2F,1.1F},1);
 }else{
   part(m,{0,0,-.18F},{1.8F,4.3F,.75F},0);
   part(m,{0,-.5F,.28F},{1.65F,2.8F,.24F},1);
   part(m,{0,-.7F,.63F},{.8F,.9F,.62F},3);
   part(m,{0,1.85F,.23F},{1.0F,.2F,.18F},2);
 }
 refresh_editor_polygon(m,true);return m;
}
}
EditorDocument make_water_starter(){
 EditorDocument d("W4 Coastal Surf and Ships Range");
 auto cube=[&](std::string name,Float3 pos,Float3 size,Float4 color,bool collision){
   EditorObject o(d.allocate_object_id(),std::move(name));
   o.flags.anchored=true;o.flags.collisionEnabled=collision;o.transform.position=pos;
   o.polygon=make_editor_polygon_cube(o.id);
   for(auto& v:o.polygon->vertices){
     v.position={(v.position.x-.5F)*size.x,(v.position.y-.5F)*size.y,(v.position.z-.5F)*size.z};
   }
   o.polygon->materials[0].baseColor=color;refresh_editor_polygon(*o.polygon,true);
   return d.add_object(std::move(o)).id;
 };
 // Land is real static collision. Open water itself is an analytic surface, not a box.
 cube("Rocky breakwater left",{-90,140,2.5F},{80,24,5},{.38F,.37F,.31F,1},true);
 cube("Rocky breakwater right",{90,140,2.5F},{80,24,5},{.40F,.39F,.32F,1},true);
 cube("Harbor dock",{0,140,3},{50,9,6},{.41F,.28F,.15F,1},true);
 cube("Port light",{-20,134,8},{2,2,10},{.90F,.72F,.2F,1},false);
 cube("Starboard light",{20,134,8},{2,2,10},{.18F,.72F,.39F,1},false);
 auto vessel=[&](bool pirate){
   EditorObject o(d.allocate_object_id(),pirate?"Pirate Ship":"Dinghy");
   o.flags.anchored=true;o.flags.collisionEnabled=false;
   o.transform.position=pirate?Float3{23,-25,.55F}:Float3{-23,-25,.22F};
   o.polygon=hull(o.id,pirate);
   auto c=role("water_vessel",pirate?"1-pirate":"0-dinghy");
   if(pirate){
     c.properties["mass_kg"]=24000.0;
     c.properties["displacement_m3"]=27.0;
     c.properties["length_m"]=26.0;
     c.properties["beam_m"]=7.0;
     c.properties["draft_m"]=3.2;
     c.properties["linear_damping"]=6500.0;
     c.properties["angular_damping"]=85000.0;
     c.properties["probes_length"]=std::int64_t{9};
     c.properties["probes_beam"]=std::int64_t{5};
     c.properties["thrust_newtons"]=22000.0;
     c.properties["rudder_torque"]=90000.0;
     c.properties["craft_type"]=std::int64_t{2};
     c.properties["thrust_newtons"]=0.0;
     c.properties["sail_area_m2"]=420.0;
     c.properties["keel_area_m2"]=14.0;
     c.properties["mast_height_m"]=8.0;
   }else{
     c.properties["mass_kg"]=480.0;
     c.properties["displacement_m3"]=.56;
     c.properties["length_m"]=4.3;
     c.properties["beam_m"]=1.8;
     c.properties["draft_m"]=.80;
     c.properties["probes_length"]=std::int64_t{3};
     c.properties["probes_beam"]=std::int64_t{3};
     c.properties["thrust_newtons"]=1400.0;
     c.properties["rudder_torque"]=380.0;
     c.properties["craft_type"]=std::int64_t{0};
   }
   o.components.push_back(std::move(c));d.add_object(std::move(o));
 };
 vessel(false);vessel(true);
 auto more=[&](int kind,const char* name,std::string key,Float3 position){
   EditorObject o(d.allocate_object_id(),name);
   o.flags.anchored=true;o.flags.collisionEnabled=false;
   o.transform.position=position;
   CookedPolygonAsset mesh=hull(o.id,false);
   Component c=role("water_vessel",std::move(key));
   c.properties["craft_type"]=std::int64_t(kind);
   if(kind==1){ // sailboat with working mast force, keel and heeling moment
     part(mesh,{0,0,3.3F},{.16F,.16F,6.7F},3);
     part(mesh,{0,.15F,3.5F},{.10F,3.8F,5.0F},2);
     c.properties["mass_kg"]=1800.0;c.properties["displacement_m3"]=2.25;
     c.properties["length_m"]=8.0;c.properties["beam_m"]=2.6;c.properties["draft_m"]=1.2;
     c.properties["linear_damping"]=320.0;c.properties["angular_damping"]=1000.0;
     c.properties["probes_length"]=std::int64_t{5};c.properties["probes_beam"]=std::int64_t{3};
     c.properties["thrust_newtons"]=0.0;c.properties["rudder_torque"]=2600.0;
     c.properties["sail_area_m2"]=29.0;c.properties["keel_area_m2"]=1.6;
     c.properties["mast_height_m"]=4.2;
   }else if(kind==3){ // speedboat, with large stern drive and hydrodynamic plane
     part(mesh,{0,-.1F,.60F},{1.1F,2.1F,.6F},3);
     part(mesh,{0,-1.95F,-.15F},{1.4F,.40F,.3F},1);
     c.properties["mass_kg"]=2700.0;c.properties["displacement_m3"]=3.3;
     c.properties["length_m"]=8.0;c.properties["beam_m"]=2.5;c.properties["draft_m"]=1.1;
     c.properties["linear_damping"]=350.0;c.properties["angular_damping"]=1800.0;
     c.properties["probes_length"]=std::int64_t{5};c.properties["probes_beam"]=std::int64_t{3};
     c.properties["thrust_newtons"]=24000.0;c.properties["rudder_torque"]=1800.0;
     c.properties["planing_factor"]=.06;
   }else if(kind==4){ // wave runner: short, nimble and steerable pump jet
     part(mesh,{0,-.6F,.55F},{.7F,1.3F,.55F},3);
     part(mesh,{0,1.1F,.5F},{.65F,.35F,.35F},1);
     c.properties["mass_kg"]=420.0;c.properties["displacement_m3"]=.53;
     c.properties["length_m"]=3.1;c.properties["beam_m"]=1.35;c.properties["draft_m"]=.55;
     c.properties["linear_damping"]=115.0;c.properties["angular_damping"]=220.0;
     c.properties["probes_length"]=std::int64_t{3};c.properties["probes_beam"]=std::int64_t{3};
     c.properties["thrust_newtons"]=5500.0;c.properties["rudder_torque"]=350.0;
     c.properties["planing_factor"]=.085;
   }else if(kind==5){ // hand-controlled surfboard with one rider silhouette
     mesh.vertices.clear();mesh.indices.clear();mesh.submeshes.clear();
     part(mesh,{0,0,-.04F},{.73F,2.2F,.12F},0);
     part(mesh,{0,-.1F,.7F},{.32F,.38F,1.1F},2);
     part(mesh,{0,-.1F,1.38F},{.28F,.28F,.3F},1);
     c.properties["mass_kg"]=95.0;c.properties["displacement_m3"]=.12;
     c.properties["length_m"]=2.2;c.properties["beam_m"]=.73;c.properties["draft_m"]=.24;
     c.properties["linear_damping"]=80.0;c.properties["angular_damping"]=70.0;
     c.properties["probes_length"]=std::int64_t{4};c.properties["probes_beam"]=std::int64_t{2};
     c.properties["thrust_newtons"]=0.0;c.properties["rudder_torque"]=0.0;
     c.properties["planing_factor"]=0.0;
   }
   refresh_editor_polygon(mesh,true);o.polygon=std::move(mesh);
   o.components.push_back(std::move(c));d.add_object(std::move(o));
 };
 more(1,"Sailboat","2-sailboat",{-55,-80,.28F});
 more(3,"Speedboat","3-speedboat",{53,-80,.35F});
 more(4,"Wave Runner","4-wave-runner",{0,-116,.18F});
 more(5,"Surfboard","5-surfboard",{0,-330,.15F});
 // Waterline at Y=-240. The model shoals offshore in -Y and becomes dry
 // beneath the sand; game and water renderer use the same depth query.
 cube("Surfing Beach",{0,-210,-.6F},{300,60,1.2F},{.78F,.69F,.47F,1},true);
 EditorObject coast(d.allocate_object_id(),"Surf Beach Waves");
 coast.flags.anchored=true;coast.flags.collisionEnabled=false;
 coast.transform.position={0,-240,0};
 auto surf=role("water_coast","coast-training");
 surf.properties["half_width_m"]=150.0;
 surf.properties["offshore_length_m"]=240.0;
 surf.properties["beach_slope"]=.05;
 surf.properties["swell_height_m"]=1.8;
 surf.properties["period_s"]=9.0;
 surf.properties["peel"]=.08;
 coast.components.push_back(std::move(surf));d.add_object(std::move(coast));
 EditorObject ocean(d.allocate_object_id(),"Ocean Surface");
 ocean.flags.anchored=true;ocean.flags.collisionEnabled=false;
 auto w=role("water_ocean","ocean");w.properties["sea_state"]=std::int64_t{1};
 w.properties["wind_x"]=5.0;w.properties["wind_y"]=8.0;
 ocean.components.push_back(std::move(w));d.add_object(std::move(ocean));
 EditorObject rules(d.allocate_object_id(),"Water Rules");
 rules.flags.anchored=true;rules.flags.collisionEnabled=false;
 rules.components.push_back(role("water_rules","rules"));d.add_object(std::move(rules));
 return d;
}
}
