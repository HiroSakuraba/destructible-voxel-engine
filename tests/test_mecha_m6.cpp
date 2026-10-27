#include "dve/mecha_game.hpp"
#include "dve/game_world.hpp"
#include "dve/editor_prototype.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/physics3d_backend.hpp"
#include <cmath>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
using namespace dve;
using namespace dve::editor;
namespace {
void require(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
Component role(const char* type,const char* key){
 auto result=make_component_from_schema(std::string("dve.prototype.")+type,1);
 require(bool(result),std::string("missing component schema ")+type);
 result->properties["key"]=std::string(key);
 return *result;
}
float distance(Float3 a,Float3 b){
 return std::hypot(std::hypot(a.x-b.x,a.y-b.y),a.z-b.z);
}
struct Fixture{
 GameWorld world{create_physics3d_world(Physics3DBackend::Reference)};
 std::unique_ptr<MechaGame> game;
 GameObjectId player{},enemyA{},enemyB{},leftShin{},rightShin{};
 explicit Fixture(bool thirdPerson=true,bool withFloor=true,bool addWall=false,
                  bool addLegs=true){
  std::string error;
  if(withFloor){
   auto floor=make_editor_polygon_cube(100);
   for(auto& v:floor.vertices)
    v.position={(v.position.x-.5F)*100.F,(v.position.y-.5F)*100.F,
                (v.position.z-.5F)*1.F-.5F};
   refresh_editor_polygon(floor,true);
   require(world.spawn_cooked_polygon_asset(std::move(floor),"Ground",{},false)!=0,
     "floor creation");
  }
  if(addWall){
   auto wall=make_editor_polygon_cube(101);
   for(auto& v:wall.vertices)
    v.position={(v.position.x-.5F)*40.F,(v.position.y-.5F)*1.F-5.F,
                (v.position.z-.5F)*10.F+3.F};
   refresh_editor_polygon(wall,true);
   require(world.spawn_cooked_polygon_asset(std::move(wall),"Behind camera",{},false)!=0,
     "cover creation");
  }
  player=world.spawn_visual_polygon_asset(make_editor_polygon_cube(1),"Player",
       make_rigid_transform({0,0,.05F},{}));
  require(player!=0,"player creation");
  auto main=role("mech_player","player");
  main.properties["camera_mode"]=std::int64_t{thirdPerson?1:0};
  require(bool(world.add_component(player,std::move(main),&error)),"player role: "+error);
  auto enemy=[&](const char* key,Float3 at,int asset){
   const auto id=world.spawn_visual_polygon_asset(make_editor_polygon_cube(asset),
        key,make_rigid_transform(at,{}));
   auto config=role("mech_target",key);
   config.properties["health"]=36.0;
   config.properties["hit_radius"]=1.5;
   require(bool(world.add_component(id,std::move(config),&error)),"enemy role: "+error);
   return id;
  };
  enemyA=enemy("targetA",{0,12,2},2);
  enemyB=enemy("targetB",{4,20,2},3);
  GameObjectDesc rules;rules.name="Rules";
  auto r=role("mech_rules","rules");
  r.properties["target_count"]=std::int64_t{2};
  rules.components.push_back(std::move(r));
  require(world.create_object(std::move(rules),&error)!=0,"rule creation: "+error);
  if(addLegs){
   for(const int side:{-1,1})for(int segment=0;segment<2;++segment){
    const auto key=std::string(side<0?"left":"right")+std::to_string(segment);
    const auto pos=Float3{float(side),0,segment==0?1.55F:.55F};
    const auto leg=world.spawn_visual_polygon_asset(make_editor_polygon_cube(20+segment+(side+1)*2),
        key,make_rigid_transform(pos,{}));
    auto c=role("mech_leg",key.c_str());
    c.properties["side"]=std::int64_t{side};
    c.properties["segment"]=std::int64_t{segment};
    require(bool(world.add_component(leg,std::move(c),&error)),"leg role: "+error);
    if(segment==1){if(side<0)leftShin=leg;else rightShin=leg;}
   }
  }
  game=std::make_unique<MechaGame>(world);
  require(game->start(&error),"start: "+error);
 }
 void tick(int n=1){for(int i=0;i<n;++i)game->step(1.F/120.F);}
 void press(const char* key){
  world.set_action_pressed(key,true);tick();
  world.set_action_pressed(key,false);tick();
 }
};
void camera_modes_and_occlusion(){
 Fixture f(true,true,true);
 require(f.game->camera_mode()==MechCameraMode::ThirdPerson,"authored chase camera");
 const auto eye=f.game->camera().position;
 require(eye.y>-5.2F,"camera clips in front of solid wall, not through it");
 f.game->look(.30F,-.12F);f.tick(20);
 require(distance(eye,f.game->camera().position)>.2F,"mouse orbit moves chase eye");
 f.press("mech_camera");
 require(f.game->camera_mode()==MechCameraMode::Tactical,"C toggles tactical mode");
 const auto tactical=f.game->camera().position;
 require(tactical.z>20.F,"tactical camera has overhead clearance");
 f.press("mech_camera");
 require(f.game->camera_mode()==MechCameraMode::ThirdPerson,"switching back restores chase mode");
 require(f.game->camera().position.z<tactical.z-8.F,"chase and tactical positions distinct");
}
void chase_movement_is_camera_relative(){
 Fixture f;
 f.game->look(-1.5707963F,0);
 f.world.set_axis("move_y",1.F);
 f.tick(80);
 f.world.set_axis("move_y",0);
 const auto p=f.game->motor().position;
 require(p.x<-2.F&&std::abs(p.y)<1.F,
         "third-person forward travel follows orbit yaw, not fixed world Y");
}
void jump_jet_cannot_cross_ceiling(){
 Fixture f;
 auto roof=make_editor_polygon_cube(110);
 for(auto& v:roof.vertices)
  v.position={(v.position.x-.5F)*30.F,(v.position.y-.5F)*30.F,
              (v.position.z-.5F)*1.F+5.F};
 refresh_editor_polygon(roof,true);
 require(f.world.spawn_cooked_polygon_asset(std::move(roof),
               "Ceiling",{},false)!=0,"create low roof");
 f.world.set_action_pressed("mech_jumpjet",true);
 f.tick(120);
 f.world.set_action_pressed("mech_jumpjet",false);
 require(f.game->motor().position.z<1.8F,
         "swept vertical capsule stops under solid roof");
}
void jump_jet_and_collision(){
 Fixture f;
 const auto start=f.game->motor().position;
 f.world.set_action_pressed("mech_jumpjet",true);
 f.tick(80);
 f.world.set_action_pressed("mech_jumpjet",false);
 const auto height=f.game->motor().position.z-start.z;
 require(height>1.F,"jump jet raises mech above ground");
 require(height<19.F,"jump height below authored 18 m maximum (with tolerance)");
 require(f.game->motor().energy<95.F,"jump jet draws power");
 require(f.game->telemetry().altitude>1.F,"HUD/telemetry reports altitude");
 f.tick(600);
 const auto landed=f.game->motor().position.z;
 require(landed>=start.z-.2F&&landed<start.z+.35F,
         "swept capsule catches ground on landing");
 require(std::abs(f.game->motor().velocity.z)<1.F,"landing cancels downward velocity");
}
void lockon_and_enemy_death(){
 Fixture f(true,true,false);
 require(f.game->locked_target_key().empty(),"initially no lock");
 f.press("mech_lock");
 require(f.game->locked_target_key()=="targetA","lock chooses first visible enemy");
 require(f.game->telemetry().lockedOn,"lock status on HUD telemetry");
 require(f.game->hud().mechLockMarker,"lock appears as projected screen-space bracket");
 f.press("mech_lock");
 require(f.game->locked_target_key()=="targetB","lock cycles to next target");
 f.press("mech_lock");
 require(f.game->locked_target_key().empty(),"cycling past last target releases lock");
 f.press("mech_lock");
 // Existing XY projectile targeting still goes through world LOS and regular damage.
 f.world.set_action_pressed("mech_primary",true);
 f.tick(20);
 f.world.set_action_pressed("mech_primary",false);
 require(!f.world.is_enabled(f.enemyA),"auto-torso lock aims at and eliminates enemy");
 f.tick(1);
 require(f.game->locked_target_key().empty(),"destroyed target releases lock");
}
void articulated_leg_presentation(){
 Fixture f;
 require(f.leftShin!=0&&f.rightShin!=0,"two authored shin segments");
 const auto before=f.world.position(f.leftShin).value();
 f.world.set_axis("move_y",1);
 f.tick(30);
 f.world.set_axis("move_y",0);
 const auto now=f.world.position(f.leftShin).value();
 const auto other=f.world.position(f.rightShin).value();
 require(distance(now,before)>0.4F,"procedural leg follows moving mech");
 const auto relative=now.y-f.game->motor().position.y;
 require(std::abs(relative)>.05F,"leg animates relative to chassis");
 require(std::abs(other.y-now.y)>.05F,"left/right gait is out of phase");
}
void save_restore_m6_and_legacy_v1(){
 Fixture f(true,true,false);
 f.press("mech_lock");
 f.game->look(.3F,.1F);
 f.world.set_action_pressed("mech_jumpjet",true);
 f.tick(16);
 f.world.set_action_pressed("mech_jumpjet",false);
 auto original=f.game->save_state();
 require(original.starts_with("DVE_MECHA 2 "),"M6 schema version 2");
 std::string error;
 require(f.game->after_restore(original,&error),"restore M6: "+error);
 require(f.game->save_state()==original,"camera, lock, jet and gait round trip");
 const auto preserved=f.game->save_state();
 require(!f.game->after_restore(original+"extra",&error),"extra data rejected");
 require(f.game->save_state()==preserved,"bad load is atomic");
 const auto pos=original.rfind("M6 ");
 require(pos!=std::string::npos,"extension marker");
 auto legacy=original.substr(0,pos);
 legacy.replace(10,1,"1"); // DVE_MECHA 2 -> DVE_MECHA 1
 require(f.game->after_restore(legacy,&error),"version-1 M5 checkpoint accepted: "+error);
 require(f.game->locked_target_key().empty(),"v1 save defaults to no target lock");
}
void validates_complete_leg_rig(){
 auto d=make_mecha_starter();
 std::string error;
 require(validate_editor_prototype(d,&error),"M6 authored starter validates: "+error);
 unsigned segments=0;
 for(const auto& [id,o]:d.objects()){
  (void)id;
  if(find_component_by_type(std::span<const Component>(o.components),"dve.prototype.mech_leg"))++segments;
 }
 require(segments==4,"Iron Yard has two thighs and two shins");
 auto invalid=make_mecha_starter();
 for(const auto& [id,o]:invalid.objects()){
  (void)o;
  auto* object=invalid.find_object(id);
  if(auto* c=find_component_by_type(std::span<Component>(object->components),"dve.prototype.mech_leg")){
   c->properties["side"]=std::int64_t{0};
   break;
  }
 }
 require(!validate_editor_prototype(invalid,&error),"invalid limb-side rejected");
}
}
int main(){
 try{
  camera_modes_and_occlusion();
  chase_movement_is_camera_relative();
  jump_jet_and_collision();
  jump_jet_cannot_cross_ceiling();
  lockon_and_enemy_death();
  articulated_leg_presentation();
  save_restore_m6_and_legacy_v1();
  validates_complete_leg_rig();
  std::cout<<"M6 mech third-person, jet, lock and gait tests passed\n";
  return 0;
 }catch(const std::exception& e){
  std::cerr<<"mecha M6: "<<e.what()<<'\n';
  return 1;
 }
}
