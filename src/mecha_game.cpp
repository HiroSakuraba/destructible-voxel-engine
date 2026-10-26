#include "dve/mecha_game.hpp"
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
  {"hit_radius",1.2,false,false,"Gameplay hit sphere radius"}});
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
   }
  }
 }
 if(players!=1||rules!=1||torsos>1||targets<1||targets>64||requiredTargets>targets)return fail("Mech scene requires one player, one rules marker, at most one torso and 64 targets");
 return true;
}
struct MechaGame::Impl {
 struct Target {GameObjectId id{};std::string key;float hp{},initial{},radius{};};
 struct TraceHit {GameObjectId id{};Float3 position{};float fraction{1};bool valid{};};
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
  hud.prompt="WASD MOVE  SHIFT BOOST  J/L AIM  MOUSE 1/2 FIRE  P PAUSE  R RESET";
  hud.message=hud.phase==PrototypePhase::Won?"TARGETS CLEARED - R TO RESTART":
      hud.phase==PrototypePhase::Paused?"PAUSED - P TO RESUME":"";
  telemetry.energy=motor.energy;telemetry.primaryCooldown=primaryCooldown;
  telemetry.rocketCooldown=rocketCooldown;telemetry.activeRockets=std::uint32_t(rockets.size());
 }
 void reset(){
  motor={};motor.position=start;motor.energy=100;
  hud.phase=PrototypePhase::Playing;hud.health=hud.maximumHealth=100;
  primaryCooldown=rocketCooldown=0;rockets.clear();nextProjectile=1;
  rounds=200;missiles=12;telemetry={};
  lastBoost=lastPause=lastRestart=lastDebug=false;
  for(auto& t:targets){t.hp=t.initial;(void)world.set_enabled(t.id,true);}
  publish();camera(true);refresh_hud();
 }
 // Test targets have gameplay hit spheres because their meshes are visual-only.
 // Resolve nearest target against both voxel/polygon swept collisions and exact voxel rays.
 TraceHit trace(Float3 from,Float3 movement,float padding=0.F){
  TraceHit best{};
  if(const auto hit=world.capsule_sweep({from,from,std::max(.01F,padding)},movement,player)){
   if(hit->time>=0&&hit->time<=best.fraction){best={hit->objectId,hit->worldPosition,hit->time,true};}
  }
  const auto distance=mag(movement);
  if(distance>1.e-5F)if(const auto hit=world.raycast(from,scaled(movement,1.F/distance),distance)){
   const auto t=hit->distance/distance;
   if(t>=0&&t<=best.fraction)best={hit->objectId,hit->worldPosition,t,true};
  }
  for(const auto& target:targets)if(target.hp>0&&world.is_enabled(target.id)){
   const auto p=world.position(target.id);if(!p)continue;
   const float t=sphere_hit(from,movement,*p,target.radius+padding);
   if(t>=0&&t<best.fraction)best={target.id,plus(from,scaled(movement,t)),t,true};
  }
  return best;
 }
 void impact(const TraceHit& hit,bool rocket){
  if(!hit.valid)return;
  ++telemetry.impacts;
  for(auto& t:targets)if(t.hp>0&&world.is_enabled(t.id)){
   const auto position=world.position(t.id);if(!position)continue;
   const float d=mag(minus(*position,hit.position));
   if(t.id!=hit.id&&(!rocket||d>=3.5F+t.radius))continue;
   // LOS check for blast. The primary ray already established direct visibility.
   if(rocket&&t.id!=hit.id&&d>t.radius){
    const auto toward=minus(*position,hit.position);
    if(const auto occlusion=world.capsule_sweep({hit.position,hit.position,.02F},toward,player))
     if(occlusion->objectId!=t.id&&occlusion->time<.95F)continue;
   }
   const float dmg=rocket?(t.id==hit.id?95.F:std::max(0.F,60.F*(1.F-d/5.F))):18.F;
   t.hp=std::max(0.F,t.hp-dmg);
   if(t.hp==0){(void)world.set_enabled(t.id,false);++telemetry.targetsDestroyed;}
  }
  if(rocket){
   // Only actual voxel geometry is carved; polygon scenery is intentionally unaffected.
   const auto nearby=world.sphere_overlap(hit.position,2.1F);
   for(const auto id:nearby)if(world.geometry_kind(id)==GameGeometryKind::Voxel){
    (void)world.queue_damage_sphere(id,hit.position,1.6F);
   }
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
  rockets.push_back({plus(plus(motor.position,{0,0,2.F}),scaled(direction,1.3F)),
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
 void move(float dt){
  const auto input=MechMotorInput{{axis(world.get_axis("move_x")),axis(world.get_axis("move_y")),0},
     world.is_action_pressed("mech_boost")&&!lastBoost};
  lastBoost=world.is_action_pressed("mech_boost");
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
void MechaGame::look(float yawDelta,float){
 if(impl_->started&&std::isfinite(yawDelta)&&impl_->hud.phase==PrototypePhase::Playing)
  impl_->motor.torsoYaw=wrap(impl_->motor.torsoYaw+yawDelta);
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
   s.targets.push_back({o.id,property(c,"key",std::string{}),hp,hp,float(property(c,"hit_radius",1.2))});
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
 if(s.world.is_action_pressed("mech_primary")||s.world.get_axis("mech_primary")>.25F)s.shoot_primary();
 if(s.world.is_action_pressed("mech_secondary")||s.world.get_axis("mech_secondary")>.25F)s.shoot_rocket();
 s.step_rockets(dt);
 s.world.tick(dt);s.camera();s.refresh_hud();
}
bool MechaGame::after_restore(std::string* error){
 // M5 will serialize full combat state. Do not pretend that quickload restores combat targets.
 if(error)*error="M1/M2 mecha quickload is not supported until combat-state serialization (M5)";
 return false;
}
} // namespace dve
