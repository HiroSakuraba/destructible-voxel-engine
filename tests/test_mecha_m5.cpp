#include "dve/mecha_game.hpp"
#include "dve/game_world.hpp"
#include "dve/editor_prototype.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/player/player_app.hpp"
#include "dve/physics3d_backend.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
using namespace dve;
using namespace dve::editor;
using namespace dve::player;
namespace {
void require(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
Component role(std::string type,std::string key){
 auto c=make_component_from_schema("dve.prototype."+type,1);
 require(bool(c),"missing component schema "+type);
 c->properties["key"]=std::move(key);return *c;
}
// capture_save_state() omits polygon geometry (file saves re-read it from the asset source);
// an in-memory restore_save_state() needs it, so attach the live immutable geometry.
GameWorldSaveState capture_restorable(const GameWorld& world){
 auto state=world.capture_save_state();
 const auto renderObjects=world.render_objects();
 for(auto& item:state.objects){
  if(item.kind!=GameGeometryKind::Polygon||item.polygon)continue;
  for(const auto& r:renderObjects)if(r.id==item.id&&r.polygon)
   item.polygon=std::make_shared<const CookedPolygonAsset>(*r.polygon);
 }
 return state;
}
struct Fixture {
 GameWorld world{create_physics3d_world(Physics3DBackend::Reference)};
 std::unique_ptr<MechaGame> game;
 GameObjectId player{},target{},extraction{},wall{};
 explicit Fixture(bool voxelCover=false){
  std::string error;
  player=world.spawn_visual_polygon_asset(make_editor_polygon_cube(1),"Player",
    make_rigid_transform({0,0,.05F},{}));
  require(bool(world.add_component(player,role("mech_player","player"),&error)),
     "player: "+error);
  target=world.spawn_visual_polygon_asset(make_editor_polygon_cube(2),"Target",
    make_rigid_transform({0,10,2},{}));
  auto t=role("mech_target","target");t.properties["health"]=36.0;
  t.properties["hit_radius"]=1.5;
  require(bool(world.add_component(target,std::move(t),&error)),"target: "+error);
  GameObjectDesc extractionDesc;extractionDesc.name="Extract";
  extractionDesc.transform.position={0,20,0};
  extractionDesc.components.push_back(role("mech_extraction","extract"));
  extraction=world.create_object(std::move(extractionDesc),&error);
  require(extraction!=0,"extraction: "+error);
  GameObjectDesc rules;rules.name="Rules";
  auto c=role("mech_rules","rules");c.properties["target_count"]=std::int64_t{1};
  rules.components.push_back(std::move(c));
  require(world.create_object(std::move(rules),&error)!=0,"rules: "+error);
  if(voxelCover){
   GameObjectDesc w;w.name="Damageable voxel cover";w.dynamic=false;
   w.transform.position={4,6,.03F};w.voxelSizeMeters=.65F;
   w.voxels=std::make_unique<VoxelObject>(900);
   for(int x=-3;x<=3;++x)for(int y=-1;y<=1;++y)for(int z=0;z<7;++z)
    (void)w.voxels->set_voxel({x,y,z},MaterialId{2});
   wall=world.create_object(std::move(w),&error);
   require(wall!=0,"wall: "+error);
  }
  game=std::make_unique<MechaGame>(world);
  require(game->start(&error),"start: "+error);
 }
 void tick(int count=1){for(int n=0;n<count;++n)game->step(1.F/120);}
 void fire(const char* action){
  world.set_action_pressed(action,true);tick();
  world.set_action_pressed(action,false);tick();
 }
};
void extraction_requires_objective_and_position(){
 Fixture f;
 f.fire("mech_primary");f.tick(15);f.fire("mech_primary");
 require(!f.world.is_enabled(f.target),"target destroyed");
 require(f.game->hud().phase==PrototypePhase::Playing,
     "destroying enemies does not skip extraction phase");
 require(f.game->hud().vehicleTelemetry.find("EXTRACT")!=std::string::npos,
     "mission HUD advances to extract");
 f.world.set_axis("move_y",1.F);
 f.tick(350);
 f.world.set_axis("move_y",0.F);
 require(f.game->hud().phase==PrototypePhase::Won,"entering extraction finishes mission");
}
void save_restore_validates_and_preserves_controller(){
 Fixture f;
 f.fire("mech_primary");
 f.world.set_axis("move_y",1.F);f.tick(25);f.world.set_axis("move_y",0.F);
 f.fire("mech_secondary");f.tick(5);
 const auto controller=f.game->save_state();
 require(controller.starts_with("DVE_MECHA 2 "),"versioned serialized combat state");
 const auto worldSnapshot=capture_restorable(f.world);
 const auto previous=f.game->telemetry().autocannonShots;
 f.tick(80); // let the projectile move; restoring must return it to the earlier position
 std::string error;
 const bool worldRestored=f.world.restore_save_state(worldSnapshot,nullptr,&error);
 require(worldRestored,"world restore: "+error);
 const bool mechaRestored=f.game->after_restore(controller,&error);
 require(mechaRestored,"mecha restore: "+error);
 require(f.game->telemetry().autocannonShots==previous,"fire counter restored");
 require(f.game->save_state()==controller,"full controller state roundtrip");
 const auto stable=f.game->save_state();
 require(!f.game->after_restore(stable+" trailing",&error),"malformed state rejected");
 require(f.game->save_state()==stable,"invalid decode does not partially change live state");
}
void restart_restores_voxels_and_mission(){
 Fixture f(true);
 require(f.world.geometry_kind(f.wall)==GameGeometryKind::Voxel,"voxel wall");
 const auto before=f.world.raycast({4,-2,2},{0,1,0},20);
 require(before&&before->objectId==f.wall,"wall blocks ray before damage");
 require(bool(f.world.queue_damage_sphere(f.wall,{4,6,2},3.1F)),"queued damage");
 f.tick(20);
 const auto after=f.world.raycast({4,-2,2},{0,1,0},20);
 require(!after||after->objectId!=f.wall,"damage breaches wall");
 f.fire("mech_primary");
 f.world.set_action_pressed("restart",true);f.tick();
 f.world.set_action_pressed("restart",false);f.tick();
 const auto restored=f.world.raycast({4,-2,2},{0,1,0},20);
 require(restored&&restored->objectId==f.wall,"restart restores exact voxel cover");
 require(f.game->hud().phase==PrototypePhase::Playing&&f.game->telemetry().autocannonShots==0,
   "restart resets combat, phase and counters");
 require(f.world.is_enabled(f.target),"restart re-enables enemies");
}
void playerapp_save_reload(){
 auto scene=make_mecha_starter();
 std::string error;
 require(validate_editor_prototype(scene,&error),"M5 authored starter: "+error);
 std::size_t extractionMarkers{};
 for(const auto& [id,o]:scene.objects()){
  (void)id;
  if(find_component_by_type(o.components,"dve.prototype.mech_extraction"))++extractionMarkers;
 }
 require(extractionMarkers==1,"Iron Yard contains extraction objective");
 const auto root=std::filesystem::current_path()/"mecha-m5-test-output";
 std::filesystem::remove_all(root);
 std::filesystem::create_directories(root);
 auto mats=EditorMaterialLibrary::make_default();
 require(export_prototype_project(scene,mats,root/"game",&error),"export: "+error);
 auto makePlayer=[&](){
  auto content=open_content_source(root/"game");
  require(bool(content),"content source");
  PlayerBootOptions options;options.enableScripts=false;
  options.physicsBackend=Physics3DBackend::Reference;options.saveDirectory=root/"saves";
  return PlayerApp::boot(std::move(content),options,&error);
 };
 auto app=makePlayer();require(bool(app),"boot: "+error);
 for(int n=0;n<20;++n)app->tick(1.F/120);
 const auto baseline=app->world().state_hash();
 const auto saved=app->save_game("checkpoint",&error);
 require(bool(saved),"save: "+error);
 for(int n=0;n<25;++n)app->tick(1.F/120);
 const auto loaded=app->load_game("checkpoint",&error);
 require(bool(loaded),"load: "+error);
 require(app->world().state_hash()==baseline,"world restores exact state and geometry");
 require(app->prototype_hud()&&app->prototype_hud()->mecha,
     "full mecha HUD restored after player session swap");
 // Full restart on the real authored Iron Yard (polygon chassis, scenery and enemies):
 // the in-memory pristine snapshot must be restorable, not just in synthetic fixtures.
 app->world().set_action_pressed("restart",true);app->tick(1.F/120);
 app->world().set_action_pressed("restart",false);app->tick(1.F/120);
 require(app->prototype_hud()->phase==PrototypePhase::Playing,"restart on authored starter keeps playing: "+app->prototype_hud()->message);
 // The restored save is also loadable directly at game boot (--load).
 auto resumed=makePlayer();require(bool(resumed),"new boot: "+error);
 auto atBoot=resumed->load_game("checkpoint",&error);
 require(bool(atBoot),"load into fresh player: "+error);
 require(resumed->world().state_hash()==baseline,"fresh boot restores same saved world");
 // Build a structurally valid DVESAVE containing a corrupt mecha game section.
 // Failure must preserve the live PlayerApp session and all world objects.
 SaveGameReadReport readReport;
 auto tampered=app->save_codec().read_file(saved->path,{},&readReport,&error);
 require(bool(tampered),"read valid save for tampering: "+error);
 auto& bytes=tampered->gameSections["game.mecha.v1"];
 require(!bytes.empty(),"mecha section is persisted in the standard DVESAVE file");
 bytes.assign({std::byte{'X'},std::byte{'Y'},std::byte{'Z'}});
 const auto invalid=root/"saves"/"invalid.dvesave";
 GameSaveStats saveStats;
 require(app->save_codec().write_file(invalid,*tampered,&saveStats,&error),
   "write invalid controller section: "+error);
 const auto liveHash=app->world().state_hash();
 const auto rejected=app->load_game("invalid",&error);
 require(!rejected,"bad mecha section rejects load");
 require(app->world().state_hash()==liveHash,
   "failed load does not mutate the running world");
 std::filesystem::remove_all(root);
}
}
int main(){
 try{
  extraction_requires_objective_and_position();
  save_restore_validates_and_preserves_controller();
  restart_restores_voxels_and_mission();
  playerapp_save_reload();
  std::cout<<"mecha M5 mission, restart and checkpoint tests passed\n";
  return 0;
 }catch(const std::exception& e){
  std::cerr<<"mecha M5: "<<e.what()<<"\n";return 1;
 }
}
