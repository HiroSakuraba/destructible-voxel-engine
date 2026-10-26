#include "dve/flying_game.hpp"
#include "dve/editor_prototype.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/player/player_app.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace dve;using namespace dve::editor;using namespace dve::player;
namespace {
void check(bool ok,const std::string& what){if(!ok)throw std::runtime_error(what);}
Component component(const char* role,std::string key){
 auto c=make_component_from_schema(std::string("dve.prototype.")+role,1);check(bool(c),"flight schema");
 c->properties["key"]=std::move(key);return *c;
}
struct Fixture {
 GameWorld world{create_physics3d_world(Physics3DBackend::Reference)};
 GameObjectId flyer{},rules{};
 std::unique_ptr<FlyingGame> game;
 Fixture(bool thermal=false){
  std::string error;
  auto mesh=make_editor_polygon_cube(3);
  flyer=world.spawn_visual_polygon_asset(std::move(mesh),"Flyer",make_rigid_transform({0,0,80},{}));
  check(flyer!=0,"spawn flyer");
  auto c=component("flight_vehicle","flyer");c.properties["wind_x"]=0.0;
  check(bool(world.add_component(flyer,std::move(c),&error)),error);
  GameObjectDesc r;r.name="Flight Rules";r.components={component("flight_rules","rules")};
  rules=world.create_object(std::move(r));check(rules!=0,"rules");
  for(unsigned i=0;i<6;++i){
   GameObjectDesc g;g.name="Gate "+std::to_string(i);
   g.transform.position={0,100.F+float(i)*100,80};
   auto config=component("flight_gate","gate"+std::to_string(i));
   config.properties["order"]=std::int64_t(i);
   config.properties["mode"]=std::int64_t(i);
   g.components={config};check(world.create_object(std::move(g))!=0,"gate");
  }
  if(thermal){
   GameObjectDesc zone;zone.name="Test thermal";
   zone.transform.position={0,0,0};
   auto w=component("flight_wind","thermal-test");
   w.properties["radius"]=85.0;w.properties["height"]=300.0;
   w.properties["strength"]=8.0;w.properties["sink_fraction"]=0.25;
   zone.components={w};
   check(world.create_object(std::move(zone))!=0,"test wind zone");
  }
  game=std::make_unique<FlyingGame>(world);
  check(game->start(&error),error);
 }
 void step(unsigned n=1,float dt=1.F/60){for(unsigned i=0;i<n;++i)game->step(dt);}
 void tap(const std::string& input){world.set_action_pressed(input,true);step();world.set_action_pressed(input,false);step();}
};
void validation(){
 auto doc=make_flying_starter();std::string reason;
 check(validate_editor_prototype(doc,&reason),"generated flight course: "+reason);
 auto invalid=make_flying_starter();
 for(const auto& [id,o]:invalid.objects()){
  (void)o;auto* object=invalid.find_object(id);
  if(auto* c=find_component_by_type(std::span<Component>{object->components},"dve.prototype.flight_vehicle")){
   c->properties["mass_kg"]=-1.0;break;
  }
 }
 check(!validate_editor_prototype(invalid,&reason),"reject negative aircraft mass");
 for(unsigned i=0;i<6;++i)check(flight_profile(FlightMode(i)).response>0,"mode profile exists");
 auto weatherDoc=make_flying_starter();unsigned zones=0;
 for(const auto& [id,o]:weatherDoc.objects()){
  (void)id;
  if(find_component_by_type(o.components,"dve.prototype.flight_wind"))++zones;
 }
 check(zones==3,"Wind Garden authors training thermal, bird thermal and ridge");
 for(const auto& [id,o]:weatherDoc.objects()){
  (void)o;
  if(auto* c=find_component_by_type(std::span<Component>{weatherDoc.find_object(id)->components},
         "dve.prototype.flight_wind")){
    c->properties["strength"]=-2.0;break;
  }
 }
 check(!validate_editor_prototype(weatherDoc,&reason),"negative thermal strength refused");
}
void transitions(){
 Fixture a,b;
 a.world.set_axis("move_y",.5F);b.world.set_axis("move_y",.5F);
 a.step(120);b.step(240,1.F/120);
 check(length(subtract(*a.world.position(a.flyer),*b.world.position(b.flyer)))<.01F,
       "fixed 120 Hz update independent of outer frame rate");
 for(unsigned i=2;i<=6;++i){
  const auto before=*a.world.position(a.flyer);
  const auto speed=a.game->telemetry().groundspeed;
  a.tap("flight_mode_"+std::to_string(i));
  check(length(subtract(*a.world.position(a.flyer),before))<5.F,"no teleport on mode request");
  check(std::isfinite(speed),"finite velocity before morph");
  a.step(240);
  check(a.game->telemetry().mode==FlightMode(i-1),"mode transitions reach destination");
  const auto p=*a.world.position(a.flyer);
  check(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z),"finite flight state");
 }
 a.tap("flight_mode_1");a.step(240);
 check(a.game->telemetry().mode==FlightMode::Float,"return to floating");
 auto now=a.world.elapsed_seconds();
 a.tap("pause");a.step(10);
 check(a.game->hud().phase==PrototypePhase::Paused&&a.world.elapsed_seconds()==now,
       "pause stops simulation");
 a.tap("pause");a.tap("crouch");a.tap("restart");
 check(a.game->telemetry().mode==FlightMode::Float,"restart resets form");
}
void weather_runtime(){
 Fixture calm,thermal(true);
 calm.step(40);thermal.step(40);
 const auto a=calm.game->telemetry(),b=thermal.game->telemetry();
 check(std::abs(a.windVelocity.z)<.01F,"no hidden wind in calm legacy flight fixture");
 check(b.thermalUpdraft>5.F&&b.windVelocity.z>5.F,"authored thermal reaches flight runtime");
 check(b.verticalSpeed>a.verticalSpeed+.1F,"rising air changes flight dynamics");
 check(std::isfinite(b.variometer)&&std::isfinite(b.mechanicalEnergyPerKilogram),
     "soaring instruments remain finite");
 thermal.tap("flight_mode_2");thermal.step(130);
 check(thermal.game->telemetry().mode==FlightMode::Glider,"glider engages under thermal");
 check(thermal.game->telemetry().thermalUpdraft>0,
       "glider still reads actual moving air after transition");
}
void gameplay_export(){
 const auto root=std::filesystem::current_path()/"flying-f1-test-output";
 std::filesystem::remove_all(root);std::filesystem::create_directories(root);
 const auto d=make_flying_starter();auto materials=EditorMaterialLibrary::make_default();std::string error;
 check(export_prototype_project(d,materials,root/"game",&error),"export flying starter: "+error);
 check(std::filesystem::exists(root/"game"/"game.dvegame"),"flight manifest exists");
 PlayerBootOptions options;options.enableScripts=false;
 options.physicsBackend=Physics3DBackend::Reference;options.saveDirectory=root/"saves";
 auto source=open_content_source(root/"game");check(bool(source),"flight content opens");
 auto app=PlayerApp::boot(std::move(source),options,&error);
 check(bool(app),"flight player boots: "+error);
 check(app->prototype_hud()&&app->prototype_hud()->flying,"flight HUD in player");
 for(unsigned n=0;n<15;++n)app->tick(1.F/60);
 check(app->prototype_hud()->active,"flight updates in player");
 const auto craft=app->world().find_by_name("Morphing Aircraft");
 check(bool(craft),"exported craft named and loaded");
 const auto initial=*app->world().position(*craft);
 auto saved=app->save_game("flight",&error);
 check(bool(saved),"flight save: "+error);
 for(unsigned n=0;n<20;++n)app->tick(1.F/60);
 check(bool(app->load_game("flight",&error)),"flight restore: "+error);
 check(length(subtract(*app->world().position(*craft),initial))<.001F,
       "flight save restores the same aircraft pose");
 std::filesystem::remove_all(root);
}
}
int main(){
 try{validation();transitions();weather_runtime();gameplay_export();std::cout<<"Flight F1 tests passed\n";return 0;}
 catch(const std::exception& e){std::cerr<<"Flight F1: "<<e.what()<<'\n';return 1;}
}
