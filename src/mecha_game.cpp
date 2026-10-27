#include "dve/mecha_game.hpp"
#include "dve/mecha_navigation.hpp"
#include "dve/mecha_leg_ik.hpp"
#include "dve/game_world.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/component.hpp"
#include "dve/query.hpp"
#include "dve/transform.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <iomanip>
#include <iterator>
#include <locale>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace dve {
namespace {
constexpr std::string_view playerType="dve.prototype.mech_player";
constexpr std::string_view torsoType="dve.prototype.mech_torso";
constexpr std::string_view legType="dve.prototype.mech_leg";
constexpr std::string_view footType="dve.prototype.mech_foot";
constexpr std::string_view targetType="dve.prototype.mech_target";
constexpr std::string_view rulesType="dve.prototype.mech_rules";
constexpr std::string_view extractionType="dve.prototype.mech_extraction";
constexpr float pi=3.14159265358979323846F;
Float3 plus(Float3 a,Float3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Float3 minus(Float3 a,Float3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
Float3 scaled(Float3 a,float s){return {a.x*s,a.y*s,a.z*s};}
float dot3(Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float mag(Float3 a){return std::sqrt(dot3(a,a));}
bool finite3(Float3 a){return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z);}
Float3 unit(Float3 a){const auto len=mag(a);return len>1.0e-6F?scaled(a,1.F/len):Float3{};}
Float3 cross3(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
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
bool actor_type(std::string_view t){return t==playerType||t==torsoType||t==targetType||t==rulesType||t==extractionType||t==legType||t==footType;}
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
 std::isfinite(c.height)&&c.height>=2*c.radius&&c.height<=20&&
 std::isfinite(c.jetAcceleration)&&c.jetAcceleration>=9.81F&&c.jetAcceleration<=90.F&&
 std::isfinite(c.jetDrainPerSecond)&&c.jetDrainPerSecond>0&&c.jetDrainPerSecond<=100.F&&
 std::isfinite(c.maximumJetHeight)&&c.maximumJetHeight>=3.F&&c.maximumJetHeight<=60.F;
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
 s.velocity.x+=correction.x;s.velocity.y+=correction.y; // M6: do not erase vertical jet velocity.
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
  {"height",3.5,false,false,"Collision capsule height (m)"},
  {"camera_mode",std::int64_t{0},false,false,"0 tactical, 1 third person"},
  {"jet_acceleration",22.0,false,false,"Jump-jet vertical acceleration"},
  {"jet_drain",32.0,false,false,"Jump-jet energy consumed per second"},
  {"jet_height",18.0,false,false,"Maximum jump height above spawn"},
  {"thigh_length",1.15,false,false,"IK upper-link length, in metres"},
  {"shin_length",1.10,false,false,"IK lower-link length, in metres"},
  {"foot_lift",0.34,false,false,"Foot swing lift clearance"},
  {"step_reach",0.85,false,false,"Forward target distance per footfall"}});
 add(std::string(torsoType),"Mech rotating torso (visual-only)",{});
 add(std::string(legType),"Mech gait segment (visual-only)",{
  {"side",std::int64_t{1},false,false,"-1 left, 1 right"},
  {"segment",std::int64_t{0},false,false,"0 thigh, 1 shin"}});
 add(std::string(footType),"Mech terrain foot (visual-only)",{
  {"side",std::int64_t{1},false,false,"-1 left, 1 right"}});
 add(std::string(targetType),"Mech target (visual-only)",{
  {"health",100.0,false,false,"Hit points"},
  {"hit_radius",1.2,false,false,"Gameplay hit sphere radius"},
  {"ai_kind",std::int64_t{0},false,false,"0 dummy, 1 scout, 2 heavy, 3 turret"},
  {"move_speed",4.0,false,false,"Enemy advance speed in metres per second"},
  {"attack_range",30.0,false,false,"Enemy maximum firing distance in metres"},
  {"damage_per_shot",4.0,false,false,"Enemy weapon damage to player"},
  {"shot_interval",1.4,false,false,"Seconds between enemy shots"}});
 add(std::string(extractionType),"Mission extraction beacon (marker)",{
  {"radius",5.0,false,false,"XY extraction radius in metres"}});
 add(std::string(rulesType),"Mech game rules (marker)",{
  {"target_count",std::int64_t{3},false,false,"Targets to destroy for victory"}});
}
bool validate_mecha_scene(std::span<const PrototypeObjectView> objects,std::string* error){
 auto fail=[&](std::string s){if(error)*error=std::move(s);return false;};
 const auto registry=ComponentTypeRegistry::make_default();
 std::set<std::string> keys;
 int players=0,rules=0,torsos=0,targets=0,extractions=0;std::int64_t requiredTargets=0;
 std::set<std::pair<std::int64_t,std::int64_t>> legSlots;
 std::set<std::int64_t> footSlots;
 for(const auto& o:objects){
  unsigned roles=0;
  for(const auto& c:o.components)if(c.enabled&&c.type.starts_with(kPrototypePrefix)){
   if(!actor_type(c.type))return fail("Cannot mix mech and other prototype roles");
   if(++roles>1)return fail("Only one mech role per object");
   std::string issue;if(!registry.validate(c,&issue))return fail(o.name+": "+issue);
   const auto key=property(c,"key",std::string{});
   if(!key_ok(key)||!keys.insert(key).second)return fail("Duplicate or invalid mech actor key");
   if(!o.marker&&(c.type==rulesType||c.type==extractionType))return fail("Mech rules/extraction require bodyless markers");
   if(c.type==extractionType){
    ++extractions;const auto radius=property(c,"radius",5.0);
    if(!std::isfinite(radius)||radius<1||radius>100)return fail("Invalid extraction radius");
   }
   if(c.type==rulesType){++rules;const auto n=property(c,"target_count",std::int64_t{3});if(n<1||n>64)return fail("Invalid target count");requiredTargets=n;}
   if(c.type==playerType){
    ++players;if(o.marker)return fail("Mech chassis must have visible geometry");
    MechMotorConfig cfg;cfg.maximumSpeed=float(property(c,"speed",11.0));
    cfg.acceleration=float(property(c,"acceleration",24.0));
    cfg.boostSpeed=float(property(c,"boost_speed",24.0));
    cfg.radius=float(property(c,"radius",.95));cfg.height=float(property(c,"height",3.5));
    cfg.jetAcceleration=float(property(c,"jet_acceleration",22.0));
    cfg.jetDrainPerSecond=float(property(c,"jet_drain",32.0));
    cfg.maximumJetHeight=float(property(c,"jet_height",18.0));
    const auto camera=property(c,"camera_mode",std::int64_t{0});
    if(camera<0||camera>1)return fail("Invalid mech camera mode");
    if(!valid_mech_motor_config(cfg))return fail("Invalid mech motor configuration");
    const auto thigh=property(c,"thigh_length",1.15);
    const auto shin=property(c,"shin_length",1.10);
    const auto lift=property(c,"foot_lift",.34);
    const auto reach=property(c,"step_reach",.85);
    if(!std::isfinite(thigh)||thigh<.4||thigh>4||
       !std::isfinite(shin)||shin<.4||shin>4||
       !std::isfinite(lift)||lift<.05||lift>1.5||
       !std::isfinite(reach)||reach<.2||reach>1.8)
     return fail("Invalid mech foot-IK configuration");
   }
   if(c.type==torsoType){++torsos;if(o.marker)return fail("Mech torso needs visible geometry");}
   if(c.type==legType){
    if(o.marker)return fail("Mech gait segment needs visible geometry");
    const auto side=property(c,"side",std::int64_t{1});
    const auto segment=property(c,"segment",std::int64_t{0});
    if((side!=-1&&side!=1)||(segment!=0&&segment!=1)||
       !legSlots.emplace(side,segment).second)return fail("Invalid or duplicate leg segment slot");
   }
   if(c.type==footType){
    if(o.marker)return fail("Mech foot must have visible geometry");
    const auto side=property(c,"side",std::int64_t{1});
    if((side!=-1&&side!=1)||!footSlots.insert(side).second)
     return fail("Invalid or duplicate mech foot side");
   }
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
 if(players!=1||rules!=1||torsos>1||extractions>1||targets<1||targets>64||requiredTargets>targets)return fail("Mech scene requires one player, one rules marker, at most one torso and 64 targets");
 if(!legSlots.empty()&&legSlots.size()!=4)return fail("Mech gait needs all four leg segments (left/right thigh and shin) or none");
 if(!footSlots.empty()&&(footSlots.size()!=2||legSlots.size()!=4))
  return fail("Mech terrain contact pads require both feet and a complete four-segment leg rig");
 return true;
}
struct MechaGame::Impl {
 struct Leg {GameObjectId id{};int side{},segment{};};
 struct FootPad {GameObjectId id{};int side{};};
 struct FootState {
  Float3 ankle{},swingFrom{},swingTo{},normal{0,0,1};
  Float3 localAnchor{};
  GameObjectId supportObject{};
  bool initialized{},planted{},supported{},lastSwing{};
 };
 struct GroundContact {Float3 point{},normal{};GameObjectId object{};};
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
 static constexpr float kJetIgnitionEnergy=10.F;
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
 GameObjectId player{},torso{},rules{},extraction{};
 Float3 extractionPosition{};float extractionRadius{5.F};
 bool extractionReady{};
 std::optional<GameWorldSaveState> initialWorld;
 std::uint32_t requiredTargets{3};
 Float3 start{};
 float primaryCooldown{},rocketCooldown{},cameraFollow{};
 bool lastBoost{},lastPause{},lastRestart{},lastDebug{},started{};
 MechCameraMode cameraMode{MechCameraMode::Tactical};
 MechCameraMode authoredCameraMode{MechCameraMode::Tactical};
 float orbitYaw{},orbitPitch{.23F},gaitPhase{};
 bool jetAirborne{},jetActive{},lastCamera{},lastLock{};
 std::string lockKey;
 MechLegIkConfig ikConfig{};
 float footLift{.34F},stepReach{.85F};
 std::array<FootState,2> feet{};
 std::vector<MechFootstepEvent> footstepEvents;
 std::vector<Leg> legs;
 std::vector<FootPad> footPads;
 std::vector<Target> targets;
 std::vector<MechProjectile> rockets;
 std::vector<RacingVisualEffect> impacts;
 std::uint64_t nextProjectile{1};
 std::uint32_t rounds{200},missiles{12};
 std::uint64_t boostUses{};
 explicit Impl(GameWorld& w):world(w){hud.active=false;hud.mecha=true;hud.maximumHealth=hud.health=100;}
 void camera(bool snap=false){
  const Float3 focus=plus(motor.position,{0,0,cameraMode==MechCameraMode::ThirdPerson?2.45F:1.5F});
  Float3 desired;
  if(cameraMode==MechCameraMode::Tactical){
   desired=plus(focus,{0,-28,35});
   cam.verticalFovRadians=.85F;
  }else{
   // Z-up shoulder camera: orbit yaw independent of the running chassis, pitched
   // upward to show ground and cover. Geometry clip avoids seeing through walls.
   const auto along=facing(orbitYaw);
   const Float3 right{along.y,-along.x,0};
   const float dist=11.F;
   desired=plus(focus,plus(scaled(along,-dist*std::cos(orbitPitch)),
                 plus(scaled(right,1.05F),{0,0,dist*std::sin(orbitPitch)+1.1F})));
   const auto ray=minus(desired,focus);const float length=mag(ray);
   if(length>.1F)if(const auto hit=world.raycast(focus,scaled(ray,1.F/length),length)){
    desired=plus(focus,scaled(ray,std::clamp((hit->distance-.35F)/length,.08F,1.F)));
   }
   cam.verticalFovRadians=1.03F;
  }
  const float alpha=snap?1.F:.18F;
  cam.position=plus(cam.position,scaled(minus(desired,cam.position),alpha));
  if(cameraMode==MechCameraMode::ThirdPerson){
   // The eased eye can cut a corner that the desired eye does not: re-clip the eased
   // position against geometry so the camera never lags through a wall.
   const auto ray=minus(cam.position,focus);const float length=mag(ray);
   if(std::isfinite(length)&&length>.1F)
    if(const auto hit=world.raycast(focus,scaled(ray,1.F/length),length))
     cam.position=plus(focus,scaled(ray,std::clamp((hit->distance-.35F)/length,.08F,1.F)));
  }
  cam.target=plus(cam.target,scaled(minus(focus,cam.target),alpha));
  cam.up={0,0,1};
 }
 // Must match the distance-driven gait phase advance in tick().
 static constexpr float gaitRadiansPerMeter=2.5F;
 // Fastest stride progress (eased 0..1 per second): bounds how far a foot can move in one frame.
 static constexpr float kSwingMaxRate=20.F;
 // Speed (m/s) at which a foot finishes a stride after the mech stops.
 static constexpr float kFinishSpeed=8.F;
 // How far ahead of the hip a landing foot can sit: stepReach, limited to what the legs can span
 // (with margin) from the current hip height above the foot.
 float stance_half_reach(Float3 hip,Float3 foot)const{
  const float dz=hip.z-foot.z,span=ikConfig.thighLength+ikConfig.shinLength-.12F;
  return std::min(stepReach,std::sqrt(std::max(0.F,span*span-dz*dz))*.92F);
 }
 static int foot_index(int side){return side<0?0:1;}
 Float3 hip_at(int side)const{
  return plus(motor.position,rotate(rotation(motor.legsYaw),
               {float(side)*1.0F,0,2.05F}));
 }
 Float3 neutral_ankle(int side)const{
  return plus(motor.position,rotate(rotation(motor.legsYaw),
               {float(side)*1.0F,0,.16F}));
 }
 // `hipRef` is where the hip will be when the foot is down (the current hip for a stance probe).
 std::optional<GroundContact> ground(Float3 xy,Float3 hipRef)const{
  // Use the same world query for polygons and destructible voxels. Probe again
  // while planted: demolished supports invalidate foot planting immediately.
  const Float3 origin{xy.x,xy.y,motor.position.z+2.35F};
  const auto hit=world.raycast(origin,{0,0,-1},5.0F);
  if(!hit||!finite3(hit->worldPosition)||!finite3(hit->worldNormal)||
      hit->worldNormal.z<.46F)return std::nullopt;
  // A high overhang or deep chasm may be ray-visible but is NOT reachable
  // by the authored two-link leg. Never classify it as a planted contact.
  const auto ankle=plus(hit->worldPosition,scaled(unit(hit->worldNormal),.115F));
  if(hit->worldPosition.z>motor.position.z+1.0F||
     mag(minus(ankle,hipRef))>
        ikConfig.thighLength+ikConfig.shinLength-.035F)return std::nullopt;
  return GroundContact{ankle,unit(hit->worldNormal),hit->objectId};
 }
 void bind_support(FootState& f,const GroundContact& support){
  f.supportObject=support.object;
  if(const auto transform=world.moving_transform(support.object))
   f.localAnchor=inverse_transform_point(transform->current,support.point);
  else f.supportObject=0;
 }
 void release_support(FootState& f){f.supportObject=0;f.localAnchor={};}
 void update_feet(float horizontalMeters,float dt,bool snap=false){
  if(legs.empty())return;
  for(const int side:{-1,1}){
   auto& f=feet[foot_index(side)];
   const auto home=neutral_ankle(side);
   const auto hip=hip_at(side);
   if(!f.initialized||snap){
    const auto hit=ground(home,hip);
    f.ankle=hit?hit->point:home;
    f.swingFrom=f.swingTo=f.ankle;
    f.normal=hit?hit->normal:Float3{0,0,1};
    f.initialized=true;f.planted=bool(hit);f.supported=bool(hit);
    f.lastSwing=false;
    if(hit)bind_support(f,*hit);else release_support(f);
   }
   const bool finishingSwing=f.lastSwing;
   if(jetAirborne){
    f.ankle=plus(home,{0,0,.26F});
    f.planted=f.supported=f.lastSwing=false;
    release_support(f);
    f.normal={0,0,1};
    continue;
   }
   const float cycle=std::fmod(std::fmod(gaitPhase+(side>0?pi:0.F),2.F*pi)+2.F*pi,2.F*pi);
   const bool wantsSwing=horizontalMeters>.0002F&&cycle<pi;
   bool stance=true;
   if(!wantsSwing&&f.lastSwing){
    // The mech stopped (or the cycle left the swing half) mid-stride: finish the step. The planned
    // landing assumed the hip would keep travelling, so pull it back within reach of the hip, then
    // move the foot there at a bounded speed; it never pops to the landing spot.
    auto to=f.swingTo;
    const float dz=hip.z-to.z,span=ikConfig.thighLength+ikConfig.shinLength-.12F;
    const float allowed=std::sqrt(std::max(0.F,span*span-dz*dz))*.92F;
    const float ox=to.x-hip.x,oy=to.y-hip.y,offset=std::hypot(ox,oy);
    if(offset>allowed&&offset>1.e-6F){to.x=hip.x+ox*(allowed/offset);to.y=hip.y+oy*(allowed/offset);}
    if(const auto hit=ground(to,hip)){to=hit->point;f.normal=hit->normal;f.supported=true;}
    else f.supported=false;
    f.swingTo=to;
    const auto remaining=minus(to,f.ankle);
    const float distance=mag(remaining),step=kFinishSpeed*std::max(dt,0.F);
    f.planted=false;
    release_support(f);
    if(distance<=step||distance<1.e-3F){f.ankle=to;f.lastSwing=false;}
    else{f.ankle=plus(f.ankle,scaled(remaining,step/distance));stance=false;}
   }else if(wantsSwing){
    float current=0.F; // eased progress along the stride, recovered from the foot itself
    if(!f.lastSwing){
     const auto horizontal=Float3{motor.velocity.x,motor.velocity.y,0};
     const auto direction=mag(horizontal)>.1F?unit(horizontal):facing(motor.legsYaw);
     // The hip keeps travelling for the rest of the swing, so aim at where the hip WILL be, with the
     // foot a little ahead of it (stepReach, limited to what the legs can actually span).
     const float remaining=std::max(0.F,pi-cycle)/gaitRadiansPerMeter;
     const auto landingHip=plus(hip,scaled(direction,remaining));
     const auto desired=plus(home,scaled(direction,remaining+stance_half_reach(hip,f.ankle)));
     const auto hit=ground(desired,landingHip);
     f.swingFrom=f.ankle;
     f.swingTo=hit?hit->point:plus(home,scaled(direction,remaining));
     f.normal=hit?hit->normal:Float3{0,0,1};
     f.supported=bool(hit);
    }else{
     const float sx=f.swingTo.x-f.swingFrom.x,sy=f.swingTo.y-f.swingFrom.y,len2=sx*sx+sy*sy;
     current=len2>1.e-4F?std::clamp(((f.ankle.x-f.swingFrom.x)*sx+(f.ankle.y-f.swingFrom.y)*sy)/len2,0.F,1.F):1.F;
    }
    // Follow the gait cycle, never moving backwards and never faster than a bounded rate, so a
    // frame hitch or a restore cannot teleport the foot.
    const float t=cycle/pi,target=t*t*(3.F-2.F*t);
    const float eased=std::clamp(target,current,std::min(1.F,current+std::max(dt,0.F)*kSwingMaxRate));
    const float phase=.5F-std::sin(std::asin(std::clamp(1.F-2.F*eased,-1.F,1.F))/3.F); // inverse smoothstep
    f.ankle=plus(plus(scaled(f.swingFrom,1.F-eased),scaled(f.swingTo,eased)),
                 {0,0,footLift*std::sin(pi*phase)});
    f.planted=false;f.lastSwing=true;stance=false;
    release_support(f);
   }
   if(stance){
    // Planted feet move with the support transform (including rotations).
    if(f.planted&&f.supportObject){
     if(world.is_enabled(f.supportObject)){
      if(const auto transform=world.moving_transform(f.supportObject)){
       const auto predicted=transform_point(transform->current,f.localAnchor);
       if(finite3(predicted)&&mag(minus(predicted,hip))<
          ikConfig.thighLength+ikConfig.shinLength+.05F)
        f.ankle=predicted;
       else{f.planted=false;f.supported=false;release_support(f);}
      }else{f.planted=false;f.supported=false;release_support(f);}
     }else{f.planted=false;f.supported=false;release_support(f);}
    }
    // Foot XY stays in world space during stance. We adjust only its height to
    // track terrain changes and keep planted feet from sliding with the hips.
    // A stance foot that the hip has outrun is dragged along at full extension (continuous), never
    // teleported to a new place.
    const float dz=hip.z-f.ankle.z,span=ikConfig.thighLength+ikConfig.shinLength-.08F;
    const float maxHorizontal=std::sqrt(std::max(0.F,span*span-dz*dz));
    const float dx=f.ankle.x-hip.x,dy=f.ankle.y-hip.y,dist=std::hypot(dx,dy);
    if(dist>maxHorizontal&&dist>1.e-6F){
     f.ankle.x=hip.x+dx*(maxHorizontal/dist);f.ankle.y=hip.y+dy*(maxHorizontal/dist);
    }
    auto support=ground(f.ankle,hip);
    if(support && (!f.supportObject || f.supportObject==support->object)){
     f.ankle={f.ankle.x,f.ankle.y,support->point.z};
     f.normal=support->normal;f.planted=f.supported=true;
     bind_support(f,*support);
     if(finishingSwing && !snap && footstepEvents.size()<2U){
      const float speed=std::hypot(motor.velocity.x,motor.velocity.y);
      footstepEvents.push_back({f.ankle,f.normal,f.supportObject,
          std::clamp(speed/config.maximumSpeed,.15F,1.F),side});
     }
    }else{
     // Missing voxel/floor support: do not leave a weight-bearing foot hovering.
     f.ankle=plus(home,{0,0,.28F});
     f.normal={0,0,1};f.planted=f.supported=false;
     release_support(f);
    }
   }
  }
 }
 void publish(){
  (void)world.set_position(player,motor.position);
  (void)world.set_rotation(player,rotation(motor.legsYaw));
  if(torso){(void)world.set_position(torso,plus(motor.position,{0,0,2.F}));
   (void)world.set_rotation(torso,rotation(motor.torsoYaw));}
  for(const auto& leg:legs){
   const auto& foot=feet[foot_index(leg.side)];
   const auto hip=hip_at(leg.side);
   const auto pole=rotate(rotation(motor.legsYaw),
              {float(leg.side)*.16F,1.F,.03F});
   const auto pose=solve_mech_leg_ik(hip,foot.ankle,pole,ikConfig,
      rotate(rotation(motor.legsYaw),{0,1,0}));
   if(!pose)continue;
   (void)world.set_position(leg.id,leg.segment==0?pose->thighCenter:pose->shinCenter);
   (void)world.set_rotation(leg.id,leg.segment==0?pose->thighRotation:pose->shinRotation);
  }
  for(const auto& pad:footPads){
   const auto& foot=feet[foot_index(pad.side)];
   (void)world.set_position(pad.id,foot.ankle);
   (void)world.set_rotation(pad.id,multiply(
       mech_align_z_axis(foot.normal),rotation(motor.legsYaw)));
  }
 }
 void refresh_hud(){
  hud.active=true;
  hud.stats="MECHA  HP "+std::to_string(int(hud.health))+"  ENERGY "+std::to_string(int(motor.energy));
  hud.vehicleTelemetry="AUTO "+std::to_string(rounds)+"  ROCKET "+std::to_string(missiles)+"  TARGETS "+
     std::to_string(telemetry.targetsDestroyed)+"/"+std::to_string(requiredTargets);
  if(hud.debug)hud.vehicleTelemetry+="  NAV "+std::to_string(telemetry.navigationReplans)+
    "  CARVES "+std::to_string(telemetry.voxelCarvesQueued);
  if(extraction)hud.vehicleTelemetry+=extractionReady?"  OBJECTIVE: EXTRACT":"  OBJECTIVE: DISABLE";
  hud.mechAudio={telemetry.autocannonShots,telemetry.rocketsFired,
                  telemetry.impacts,telemetry.enemyShots,boostUses};
  hud.vehicleTelemetry+=cameraMode==MechCameraMode::ThirdPerson?"  CAM: CHASE":"  CAM: TACTICAL";
  if(!lockKey.empty())hud.vehicleTelemetry="LOCK "+lockKey+"  "+hud.vehicleTelemetry;
  hud.mechLockMarker=false;
  if(!lockKey.empty())for(const auto& t:targets)if(t.key==lockKey&&t.hp>0){
   if(const auto p=world.position(t.id)){
    const auto forward=unit(minus(cam.target,cam.position));
    const auto right=unit(cross3(forward,cam.up));
    const auto up=unit(cross3(right,forward));
    const auto delta=minus(*p,cam.position);
    const auto depth=dot3(delta,forward);
    const auto size=depth*std::tan(cam.verticalFovRadians*.5F);
    if(size>.1F){
     hud.mechLockX=dot3(delta,right)/size;
     hud.mechLockY=dot3(delta,up)/size;
     hud.mechLockMarker=std::isfinite(hud.mechLockX)&&std::isfinite(hud.mechLockY);
    }
   }
  }
  hud.stats+="  ALT "+std::to_string(int(std::max(0.F,motor.position.z-start.z)));
  telemetry.altitude=std::max(0.F,motor.position.z-start.z);
  telemetry.verticalSpeed=motor.velocity.z;telemetry.jetActive=jetActive;
  telemetry.lockedOn=!lockKey.empty();
  hud.prompt="WASD MOVE SHIFT BOOST SPACE JETS C CAMERA TAB LOCK  MOUSE 1/2 FIRE F5/F9 SAVE P PAUSE R RESET";
  hud.message=hud.phase==PrototypePhase::Won?"MISSION COMPLETE - R TO RESTART":
      hud.phase==PrototypePhase::Paused?"PAUSED - P TO RESUME":
      hud.phase==PrototypePhase::Lost?"MECH DISABLED - R TO RESTART":
      extractionReady?"HOSTILES CLEARED - REACH EXTRACTION BEACON":"";
  telemetry.energy=motor.energy;telemetry.primaryCooldown=primaryCooldown;
  telemetry.rocketCooldown=rocketCooldown;telemetry.activeRockets=std::uint32_t(rockets.size());
 }
 void reset(){
  motor={};motor.position=start;motor.energy=100;
  hud.phase=PrototypePhase::Playing;hud.health=hud.maximumHealth=100;
  primaryCooldown=rocketCooldown=0;rockets.clear();impacts.clear();nextProjectile=1;
  rounds=200;missiles=12;telemetry={};boostUses=0;extractionReady=false;
  cameraMode=authoredCameraMode;
  jetAirborne=jetActive=false;gaitPhase=0;lockKey.clear();
  orbitYaw=0;orbitPitch=.23F;
  lastCamera=lastLock=false;
  lastBoost=lastPause=lastRestart=lastDebug=false;
  for(std::size_t i=0;i<targets.size();++i){
   auto& t=targets[i];t.hp=t.initial;
   t.cooldown=.9F+float(i)*.35F;t.replanCountdown=0;t.path.clear();t.waypoint=0;t.turretYaw=0;
   (void)world.set_enabled(t.id,true);(void)world.set_position(t.id,t.spawn);
   (void)world.set_rotation(t.id,rotation(0));
  }
  feet={};footstepEvents.clear();update_feet(0.F,0.F,true);
  publish();camera(true);refresh_hud();
 }
 Target* locked(){
  if(lockKey.empty())return nullptr;
  for(auto& t:targets)if(t.key==lockKey&&t.hp>0&&world.is_enabled(t.id)){
   const auto p=world.position(t.id);
   if(!p||mag(minus(*p,motor.position))>85.F)break;
   return &t;
  }
  lockKey.clear();return nullptr;
 }
 // Cycle targets in stable authored-key order. An obstructed or destroyed target
 // cannot be captured; LOS is rechecked each fixed tick as voxels change.
 void select_lock(){
  std::vector<std::string> options;
  for(const auto& t:targets)if(t.hp>0&&world.is_enabled(t.id)){
   const auto p=world.position(t.id);if(!p)continue;
   const auto delta=minus(*p,motor.position);
   const auto radius=mag(delta);
   if(radius>85.F||radius<.1F)continue;
   const auto horiz=unit(Float3{delta.x,delta.y,0});
   if(dot3(horiz,facing(motor.torsoYaw))<-.15F)continue;
   // Test visibility from the weapon muzzle toward the target's visual sphere.
   const auto origin=plus(motor.position,{0,0,2.F});
   const auto travel=minus(*p,origin);const auto dist=mag(travel);
   if(dist>.1F)if(const auto hit=world.raycast(origin,unit(travel),dist))
    if(hit->objectId!=t.id&&hit->distance<dist-t.radius)continue;
   options.push_back(t.key);
  }
  std::sort(options.begin(),options.end());
  if(options.empty()){lockKey.clear();return;}
  const auto it=std::find(options.begin(),options.end(),lockKey);
  lockKey=it==options.end()?options.front():
     (std::next(it)==options.end()?std::string{}:*std::next(it));
 }
 void update_lock(float dt){
  Target* t=locked();if(!t)return;
  const auto p=world.position(t->id);if(!p)return;
  const auto travel=minus(*p,motor.position);
  const auto origin=plus(motor.position,{0,0,2.F});
  const auto ray=minus(*p,origin);const float rayLength=mag(ray);
  if(rayLength>.1F)if(const auto hit=world.raycast(origin,scaled(ray,1.F/rayLength),rayLength))
   if(hit->objectId!=t->id&&hit->distance<rayLength-t->radius){
    lockKey.clear();return;
   }
  // A target directly above/below the muzzle has no heading: keep the current yaw.
  if(std::hypot(travel.x,travel.y)>.05F)motor.torsoYaw=turn(motor.torsoYaw,angle_to(travel),dt*9.F);
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
  if(telemetry.targetsDestroyed>=requiredTargets){
   extractionReady=true;
   if(!extraction)hud.phase=PrototypePhase::Won;
  }
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
  const float x=axis(world.get_axis("move_x")),y=axis(world.get_axis("move_y"));
  Float3 requested{x,y,0};
  if(cameraMode==MechCameraMode::ThirdPerson){
   // M6 chase-mode movement follows the camera rather than world XY axes.
   const auto forward=facing(orbitYaw);
   const Float3 right{forward.y,-forward.x,0};
   requested=plus(scaled(right,x),scaled(forward,y));
  }
  const auto input=MechMotorInput{requested,boost&&!lastBoost};
  if(boost&&!lastBoost)++boostUses;
   lastBoost=boost;
  const float energyBefore=motor.energy;
  auto displace=integrate_mech_motor(motor,config,input,dt);
  // Opt-in vertical locomotion. No jet use => legacy planar M1 fixtures unchanged.
  // Once airborne, gravity and collision determine the landing; no teleport/snap.
  // Fuel is judged on the energy held BEFORE this frame's regeneration (judging it after
  // gave free fuel every frame at zero energy). Sustained thrust needs one frame of fuel;
  // re-igniting after a cut-out needs kJetIgnitionEnergy, which is the recharge cooldown.
  const float frameFuel=config.jetDrainPerSecond*dt;
  const bool wasThrusting=jetActive;
  jetActive=action_down("mech_jumpjet","jump")&&
     energyBefore>=(wasThrusting?frameFuel:std::max(frameFuel,kJetIgnitionEnergy))&&
     motor.position.z<start.z+config.maximumJetHeight;
  if(jetActive){
   jetAirborne=true;
   motor.velocity.z=std::min(12.F,motor.velocity.z+(config.jetAcceleration-9.81F)*dt);
   // Thrusting suppresses regeneration; a boost spent this frame still counts.
   motor.energy=std::max(0.F,std::min(motor.energy,energyBefore)-frameFuel);
  }else if(jetAirborne)motor.velocity.z=std::max(-28.F,motor.velocity.z-9.81F*dt);
  if(jetAirborne){
   displace.z=motor.velocity.z*dt;
   // Hard cap the apex as well as thrust activation: accumulated upward inertia
   // must not coast above the authored maximum altitude.
   const float remainingHeight=std::max(0.F,start.z+config.maximumJetHeight-motor.position.z);
   if(displace.z>remainingHeight){displace.z=remainingHeight;motor.velocity.z=0;}
  }
  const Float3 before=motor.position;
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
   if(jetAirborne&&n.z>.5F&&motor.velocity.z<=0){
    jetAirborne=false;motor.velocity.z=0;
   }
  }
  const auto moved=minus(motor.position,before);
  const float horizontal=std::hypot(moved.x,moved.y);
  if(jetAirborne){/* airborne pose is frozen; keep the phase so landing does not pop */}
  else if(horizontal>.0001F)gaitPhase=wrap(gaitPhase+horizontal*2.5F);
  else{
   // Standing still: settle both feet onto the ground (phase 0 or pi) instead of
   // freezing mid-stride with one foot raised.
   const float rest=std::round(gaitPhase/pi)*pi;
   gaitPhase=wrap(turn(gaitPhase,rest,dt*7.F));
  }
  update_feet(horizontal,dt);
  const float aimX=axis(world.get_axis("look_x"));
  const float aimY=axis(world.get_axis("look_y"));
  if(cameraMode==MechCameraMode::ThirdPerson){
   orbitYaw=wrap(orbitYaw-aimX*dt*1.8F);
   orbitPitch=std::clamp(orbitPitch+aimY*dt*.8F,.05F,1.12F);
   if(lockKey.empty())motor.torsoYaw=turn(motor.torsoYaw,orbitYaw,dt*10.F);
  }else if(std::abs(aimX)+std::abs(aimY)>.15F)
   motor.torsoYaw=turn(motor.torsoYaw,angle_to({aimX,aimY,0}),dt*7.5F);
  publish();
 }
};
MechaGame::MechaGame(GameWorld& w):impl_(std::make_unique<Impl>(w)){}
MechaGame::~MechaGame()=default;
const PrototypeHud& MechaGame::hud()const noexcept{return impl_->hud;}
PrototypeCamera MechaGame::camera()const noexcept{return impl_->cam;}
const MechMotorState& MechaGame::motor()const noexcept{return impl_->motor;}
MechCameraMode MechaGame::camera_mode()const noexcept{return impl_->cameraMode;}
std::array<MechFootContact,2> MechaGame::foot_contacts()const noexcept{
 std::array<MechFootContact,2> result{};
 for(std::size_t i=0;i<2;++i){
  const auto& f=impl_->feet[i];
  result[i]={f.ankle,f.normal,f.supportObject,f.planted,f.supported};
 }
 return result;
}
std::span<const MechFootstepEvent> MechaGame::footstep_events()const noexcept{
 return impl_->footstepEvents;
}
std::string_view MechaGame::locked_target_key()const noexcept{return impl_->lockKey;}
const MechCombatTelemetry& MechaGame::telemetry()const noexcept{return impl_->telemetry;}
std::span<const RacingVisualEffect> MechaGame::impact_visuals()const noexcept{return impl_->impacts;}
void MechaGame::look(float yawDelta,float pitchDelta){
 if(!impl_->started||impl_->hud.phase!=PrototypePhase::Playing||
    !std::isfinite(yawDelta)||!std::isfinite(pitchDelta))return;
 if(impl_->cameraMode==MechCameraMode::ThirdPerson){
  impl_->orbitYaw=wrap(impl_->orbitYaw-yawDelta);
  impl_->orbitPitch=std::clamp(impl_->orbitPitch+pitchDelta,.05F,1.12F);
  if(impl_->lockKey.empty())impl_->motor.torsoYaw=impl_->orbitYaw;
 }else impl_->motor.torsoYaw=wrap(impl_->motor.torsoYaw-yawDelta);
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
   s.config.jetAcceleration=float(property(c,"jet_acceleration",22.0));
   s.config.jetDrainPerSecond=float(property(c,"jet_drain",32.0));
   s.config.maximumJetHeight=float(property(c,"jet_height",18.0));
   s.ikConfig.thighLength=float(property(c,"thigh_length",1.15));
   s.ikConfig.shinLength=float(property(c,"shin_length",1.10));
   s.footLift=float(property(c,"foot_lift",.34));
   s.stepReach=float(property(c,"step_reach",.85));
   s.cameraMode=s.authoredCameraMode=MechCameraMode(property(c,"camera_mode",std::int64_t{0}));
  }else if(c.type==torsoType)s.torso=o.id;
  else if(c.type==footType){
   s.footPads.push_back({o.id,int(property(c,"side",std::int64_t{1}))});
  }
  else if(c.type==legType){
   s.legs.push_back({o.id,int(property(c,"side",std::int64_t{1})),
                          int(property(c,"segment",std::int64_t{0}))});
  }
  else if(c.type==extractionType){
   s.extraction=o.id;s.extractionRadius=float(property(c,"radius",5.0));
   s.extractionPosition=s.world.position(o.id).value_or(Float3{});
  }
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
 for(const auto& pad:s.footPads)if(s.world.has_collision(pad.id).value_or(true)){
  if(error)*error="Mech visual feet cannot have physics collision";return false;
 }
 for(const auto& leg:s.legs)if(s.world.has_collision(leg.id).value_or(true)){
  if(error)*error="Mech visual legs cannot have physics collision";return false;
 }
 for(const auto id:{s.player,s.torso})if(id&&s.world.has_collision(id).value_or(true)){
  if(error)*error="Mech player and torso must disable physical collision (mech motor owns collision)";
  return false;
 }
 if(!configure_physics3d_gravity(s.world.physics(),{0,0,-9.81F})){
  if(error)*error="Mech game requires Z-up gravity";return false;
 }
 s.start=*s.world.position(s.player);s.started=true;s.reset();
 s.initialWorld=s.world.capture_save_state();
 // capture_save_state() leaves polygon geometry empty (file saves re-read it from the asset
 // source), but an in-memory restore_save_state() needs it. Attach the live immutable
 // geometry so the pristine snapshot can restore polygon scenery, chassis and enemies.
 {
  std::map<GameObjectId,const CookedPolygonAsset*> polygons;
  const auto renderObjects=s.world.render_objects();
  for(const auto& r:renderObjects)if(r.polygon)polygons.emplace(r.id,r.polygon);
  for(auto& item:s.initialWorld->objects){
   if(item.kind!=GameGeometryKind::Polygon||item.polygon)continue;
   const auto it=polygons.find(item.id);
   if(it==polygons.end()){if(error)*error="Mech restart snapshot: missing polygon geometry";s.initialWorld.reset();return false;}
   item.polygon=std::make_shared<const CookedPolygonAsset>(*it->second);
  }
 }
 return true;
}
void MechaGame::step(float dt){
 auto& s=*impl_;if(!s.started||!std::isfinite(dt)||dt<=0||dt>.1F)return;
 s.footstepEvents.clear();
 auto edge=[&](const char* a,bool& held){const bool pressed=s.world.is_action_pressed(a);
  const bool pressedEdge=pressed&&!held;held=pressed;return pressedEdge;};
 if(edge("prototype_debug",s.lastDebug))s.hud.debug=!s.hud.debug;
 if(edge("restart",s.lastRestart)){
  if(s.initialWorld){
   std::string issue;
   if(!s.world.restore_save_state(*s.initialWorld,nullptr,&issue)){
    s.hud.phase=PrototypePhase::Lost;s.hud.message="RESET FAILED: "+issue;
    return;
   }
  }
  s.reset();s.lastRestart=true;return; // consume the press edge even after world-input restore
 }
 if(edge("mech_camera",s.lastCamera)){
  s.cameraMode=s.cameraMode==MechCameraMode::ThirdPerson?
      MechCameraMode::Tactical:MechCameraMode::ThirdPerson;
  s.orbitYaw=s.motor.torsoYaw;s.camera(true);
 }
 if(edge("mech_lock",s.lastLock)&&s.hud.phase==PrototypePhase::Playing)s.select_lock();
 if(edge("pause",s.lastPause))
  s.hud.phase=s.hud.phase==PrototypePhase::Paused?PrototypePhase::Playing:PrototypePhase::Paused;
 if(s.hud.phase!=PrototypePhase::Playing){s.refresh_hud();return;}
 s.primaryCooldown=std::max(0.F,s.primaryCooldown-dt);
 s.rocketCooldown=std::max(0.F,s.rocketCooldown-dt);
 s.move(dt);
 s.update_lock(dt);s.publish();
 // Gamepad triggers are analog axes in PlayerInput, while mouse buttons are actions.
 // Either representation may drive a weapon; never discard the digital binding.
 if(s.primary_down())s.shoot_primary();
 if(s.secondary_down())s.shoot_rocket();
 s.step_rockets(dt);
 if(s.hud.phase==PrototypePhase::Playing)s.step_enemies(dt);
  if(s.extractionReady&&s.extraction&&s.hud.phase==PrototypePhase::Playing){
   const auto offset=minus(s.motor.position,s.extractionPosition);
   if(std::hypot(offset.x,offset.y)<=s.extractionRadius)s.hud.phase=PrototypePhase::Won;
  }
 s.world.tick(dt);s.age_effects(dt);s.camera();s.refresh_hud();
}
// A small versioned application section in DVESAVE. World voxels, objects, transforms,
// physics and timing stay in the authoritative GameWorld section; these are ONLY the
// mecha-controller values absent from GameWorld (ammo, aim, AI cooldowns, mission state).
std::string MechaGame::save_state() const {
 const auto& s=*impl_;
 if(!s.started)return {};
 // Navigation paths are a cache: a replan is forced when one is empty. A route longer than the
 // loader accepts (256 points), or a section that would exceed the 64 KiB loader cap, must
 // not produce a save that cannot be loaded, so such paths are dropped instead.
 const auto encode=[&](bool paths){
 std::ostringstream out;out.imbue(std::locale::classic());
 out<<std::setprecision(9)<<"DVE_MECHA 3 "<<unsigned(s.hud.phase)<<' '
    <<s.hud.health<<' '<<int(s.hud.debug)<<' '<<int(s.extractionReady)<<' '
    <<s.motor.position.x<<' '<<s.motor.position.y<<' '<<s.motor.position.z<<' '
    <<s.motor.velocity.x<<' '<<s.motor.velocity.y<<' '<<s.motor.velocity.z<<' '
    <<s.motor.legsYaw<<' '<<s.motor.torsoYaw<<' '<<s.motor.energy<<' '
    <<s.primaryCooldown<<' '<<s.rocketCooldown<<' '
    <<s.rounds<<' '<<s.missiles<<' '<<s.nextProjectile<<' '<<s.boostUses<<' '
    <<s.telemetry.autocannonShots<<' '<<s.telemetry.rocketsFired<<' '
    <<s.telemetry.targetsDestroyed<<' '<<s.telemetry.impacts<<' '
    <<s.telemetry.voxelCarvesQueued<<' '<<s.telemetry.enemyShots<<' '
    <<s.telemetry.navigationReplans<<' '<<s.targets.size()<<'\n';
 for(const auto& t:s.targets){
  const bool keep=paths&&t.path.size()<=256U&&t.waypoint<=t.path.size();
  const auto p=s.world.position(t.id).value_or(t.spawn);
  out<<std::quoted(t.key)<<' '<<t.hp<<' '<<t.cooldown<<' '
     <<(keep?t.replanCountdown:0.F)<<' '<<t.turretYaw<<' '
     <<p.x<<' '<<p.y<<' '<<p.z<<' '
     <<(keep?t.waypoint:std::size_t{0})<<' '<<(keep?t.path.size():std::size_t{0})<<'\n';
  if(keep)for(const auto& q:t.path)out<<q.x<<' '<<q.y<<' '<<q.z<<'\n';
 }
 out<<s.rockets.size()<<'\n';
 for(const auto& p:s.rockets)
  out<<p.position.x<<' '<<p.position.y<<' '<<p.position.z<<' '
     <<p.velocity.x<<' '<<p.velocity.y<<' '<<p.velocity.z<<' '
     <<p.remainingSeconds<<' '<<p.sequence<<'\n';
 out<<"M6 "<<unsigned(s.cameraMode)<<' '<<s.orbitYaw<<' '<<s.orbitPitch<<' '
    <<int(s.jetAirborne)<<' '<<s.gaitPhase<<' '<<std::quoted(s.lockKey)<<'\n';
 out<<"M7 2\n";
 for(const auto& f:s.feet){
  out<<int(f.initialized)<<' '<<int(f.planted)<<' '<<int(f.supported)<<' '
     <<int(f.lastSwing)<<' '
     <<f.ankle.x<<' '<<f.ankle.y<<' '<<f.ankle.z<<' '
     <<f.swingFrom.x<<' '<<f.swingFrom.y<<' '<<f.swingFrom.z<<' '
     <<f.swingTo.x<<' '<<f.swingTo.y<<' '<<f.swingTo.z<<' '
     <<f.normal.x<<' '<<f.normal.y<<' '<<f.normal.z<<'\n';
 }
 return out.str();
 };
 auto encoded=encode(true);
 if(encoded.size()>65536U)encoded=encode(false);
 return encoded;
}
bool MechaGame::after_restore(std::string_view encoded,std::string* error){
 auto& s=*impl_;
 const auto fail=[&](std::string reason){if(error)*error="Invalid mecha save: "+reason;return false;};
 if(encoded.empty())return fail("missing mecha game section (older saves cannot restore M1-M4 combat state)");
 if(encoded.size()>65536U)return fail("section exceeds 64 KiB");
 std::istringstream in{std::string(encoded)};in.imbue(std::locale::classic());
 std::string magic;unsigned version{},phase{},debug{},ready{};
 MechMotorState motor{};
 float health{},primary{},secondary{};
 unsigned rounds{},missiles{};
 std::uint64_t next{},boosts{};
 MechCombatTelemetry telemetry{};
 std::size_t nTargets{};
 in>>magic>>version>>phase>>health>>debug>>ready
   >>motor.position.x>>motor.position.y>>motor.position.z
   >>motor.velocity.x>>motor.velocity.y>>motor.velocity.z
   >>motor.legsYaw>>motor.torsoYaw>>motor.energy
   >>primary>>secondary>>rounds>>missiles>>next>>boosts
   >>telemetry.autocannonShots>>telemetry.rocketsFired
   >>telemetry.targetsDestroyed>>telemetry.impacts
   >>telemetry.voxelCarvesQueued>>telemetry.enemyShots
   >>telemetry.navigationReplans>>nTargets;
 if(!in||magic!="DVE_MECHA"||(version!=1&&version!=2&&version!=3)||phase>3||debug>1||ready>1
    ||!finite3(motor.position)||!finite3(motor.velocity)
    ||!std::isfinite(motor.legsYaw)||!std::isfinite(motor.torsoYaw)
    ||!std::isfinite(motor.energy)||motor.energy<0||motor.energy>100
    ||!std::isfinite(health)||health<0||health>s.hud.maximumHealth
    ||!std::isfinite(primary)||primary<0||primary>1
    ||!std::isfinite(secondary)||secondary<0||secondary>2
    ||rounds>200||missiles>12||next==0||next>1000000000000ULL
    ||nTargets!=s.targets.size()||nTargets>64)return fail("header out of range");
 struct TargetSnapshot {
  float hp{},cooldown{},replan{},yaw{};
  Float3 position{};std::size_t waypoint{};
  std::vector<Float3> path;
 };
 std::map<std::string,TargetSnapshot> saved;
 std::uint64_t defeated{};
 for(std::size_t i=0;i<nTargets;++i){
  std::string key;TargetSnapshot t;std::size_t points{};
  in>>std::quoted(key)>>t.hp>>t.cooldown>>t.replan>>t.yaw
    >>t.position.x>>t.position.y>>t.position.z>>t.waypoint>>points;
  if(!in||!key_ok(key)||!std::isfinite(t.hp)||t.hp<0
    ||!std::isfinite(t.cooldown)||t.cooldown<0||t.cooldown>30
    ||!std::isfinite(t.replan)||t.replan<0||t.replan>10
    ||!std::isfinite(t.yaw)||!finite3(t.position)
    ||points>256||t.waypoint>points)return fail("enemy state out of range");
  t.path.resize(points);
  for(auto& q:t.path){
   in>>q.x>>q.y>>q.z;
   if(!in||!finite3(q))return fail("navigation path invalid");
  }
  if(t.hp==0)++defeated;
  if(!saved.emplace(std::move(key),std::move(t)).second)return fail("duplicate enemy key");
 }
 std::size_t rocketCount{};in>>rocketCount;
 if(!in||rocketCount>128)return fail("invalid rocket count");
 std::vector<MechProjectile> rockets;rockets.reserve(rocketCount);
 std::set<std::uint64_t> sequences;
 for(std::size_t i=0;i<rocketCount;++i){
  MechProjectile p;
  in>>p.position.x>>p.position.y>>p.position.z
    >>p.velocity.x>>p.velocity.y>>p.velocity.z
    >>p.remainingSeconds>>p.sequence;
  if(!in||!finite3(p.position)||!finite3(p.velocity)
     ||!std::isfinite(p.remainingSeconds)||p.remainingSeconds<=0||p.remainingSeconds>4
     ||p.sequence==0||p.sequence>=next||!sequences.insert(p.sequence).second)
   return fail("invalid rocket");
  rockets.push_back(p);
 }
 // M6 is appended so v1 checkpoints are still readable without relaxing old
 // world-state validation. New checkpoints preserve camera, lock and air state.
 unsigned savedCamera=unsigned(s.cameraMode),savedAirborne=0;
 float savedOrbit=0.F,savedPitch=.23F,savedGait=0.F;
 std::string savedLock;
 if(version>=2){
  std::string marker;
  in>>marker>>savedCamera>>savedOrbit>>savedPitch>>savedAirborne>>savedGait
    >>std::quoted(savedLock);
  if(!in||marker!="M6"||savedCamera>1||savedAirborne>1||
     !std::isfinite(savedOrbit)||std::abs(savedOrbit)>pi+0.001F||
     !std::isfinite(savedPitch)||savedPitch<.05F||savedPitch>1.12F||
     !std::isfinite(savedGait)||std::abs(savedGait)>pi+.001F||
     (!savedLock.empty()&&!key_ok(savedLock)))
   return fail("invalid M6 camera, jet or lock state");
 }
 std::array<Impl::FootState,2> savedFeet{};
 if(version>=3){
  std::string marker;unsigned count{};
  in>>marker>>count;
  if(!in||marker!="M7"||count!=2)return fail("invalid foot-state envelope");
  for(auto& foot:savedFeet){
   unsigned initialized{},planted{},supported{},swing{};
   in>>initialized>>planted>>supported>>swing
     >>foot.ankle.x>>foot.ankle.y>>foot.ankle.z
     >>foot.swingFrom.x>>foot.swingFrom.y>>foot.swingFrom.z
     >>foot.swingTo.x>>foot.swingTo.y>>foot.swingTo.z
     >>foot.normal.x>>foot.normal.y>>foot.normal.z;
   if(!in||initialized>1||planted>1||supported>1||swing>1||
      !finite3(foot.ankle)||!finite3(foot.swingFrom)||
      !finite3(foot.swingTo)||!finite3(foot.normal)||
      mag(foot.normal)<.8F||mag(foot.normal)>1.2F||
      (planted&&!initialized)||(supported&&!initialized)||
      (planted&&!supported)||(planted&&swing)||
      mag(minus(foot.ankle,motor.position))>25.F||
      mag(minus(foot.swingFrom,motor.position))>25.F||
      mag(minus(foot.swingTo,motor.position))>25.F)
    return fail("invalid M7 foot state");
   foot.initialized=initialized!=0;foot.planted=planted!=0;
   foot.supported=supported!=0;foot.lastSwing=swing!=0;
  }
 }
 in>>std::ws;
 if(!in.eof())return fail("trailing data");
 if(defeated!=telemetry.targetsDestroyed||ready!=(defeated>=s.requiredTargets))
  return fail("mission or kill count inconsistent");
 if(!savedLock.empty()&&(!saved.contains(savedLock)||saved.at(savedLock).hp<=0))
  return fail("locked target is not alive");
 if((phase==unsigned(PrototypePhase::Won)&&!ready)||
    (phase==unsigned(PrototypePhase::Lost)&&health>0))
  return fail("phase inconsistent with objectives or health");
 if(auto position=s.world.position(s.player);!position||
    mag(minus(*position,motor.position))>.015F)return fail("saved chassis transform mismatch");
 for(const auto& t:s.targets){
  const auto it=saved.find(t.key);if(it==saved.end())return fail("missing enemy key");
  const auto& snap=it->second;
  if(snap.hp>t.initial)return fail("enemy health exceeds authored maximum");
  if(s.world.is_enabled(t.id)!=(snap.hp>0))return fail("world enemy enabled state mismatch");
  const auto position=s.world.position(t.id);
  if(!position||mag(minus(*position,snap.position))>.015F)
   return fail("saved enemy transform mismatch");
 }
 // Parse-and-validate before committing: failed quickloads retain the live PlayerSession.
 s.cameraMode=MechCameraMode(savedCamera);
 s.orbitYaw=savedOrbit;s.orbitPitch=savedPitch;s.jetAirborne=savedAirborne!=0;
 s.jetActive=false;s.gaitPhase=savedGait;s.lockKey=std::move(savedLock);
 if(savedAirborne==0)motor.velocity.z=0; // a grounded mech carries no vertical velocity
 if(version>=3)s.feet=savedFeet;
 else s.feet={};
 s.footstepEvents.clear();
 s.motor=motor;s.primaryCooldown=primary;s.rocketCooldown=secondary;
 if(version<3)s.update_feet(0.F,0.F,true);
 else for(int side:{-1,1}){
  auto& f=s.feet[Impl::foot_index(side)];
  const auto support=s.ground(f.ankle,s.hip_at(side));
  if(f.planted&&support)s.bind_support(f,*support);
  else s.release_support(f);
 }
 s.rounds=rounds;s.missiles=missiles;s.nextProjectile=next;s.boostUses=boosts;
 s.telemetry=telemetry;s.rockets=std::move(rockets);
 s.extractionReady=ready!=0;s.hud.phase=PrototypePhase(phase);
 s.hud.health=health;s.hud.debug=debug!=0;
 for(auto& t:s.targets){
  auto& snap=saved.at(t.key);
  t.hp=snap.hp;t.cooldown=snap.cooldown;t.replanCountdown=snap.replan;
  t.turretYaw=snap.yaw;t.path=std::move(snap.path);t.waypoint=snap.waypoint;
 }
 s.impacts.clear();s.lastBoost=s.lastPause=s.lastRestart=s.lastDebug=false;
 s.publish();s.camera(true);s.refresh_hud();
 return true;
}

} // namespace dve
