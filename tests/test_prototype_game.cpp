#include "dve/editor_native.hpp"
#include "dve/editor_prototype.hpp"
#include "dve/editor_scene_export.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/gameplay_runtime.hpp"
#include "dve/player/player_app.hpp"
#include "dve/player/player_renderer.hpp"
#include "dve/v235_foundations.hpp"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <system_error>
using namespace dve;using namespace dve::editor;using namespace dve::player;
namespace {
void check(bool v,const std::string& e){if(!v)throw std::runtime_error(e);}
GameObjectId named(PlayerApp& app,const char* name){auto id=app.world().find_by_name(name);check(bool(id),std::string("missing ")+name);return *id;}
void teleport(PlayerApp& app,Float3 p){auto id=named(app,"Player Start");check(app.world().set_position(id,p),"teleport pawn");app.world().gameplay().character(id)->velocity={};}
void tick(PlayerApp& app,unsigned n=1){for(unsigned i=0;i<n;++i)app.tick(1.F/60);}
void tap(PlayerApp& app,const char* key){platform::PlatformEvent e;e.type=platform::EventType::KeyDown;e.key=key;app.handle_event(e);e.type=platform::EventType::KeyUp;app.handle_event(e);tick(app);}
std::unique_ptr<PlayerApp> boot(const std::filesystem::path& path,const std::filesystem::path& saves){std::string e;PlayerBootOptions options;options.enableScripts=false;options.physicsBackend=Physics3DBackend::Reference;options.saveDirectory=saves;auto content=open_content_source(path);check(bool(content),"content");auto app=PlayerApp::boot(std::move(content),options,&e);check(bool(app),e);return app;}
void authoring(const std::filesystem::path& root){
 auto d=make_prototype_starter();std::string e;check(d.validate(&e)&&validate_editor_prototype(d,&e),e);auto copy=clone_editor_document(d);auto* first=copy.find_object(copy.objects().begin()->first);auto original=std::get<std::string>(first->components[0].properties.at("key"));first->components[0].properties["key"]=std::string("gate");check(!validate_editor_prototype(copy,&e),"duplicate keys rejected");first->components[0].properties["key"]=original;
 for(const auto& [id,o]:copy.objects())for(const auto& c:o.components)if(c.type=="dve.prototype.link"){copy.find_object(id)->components.back().properties["target"]=std::string("missing");check(!validate_editor_prototype(copy,&e),"missing link rejected");goto checked;}
 checked:
 NativeEditorController controller{EditorWorkspace(EditorDocument("Empty"))};controller.open_prototype_panel();check(controller.prototype_panel_action("starter"),controller.prototype_panel().message);check(controller.workspace().document().objects().size()==d.objects().size(),"starter inserted");check(controller.workspace().commands().undo(controller.workspace().document()).success&&controller.workspace().document().objects().empty(),"starter one undo");check(controller.workspace().commands().redo(controller.workspace().document()).success,"starter redo");check(!controller.prototype_panel_action("starter"),"starter cannot overwrite scene");
 auto& doc=controller.workspace().document();EditorObjectId button=0,gate=0;for(const auto& [id,o]:doc.objects()){if(o.name=="Switch")button=id;if(o.name=="Door")gate=id;}controller.workspace().select_object(button);check(controller.prototype_panel_action("component"),"select connection");auto* object=doc.find_object(button);ComponentId link=object->components.back().id;
 check(controller.prototype_panel_action("edit:"+std::to_string(link)+":target"),"open target picker");check(controller.prototype_panel().picking,"picker active");check(controller.prototype_panel_action("target:"+std::to_string(gate)),"pick actual object reference");check(std::get<std::string>(object->components.back().properties.at("target"))=="gate","picker stores stable key");check(controller.workspace().commands().undo(doc).success,"connection undo");
 auto library=EditorMaterialLibrary::make_default();check(export_prototype_project(d,library,root/"project",&e),e);check(!export_prototype_project(d,library,root/"project",&e),"no output overwrite");
 auto read=EditorDocument::load(root/"project"/"scenes"/"prototype.dvescene",&e);check(read&&validate_editor_prototype(*read,&e),e);
 EditorPlaySession session;EditorWorkspace workspace(clone_editor_document(d));auto before=workspace.document().objects().size();EditorPlaySessionConfig config;config.preferProductionPhysics=false;config.runStartupScriptInPlay=false;check(session.start(workspace,library,EditorMode::Play,config,{},&e),e);session.set_action_pressed("interact",true);check(session.update(workspace,.02F,&e),e);check(session.hud().prototype&&session.hud().prototype->active,"same prototype active in editor Play");check(session.runtime_action_pressed("interact"),"input reaches runtime");session.clear_input();check(session.update(workspace,.02F,&e)&&!session.runtime_action_pressed("interact"),"cleared input releases runtime actions");check(session.prototype_camera()->up.z==1,"Z-up possessed camera");check(session.pause(&e)&&session.single_step(workspace,&e),e);check(session.stop(workspace,&e),e);check(workspace.document().objects().size()==before,"Stop restores authoring objects");for(const auto& [id,o]:workspace.document().objects()){(void)id;check(!find_component_by_type(o.components,"dve.prototype.state"),"runtime metadata not written to authored scene");}
}
void playable(const std::filesystem::path& root){
 auto app=boot(root/"project",root/"saves");check(app->prototype_hud()&&app->world().geometry_kind(named(*app,"Player Start"))==GameGeometryKind::Marker,"exported player is a marker");tick(*app);const auto initial=app->camera(16.F/9);check(initial.up.z==1,"player camera Z up");
 // Input genuinely drives the shared capsule controller and the closed door blocks it.
 app->world().set_axis("move_y",1); // PlayerInput overwrites only bindings, so use actual key events.
 platform::PlatformEvent key;key.type=platform::EventType::KeyDown;key.key="w";app->handle_event(key);tick(*app,50);key.type=platform::EventType::KeyUp;app->handle_event(key);tick(*app,10);
 auto pawn=named(*app,"Player Start");auto p=app->world().position(pawn).value();check(p.y>1&&p.y<2.7F,"movement advances and stops at closed gate: "+std::to_string(p.y)+" z="+std::to_string(p.z));
 teleport(*app,{0,0,.05F});tap(*app,"e");check(!app->world().is_enabled(named(*app,"Door")),"short E tap fires switch link and opens door");tick(*app,2);
 teleport(*app,{0,2,.05F});key.type=platform::EventType::KeyDown;app->handle_event(key);tick(*app,24);key.type=platform::EventType::KeyUp;app->handle_event(key);tick(*app,6);check(app->world().position(pawn)->y>3.5F,"opened gate no longer blocks capsule movement");
 for(auto pos:{Float3{1,1,.05F},Float3{-.8F,5,.05F},Float3{.8F,7,.05F}}){teleport(*app,pos);tick(*app,2);}check(app->prototype_hud()->collected==3,"three pickups collected once");tick(*app,3);check(app->prototype_hud()->collected==3,"no duplicate collection");
 teleport(*app,{0,5,.05F});tick(*app,2);teleport(*app,{0,8,.05F});tick(*app);check(app->prototype_hud()->health==75,"damage strip applies once");tick(*app,3);check(app->prototype_hud()->health==75,"damage interval respected");
 auto saved=app->save_game("checkpoint");check(bool(saved),"prototype save");auto hp=app->prototype_hud()->health;tick(*app,3);std::string error;auto loaded=app->load_game("checkpoint",&error);check(bool(loaded),error);check(app->prototype_hud()->health==hp&&app->prototype_hud()->collected==3,"saved health and count restored");tick(*app);check(app->prototype_hud()->health==hp,"damage cooldown restored");
 teleport(*app,{0,5,.05F});tick(*app,2);const auto time=app->world().elapsed_seconds();tap(*app,"p");tick(*app,10);check(app->prototype_hud()->phase==PrototypePhase::Paused&&app->world().elapsed_seconds()==time,"game pause freezes simulation");tap(*app,"p");check(app->prototype_hud()->phase==PrototypePhase::Playing,"resume");
 teleport(*app,{0,5,-50});tick(*app);check(app->prototype_hud()->health==100&&std::abs(app->world().position(pawn)->y-5)<.1F,"death respawns at checkpoint with health");
 teleport(*app,{0,10,.05F});tick(*app);check(app->prototype_hud()->phase==PrototypePhase::Won,"goal wins after required pickups");auto wonTime=app->world().elapsed_seconds();tick(*app,3);check(app->world().elapsed_seconds()==wonTime,"won state frozen");
 auto renderer=make_cpu_player_renderer(nullptr);check(renderer->resize(384,216,&error),error);std::vector<GameRenderObject> objects;check(renderer->render(app->render_view(objects,16.F/9),&error),error);const auto* image=renderer->readback();check(image&&image->valid(),"HUD render");std::ofstream ppm(root/"prototype.ppm",std::ios::binary);ppm<<"P6\n"<<image->width<<' '<<image->height<<"\n255\n";for(std::size_t n=0;n<image->pixels.size();n+=4){ppm.put(char(image->pixels[n]));ppm.put(char(image->pixels[n+1]));ppm.put(char(image->pixels[n+2]));}
 auto pixels=image->pixels;auto saveWin=app->save_game("won");check(bool(saveWin),"save won");check(bool(app->load_game("won",&error)),error);objects.clear();check(renderer->render(app->render_view(objects,16.F/9),&error),error);check(renderer->readback()->pixels==pixels,"save/load retains camera and HUD exactly");
 tap(*app,"r");check(app->prototype_hud()->phase==PrototypePhase::Playing&&app->prototype_hud()->collected==0&&app->world().is_enabled(named(*app,"Door"))&&app->world().is_enabled(named(*app,"Collectible 1")),"restart resets kit state");
 // Same playable scene from a package, without any editor or Lua runtime linkage.
 std::vector<std::filesystem::path> files;for(const auto& file:std::filesystem::recursive_directory_iterator(root/"project"))if(file.is_regular_file())files.push_back(std::filesystem::relative(file.path(),root/"project"));check(build_dvepak(root/"project",files,root/"game.dvepak",{},nullptr,&error),error);auto packed=boot(root/"game.dvepak",root/"packed-saves");tick(*packed);tap(*packed,"e");check(!packed->world().is_enabled(named(*packed,"Door")),"packaged game interactions work");
}
void cycles_and_losses(const std::filesystem::path& root){
 auto doc=make_prototype_starter();EditorObject* gate=nullptr;for(const auto& [id,o]:doc.objects()){if(o.name=="Door")gate=doc.find_object(id);if(o.name=="Game Rules")doc.find_object(id)->components[0].properties["auto_respawn"]=false;}
 for(const auto* event:{"opened","closed"}){auto c=make_component_from_schema("dve.prototype.link",100+gate->components.size()).value();c.properties["event"]=std::string(event);c.properties["target"]=std::string("gate");c.properties["action"]=std::string("toggle");gate->components.push_back(c);}
 std::string e;check(export_prototype_project(doc,EditorMaterialLibrary::make_default(),root/"cycle",&e),e);auto app=boot(root/"cycle",root/"cycle-saves");tap(*app,"e");check(app->prototype_hud()->phase==PrototypePhase::Lost&&app->prototype_hud()->message.find("cycle")!=std::string::npos,"event cycles terminate at budget");
 auto normal=make_prototype_starter();for(const auto& [id,o]:normal.objects())if(o.name=="Game Rules")normal.find_object(id)->components[0].properties["auto_respawn"]=false;check(export_prototype_project(normal,EditorMaterialLibrary::make_default(),root/"loss",&e),e);auto loss=boot(root/"loss",root/"loss-saves");teleport(*loss,{0,0,-50});tick(*loss);check(loss->prototype_hud()->phase==PrototypePhase::Lost,"loss screen without automatic respawn");
}
}
void regressions(const std::filesystem::path& root){
 std::string e;auto library=EditorMaterialLibrary::make_default();
 // Starter creation extends the open scene (keeps its name) and undo removes exactly the starter.
 {NativeEditorController controller{EditorWorkspace(EditorDocument("My scene"))};controller.open_prototype_panel();check(controller.prototype_panel_action("starter"),controller.prototype_panel().message);
  check(controller.workspace().document().name()=="My scene"&&controller.workspace().document().dirty(),"starter keeps the scene identity and marks it dirty");
  check(controller.workspace().commands().undo(controller.workspace().document()).success&&controller.workspace().document().objects().empty()&&controller.workspace().document().name()=="My scene","starter undo leaves the scene empty");}
 // A text edit started on one object must not be committed into another object after the selection changes.
 {NativeEditorController controller{EditorWorkspace(EditorDocument("Empty"))};controller.open_prototype_panel();check(controller.prototype_panel_action("starter"),controller.prototype_panel().message);
  auto& doc=controller.workspace().document();EditorObjectId button=0;for(const auto& [id,o]:doc.objects())if(o.name=="Switch")button=id;controller.workspace().select_object(button);
  check(controller.prototype_panel_action("edit:1:key"),"begin key edit");controller.prototype_panel_text("hacked");check(controller.prototype_panel_action("marker"),"create marker while editing");
  const auto marker=*controller.workspace().selected_object();check(marker!=button,"marker selected");check(controller.prototype_panel().editingProperty.empty(),"unrelated action abandons the edit");(void)controller.prototype_panel_key("enter");
  check(std::get<std::string>(doc.find_component(marker,1)->properties.at("key")).starts_with("player_start-"),"stale edit did not touch the new marker");check(std::get<std::string>(doc.find_component(button,1)->properties.at("key"))=="button","original actor unchanged");}
 // A .dmesh-backed object is polygon geometry, never a valid Player Start marker.
 {auto d=make_prototype_starter();for(const auto& [id,o]:d.objects())if(o.name=="Player Start"){d.find_object(id)->sourceAsset="assets/player.dmesh";check(!prototype_object_is_marker(*d.find_object(id)),"dmesh source is not a marker");}
  check(!validate_editor_prototype(d,&e),"Player Start backed by a .dmesh asset is rejected");}
 // Tagging the Player Start synthesizes dve.membership: the exported marker ids stay unique and the scene boots.
 {auto d=make_prototype_starter();for(const auto& [id,o]:d.objects())if(o.name=="Player Start")d.find_object(id)->tags={"hero"};check(export_prototype_project(d,library,root/"tagged",&e),e);
  auto app=boot(root/"tagged",root/"tagged-saves");check(app->prototype_hud()&&app->world().geometry_kind(named(*app,"Player Start"))==GameGeometryKind::Marker,"tagged Player Start boots as a marker");
  const auto* components=app->world().components(named(*app,"Player Start"));check(components&&find_component_by_type(*components,"dve.membership")&&find_component_by_type(*components,"dve.marker"),"membership and marker both exported");}
 // A trailing separator names the same new folder.
 {auto d=make_prototype_starter();check(export_prototype_project(d,library,(root/"slash").string()+"/",&e),e);check(std::filesystem::is_regular_file(root/"slash"/"game.dvegame"),"trailing-separator output written");}
}
int main(){auto root=std::filesystem::temp_directory_path()/("dve-prototype-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));try{std::filesystem::create_directories(root);authoring(root);playable(root);cycles_and_losses(root);regressions(root);std::error_code removed;std::filesystem::remove_all(root,removed);std::cout<<"Prototype kit: PASS\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<"; artifacts "<<root<<'\n';return 1;}}
