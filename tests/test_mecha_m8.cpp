#include "dve/mecha_leg_ik.hpp"
#include "dve/mecha_game.hpp"
#include "dve/game_world.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/editor_prototype.hpp"
#include "dve/physics3d_backend.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <utility>
#include <vector>
#include <stdexcept>
#include <string>
using namespace dve;
using namespace dve::editor;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
float length3(Float3 a,Float3 b){
 return std::hypot(std::hypot(a.x-b.x,a.y-b.y),a.z-b.z);
}
bool finite(Float3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
Component role(const char* type,std::string key){
 auto c=make_component_from_schema(std::string("dve.prototype.")+type,1);
 check(bool(c),"missing M7 component schema");
 c->properties["key"]=std::move(key);
 return *c;
}
struct Scene {
 GameWorld world{create_physics3d_world(Physics3DBackend::Reference)};
 std::unique_ptr<MechaGame> game;
 GameObjectId ground{},player{},footLeft{},footRight{};
 explicit Scene(bool slope=false,bool makeGround=true,bool voxelFloor=false){
  std::string error;
  if(makeGround){
   auto mesh=make_editor_polygon_cube(100);
   for(auto& v:mesh.vertices){
    const float x=(v.position.x-.5F)*90.F;
    v.position={x,(v.position.y-.5F)*90.F,
               (v.position.z-.5F)*.8F-.45F+(slope?.16F*x:0.F)};
   }
   refresh_editor_polygon(mesh,true);
   ground=world.spawn_cooked_polygon_asset(std::move(mesh),"Ground",{},false);
   check(ground!=0,"create ground");
  }
  if(voxelFloor){
   GameObjectDesc w;w.name="Voxel floor";w.dynamic=false;
   w.transform.position={-4,-4,-.55F};w.voxelSizeMeters=.5F;
   w.voxels=std::make_unique<VoxelObject>(900);
   for(int x=0;x<16;++x)for(int y=0;y<16;++y)(void)w.voxels->set_voxel({x,y,0},MaterialId{2});
   ground=world.create_object(std::move(w),&error);
   check(ground!=0,"create voxel floor");
  }
  player=world.spawn_visual_polygon_asset(make_editor_polygon_cube(1),"Mecha",
      make_rigid_transform({0,0,.05F},{}));
  check(player!=0,"create mech");
  auto chassis=role("mech_player","player");
  check(bool(world.add_component(player,std::move(chassis),&error)),"mech role");
  auto target=world.spawn_visual_polygon_asset(make_editor_polygon_cube(2),"Dummy",
      make_rigid_transform({10,10,2},{}));
  check(target!=0,"create target");
  auto t=role("mech_target","dummy");t.properties["health"]=100.0;
  check(bool(world.add_component(target,std::move(t),&error)),"target role");
  GameObjectDesc rules;rules.name="Rules";
  auto c=role("mech_rules","rules");c.properties["target_count"]=std::int64_t{1};
  rules.components.push_back(std::move(c));
  check(world.create_object(std::move(rules),&error)!=0,"rules marker");
  for(int side:{-1,1}){
   for(int segment:{0,1}){
    const std::string key=std::string(side<0?"left":"right")+
                           (segment==0?"_upper":"_lower");
    auto id=world.spawn_visual_polygon_asset(make_editor_polygon_cube(
        5+(side+1)*2+segment),key,make_rigid_transform(
        {float(side),0,segment==0?1.55F:.55F},{}));
    auto leg=role("mech_leg",key);
    leg.properties["side"]=std::int64_t{side};
    leg.properties["segment"]=std::int64_t{segment};
    check(bool(world.add_component(id,std::move(leg),&error)),"leg role");
   }
   const std::string key=side<0?"left_foot":"right_foot";
   const auto id=world.spawn_visual_polygon_asset(make_editor_polygon_cube(15+side),
     key,make_rigid_transform({float(side),0,.2F},{}));
   auto foot=role("mech_foot",key);
   foot.properties["side"]=std::int64_t{side};
   check(bool(world.add_component(id,std::move(foot),&error)),"foot role");
   if(side<0)footLeft=id;else footRight=id;
  }
  game=std::make_unique<MechaGame>(world);
  check(game->start(&error),"mecha start");
 }
 void tick(unsigned n=1){for(unsigned i=0;i<n;++i)game->step(1.F/120);}
};

void moving_support_anchor_follows_translation(){
 Scene s(false,false);
 auto mesh=make_editor_polygon_cube(115);
 for(auto& v:mesh.vertices)
  v.position={(v.position.x-.5F)*12.F,(v.position.y-.5F)*12.F,
              (v.position.z-.5F)*.8F-.05F};
 refresh_editor_polygon(mesh,true);
 const auto platform=s.world.spawn_cooked_polygon_asset(
   std::move(mesh),"Kinematic-test platform",{},true);
 check(platform!=0,"dynamic polygon support exists");
 s.tick(3);
 const auto before=s.game->foot_contacts();
 check(before[0].supported&&before[1].supported,"platform grounds both feet");
 check(before[0].supportObject==platform&&before[1].supportObject==platform,
       "ankles remember actual support object");
 check(s.world.set_position(platform,{.35F,0,0}),"translate support under feet");
 s.tick();
 const auto after=s.game->foot_contacts();
 check(after[0].supportObject==platform&&after[1].supportObject==platform,
       "support binding survives motion");
 check(after[0].ankle.x-before[0].ankle.x>.23F,
       "left planted ankle follows support transform, not fixed world coordinate");
 check(after[1].ankle.x-before[1].ankle.x>.23F,
       "right planted ankle follows support translation");
 check(s.world.destroy_object(platform),"destroy moving platform");
 s.tick();
 const auto released=s.game->foot_contacts();
 check(!released[0].planted&&!released[1].planted,
       "destroyed support releases both contact anchors");
 check(released[0].supportObject==0&&released[1].supportObject==0,
       "destroyed platform IDs are cleared");
}
void footstep_events_are_grounded_and_bounded(){
 Scene s;
 check(s.game->footstep_events().empty(),"initial scene emits no false footstep");
 s.tick(20);
 check(s.game->footstep_events().empty(),"standing still emits no footsteps");
 s.world.set_axis("move_y",1.F);
 unsigned events=0;
 for(int i=0;i<450;++i){
  s.tick();
  const auto reported=s.game->footstep_events();
  check(reported.size()<=2,"at most two footfall events per fixed step");
  for(const auto& e:reported){
   check(e.side==-1||e.side==1,"valid left/right footstep source");
   check(e.supportObject==s.ground,"footsteps report the contacting surface");
   check(e.intensity>=.15F&&e.intensity<=1.F,"intensity bounded");
   check(finite(e.position)&&finite(e.normal),"event has finite frame");
   ++events;
  }
 }
 check(events>=2,"walking generates left/right landing events");
 s.world.set_axis("move_y",0.F);
 s.tick(240);
 check(s.game->footstep_events().empty(),"standing still stops emitting footsteps");
}
void ankle_articulation_has_mechanical_limit(){
 const auto shallow=mech_limit_ankle_normal({.2F,0,.98F});
 check(shallow.x>.15F&&shallow.z>.9F,"gentle slope preserves actual normal");
 const auto steep=mech_limit_ankle_normal({.98F,0,.2F});
 const float tilt=std::atan2(std::hypot(steep.x,steep.y),steep.z);
 check(std::abs(tilt-.61F)<.002F,"extreme slope ankle tilt is capped");
 const auto invalid=mech_limit_ankle_normal({0,0,-1});
 check(invalid.z==1.F,"downward surface normals never flip an ankle");
}
void checkpoint_rebinds_moving_support_without_resetting_footfall(){
 Scene s;
 const auto snapshot=s.game->save_state();
 check(snapshot.starts_with("DVE_MECHA 3 "),"M8 keeps existing M7 controller envelope");
 std::string error;
 if(!s.game->after_restore(snapshot,&error))throw std::runtime_error("M8 save reload: "+error);
 check(s.game->footstep_events().empty(),"loading does not play a false footstep");
 const auto feet=s.game->foot_contacts();
 check(feet[0].supportObject==s.ground&&feet[1].supportObject==s.ground,
       "save load rebinds support anchors to restored world");
 check(s.game->save_state()==snapshot,"restoring support-local cache is save transparent");
}
}
int main(){
 try {
  moving_support_anchor_follows_translation();
  footstep_events_are_grounded_and_bounded();
  ankle_articulation_has_mechanical_limit();
  checkpoint_rebinds_moving_support_without_resetting_footfall();
  std::cout<<"M8 moving-platform contacts and footstep events passed\n";
  return 0;
 }catch(const std::exception& e){
  std::cerr<<"mecha M8: "<<e.what()<<'\n';return 1;
 }
}
