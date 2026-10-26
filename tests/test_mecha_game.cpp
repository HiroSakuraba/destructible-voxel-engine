#include "dve/mecha_game.hpp"
#include "dve/editor_prototype.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/player/player_app.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using namespace dve;
using namespace dve::editor;
using namespace dve::player;
namespace {
void check(bool yes,std::string message){if(!yes)throw std::runtime_error(std::move(message));}
Component role(std::string name,std::string key){
 auto c=make_component_from_schema("dve.prototype."+name,1);
 check(bool(c),"mech role schema not registered");
 c->properties["key"]=std::move(key);return *c;
}
struct FixtureOptions {
 bool barrier{};            // 8 m wide, 0.25 m thick polygon wall at y=5 (spans z -1..5)
 Float3 targetPosition{0,10,2};
 double targetRadius{1.5};
 double targetHealth{36.0};
 bool thinWall{};           // extra 0.25 m polygon wall centred on thinWallY
 float thinWallY{5.F};
};
struct Fixture {
 GameWorld world{create_physics3d_world(Physics3DBackend::Reference)};
 GameObjectId player{},target{},rules{};
 std::unique_ptr<MechaGame> game;
 explicit Fixture(bool barrier=false):Fixture(FixtureOptions{.barrier=barrier}){}
 explicit Fixture(const FixtureOptions& options){
  const bool barrier=options.barrier;
  std::string error;
  auto mesh=make_editor_polygon_cube(1);
  player=world.spawn_visual_polygon_asset(std::move(mesh),"Player",make_rigid_transform({0,0,.05F},{}));
  check(player!=0,"create player");
  check(bool(world.add_component(player,role("mech_player","player"),&error)),error);
  auto targetMesh=make_editor_polygon_cube(2);
  target=world.spawn_visual_polygon_asset(std::move(targetMesh),"Target",make_rigid_transform(options.targetPosition,{}));
  check(target!=0,"create target");
  auto component=role("mech_target","target");component.properties["hit_radius"]=options.targetRadius;
  component.properties["health"]=options.targetHealth;
  check(bool(world.add_component(target,std::move(component),&error)),error);
  GameObjectDesc rulesDesc;rulesDesc.name="Rules";
  auto rulesComponent=role("mech_rules","rules");rulesComponent.properties["target_count"]=std::int64_t{1};
  rulesDesc.components.push_back(std::move(rulesComponent));
  rules=world.create_object(std::move(rulesDesc));check(rules!=0,"create rules");
  if(barrier){
   auto obstacle=make_editor_polygon_cube(3);
   for(auto& v:obstacle.vertices)v.position={(v.position.x-.5F)*8,(v.position.y-.5F)*.25F+5,
      (v.position.z-.5F)*6+2};
   refresh_editor_polygon(obstacle,true);
   check(world.spawn_cooked_polygon_asset(std::move(obstacle),"Barrier",{},false)!=0,"create barrier");
  }
  if(options.thinWall){
   auto thin=make_editor_polygon_cube(5);
   for(auto& v:thin.vertices)v.position={(v.position.x-.5F)*8,(v.position.y-.5F)*.25F+options.thinWallY,
      (v.position.z-.5F)*6+2};
   refresh_editor_polygon(thin,true);
   check(world.spawn_cooked_polygon_asset(std::move(thin),"Thin Wall",{},false)!=0,"create thin wall");
  }
  game=std::make_unique<MechaGame>(world);
  check(game->start(&error),"start mecha: "+error);
 }
 void tick(int count=1){for(int n=0;n<count;++n)game->step(1.F/120);}
 void fire(const char* action,int frames=1){
  world.set_action_pressed(action,true);tick(frames);
  world.set_action_pressed(action,false);tick(1);
 }
};
void motor_contract(){
 MechMotorConfig cfg;
 check(valid_mech_motor_config(cfg),"default motor valid");
 MechMotorConfig bad=cfg;bad.radius=0;check(!valid_mech_motor_config(bad),"invalid collision radius rejected");
 bad=cfg;bad.maximumSpeed=std::numeric_limits<float>::infinity();
 check(!valid_mech_motor_config(bad),"infinity rejected");
 MechMotorState a,b;MechMotorInput input;input.move={0,1,0};
 for(int i=0;i<60;++i)(void)integrate_mech_motor(a,cfg,input,1.F/120);
 for(int i=0;i<60;++i)(void)integrate_mech_motor(b,cfg,input,1.F/120);
 check(std::abs(a.velocity.y-b.velocity.y)<1.e-6F,"deterministic fixed-step integration");
 check(a.velocity.y>8.0F&&a.velocity.y<=cfg.maximumSpeed,"correct acceleration and speed cap");
 (void)integrate_mech_motor(a,cfg,{Float3{1,0,0},true},1.F/120);
 check(a.energy<70.F,"boost consumes energy");
 const auto before=a.energy;
 (void)integrate_mech_motor(a,cfg,{Float3{},false},1.F/120);
 check(a.energy>before,"energy regenerates");
}
void validation(){
 auto doc=make_mecha_starter();std::string error;
 check(validate_editor_prototype(doc,&error),"starter validates: "+error);
 auto invalid=make_mecha_starter();
 for(const auto& [id,o]:invalid.objects()){
  (void)o;
  auto* obj=invalid.find_object(id);
  if(auto* c=find_component_by_type(std::span<Component>{obj->components},"dve.prototype.mech_player")){
   c->properties["radius"]=-1.0;break;
  }
 }
 check(!validate_editor_prototype(invalid,&error),"invalid motor configuration rejected");
}
void combat(){
 Fixture clear;
 clear.fire("mech_primary");
 check(clear.game->telemetry().autocannonShots==1,"autocannon fires once");
 check(clear.world.is_enabled(clear.target),"one shot does not destroy target");
 clear.tick(14);clear.fire("mech_primary");
 check(!clear.world.is_enabled(clear.target),"second direct hit disables target");
 check(clear.game->hud().phase==PrototypePhase::Won,"target elimination wins");
 clear.fire("restart");
 check(clear.world.is_enabled(clear.target),"restart re-enables target");
 check(clear.game->telemetry().targetsDestroyed==0,"restart resets kill count");
 Fixture wall(true);
 wall.fire("mech_primary");
 wall.tick(14);wall.fire("mech_primary");
 check(wall.world.is_enabled(wall.target),"cover occludes target and prevents damage");
 Fixture rocket;
 rocket.fire("mech_secondary");
 rocket.tick(50);
 check(rocket.game->telemetry().rocketsFired==1,"rocket was spawned");
 check(rocket.game->telemetry().impacts>=1,"swept rocket hits target");
 check(!rocket.world.is_enabled(rocket.target),"direct rocket kills target");
}
// Regression (Codex P2 4381797405): editor play mode sends fire/aim/sprint while exported players
// bind mech_primary/mech_secondary/mech_boost. Both action sets must drive the same behaviour.
void editor_action_aliases(){
 Fixture primary;
 primary.fire("fire");
 check(primary.game->telemetry().autocannonShots==1,"editor 'fire' action fires the autocannon");
 Fixture secondary;
 secondary.fire("aim");
 check(secondary.game->telemetry().rocketsFired==1,"editor 'aim' action fires the rocket launcher");
 auto boosted=[](const char* action){
  Fixture f;
  f.world.set_axis("move_y",1.F);
  f.world.set_action_pressed(action,true);
  f.tick(1);
  return f.game->motor().energy;
 };
 const float exported=boosted("mech_boost"),editor=boosted("sprint"),idle=boosted("unbound_action");
 check(exported<idle,"mech_boost drains boost energy");
 check(std::abs(editor-exported)<1.e-5F&&editor<idle,"editor 'sprint' action boosts like mech_boost");
}
void movement(){
 Fixture moving;
 moving.world.set_axis("move_y",1.F);
 moving.tick(120);
 check(moving.world.position(moving.player)->y>4.F,"mech moved forward");
 check(moving.game->motor().legsYaw==0,"forward leg yaw unchanged");
 const auto legYaw=moving.game->motor().legsYaw;
 moving.world.set_axis("look_x",1.F);
 moving.world.set_axis("look_y",0.F);
 moving.tick(30);
 check(std::abs(moving.game->motor().torsoYaw-legYaw)>.1F,"independent torso yaw");
 Fixture blocked(true);
 blocked.world.set_axis("move_y",1.F);blocked.tick(300);
 check(blocked.world.position(blocked.player)->y<5,"sweep blocks high speed travel");
 const auto elapsed=blocked.world.elapsed_seconds();
 blocked.fire("pause");blocked.tick(25);
 check(blocked.game->hud().phase==PrototypePhase::Paused&&blocked.world.elapsed_seconds()==elapsed,
  "paused game freezes simulation");
}
void exported_starter(){
 auto scene=make_mecha_starter();
 const auto root=std::filesystem::current_path()/"mecha-m1-m2-test-output";
 std::filesystem::remove_all(root);std::filesystem::create_directories(root);
 std::string error;auto mats=EditorMaterialLibrary::make_default();
 check(export_prototype_project(scene,mats,root/"game",&error),"export: "+error);
 auto content=open_content_source(root/"game");check(bool(content),"content source opens");
 PlayerBootOptions options;options.enableScripts=false;options.physicsBackend=Physics3DBackend::Reference;
 options.saveDirectory=root/"saves";
 auto player=PlayerApp::boot(std::move(content),options,&error);
 check(bool(player),"standalone player boots: "+error);
 check(player->prototype_hud()&&player->prototype_hud()->active,"HUD in standalone player");
 for(int i=0;i<30;++i)player->tick(1.F/120);
 check(player->prototype_hud()->active,"game steps in player");
 std::filesystem::remove_all(root);
}
}
int main(){
 try{
  motor_contract();validation();movement();combat();editor_action_aliases();exported_starter();
  std::cout<<"mecha M1/M2 tests passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<"mecha M1/M2: "<<e.what()<<'\n';return 1;}
}
