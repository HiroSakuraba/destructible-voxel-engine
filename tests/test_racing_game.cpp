#include "dve/racing_game.hpp"
#include <algorithm>
#include "dve/editor_native.hpp"
#include "dve/editor_prototype.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/player/player_app.hpp"
#include "dve/player/player_renderer.hpp"
#include "dve/v235_foundations.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace dve;using namespace dve::editor;using namespace dve::player;
namespace {
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
Component comp(const char* role,std::string key){auto c=make_component_from_schema(std::string("dve.prototype.")+role,1);check(bool(c),"component schema");c->properties["key"]=std::move(key);return *c;}
struct Fixture {
 GameWorld world{create_physics3d_world(Physics3DBackend::Reference)};std::unique_ptr<RacingGame> game;GameObjectId car{},rules{};
 explicit Fixture(bool barrier=false,double grip=7,bool sideBarrier=false,std::string profile="street"){std::string error;auto floor=make_editor_polygon_cube(1);for(auto& v:floor.vertices)v.position={(v.position.x-.5F)*200,(v.position.y-.5F)*200,(v.position.z-1)*.5F};refresh_editor_polygon(floor,true);check(world.spawn_cooked_polygon_asset(std::move(floor),"Floor",{},false)!=0,"floor");
 auto mesh=make_editor_polygon_cube(2);car=world.spawn_visual_polygon_asset(std::move(mesh),"Car",make_rigid_transform({0,0,.04F},{}));auto carConfig=comp("race_car","car");carConfig.properties["grip"]=grip;carConfig.properties["handling_profile"]=std::move(profile);check(bool(world.add_component(car,std::move(carConfig),&error)),error);
 GameObjectDesc d;d.name="Rules";auto c=comp("race_rules","rules");c.properties["countdown"]=0.0;c.properties["laps"]=std::int64_t{2};d.components={c};rules=world.create_object(std::move(d));
 for(unsigned n=0;n<4;++n){GameObjectDesc gate;gate.name="Gate "+std::to_string(n);gate.transform.position={0,n?float(n)*10:40,0};auto g=comp("race_gate","gate"+std::to_string(n));g.properties["order"]=std::int64_t(n);gate.components={g};check(world.create_object(std::move(gate))!=0,"gate");}
 if(barrier){auto wall=make_editor_polygon_cube(10);for(auto& v:wall.vertices)v.position={(v.position.x-.5F)*20,v.position.y*.1F+6,v.position.z*4};refresh_editor_polygon(wall,true);check(world.spawn_cooked_polygon_asset(std::move(wall),"Wall",{},false)!=0,"wall");}
 if(sideBarrier){auto side=make_editor_polygon_cube(11);for(auto& v:side.vertices)v.position={(v.position.x-.5F)*.1F+.962F,(v.position.y-.5F)*40,v.position.z*4};refresh_editor_polygon(side,true);check(world.spawn_cooked_polygon_asset(std::move(side),"Side Wall",{},false)!=0,"side wall");}
 game=std::make_unique<RacingGame>(world);check(game->start(&error),error);
 }
 void tick(unsigned count=1,float dt=1.F/60){for(unsigned n=0;n<count;++n)game->step(dt);}
 void tap(const char* key){world.set_action_pressed(key,true);tick();world.set_action_pressed(key,false);tick();}
};
void gate_rules(){RigidTransform p;check(racing_gate_crossed({0,-2,0},{0,2,0},p,3,2),"forward gate crossing");check(!racing_gate_crossed({0,2,0},{0,-2,0},p,3,2),"reverse gate ignored");check(!racing_gate_crossed({4,-2,0},{4,2,0},p,3,2),"wide shortcut ignored");check(!racing_gate_crossed({0,-2,3},{0,2,3},p,3,2),"over-gate shortcut ignored");
 auto d=make_racing_starter();std::string error;check(validate_editor_prototype(d,&error),error);for(const auto& [id,o]:d.objects()){(void)o;if(auto* c=find_component_by_type(std::span<Component>{d.find_object(id)->components},"dve.prototype.race_gate")){if(std::get<std::int64_t>(c->properties.at("order"))==0)continue;c->properties["order"]=std::int64_t{0};break;}}
 check(!validate_editor_prototype(d,&error),"duplicate gate orders refused");
}
void driving(){Fixture a,b;a.world.set_axis("move_y",1);b.world.set_axis("move_y",1);a.tick(120);b.tick(240,1.F/120);
 check(length(subtract(*a.world.position(a.car),*b.world.position(b.car)))<.001F,"fixed driving identical at 60 and 120 Hz");check(a.world.position(a.car)->y>15,"throttle actually drives");
 const auto speed=a.game->status().speedMetersPerSecond;a.world.set_action_pressed("jump",true);a.tick(30);check(a.game->status().speedMetersPerSecond<speed,"handbrake reduces speed");a.world.set_action_pressed("jump",false);
 const auto before=a.world.transform(a.car)->rotation;a.world.set_axis("move_x",1);a.tick(30);const auto after=a.world.transform(a.car)->rotation;check(std::abs(before.z-after.z)>.01F,"steering rotates the car");
 auto time=a.world.elapsed_seconds();a.tap("pause");a.tick(10);check(a.game->hud().phase==PrototypePhase::Paused&&a.world.elapsed_seconds()==time,"pause freezes world and race timer");a.tap("pause");
 auto camera=a.game->camera();a.tap("interact");check(length(subtract(camera.position,a.game->camera().position))>1,"chase and hood camera differ");
 const auto elapsed=a.game->status().elapsed;a.tap("crouch");check(a.game->status().elapsed>=elapsed+5,"recovery adds penalty");
 a.tap("restart");check(a.game->status().completedLaps==0&&a.game->status().nextGate==1&&a.game->status().elapsed<.1,"restart resets attempt");
 Fixture blocked(true);blocked.world.set_axis("move_y",1);blocked.tick(300);check(blocked.world.position(blocked.car)->y<5,"swept collision stops car at thin wall");
 Fixture low(false,.2),high(false,20);for(auto* fixture:{&low,&high}){fixture->world.set_axis("move_y",1);fixture->world.set_axis("move_x",.8F);fixture->tick(120);}check(length(subtract(*low.world.position(low.car),*high.world.position(high.car)))>.1F,"grip tuning changes cornering trajectory");
 Fixture reverse;reverse.world.set_axis("move_y",-1);reverse.tick(90);check(reverse.world.position(reverse.car)->y< -1,"reverse drives backwards");
 Fixture service;service.world.set_axis("move_y",1);service.tick(60);const float serviceSpeed=service.game->status().speedMetersPerSecond;
 service.world.set_axis("race_brake",1);service.tick(40);
 check(service.game->status().speedMetersPerSecond<serviceSpeed*.5F,"dedicated service brake overrides throttle");
 check(service.game->telemetry().longitudinalAccelerationMetersPerSecondSquared<=0,"service brake reports deceleration");
 Fixture standard(false,7,false,"street"),drift(false,7,false,"drift");
 for(auto* fixture:{&standard,&drift}){fixture->world.set_axis("move_y",1);fixture->world.set_axis("move_x",.8F);fixture->tick(120);}
 check(length(subtract(*standard.world.position(standard.car),*drift.world.position(drift.car)))>.1F,"handling profiles alter trajectories");
 check(standard.game->telemetry().grounded,"grounded telemetry");
 check(std::isfinite(drift.game->telemetry().lateralSlipMetersPerSecond)&&std::abs(drift.game->telemetry().yawRateRadiansPerSecond)>.001F,"slip/yaw telemetry");
 Fixture wall(false,7,true);wall.world.set_axis("move_y",1);wall.tick(24);const auto yBefore=wall.world.position(wall.car)->y;
 wall.world.set_axis("move_x",1);wall.tick(25);wall.world.set_axis("move_x",-1);wall.tick(40);
 check(wall.world.position(wall.car)->y>yBefore+.15F,"steering near parallel barrier must not trap car");
 check(wall.game->telemetry().collisionCount<500U,"bounded collision reporting");
 auto chase=wall.game->camera();check(std::isfinite(chase.position.x)&&std::isfinite(chase.position.y),"smooth camera finite");
 auto bad=comp("race_car","invalid");bad.properties["handling_profile"]=std::string{"hovercraft"};
 check(!validate_racing_scene(std::vector<PrototypeObjectView>{{0,"Bad car",std::span<const Component>(&bad,1),false}},nullptr),"invalid profile rejected");
}
void race_and_restore(){Fixture f;f.world.set_axis("move_y",1);
 // Crossing finish before all checkpoints cannot complete a lap.
 check(f.world.set_position(f.car,{0,39.99F,.04F}),"place at finish");f.tick(8);check(f.game->status().completedLaps==0,"finish shortcut rejected");
 for(unsigned lap=0;lap<2;++lap)for(unsigned gate:{1U,2U,3U,0U}){const float y=gate?float(gate)*10:40;check(f.world.set_position(f.car,{0,y-.01F,.04F}),"place before expected gate");f.tick(10);check(f.game->status().nextGate==(gate+1)%4,"expected gate accepted");}
 check(f.game->hud().phase==PrototypePhase::Won&&f.game->status().completedLaps==2&&f.game->status().bestLap>0,"ordered lap sequence wins and records lap times");const auto best=f.game->status().bestLap,time=f.world.elapsed_seconds();f.tick(10);check(f.world.elapsed_seconds()==time,"finish freezes simulation");f.tap("restart");check(f.game->status().bestLap==best&&f.game->hud().phase==PrototypePhase::Playing,"restart retains session best");
 f.tick(10);auto snapshot=f.world.capture_save_state();for(auto& object:snapshot.objects)if(object.kind==GameGeometryKind::Polygon)for(const auto& view:f.world.render_objects())if(view.id==object.id)object.polygon=std::make_shared<CookedPolygonAsset>(*view.polygon);const auto elapsed=f.game->status().elapsed;f.tick(20);std::string error;check(f.world.restore_save_state(snapshot,nullptr,&error),error);check(f.game->after_restore(&error),error);check(f.game->status().elapsed==elapsed,"save restore retains race clock");
 auto* states=f.world.components(f.rules);auto* state=find_component_by_type(*states,"dve.prototype.race_state");check(state!=nullptr,"save state component");check(f.world.set_component_property(f.rules,state->id,"data",std::string{"broken"}),"inject invalid state");check(!f.game->after_restore(&error),"malformed save payload refused");
}
std::unique_ptr<PlayerApp> boot(const std::filesystem::path& path,const std::filesystem::path& saves){PlayerBootOptions options;options.enableScripts=false;options.physicsBackend=Physics3DBackend::Reference;options.saveDirectory=saves;std::string error;auto content=open_content_source(path);check(bool(content),"content source");auto app=PlayerApp::boot(std::move(content),options,&error);check(bool(app),error);return app;}
void integration(const std::filesystem::path& root){
 std::string error;auto document=make_racing_starter();auto materials=EditorMaterialLibrary::make_default();check(export_prototype_project(document,materials,root/"project",&error),error);check(!export_prototype_project(document,materials,root/"project",&error),"existing output refused");
 auto controller=std::make_unique<NativeEditorController>();controller->resize(1280,800);controller->open_prototype_panel();check(controller->prototype_panel_action("racing"),controller->prototype_panel().message);auto& d=controller->workspace().document();check(controller->workspace().commands().undo(d).success&&d.objects().empty(),"racing starter one undo");check(controller->workspace().commands().redo(d).success,"racing redo");check(!controller->prototype_panel_action("racing"),"starter refuses occupied scene");
 EditorWorkspace workspace(clone_editor_document(document));EditorPlaySession session;EditorPlaySessionConfig config;config.physicsBackend=Physics3DBackend::Reference;check(session.start(workspace,materials,EditorMode::Play,config,{},&error),error);check(session.update(workspace,1.F/60,&error),error);check(session.hud().prototype&&session.hud().prototype->racing,"editor uses racing runtime");session.set_axis("move_y",1);for(unsigned n=0;n<250;++n)check(session.update(workspace,1.F/60,&error),error);check(session.prototype_camera()->up.z==1,"Z-up racing camera in editor");check(session.stop(workspace,&error),error);for(const auto& [id,original]:document.objects()){const auto* restored=workspace.document().find_object(id);check(restored&&length(subtract(restored->transform.position,original.transform.position))<.0001F,"Stop restores authored transforms");check(!find_component_by_type(restored->components,"dve.prototype.race_state"),"runtime state never leaks into authored scene");}
 auto app=boot(root/"project",root/"saves");check(app->prototype_hud()&&app->prototype_hud()->racing,"standalone racing HUD");const auto car=*app->world().find_by_name("Race Car");const auto start=*app->world().position(car);platform::PlatformEvent e;e.type=platform::EventType::KeyDown;e.key="w";app->handle_event(e);for(unsigned n=0;n<150;++n)app->tick(1.F/60);check(length(subtract(*app->world().position(car),start))<.001F,"countdown locks car");for(unsigned n=0;n<150;++n)app->tick(1.F/60);check(app->world().position(car)->x>start.x+10,"keyboard drives exported car");e.type=platform::EventType::KeyUp;app->handle_event(e);
 auto saved=app->save_game("race",&error);check(bool(saved),error);const auto hud=app->prototype_hud()->prompt;const auto position=*app->world().position(car);for(unsigned n=0;n<10;++n)app->tick(1.F/60);check(bool(app->load_game("race",&error)),error);check(app->prototype_hud()->prompt==hud&&length(subtract(*app->world().position(car),position))<.0001F,"standalone save restores race and car");
 auto renderer=make_cpu_player_renderer(nullptr);check(renderer->resize(480,270,&error),error);std::vector<GameRenderObject> objects;check(renderer->render(app->render_view(objects,16.F/9),&error),error);const auto* image=renderer->readback();check(image&&image->valid(),"racing framebuffer");std::ofstream ppm(root/"racing.ppm",std::ios::binary);ppm<<"P6\n"<<image->width<<' '<<image->height<<"\n255\n";for(std::size_t n=0;n<image->pixels.size();n+=4){ppm.put(char(image->pixels[n]));ppm.put(char(image->pixels[n+1]));ppm.put(char(image->pixels[n+2]));}
 std::vector<std::filesystem::path> files;for(const auto& f:std::filesystem::recursive_directory_iterator(root/"project"))if(f.is_regular_file())files.push_back(std::filesystem::relative(f.path(),root/"project"));check(build_dvepak(root/"project",files,root/"racing.dvepak",{},nullptr,&error),error);auto packed=boot(root/"racing.dvepak",root/"packed-saves");check(packed->prototype_hud()->racing,"packaged racer boots same runtime");
}
}
int main(){try{const auto root=std::filesystem::current_path()/"racing-test-output";std::filesystem::remove_all(root);std::filesystem::create_directories(root);gate_rules();driving();race_and_restore();integration(root);std::cout<<"Racing handling, gates, time trial, editor/player, save and package checks passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
