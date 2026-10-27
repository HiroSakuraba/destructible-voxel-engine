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
void analytic_ik_contract(){
 constexpr float epsilon=2.e-4F;
 MechLegIkConfig lengths;
 const Float3 hip{0,0,2},foot{.25F,.4F,.2F};
 const auto pose=solve_mech_leg_ik(hip,foot,{0,1,0},lengths);
 check(bool(pose),"ordinary IK solve");
 check(length3(pose->hip,pose->knee)-lengths.thighLength<epsilon,
   "thigh joint length maintained");
 check(std::abs(length3(pose->hip,pose->knee)-lengths.thighLength)<epsilon,
   "thigh link is rigid");
 check(std::abs(length3(pose->knee,pose->ankle)-lengths.shinLength)<epsilon,
   "shin link is rigid");
 check(pose->knee.y>pose->hip.y,"forward pole biases knee");
 const auto far=solve_mech_leg_ik(hip,{0,0,-100},{0,1,0},lengths);
 check(bool(far)&&far->reachClamped,"unreachable ground clamps reach");
 check(length3(far->hip,far->ankle)<2.26F,"unreachable does not stretch");
 const auto folded=solve_mech_leg_ik(hip,hip,{0,0,1},lengths);
 check(bool(folded)&&finite(folded->knee),"singular folded pose is finite");
 check(!solve_mech_leg_ik(hip,foot,{0,1,0},MechLegIkConfig{0,1}),
   "invalid limb geometry rejected");
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
void planted_world_stability_and_swing(){
 Scene s;
 auto feet=s.game->foot_contacts();
 check(feet[0].planted&&feet[1].planted,"feet start supported on floor");
 check(feet[0].ankle.x<0&&feet[1].ankle.x>0,"left/right order correct");
 s.tick(20);
 const auto stationary=s.game->foot_contacts();
 check(length3(stationary[0].ankle,feet[0].ankle)<1.e-5F,
       "stationary planted foot does not slide");
 s.world.set_axis("move_y",1.F);
 s.tick(12);
 const auto moving=s.game->foot_contacts();
 check(moving[0].supported||moving[1].supported,
       "at least one foot has a tested landing");
 check(length3(s.world.position(s.footLeft).value(),moving[0].ankle)<.02F,
       "visual foot pad follows IK ankle");
 const auto upper=s.world.position(s.footRight);
 check(bool(upper)&&finite(*upper),"opposite visual foot remains finite");
 s.world.set_axis("move_y",0.F);
 s.tick(50);
 const auto idle=s.game->foot_contacts();
 check(idle[0].supported&&idle[1].supported,
       "foot placement reattaches after walking");
}
void bone_roll_follows_chassis_heading(){
 // The visual limb boxes are not square: their roll about the bone axis must follow the
 // chassis heading, not the (arbitrary) tilt direction of the bone.
 const MechLegIkConfig cfg;
 for(const float yaw:{0.F,.6F,1.1F,2.3F,-1.7F}){
  const auto q=quaternion_from_axis_angle({0,0,1},yaw);
  const Float3 hip=rotate(q,{1,0,2.05F}),foot=rotate(q,{1.12F,.62F,.16F});
  const Float3 pole=rotate(q,{.16F,1,.03F}),forward=rotate(q,{0,1,0});
  const auto pose=solve_mech_leg_ik(hip,foot,pole,cfg,forward);
  check(bool(pose),"roll pose");
  // A downward bone's +X is mirrored, which a symmetric box cannot show; only the axis matters.
  const auto thighAxis=rotate(pose->thighRotation,{0,0,1});
  const Float3 thigh{pose->knee.x-pose->hip.x,pose->knee.y-pose->hip.y,pose->knee.z-pose->hip.z};
  check(thighAxis.x*thigh.x+thighAxis.y*thigh.y+thighAxis.z*thigh.z>cfg.thighLength*.999F,
        "thigh local Z still lies along the bone");
  for(const auto& bone:{pose->thighRotation,pose->shinRotation}){
   const auto x=rotate(bone,{1,0,0});
   const auto side=rotate(q,{1,0,0});
   check(std::abs(x.x*side.x+x.y*side.y+x.z*side.z)>.9F,
         "bone local X stays on the chassis lateral axis for every heading");
  }
 }
}
void feet_never_teleport_while_walking_or_stopping(){
 // Stopping in the middle of a stride used to pop a swinging foot (and a foot the hip had outrun)
 // up to ~1.2 m in a single tick. Planted feet may not slide beyond a small drag and a stopped mech
 // may only finish its step at a bounded speed.
 float worstStopped=0.F,worstPlantedSlide=0.F,worstReach=0.F;
 for(unsigned stop=10;stop<90;stop+=3){
  Scene s;
  s.world.set_axis("move_y",1.F);
  auto previous=s.game->foot_contacts();
  unsigned idleTicks=0;
  for(unsigned i=0;i<stop+260;++i){
   if(i==stop)s.world.set_axis("move_y",0.F);
   s.tick();
   idleTicks=std::hypot(s.game->motor().velocity.x,s.game->motor().velocity.y)<1.e-4F?idleTicks+1:0;
   const bool stoppedBefore=idleTicks>90;
   const auto now=s.game->foot_contacts();
   for(int f=0;f<2;++f){
    const float d=length3(now[f].ankle,previous[f].ankle);
    const auto& m=s.game->motor();
    const auto off=rotate(quaternion_from_axis_angle({0,0,1},m.legsYaw),{f?1.F:-1.F,0,2.05F});
    const Float3 hip{m.position.x+off.x,m.position.y+off.y,m.position.z+off.z};
    worstReach=std::max(worstReach,length3(hip,now[f].ankle));
    if(idleTicks>0)worstStopped=std::max(worstStopped,d);
    if(stoppedBefore&&d>=1.e-5F)std::cout<<"stop "<<stop<<" i "<<i<<" foot "<<f<<" d "<<d<<" planted "<<now[f].planted<<" "<<now[f].ankle.y<<","<<now[f].ankle.z<<" idle "<<idleTicks<<"\n";
    if(previous[f].planted&&now[f].planted)worstPlantedSlide=std::max(worstPlantedSlide,d);
   }
   previous=now;
  }
 }
 check(worstReach<MechLegIkConfig{}.thighLength+MechLegIkConfig{}.shinLength-.04F,
       "walking and stopping never over-extend the legs (pads stay attached)");
 check(worstStopped<.07F,"stopping mid-stride finishes the step without teleporting");
 check(worstPlantedSlide<.12F,"planted feet are never teleported");
}
void slope_normal_and_height(){
 Scene s(true);
 const auto feet=s.game->foot_contacts();
 check(feet[0].supported&&feet[1].supported,"slope supports both feet");
 check(feet[1].ankle.z>feet[0].ankle.z+.16F,
       "feet independently follow ground incline");
 check(std::abs(feet[0].normal.x)>.05F&&feet[0].normal.z>.9F,
       "ground normal follows actual polygon slope");
 check(length3(s.world.position(s.footLeft).value(),feet[0].ankle)<.02F,
       "left visual foot pad has real contact position");
}
void unsupported_contact_releases_plant(){
 Scene s;
 check(s.game->foot_contacts()[0].planted,"plant initially supported");
 check(s.world.destroy_object(s.ground),"destroy support beneath the rig");
 s.tick(2);
 const auto feet=s.game->foot_contacts();
 check(!feet[0].supported&&!feet[1].supported,"lost floor releases both feet");
 check(!feet[0].planted&&!feet[1].planted,"unsupported feet cannot remain planted");
 check(finite(feet[0].ankle)&&finite(feet[1].ankle),"unsupported IK stays finite");
}
void destroyed_voxel_floor_releases_planted_feet(){
 Scene s(false,false,true);
 const auto before=s.game->foot_contacts();
 check(before[0].planted&&before[1].planted,"feet plant on a voxel floor");
 check(std::abs(before[0].ankle.z-.065F)<.06F,"voxel floor height measured");
 s.tick(20);
 check(s.game->foot_contacts()[0].planted,"voxel contact persists");
 check(bool(s.world.damage_sphere(s.ground,{0,0,-.3F},2.2F)),"carve the supporting voxels");
 s.tick(3);
 const auto after=s.game->foot_contacts();
 check(!after[0].planted&&!after[1].planted&&!after[0].supported&&!after[1].supported,
       "carving the voxels under planted feet releases both");
 check(finite(after[0].ankle)&&finite(after[1].ankle),"released feet stay finite");
 check(length3(s.world.position(s.footLeft).value(),after[0].ankle)<.02F,
       "visual pad follows the retracted foot");
}
void controller_compatibility(){
 Scene s;
 auto current=s.game->save_state();
 check(current.starts_with("DVE_MECHA 3 "),"M7 save schema v3");
 std::string error;
 check(s.game->after_restore(current,&error),"M7 restore");
 check(s.game->save_state()==current,"foot contacts round trip bit-for-bit");
 const auto preserved=s.game->save_state();
 auto corrupted=current;
 const auto pos=corrupted.find("M7 2");
 check(pos!=std::string::npos,"M7 section is present");
 corrupted.replace(pos,4,"M7 9");
 check(!s.game->after_restore(corrupted,&error),"invalid M7 count rejected");
 check(s.game->save_state()==preserved,"rejected controller restore is atomic");
 // M6 saves lack the appended foot section; initialize plants from the restored world.
 const auto v3pos=current.find("M7 2");
 auto v2=current.substr(0,v3pos);
 v2.replace(10,1,"2");
 check(s.game->after_restore(v2,&error),"M6 controller still accepted");
 check(s.game->foot_contacts()[0].supported,"M6 migration rebuilds ground contacts");
 // Original M5 payload has no M6 or M7 extension.
 const auto m6=v2.find("M6 ");
 auto v1=v2.substr(0,m6);
 v1.replace(10,1,"1");
 check(s.game->after_restore(v1,&error),"M5 controller still accepted");
 check(s.game->foot_contacts()[1].supported,"M5 migration rebuilds terrain contacts");
}
std::string replace_foot_token(std::string save,int foot,int token,const std::string& value){
 const auto start=save.find("M7 2\n")+5;
 std::size_t line=start;
 for(int i=0;i<foot;++i)line=save.find('\n',line)+1;
 const auto end=save.find('\n',line);
 std::istringstream in(save.substr(line,end-line));
 std::vector<std::string> tokens;for(std::string t;in>>t;)tokens.push_back(t);
 tokens.at(std::size_t(token))=value;
 std::string joined;for(const auto& t:tokens)joined+=(joined.empty()?"":" ")+t;
 return save.replace(line,end-line,joined);
}
void foot_save_validation_and_determinism(){
 const auto run=[](unsigned ticks){
  Scene s;s.world.set_axis("move_y",1.F);s.tick(ticks);
  return s.game->save_state();
 };
 check(run(37)==run(37),"identical inputs give an identical controller save");
 Scene s;s.world.set_axis("move_y",1.F);s.tick(37);
 const auto mid=s.game->save_state();
 std::string error;
 check(s.game->after_restore(mid,&error),"mid-stride save restores");
 check(s.game->save_state()==mid,"mid-stride foot state round trips");
 s.tick(60); // the restored feet keep walking without a pop
 for(const auto& foot:s.game->foot_contacts())check(finite(foot.ankle),"restored feet stay finite");
 const auto before=s.game->save_state();
 // Token layout per foot: initialized planted supported swing ankle(3) from(3) to(3) normal(3).
 const std::pair<int,const char*> bad[]{
  {1,"2"},{2,"2"},{3,"7"},{4,"inf"},{4,"nan"},{7,"1e30"},{10,"-9e9"},{13,"0"},{13,"9"},{14,"abc"}};
 for(int foot=0;foot<2;++foot)for(const auto& [token,value]:bad){
  const auto corrupted=replace_foot_token(mid,foot,token,value);
  check(!s.game->after_restore(corrupted,&error),"malformed foot state is rejected");
  check(s.game->save_state()==before,"a rejected foot state leaves the controller untouched");
 }
 // planted feet cannot be mid-swing, and cannot be unsupported or uninitialized.
 auto planted=mid;
 for(int foot=0;foot<2;++foot){
  planted=replace_foot_token(planted,foot,0,"1");planted=replace_foot_token(planted,foot,1,"1");
  planted=replace_foot_token(planted,foot,2,"1");planted=replace_foot_token(planted,foot,3,"1");
 }
 check(!s.game->after_restore(planted,&error),"a planted foot cannot also be swinging");
 auto hovering=mid;
 for(int foot=0;foot<2;++foot){
  hovering=replace_foot_token(hovering,foot,1,"1");hovering=replace_foot_token(hovering,foot,2,"0");
  hovering=replace_foot_token(hovering,foot,3,"0");
 }
 check(!s.game->after_restore(hovering,&error),"a planted foot must be supported");
 check(!s.game->after_restore(mid+"extra",&error),"trailing data after the foot section is rejected");
 check(!s.game->after_restore(mid.substr(0,mid.rfind('\n',mid.size()-2)+1),&error),
       "a truncated foot section is rejected");
}
void prefab_validation(){
 auto starter=make_mecha_starter();std::string error;
 check(validate_editor_prototype(starter,&error),"M7 starter validates");
 unsigned feet=0,legs=0;
 for(const auto& [id,obj]:starter.objects()){
  (void)id;
  if(find_component_by_type(std::span<const Component>(obj.components),"dve.prototype.mech_foot"))++feet;
  if(find_component_by_type(std::span<const Component>(obj.components),"dve.prototype.mech_leg"))++legs;
 }
 check(feet==2&&legs==4,"Iron Yard has two foot pads and four IK segments");
 auto invalid=make_mecha_starter();
 for(const auto& [id,obj]:invalid.objects()){
  (void)obj;auto* o=invalid.find_object(id);
  if(auto* foot=find_component_by_type(std::span<Component>(o->components),"dve.prototype.mech_foot")){
   foot->properties["side"]=std::int64_t{0};break;
  }
 }
 check(!validate_editor_prototype(invalid,&error),"invalid foot side rejected");
}
}
int main(){
 try{
  analytic_ik_contract();
  planted_world_stability_and_swing();
  bone_roll_follows_chassis_heading();
  feet_never_teleport_while_walking_or_stopping();
  slope_normal_and_height();
  unsupported_contact_releases_plant();
  destroyed_voxel_floor_releases_planted_feet();
  controller_compatibility();
  foot_save_validation_and_determinism();
  prefab_validation();
  std::cout<<"M7: leg IK, planted contacts, slopes, lost supports and saves passed\n";
  return 0;
 }catch(const std::exception& e){
  std::cerr<<"mecha M7: "<<e.what()<<"\n";return 1;
 }
}
