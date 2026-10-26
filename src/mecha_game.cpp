#include "dve/mecha_game.hpp"
#include "dve/mecha_navigation.hpp"
#include "dve/game_world.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/component.hpp"
#include "dve/query.hpp"
#include "dve/transform.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace dve {
namespace {
constexpr std::string_view playerType="dve.prototype.mech_player";
constexpr std::string_view torsoType="dve.prototype.mech_torso";
constexpr std::string_view targetType="dve.prototype.mech_target";
constexpr std::string_view rulesType="dve.prototype.mech_rules";
constexpr float pi=3.14159265358979323846F;
Float3 plus(Float3 a,Float3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Float3 minus(Float3 a,Float3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
Float3 scaled(Float3 a,float s){return {a.x*s,a.y*s,a.z*s};}
float dot3(Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float mag(Float3 a){return std::sqrt(dot3(a,a));}
bool finite3(Float3 a){return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z);}
Float3 unit(Float3 a){const auto len=mag(a);return len>1.0e-6F?scaled(a,1.F/len):Float3{};}
float axis(float v){return std::isfinite(v)?std::clamp(v,-1.F,1.F):0.F;}
float angle_to(Float3 forward){return std::atan2(-forward.x,forward.y);}
float wrap(float a){return std::remainder(a,2.F*pi);}
float turn(float from,float toward,float limit){return wrap(from+std::clamp(wrap(toward-from),-limit,limit));}
Float3 facing(float yaw){return {-std::sin(yaw),std::cos(yaw),0};}
Quaternion rotation(float yaw){return quaternion_from_axis_angle({0,0,1},yaw);}
template<class T>T property(const Component& c,std::string_view key,T fallback){
 const auto it=c.properties.find(key);
 if(it!=c.properties.end())if(const auto v=std::get_if<T>(&it->second))return *v;
 return fallback;
}
bool actor_type(std::string_view t){return t==playerType||t==torsoType||t==targetType||t==rulesType;}
bool key_ok(std::string_view s){
 return !s.empty()&&s.size()<=64&&s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")==std::string_view::npos;
}
float sphere_hit(Float3 start,Float3 travel,Float3 center,float radius){
 // First segment hit in [0,1]. If the muzzle is inside the sphere, return zero.
 const auto m=minus(start,center);const auto a=dot3(travel,travel);
 if(a<1.e-9F)return -1;
 const auto b=dot3(m,travel),c=dot3(m,m)-radius*radius;
 if(c<=0)return 0;
 const auto discriminant=b*b-a*c;
 if(discriminant<0)return -1;
 const auto t=(-b-std::sqrt(discriminant))/a;
 return t>=0&&t<=1?t:-1;
}
}
bool valid_mech_motor_config(const MechMotorConfig& c) noexcept {
 return std::isfinite(c.maximumSpeed)&&c.maximumSpeed>0&&c.maximumSpeed<=100&&
 std::isfinite(c.acceleration)&&c.acceleration>0&&c.acceleration<=200&&
 std::isfinite(c.braking)&&c.braking>0&&c.braking<=200&&
 std::isfinite(c.boostSpeed)&&c.boostSpeed>=c.maximumSpeed&&c.boostSpeed<=200&&
 std::isfinite(c.boostEnergyCost)&&c.boostEnergyCost>0&&c.boostEnergyCost<=100&&
 std::isfinite(c.energyRegeneration)&&c.energyRegeneration>=0&&c.energyRegeneration<=100&&
 std::isfinite(c.radius)&&c.radius>=.2F&&c.radius<=5&&
 std::isfinite(c.height)&&c.height>=2*c.radius&&c.height<=20;
}
Float3 integrate_mech_motor(MechMotorState& s,const MechMotorConfig& cfg,
                            MechMotorInput in,float dt) noexcept {
 if(!valid_mech_motor_config(cfg)||!std::isfinite(dt)||dt<=0||dt>.1F||
    !finite3(s.velocity)||!finite3(s.position)||!std::isfinite(s.energy))return {};
 auto move=in.move;move.z=0;
 const auto amplitude=std::min(1.F,mag(move));
 move=unit(move);
 const bool boost=in.boost&&amplitude>.1F&&s.energy>=cfg.boostEnergyCost;
 if(boost)s.energy-=cfg.boostEnergyCost;
 else s.energy=std::min(100.F,s.energy+cfg.energyRegeneration*dt);
 const float speed=boost?cfg.boostSpeed:cfg.maximumSpeed;
 const auto desired=scaled(move,speed*amplitude);
 const auto delta=minus(desired,Float3{s.velocity.x,s.velocity.y,0});
 const float maxDelta=(amplitude>.01F?cfg.acceleration:cfg.braking)*dt;
 const auto correction=scaled(delta,mag(delta)>maxDelta?maxDelta/mag(delta):1.F);
 s.velocity.x+=correction.x;s.velocity.y+=correction.y;s.velocity.z=0;
 if(boost){s.velocity.x=desired.x;s.velocity.y=desired.y;}
 if(amplitude>.05F)s.legsYaw=turn(s.legsYaw,angle_to(move),dt*4.5F);
 return scaled(s.velocity,dt);
}
bool scene_has_mecha(std::span<const PrototypeObjectView> objects){
 for(const auto& o:objects)for(const auto& c:o.components)if(c.enabled&&actor_type(c.type))return true;
 return false;
}
void register_mecha_components(ComponentTypeRegistry& r){
 auto add=[&](std::string type,std::string name,std::vector<ComponentPropertySchema> properties){
  properties.insert(properties.begin(),{"key",std::string{"mech"},true,false,"Unique stable actor key"});
  std::string ignored;(void)r.register_type({std::move(type),std::move(name),false,std::move(properties)},&ignored);
 };
 add(std::string(playerType),"Mech chassis (Z-up, visual-only)",{
  {"speed",11.0,false,false,"Maximum movement speed (m/s)"},
  {"acceleration",24.0,false,false,"Acceleration (m/s2)"},
  {"boost_speed",24.0,false,false,"Directional boost speed (m/s)"},
  {"radius",.95,false,false,"Collision capsule radius (m)"},
  {"height",3.5,false,false,"Collision capsule height (m)"}});
 add(std::string(torsoType),"Mech rotating torso (visual-only)",{});
 add(std::string(targetType),"Mech target (visual-only)",{
  {"health",100.0,false,false,"Hit points"},
  {"hit_radius",1.2,false,false,"Gameplay hit sphere radius"},
  {"ai_kind",std::int64_t{0},false,false,"0 dummy, 1 scout, 2 heavy, 3 turret"},
  {"move_speed",4.0,false,false,"Enemy advance speed in metres per second"},
  {"attack_range",30.0,false,false,"Enemy maximum firing distance in metres"},
  {"damage_per_shot",4.0,false,false,"Enemy weapon damage to player"},
  {"shot_interval",1.4,false,false,"Seconds between enemy shots"}});
 add(std::string(rulesType),"Mech game rules (marker)",{
  {"target_count",std::int64_t{3},false,false,"Targets to destroy for victory"}});
}
bool validate_mecha_scene(std::span<const PrototypeObjectView> objects,std::string* error){
 auto fail=[&](std::string s){if(error)*error=std::move(s);return false;};
 const auto registry=ComponentTypeRegistry::make_default();
 std::set<std::string> keys;
 int players=0,rules=0,torsos=0,targets=0;std::int64_t requiredTargets=0;
 for(const auto& o:objects){
  unsigned roles=0;
  for(const auto& c:o.components)if(c.enabled&&c.type.starts_with(kPrototypePrefix)){
   if(!actor_type(c.type))return fail("Cannot mix mech and other prototype roles");
   if(++roles>1)return fail("Only one mech role per object");
   std::string issue;if(!registry.validate(c,&issue))return fail(o.name+": "+issue);
   const auto key=property(c,"key",std::string{});
   if(!key_ok(key)||!keys.insert(key).second)return fail("Duplicate or invalid mech actor key");
   if(!o.marker&&(c.type==rulesType))return fail("Mech rules require bodyless marker");
   if(c.type==rulesType){++rules;const auto n=property(c,"target_count",std::int64_t{3});if(n<1||n>64)return fail("Invalid target count");requiredTargets=n;}
   if(c.type==playerType){
    ++players;if(o.marker)return fail("Mech chassis must have visible geometry");
    MechMotorConfig cfg;cfg.maximumSpeed=float(property(c,"speed",11.0));
    cfg.acceleration=float(property(c,"acceleration",24.0));
    cfg.boostSpeed=float(property(c,"boost_speed",24.0));
    cfg.radius=float(property(c,"radius",.95));cfg.height=float(property(c,"height",3.5));
    if(!valid_mech_motor_config(cfg))return fail("Invalid mech motor configuration");
   }
   if(c.type==torsoType){++torsos;if(o.marker)return fail("Mech torso needs visible geometry");}
   if(c.type==targetType){++targets;if(o.marker)return fail("Mech target needs visible geometry");
    auto h=property(c,"health",100.0),r=property(c,"hit_radius",1.2);
    if(!std::isfinite(h)||h<=0||h>100000||!std::isfinite(r)||r<.1||r>10)return fail("Invalid mech target health or radius");
    const auto kind=property(c,"ai_kind",std::int64_t{0});
    const auto speed=property(c,"move_speed",4.0),range=property(c,"attack_range",30.0);
    const auto damage=property(c,"damage_per_shot",4.0),interval=property(c,"shot_interval",1.4);
    if(kind<0||kind>3||!std::isfinite(speed)||speed<0||speed>30||
       !std::isfinite(range)||range<2||range>200||
       !std::isfinite(damage)||damage<0||damage>100||
       !std::isfinite(interval)||interval<.15||interval>30)return fail("Invalid mech enemy parameters");
   }
  }
 }
 if(players!=1||rules!=1||torsos>1||targets<1||targets>64||requiredTargets>targets)return fail("Mech scene requires one player, one rules marker, at most one torso and 64 targets");
 return true;
}
struct MechaGame::Impl {
 struct Target {
  GameObjectId id{};std::string key;float hp{},initial{},radius{};
  MechEnemyKind kind{MechEnemyKind::Dummy};
  float moveSpeed{},attackRange{30},damagePerShot{4},shotInterval{1.4};
  float cooldown{},replanCountdown{},turretYaw{};
  Float3 spawn{};
  std::vector<Float3> path{};
  std::size_t waypoint{};
 };
 static constexpr float kBlastStandoff=.1F;
 // normal always faces back toward the trace origin (the exposed side the shot came from).
 struct TraceHit {GameObjectId id{};Float3 position{};float fraction{1};bool valid{};Float3 normal{};};
 // Editor play mode drives fire/aim/sprint (mouse buttons, shift) while exported players bind
 // mech_primary/mech_secondary/mech_boost. Accept either action set so both paths are playable.
 bool action_down(const char* mech,const char* editor)const{
  return world.is_action_pressed(mech)||world.is_action_pressed(editor);
 }
 bool primary_down()const{return action_down("mech_primary","fire")||world.get_axis("mech_primary")>.25F;}
 bool secondary_down()const{return action_down("mech_secondary","aim")||world.get_axis("mech_secondary")>.25F;}
 bool boost_down()const{return action_down("mech_boost","sprint");}
 GameWorld& world;
 PrototypeHud hud{};
 PrototypeCamera cam{};
 MechMotorState motor{};
 MechMotorConfig config{};
 MechCombatTelemetry telemetry{};
 GameObjectId player{},torso{},rules{};
 std::uint32_t requiredTargets{3};
 Float3 start{};
 float primaryCooldown{},rocketCooldown{},cameraFollow{};
 bool lastBoost{},lastPause{},lastRestart{},lastDebug{},started{};
 std::vector<Target> targets;
 std::vector<MechProjectile> rockets;
 std::vector<RacingVisualEffect> impacts;
 std::uint64_t nextProjectile{1};
 std::uint32_t rounds{200},missiles{12};
 explicit Impl(GameWorld& w):world(w){hud.active=false;hud.mecha=true;hud.maximumHealth=hud.health=100;}
 void camera(bool snap=false){
  const Float3 focus=plus(motor.position,{0,0,1.5F});
  const Float3 desired=plus(focus,{0,-28,35});
  const float alpha=snap?1.F:.18F;
  cam.position=plus(cam.position,scaled(minus(desired,cam.position),alpha));
  cam.target=plus(cam.target,scaled(minus(focus,cam.target),alpha));
  cam.up={0,0,1};cam.verticalFovRadians=.85F;
 }
 void publish(){
  (void)world.set_position(player,motor.position);
  (void)world.set_rotation(player,rotation(motor.legsYaw));
  if(torso){(void)world.set_position(torso,plus(motor.position,{0,0,2.F}));
   (void)world.set_rotation(torso,rotation(motor.torsoYaw));}
 }
 void refresh_hud(){
  hud.active=true;
  hud.stats="MECHA  HP "+std::to_string(int(hud.health))+"  ENERGY "+std::to_string(int(motor.energy));
  hud.vehicleTelemetry="AUTO "+std::to_string(rounds)+"  ROCKET "+std::to_string(missiles)+"  TARGETS "+
     std::to_string(telemetry.targetsDestroyed)+"/"+std::to_string(requiredTargets);
  if(hud.debug)hud.vehicleTelemetry+="  NAV "+std::to_string(telemetry.navigationReplans)+
    "  CARVES "+std::to_string(telemetry.voxelCarvesQueued);
  hud.prompt="WASD MOVE  SHIFT BOOST  J/L AIM  MOUSE 1/2 FIRE  P PAUSE  R RESET";
  hud.message=hud.phase==PrototypePhase::Won?"TARGETS CLEARED - R TO RESTART":
      hud.phase==PrototypePhase::Paused?"PAUSED - P TO RESUME":
      hud.phase==PrototypePhase::Lost?"MECH DISABLED - R TO RESTART":"";
  telemetry.energy=motor.energy;telemetry.primaryCooldown=primaryCooldown;
  telemetry.rocketCooldown=rocketCooldown;telemetry.activeRockets=std::uint32_t(rockets.size());
 }
 void reset(){
  motor={};motor.position=start;motor.energy=100;
  hud.phase=PrototypePhase::Playing;hud.health=hud.maximumHealth=100;
  primaryCooldown=rocketCooldown=0;rockets.clear();impacts.clear();nextProjectile=1;
  rounds=200;missiles=12;telemetry={};
  lastBoost=lastPause=lastRestart=lastDebug=false;
  for(std::size_t i=0;i<targets.size();++i){
   auto& t=targets[i];t.hp=t.initial;
   t.cooldown=.9F+float(i)*.35F;t.replanCountdown=0;t.path.clear();t.waypoint=0;t.turretYaw=0;
   (void)world.set_enabled(t.id,true);(void)world.set_position(t.id,t.spawn);
   (void)world.set_rotation(t.id,rotation(0));
  }
  publish();camera(true);refresh_hud();
 }
 // Test targets have gameplay hit spheres because their meshes are visual-only.
 // Resolve nearest target against both voxel/polygon swept collisions and exact voxel rays.
 TraceHit trace(Float3 from,Float3 movement,float padding=0.F){
  TraceHit best{};
  const auto distance=mag(movement);
  const Float3 back=distance>1.e-5F?scaled(movement,-1.F/distance):Float3{0,0,0};
  // Surface normals can point either way for overlap contacts; orient them to the exposed side.
  auto exposed=[&](Float3 n){
   if(mag(n)<1.e-4F)return back;
   if(n.x*movement.x+n.y*movement.y+n.z*movement.z>0)n=scaled(n,-1.F);
   return scaled(n,1.F/mag(n));
  };
  if(const auto hit=world.capsule_sweep({from,from,std::max(.01F,padding)},movement,player)){
   if(hit->time>=0&&hit->time<=best.fraction){
    best={hit->objectId,hit->worldPosition,hit->time,true,exposed(hit->worldNormal)};}
  }
  if(distance>1.e-5F)if(const auto hit=world.raycast(from,scaled(movement,1.F/distance),distance)){
   const auto t=hit->distance/distance;
   if(t>=0&&t<=best.fraction)best={hit->objectId,hit->worldPosition,t,true,exposed(hit->worldNormal)};
  }
  for(const auto& target:targets)if(target.hp>0&&world.is_enabled(target.id)){
   const auto p=world.position(target.id);if(!p)continue;
   const float t=sphere_hit(from,movement,*p,target.radius+padding);
   if(t>=0&&t<best.fraction)best={target.id,plus(from,scaled(movement,t)),t,true,back};
  }
  return best;
 }
 void effect(Float3 at,bool large){
  if(impacts.size()>=96U)impacts.erase(impacts.begin());
  impacts.push_back({RacingVisualKind::Dust,at,at,large?1.4F:.45F,0,
    large?.85F:.28F,large?.65F:.28F});
 }
 void age_effects(float dt){
  for(auto& e:impacts)e.age+=dt;
  impacts.erase(std::remove_if(impacts.begin(),impacts.end(),
    [](const RacingVisualEffect& e){return e.age>=e.lifetime;}),impacts.end());
 }
 void impact(const TraceHit& hit,bool rocket){
  if(!hit.valid)return;
  ++telemetry.impacts;
  effect(hit.position,rocket);
  for(auto& t:targets)if(t.hp>0&&world.is_enabled(t.id)){
   const auto position=world.position(t.id);if(!position)continue;
   const float d=mag(minus(*position,hit.position));
   if(t.id!=hit.id&&(!rocket||d>=3.5F+t.radius))continue;
   // LOS check for blast. The primary ray already established direct visibility.
   if(rocket&&t.id!=hit.id&&d>t.radius){
    // The contact point lies on the struck surface; a zero-length capsule there overlaps the
    // cover itself and would hide targets on the exposed side. Start just outside the surface.
    const auto origin=plus(hit.position,scaled(hit.normal,kBlastStandoff));
    const auto toward=minus(*position,origin);
    if(const auto occlusion=world.capsule_sweep({origin,origin,.02F},toward,player))
     if(occlusion->objectId!=t.id&&occlusion->time<.95F)continue;
   }
   const float dmg=rocket?(t.id==hit.id?95.F:std::max(0.F,60.F*(1.F-d/5.F))):18.F;
   t.hp=std::max(0.F,t.hp-dmg);
   if(t.hp==0){(void)world.set_enabled(t.id,false);++telemetry.targetsDestroyed;}
  }
  if(rocket){
   // Geometry, collision, sightlines and surface marks update at the GameWorld tick boundary.
   // Query nearby before mutation to avoid double applications to fragments created by this shot.
   // The crater needs to cross the 1.95 m thick starter barricades even when
   // the projectile strikes their near face. Geometry damage is independent of
   // the wider gameplay splash (handled above).
   const auto nearby=world.sphere_overlap(hit.position,3.1F);
   for(const auto id:nearby)if(world.geometry_kind(id)==GameGeometryKind::Voxel){
    if(world.queue_damage_sphere(id,hit.position,2.7F))++telemetry.voxelCarvesQueued;
   }
  }else if(world.geometry_kind(hit.id)==GameGeometryKind::Voxel){
   (void)world.add_surface_mark(hit.id,hit.position,.32F);
  }
  if(telemetry.targetsDestroyed>=requiredTargets)hud.phase=PrototypePhase::Won;
 }
 void shoot_primary(){
  if(primaryCooldown>0||rounds==0)return;
  primaryCooldown=.105F;--rounds;++telemetry.autocannonShots;
  const auto from=plus(motor.position,{0,0,2.F});
  impact(trace(from,scaled(facing(motor.torsoYaw),130.F)),false);
 }
 void shoot_rocket(){
  if(rocketCooldown>0||missiles==0||rockets.size()>=128)return;
  rocketCooldown=.85F;--missiles;++telemetry.rocketsFired;
  const auto direction=facing(motor.torsoYaw);
  const auto origin=plus(motor.position,{0,0,2.F});
  // The spawn point is 1.3 m ahead of the firing origin. A thin wall can fit inside that gap,
  // so sweep it with the same radius as step_rockets and detonate on the first obstruction.
  const auto muzzleHit=trace(origin,scaled(direction,1.3F),.15F);
  if(muzzleHit.valid){impact(muzzleHit,true);return;}
  rockets.push_back({plus(origin,scaled(direction,1.3F)),
                     scaled(direction,37.F),4.0F,nextProjectile++});
 }
 void step_rockets(float dt){
  for(auto it=rockets.begin();it!=rockets.end();){
   const auto move=scaled(it->velocity,dt);
   const auto hit=trace(it->position,move,.15F);
   it->remainingSeconds-=dt;
   if(hit.valid){impact(hit,true);it=rockets.erase(it);}
   else if(it->remainingSeconds<=0)it=rockets.erase(it);
   else{it->position=plus(it->position,move);++it;}
  }
 }

 // M4: each active enemy uses the current world geometry. Cover edits can reveal a firing
 // lane immediately; route replans are rate-limited and use the Z-up XY tactical grid.
 bool can_see(Float3 from,Float3 destination,GameObjectId ignore){
  const auto delta=minus(destination,from);const auto distance=mag(delta);
  if(distance<1.e-4F)return true;
  // Use the unified world ray query so polygon and voxel obstacles agree with weapon hits.
  if(const auto hit=world.raycast(from,scaled(delta,1.F/distance),distance))
   if(hit->objectId!=ignore&&hit->distance<distance-.4F)return false;
  return true;
 }
 Capsule enemy_capsule(Float3 at)const{
  return {{at.x,at.y,1.0F},{at.x,at.y,2.6F},.62F};
 }
 void step_enemies(float dt){
  const auto playerAim=plus(motor.position,{0,0,1.5F});
  for(auto& enemy:targets){
   if(enemy.hp<=0||enemy.kind==MechEnemyKind::Dummy||!world.is_enabled(enemy.id))continue;
   const auto opt=world.position(enemy.id);if(!opt)continue;
   Float3 pos=*opt;
   const auto muzzle=plus(pos,{0,0,.35F});
   const auto toPlayer=minus(playerAim,muzzle);
   const float distance=mag(toPlayer);
   if(distance<.01F)continue;
   const auto heading=unit(Float3{toPlayer.x,toPlayer.y,0});
   const bool seen=can_see(muzzle,playerAim,enemy.id);
   enemy.turretYaw=turn(enemy.turretYaw,angle_to(heading),dt*2.5F);
   (void)world.set_rotation(enemy.id,rotation(enemy.turretYaw));
   enemy.cooldown=std::max(0.F,enemy.cooldown-dt);
   enemy.replanCountdown=std::max(0.F,enemy.replanCountdown-dt);
   if(enemy.kind!=MechEnemyKind::Turret&&enemy.moveSpeed>.01F){
    Float3 desired{};
    if(!seen){
     if(enemy.replanCountdown<=0||enemy.path.empty()||enemy.waypoint>=enemy.path.size()){
      auto walkable=[&](Float3 p){return !world.capsule_overlaps(enemy_capsule(p),enemy.id);};
      const auto route=plan_mech_navigation_xy(pos,motor.position,walkable);
      enemy.path=route.waypoints;enemy.waypoint=0;enemy.replanCountdown=.6F;
      ++telemetry.navigationReplans;
     }
     while(enemy.waypoint<enemy.path.size()&&
           mag(minus(enemy.path[enemy.waypoint],pos))<1.8F)++enemy.waypoint;
     if(enemy.waypoint<enemy.path.size())
      desired=unit(Float3{enemy.path[enemy.waypoint].x-pos.x,
                          enemy.path[enemy.waypoint].y-pos.y,0});
    }else{
     enemy.path.clear();enemy.waypoint=0;
     if(enemy.kind==MechEnemyKind::Scout){
      // Orbit when within weapon range; close first when farther away.
      if(distance>18.F)desired=heading;
      else if(distance<9.F)desired=scaled(heading,-1.F);
      else desired={heading.y,-heading.x,0};
     }else if(distance>17.F)desired=heading;
     else if(distance<9.F)desired=scaled(heading,-.55F);
    }
    const auto step=scaled(desired,enemy.moveSpeed*dt);
    if(mag(step)>.0001F){
     if(const auto hit=world.capsule_sweep(enemy_capsule(pos),step,enemy.id)){
      const float u=std::clamp(hit->time-.01F,0.F,1.F);
      pos=plus(pos,scaled(step,u));
      // A blocked path is stale, replan next tick against updated cover.
      enemy.replanCountdown=0;
     }else pos=plus(pos,step);
     (void)world.set_position(enemy.id,pos);
    }
   }
   const auto nowMuzzle=plus(pos,{0,0,.35F});
   const float nowDistance=mag(minus(playerAim,nowMuzzle));
   // No through-wall shots. A newly destroyed wall opens LOS automatically.
   if(enemy.cooldown<=0&&nowDistance<enemy.attackRange&&
      can_see(nowMuzzle,playerAim,enemy.id)){
    enemy.cooldown=enemy.shotInterval;
    ++telemetry.enemyShots;hud.health=std::max(0.F,hud.health-enemy.damagePerShot);
    effect(playerAim,false);
    if(hud.health<=0){
     hud.phase=PrototypePhase::Lost;
     break;
    }
   }
  }
 }
 void move(float dt){
  const bool boost=boost_down();
  const auto input=MechMotorInput{{axis(world.get_axis("move_x")),axis(world.get_axis("move_y")),0},
     boost&&!lastBoost};
  lastBoost=boost;
  auto displace=integrate_mech_motor(motor,config,input,dt);
  // Swept capsule handles polygon and voxel blocking; two slide iterations.
  for(int i=0;i<2&&mag(displace)>1.e-5F;++i){
   const auto feet=motor.position;
   const Capsule capsule{plus(feet,{0,0,config.radius}),
                         plus(feet,{0,0,config.height-config.radius}),config.radius};
   const auto hit=world.capsule_sweep(capsule,displace,player);
   if(!hit){motor.position=plus(motor.position,displace);break;}
   const auto fraction=std::clamp(hit->time-.002F/std::max(mag(displace),.002F),0.F,1.F);
   motor.position=plus(motor.position,scaled(displace,fraction));
   const auto remaining=scaled(displace,1.F-fraction);
   const auto n=hit->worldNormal;
   displace=minus(remaining,scaled(n,std::min(0.F,dot3(remaining,n))));
   const auto vn=dot3(motor.velocity,n);
   if(vn<0)motor.velocity=minus(motor.velocity,scaled(n,vn));
  }
  const float aimX=axis(world.get_axis("look_x"));
  const float aimY=axis(world.get_axis("look_y"));
  if(std::abs(aimX)+std::abs(aimY)>.15F)
   motor.torsoYaw=turn(motor.torsoYaw,angle_to({aimX,aimY,0}),dt*7.5F);
  publish();
 }
};
MechaGame::MechaGame(GameWorld& w):impl_(std::make_unique<Impl>(w)){}
MechaGame::~MechaGame()=default;
const PrototypeHud& MechaGame::hud()const noexcept{return impl_->hud;}
PrototypeCamera MechaGame::camera()const noexcept{return impl_->cam;}
const MechMotorState& MechaGame::motor()const noexcept{return impl_->motor;}
const MechCombatTelemetry& MechaGame::telemetry()const noexcept{return impl_->telemetry;}
std::span<const RacingVisualEffect> MechaGame::impact_visuals()const noexcept{return impl_->impacts;}
void MechaGame::look(float yawDelta,float){
 if(impl_->started&&std::isfinite(yawDelta)&&impl_->hud.phase==PrototypePhase::Playing)
  impl_->motor.torsoYaw=wrap(impl_->motor.torsoYaw-yawDelta); // Pointer right turns toward screen right (+X).
}
bool MechaGame::start(std::string* error){
 auto& s=*impl_;if(s.started){if(error)*error="Mecha game already started";return false;}
 std::vector<PrototypeObjectView> views;
 for(const auto id:s.world.object_ids())views.push_back({id,"Object "+std::to_string(id),
       *s.world.components(id),s.world.geometry_kind(id)==GameGeometryKind::Marker});
 if(!validate_mecha_scene(views,error))return false;
 for(const auto& o:views)for(const auto& c:o.components)if(c.enabled&&actor_type(c.type)){
  if(!s.world.is_enabled(o.id)){if(error)*error="Mecha actors must be enabled";return false;}
  if(c.type==playerType){
   s.player=o.id;s.config.maximumSpeed=float(property(c,"speed",11.0));
   s.config.acceleration=float(property(c,"acceleration",24.0));
   s.config.boostSpeed=float(property(c,"boost_speed",24.0));
   s.config.radius=float(property(c,"radius",.95));s.config.height=float(property(c,"height",3.5));
  }else if(c.type==torsoType)s.torso=o.id;
  else if(c.type==rulesType){s.rules=o.id;s.requiredTargets=std::uint32_t(property(c,"target_count",std::int64_t{3}));}
  else if(c.type==targetType){
   const auto hp=float(property(c,"health",100.0));
   Impl::Target t;
   t.id=o.id;t.key=property(c,"key",std::string{});
   t.hp=t.initial=hp;t.radius=float(property(c,"hit_radius",1.2));
   t.kind=MechEnemyKind(property(c,"ai_kind",std::int64_t{0}));
   t.moveSpeed=float(property(c,"move_speed",4.0));
   t.attackRange=float(property(c,"attack_range",30.0));
   t.damagePerShot=float(property(c,"damage_per_shot",4.0));
   t.shotInterval=float(property(c,"shot_interval",1.4));
   t.spawn=s.world.position(o.id).value_or(Float3{});
   s.targets.push_back(std::move(t));
  }
 }
 for(const auto id:{s.player,s.torso})if(id&&s.world.has_collision(id).value_or(true)){
  if(error)*error="Mech player and torso must disable physical collision (mech motor owns collision)";
  return false;
 }
 if(!configure_physics3d_gravity(s.world.physics(),{0,0,-9.81F})){
  if(error)*error="Mech game requires Z-up gravity";return false;
 }
 s.start=*s.world.position(s.player);s.started=true;s.reset();
 return true;
}
void MechaGame::step(float dt){
 auto& s=*impl_;if(!s.started||!std::isfinite(dt)||dt<=0||dt>.1F)return;
 auto edge=[&](const char* a,bool& held){const bool pressed=s.world.is_action_pressed(a);
  const bool pressedEdge=pressed&&!held;held=pressed;return pressedEdge;};
 if(edge("prototype_debug",s.lastDebug))s.hud.debug=!s.hud.debug;
 if(edge("restart",s.lastRestart)){s.reset();return;}
 if(edge("pause",s.lastPause))
  s.hud.phase=s.hud.phase==PrototypePhase::Paused?PrototypePhase::Playing:PrototypePhase::Paused;
 if(s.hud.phase!=PrototypePhase::Playing){s.refresh_hud();return;}
 s.primaryCooldown=std::max(0.F,s.primaryCooldown-dt);
 s.rocketCooldown=std::max(0.F,s.rocketCooldown-dt);
 s.move(dt);
 // Gamepad triggers are analog axes in PlayerInput, while mouse buttons are actions.
 // Either representation may drive a weapon; never discard the digital binding.
 if(s.primary_down())s.shoot_primary();
 if(s.secondary_down())s.shoot_rocket();
 s.step_rockets(dt);
 if(s.hud.phase==PrototypePhase::Playing)s.step_enemies(dt);
 s.world.tick(dt);s.age_effects(dt);s.camera();s.refresh_hud();
}
bool MechaGame::after_restore(std::string* error){
 // M5 will serialize full combat state. Do not pretend that quickload restores combat targets.
 if(error)*error="M1/M2 mecha quickload is not supported until combat-state serialization (M5)";
 return false;
}
} // namespace dve
