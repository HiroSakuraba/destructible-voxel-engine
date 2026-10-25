#include "dve/water_game.hpp"
#include "dve/editor_prototype.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/player/player_app.hpp"
#include "dve/player/player_renderer.hpp"
#include <cmath>
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace dve;using namespace dve::editor;using namespace dve::player;
namespace {
void check(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
void scene_validation(){
 auto doc=make_water_starter();std::string error;
 check(validate_editor_prototype(doc,&error),"W1 starter validates: "+error);
 unsigned vessels=0,oceans=0;
 for(const auto& [id,o]:doc.objects()){
  (void)id;
  if(find_component_by_type(o.components,"dve.prototype.water_vessel"))++vessels;
  if(find_component_by_type(o.components,"dve.prototype.water_ocean"))++oceans;
 }
 check(vessels==5&&oceans==1,"starter contains five specialized hulls and one ocean");
 auto invalid=make_water_starter();
 for(const auto& [id,o]:invalid.objects()){
  (void)o;
  if(auto* c=find_component_by_type(std::span<Component>{invalid.find_object(id)->components},
                                     "dve.prototype.water_vessel")){
   c->properties["mass_kg"]=-3.0;break;
  }
 }
 check(!validate_editor_prototype(invalid,&error),"reject nonphysical hull mass");
}
std::unique_ptr<PlayerApp> boot(const std::filesystem::path& dir,const std::filesystem::path& saves){
 PlayerBootOptions options;options.enableScripts=false;
 options.physicsBackend=Physics3DBackend::Reference;options.saveDirectory=saves;
 auto content=open_content_source(dir);
 check(bool(content),"content source opens");
 std::string error;auto app=PlayerApp::boot(std::move(content),options,&error);
 check(bool(app),"water player boots: "+error);return app;
}
void integration(){
 auto root=std::filesystem::current_path()/"water-w1-test-output";
 std::filesystem::remove_all(root);std::filesystem::create_directories(root);
 auto scene=make_water_starter();auto materials=EditorMaterialLibrary::make_default();
 std::string error;
 check(export_prototype_project(scene,materials,root/"project",&error),"export water range: "+error);
 auto app=boot(root/"project",root/"saves");
 check(app->prototype_hud()&&app->prototype_hud()->water,"water gameplay HUD active");
 const auto dinghy=app->world().find_by_name("Dinghy");
 const auto ship=app->world().find_by_name("Pirate Ship");
 const auto sailboat=app->world().find_by_name("Sailboat");
 const auto speedboat=app->world().find_by_name("Speedboat");
 const auto waveRunner=app->world().find_by_name("Wave Runner");
 check(bool(dinghy)&&bool(ship)&&bool(sailboat)&&bool(speedboat)&&bool(waveRunner),
       "all five distinct craft exported and loaded");
 const auto before=*app->world().position(*dinghy);
 platform::PlatformEvent input{};input.type=platform::EventType::KeyDown;input.key="w";
 app->handle_event(input);
 for(unsigned i=0;i<90;++i)app->tick(1.F/60.F);
 const auto after=*app->world().position(*dinghy);
 check(length(subtract(after,before))>.4F,"dinghy actually moves under throttle and buoyancy");
 check(std::isfinite(after.z)&&std::abs(after.z)<15,"dinghy responds without instability");
 const auto oldText=app->prototype_hud()->stats;
 input.key="2";app->handle_event(input);app->tick(1.F/60.F);
 input.type=platform::EventType::KeyUp;app->handle_event(input);
 input.type=platform::EventType::KeyDown;input.key="w";app->handle_event(input);
 check(app->prototype_hud()->stats!=oldText,"switch to pirate ship updates active camera and HUD");
 const auto shipBefore=*app->world().position(*ship);
 for(unsigned i=0;i<90;++i)app->tick(1.F/60.F);
 check(length(subtract(*app->world().position(*ship),shipBefore))>.2F,
       "pirate ship moves under real wind rather than an auxiliary engine");
 // Sailboat has authentic wind propulsion and keel response. Q/E trims the sail.
 input.key="3";app->handle_event(input);app->tick(1.F/60.F);
 input.type=platform::EventType::KeyUp;app->handle_event(input);
 const auto sailBefore=*app->world().position(*sailboat);
 for(unsigned i=0;i<100;++i)app->tick(1.F/60.F);
 check(length(subtract(*app->world().position(*sailboat),sailBefore))>.1F,
       "wind moves the unpowered sailboat");
 input.type=platform::EventType::KeyDown;input.key="e";app->handle_event(input);
 for(unsigned i=0;i<20;++i)app->tick(1.F/60.F);
 input.type=platform::EventType::KeyUp;app->handle_event(input);
 // Planing speedboat and pump-jet craft both respond to authored motor controls.
 for(const auto [key,id]:{std::pair<const char*,GameObjectId>{"4",*speedboat},
                          {"5",*waveRunner}}){
   input.type=platform::EventType::KeyDown;input.key=key;app->handle_event(input);app->tick(1.F/60.F);
   input.type=platform::EventType::KeyUp;app->handle_event(input);
   const auto beforeMotor=*app->world().position(id);
   for(unsigned i=0;i<90;++i)app->tick(1.F/60.F);
   check(length(subtract(*app->world().position(id),beforeMotor))>.15F,
         "specialized motor vessel responds to W throttle");
 }
 // Restore existing #2 selection for the legacy save-position round trip.
 input.type=platform::EventType::KeyDown;input.key="2";app->handle_event(input);app->tick(1.F/60.F);
 input.type=platform::EventType::KeyUp;app->handle_event(input);
 auto save=app->save_game("water",&error);check(bool(save),"save W1 state: "+error);
 const auto savedPos=*app->world().position(*ship);
 for(unsigned i=0;i<30;++i)app->tick(1.F/60.F);
 check(bool(app->load_game("water",&error)),"restore W1: "+error);
 check(length(subtract(*app->world().position(*ship),savedPos))<.001F,
       "water save retains ship 6-DOF position");
 auto renderer=make_cpu_player_renderer(nullptr);
 check(renderer->resize(320,180,&error),error);
 std::vector<GameRenderObject> objects;
 auto waterView=app->render_view(objects,16.F/9.F);
 check(waterView.ocean!=nullptr&&waterView.ocean->waves().size()>0,
       "player sees authoritative physics ocean waves");
 check(renderer->render(waterView,&error),"render water: "+error);
 const auto pixels=renderer->readback()->pixels;
 // The W3 spray projection uses the same depth-aware rendering path as the
 // real spray particles spawned from the ships' fixed-tick bow emitters.
 WaterSprayEffect spray{};
 const auto forward=normalize(subtract(waterView.camera.target,waterView.camera.position));
 spray.position=add(waterView.camera.position,multiply(forward,5.F));
 spray.radius=.8F;spray.opacity=.95F;spray.lifetime=1;
 const std::array<WaterSprayEffect,1> effects{spray};
 waterView.waterSpray=effects;
 check(renderer->render(waterView,&error),"render injected bow spray: "+error);
 check(renderer->readback()->pixels!=pixels,
       "spray is rendered in front of the physical ocean");
 waterView.waterSpray={};
 waterView.ocean=nullptr;
 check(renderer->render(waterView,&error),"render no water: "+error);
 check(renderer->readback()->pixels!=pixels,
       "analytic ocean renders visibly and depth-composites with ships");
 std::filesystem::remove_all(root);
}
}
int main(){
 try{scene_validation();integration();std::cout<<"W1 playable water starter passed\n";return 0;}
 catch(const std::exception& e){std::cerr<<"W1 runtime: "<<e.what()<<'\n';return 1;}
}
