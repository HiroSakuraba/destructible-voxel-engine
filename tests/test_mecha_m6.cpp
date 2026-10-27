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
#include <sstream>
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
void jump_jet_fuel_is_finite_and_has_recharge_cooldown(){
 Fixture f;
 const float dt=1.F/120.F,drain=32.F,regen=16.F;
 f.world.set_action_pressed("mech_jumpjet",true);
 float thrustSeconds=0.F,previous=f.game->motor().energy;bool thrustRaisedEnergy=false;
 int cutouts=0;bool wasThrusting=false;
 for(int i=0;i<1440;++i){ // twelve seconds of held Space
  f.tick();
  const bool thrusting=f.game->telemetry().jetActive;
  if(thrusting)thrustSeconds+=dt;
  if(thrusting&&f.game->motor().energy>previous+1.e-4F)thrustRaisedEnergy=true;
  if(wasThrusting&&!thrusting)++cutouts;
  wasThrusting=thrusting;previous=f.game->motor().energy;
  require(f.game->motor().energy>=0.F&&f.game->motor().energy<=100.F,"energy stays in range");
 }
 f.world.set_action_pressed("mech_jumpjet",false);
 // Every second of thrust costs 32 energy and only idle time regenerates (16/s): a
 // 100-unit tank cannot fund more thrust than this, however long Space is held.
 const float budget=(100.F+regen*(12.F-thrustSeconds))/drain;
 require(thrustSeconds<=budget+.05F,"jump-jet thrust is limited by fuel, not free at zero energy");
 require(!thrustRaisedEnergy,"thrust frames never regenerate energy");
 require(cutouts>=1,"jets cut out when the tank runs dry");
 require(thrustSeconds<9.F,"held Space cannot hover for the whole twelve seconds");
}
void eased_chase_eye_never_cuts_through_a_pillar(){
 Fixture f;
 auto pillar=make_editor_polygon_cube(120);
 for(auto& v:pillar.vertices)
  v.position={(v.position.x-.5F)*.8F+2.F,(v.position.y-.5F)*1.F-10.4F,
              (v.position.z-.5F)*12.F+5.F};
 refresh_editor_polygon(pillar,true);
 require(f.world.spawn_cooked_polygon_asset(std::move(pillar),"Pillar",{},false)!=0,
         "pillar creation");
 f.tick(60); // eye settles beside the pillar
 for(int i=0;i<90;++i){
  f.game->look(i<6?-.03F:0.F,0.F); // swing the desired eye across the pillar; the eased chord clips it
  f.tick();
  const auto e=f.game->camera().position;
  require(!(e.x>1.6F&&e.x<2.4F&&e.y>-10.9F&&e.y<-9.9F&&e.z<11.F),
          "eased camera must not lag through solid cover");
 }
}
void idle_gait_settles_both_feet_and_pauses_gate_lock(){
 Fixture f;
 f.world.set_axis("move_y",1.F);f.tick(23);f.world.set_axis("move_y",0.F);
 f.tick(120);
 const auto base=f.game->motor().position;
 const auto l=f.world.position(f.leftShin).value(),r=f.world.position(f.rightShin).value();
 require(std::abs((l.y-base.y))<.03F&&std::abs((r.y-base.y))<.03F,
         "standing mech plants both feet instead of freezing mid-stride");
 require(std::abs(l.z-r.z)<.01F,"neither foot stays raised when idle");
 f.press("pause");
 f.press("mech_lock");
 require(f.game->locked_target_key().empty(),"target lock is ignored while paused");
 f.press("pause");
 f.press("mech_lock");
 require(f.game->locked_target_key()=="targetA","lock works again after resume");
}
void restart_clears_jet_gait_lock_and_camera(){
 Fixture f;
 f.press("mech_lock");
 f.world.set_axis("move_y",1.F);
 f.world.set_action_pressed("mech_jumpjet",true);
 f.tick(60);
 f.world.set_action_pressed("mech_jumpjet",false);f.world.set_axis("move_y",0.F);
 require(f.game->telemetry().altitude>.2F,"airborne before restart");
 f.press("restart");
 require(f.game->locked_target_key().empty(),"restart drops target lock");
 require(f.game->telemetry().altitude<.01F&&std::abs(f.game->motor().velocity.z)<1.e-4F,
         "restart lands the jets and clears vertical velocity");
 require(f.game->camera_mode()==MechCameraMode::ThirdPerson,"restart restores authored camera");
 require(distance(f.game->motor().position,{0,0,.05F})<.01F,"restart returns to spawn");
}
void grounded_checkpoint_cannot_smuggle_vertical_velocity(){
 Fixture f;
 auto save=f.game->save_state();
 // Token 11 is velocity.z in the controller header; the checkpoint says "not airborne".
 std::istringstream in(save);std::string tok,rebuilt;
 for(int i=0;in>>tok&&i<11;++i)rebuilt+=tok+" ";
 const auto restOfLine=save.substr(rebuilt.size());
 const auto after=restOfLine.find(' ');
 require(after!=std::string::npos,"parse save header");
 std::string error;
 require(f.game->after_restore(rebuilt+"7 "+restOfLine.substr(after+1),&error),
         "restore: "+error);
 require(std::abs(f.game->motor().velocity.z)<1.e-6F,
         "grounded checkpoint velocity.z is sanitized to zero");
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
int main(int argc,char** argv){
 try{
  // Optional argv[1] runs only the named case (debugging: dve_mecha_m6_tests jump_jet_and_collision).
  const std::string only=argc>1?argv[1]:"";
  const auto run=[&](const char* name,void(*test)()){if(only.empty()||only==name)test();};
  run("camera_modes_and_occlusion",camera_modes_and_occlusion);
  run("chase_movement_is_camera_relative",chase_movement_is_camera_relative);
  run("jump_jet_and_collision",jump_jet_and_collision);
  run("jump_jet_cannot_cross_ceiling",jump_jet_cannot_cross_ceiling);
  run("jump_jet_fuel_is_finite_and_has_recharge_cooldown",jump_jet_fuel_is_finite_and_has_recharge_cooldown);
  run("eased_chase_eye_never_cuts_through_a_pillar",eased_chase_eye_never_cuts_through_a_pillar);
  run("lockon_and_enemy_death",lockon_and_enemy_death);
  run("idle_gait_settles_both_feet_and_pauses_gate_lock",idle_gait_settles_both_feet_and_pauses_gate_lock);
  run("restart_clears_jet_gait_lock_and_camera",restart_clears_jet_gait_lock_and_camera);
  run("grounded_checkpoint_cannot_smuggle_vertical_velocity",grounded_checkpoint_cannot_smuggle_vertical_velocity);
  run("articulated_leg_presentation",articulated_leg_presentation);
  run("save_restore_m6_and_legacy_v1",save_restore_m6_and_legacy_v1);
  run("validates_complete_leg_rig",validates_complete_leg_rig);
  std::cout<<"M6 mech third-person, jet, lock and gait tests passed\n";
  return 0;
 }catch(const std::exception& e){
  std::cerr<<"mecha M6: "<<e.what()<<'\n';
  return 1;
 }
}
