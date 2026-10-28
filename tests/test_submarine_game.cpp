#include "dve/editor_prototype.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/player/player_app.hpp"
#include "dve/editor_component_inspector.hpp"
#include <filesystem>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace dve;using namespace dve::editor;using namespace dve::player;
namespace {
void check(bool v,const std::string& what){if(!v)throw std::runtime_error(what);}
}
int main(){
 try{
  auto scene=make_submarine_starter();
  std::string err;
  check(validate_editor_prototype(scene,&err),"mini-sub starter validation "+err);
  const auto root=std::filesystem::current_path()/"submarine-s1-test-output";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  check(export_prototype_project(scene,EditorMaterialLibrary::make_default(),
    root/"project",&err),"export submarine project "+err);
  PlayerBootOptions opts;opts.enableScripts=false;
  opts.physicsBackend=Physics3DBackend::Reference;opts.saveDirectory=root/"saves";
  auto content=open_content_source(root/"project");
  check(bool(content),"exported content source");
  auto game=PlayerApp::boot(std::move(content),opts,&err);
  check(bool(game),"player boots submarine project: "+err);
  auto sub=game->world().find_by_name("Deep Explorer Mini-Sub");
  check(bool(sub),"mini-sub model loaded");
  check(game->prototype_hud()&&game->prototype_hud()->water,"active water HUD");
  check(game->prototype_hud()->stats.find("MINI-SUB")!=std::string::npos,"mini-sub branch dispatches");
  const float startZ=game->world().position(*sub)->z;
  platform::PlatformEvent key{};key.type=platform::EventType::KeyDown;key.key="left ctrl";
  game->handle_event(key);
  for(unsigned i=0;i<180;++i)game->tick(1.F/60.F);
  const float descended=game->world().position(*sub)->z;
  check(descended<startZ-.1F,"vertical thruster initiates dive");
  check(descended>-39.1F,"physical floor prevents driving below seafloor");
  key.type=platform::EventType::KeyUp;game->handle_event(key);
  key.type=platform::EventType::KeyDown;key.key="h";game->handle_event(key);
  game->tick(1.F/60.F);
  check(game->prototype_hud()->message.find("DEPTH HOLD")!=std::string::npos,
        "depth hold toggle engages at current water depth");
  key.type=platform::EventType::KeyUp;game->handle_event(key);
  key.type=platform::EventType::KeyDown;key.key="x";game->handle_event(key);
  for(unsigned i=0;i<40;++i)game->tick(1.F/60.F);
  key.type=platform::EventType::KeyUp;game->handle_event(key);
  const auto position=*game->world().position(*sub);
  check(game->prototype_hud()->prompt.find("BALLAST")!=std::string::npos,
        "ballast instrument shown while submerged");
  check(bool(game->save_game("submarine",&err)),"submarine save succeeds: "+err);
  key.type=platform::EventType::KeyDown;key.key="w";game->handle_event(key);
  for(unsigned i=0;i<120;++i)game->tick(1.F/60.F);
  key.type=platform::EventType::KeyUp;game->handle_event(key);
  check(bool(game->load_game("submarine",&err)),"submarine restore: "+err);
  check(length(subtract(*game->world().position(*sub),position))<.002F,
        "submarine 6DOF state restored after dive");
  check(game->prototype_hud()->message.find("DEPTH HOLD")!=std::string::npos,
        "depth-hold state restored");
  std::filesystem::remove_all(root);
  std::cout<<"S1 exported Deep Explorer dive, depth hold and save passed\n";return 0;
 }catch(const std::exception& e){
   std::cerr<<"S1 player: "<<e.what()<<'\n';return 1;
 }
}
