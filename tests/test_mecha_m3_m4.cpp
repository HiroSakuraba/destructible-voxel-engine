#include "dve/mecha_game.hpp"
#include "dve/mecha_navigation.hpp"
#include "dve/editor_prototype.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/game_world.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
using namespace dve;
using namespace dve::editor;
namespace {
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
Component role(const char* name,std::string key){
 auto c=make_component_from_schema(std::string("dve.prototype.")+name,1);
 check(bool(c),"missing mecha schema");c->properties["key"]=std::move(key);return *c;
}
struct Scenario {
 GameWorld world{create_physics3d_world(Physics3DBackend::Reference)};
 GameObjectId player{},enemy{},barrier{};
 std::unique_ptr<MechaGame> game;
 Scenario(MechEnemyKind kind,bool hasWall=false,bool wallVoxels=false,
          Float3 playerPos={0,-8,.05F},Float3 enemyPos={0,10,1.6F}){
  std::string error;
  player=world.spawn_visual_polygon_asset(make_editor_polygon_cube(1),
     "Mech",make_rigid_transform(playerPos,{}));
  check(player!=0,"spawn player");
  check(bool(world.add_component(player,role("mech_player","player"),&error)),"add player: "+error);
  enemy=world.spawn_visual_polygon_asset(make_editor_polygon_cube(2),
     "Enemy",make_rigid_transform(enemyPos,{}));
  check(enemy!=0,"spawn enemy");
  auto config=role("mech_target","enemy");
  config.properties["health"]=100.0;
  config.properties["hit_radius"]=1.2;
  config.properties["ai_kind"]=std::int64_t(kind);
  config.properties["move_speed"]=5.0;
  config.properties["attack_range"]=35.0;
  config.properties["damage_per_shot"]=4.0;
  config.properties["shot_interval"]=.65;
  check(bool(world.add_component(enemy,std::move(config),&error)),"add enemy: "+error);
  GameObjectDesc rules;rules.name="Rules";
  auto rule=role("mech_rules","rules");rule.properties["target_count"]=std::int64_t{1};
  rules.components={std::move(rule)};
  check(world.create_object(std::move(rules))!=0,"create rules");
  if(hasWall){
   if(wallVoxels){
    GameObjectDesc wall;wall.name="Voxel Cover";wall.dynamic=false;
    wall.transform.position={0,0,.03F};wall.voxelSizeMeters=.65F;
    wall.voxels=std::make_unique<VoxelObject>(400);
    for(int x=-7;x<=7;++x)for(int y=-1;y<=1;++y)for(int z=0;z<7;++z)
     (void)wall.voxels->set_voxel({x,y,z},MaterialId{2});
    barrier=world.create_object(std::move(wall),&error);
   }else{
    auto poly=make_editor_polygon_cube(4);
    for(auto& v:poly.vertices)v.position={(v.position.x-.5F)*11,
         (v.position.y-.5F)*1.3F,(v.position.z-.5F)*5+2.5F};
    refresh_editor_polygon(poly,true);
    barrier=world.spawn_cooked_polygon_asset(std::move(poly),"Solid Cover",{},false);
   }
   check(barrier!=0,"create blocking cover: "+error);
  }
  game=std::make_unique<MechaGame>(world);
  check(game->start(&error),"start mecha: "+error);
 }
 void tick(int count){for(int i=0;i<count;++i)game->step(1.F/120);}
};
void pathfinding(){
 const auto blocked=[](Float3 p){return !(p.x>=2.F&&p.x<=4.F&&std::abs(p.y)<=2.F);};
 auto route=plan_mech_navigation_xy({0,0,0},{8,0,0},blocked,{2.F,5.F,2048});
 check(route.reachedGoal&&!route.waypoints.empty(),"A* navigates around barrier");
 check(route.examinedCells<2048,"A* respects cell budget");
 for(const auto& p:route.waypoints)check(blocked(p),"route does not enter blocked cells");
 const auto last=route.waypoints.back();
 check(std::abs(last.x-8)<.01F&&std::abs(last.y)<.01F,"path reaches exact goal");
 const auto inaccessible=plan_mech_navigation_xy({0,0,0},{8,0,0},
    [](Float3 p){return p.x<1.5F;},{2.F,5.F,2048});
 check(!inaccessible.reachedGoal,"completely blocked goal gives no false route");
 auto invalid=plan_mech_navigation_xy({0,0,0},{8,0,0},blocked,{.01F,0,1024});
 check(!invalid.reachedGoal,"invalid nav settings rejected");
}
void authored_starter(){
 auto document=make_mecha_starter();
 std::string error;
 check(validate_editor_prototype(document,&error),"M1-M4 editor scene validates: "+error);
 int cover=0,scouts=0,heavies=0,turrets=0;
 for(const auto& [id,o]:document.objects()){
  (void)id;
  if(o.name.find("Destructible")!=std::string::npos){
   check(o.voxels&&o.voxels->occupied_voxel_count()>100,"authored walls contain actual voxels");
   check(o.flags.collisionEnabled,"voxel walls are collidable");
   ++cover;
  }
  if(const auto* c=find_component_by_type(o.components,"dve.prototype.mech_target")){
   const auto kind=std::get<std::int64_t>(c->properties.at("ai_kind"));
   if(kind==1)++scouts;if(kind==2)++heavies;if(kind==3)++turrets;
  }
 }
 check(cover>=2&&scouts==1&&heavies==1&&turrets==1,"starter has real voxel cover and all three AI types");
}
void destruction(){
 Scenario s(MechEnemyKind::Dummy,true,true);
 check(s.world.geometry_kind(s.barrier)==GameGeometryKind::Voxel,"cover has voxel geometry");
 const auto pre=s.world.raycast({0,-8,2}, {0,1,0},15);
 check(pre&&pre->objectId==s.barrier,"voxel cover blocks initial ray");
 s.world.set_action_pressed("mech_secondary",true);
 s.tick(1);
 s.world.set_action_pressed("mech_secondary",false);
 s.tick(150);
 check(s.game->telemetry().rocketsFired==1,"rocket fires");
 check(s.game->telemetry().voxelCarvesQueued>=1,"rocket enqueues bounded voxel carve");
 check(!s.game->impact_visuals().empty()||s.game->telemetry().impacts>0,"impact occurs");
 check(s.world.pending_destruction_count()==0,"voxel updates eventually finish");
 const auto post=s.world.raycast({0,-8,2},{0,1,0},15);
 check(!post||post->objectId!=s.barrier,"rocket creates open line through voxel wall");
}
void enemy_behavior(){
 Scenario turret(MechEnemyKind::Turret);
 turret.tick(250);
 check(turret.game->telemetry().enemyShots>0,"turret fires through clear LOS");
 check(turret.game->hud().health<100,"enemy fire damages player");
 Scenario behindWall(MechEnemyKind::Turret,true,false);
 behindWall.tick(250);
 check(behindWall.game->telemetry().enemyShots==0,"turret cannot shoot through polygon cover");
 check(behindWall.game->hud().health==100,"cover protects player");
 Scenario scout(MechEnemyKind::Scout,false,false,{0,-22,.05F},{0,20,1.6F});
 const auto before=scout.world.position(scout.enemy)->y;
 scout.tick(180);
 check(scout.world.position(scout.enemy)->y<before-1,"scout advances to engage");
 Scenario blocked(MechEnemyKind::Heavy,true,false,{0,-8,.05F},{0,10,1.6F});
 blocked.tick(180);
 check(blocked.game->telemetry().navigationReplans>0,"heavy requests path when LOS blocked");
}
}
int main(){
 try{
  pathfinding();authored_starter();destruction();enemy_behavior();
  std::cout<<"mecha M3/M4 focused tests passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<"mecha M3/M4: "<<e.what()<<'\n';return 1;}
}
