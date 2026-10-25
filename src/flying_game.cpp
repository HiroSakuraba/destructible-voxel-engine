#include "dve/flying_game.hpp"
#include "dve/game_world.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/flight_wind.hpp"
#include "dve/flight_aerodynamics.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
namespace dve {
namespace {
constexpr float gravity=9.81F;
constexpr float stepSeconds=1.0F/120.0F;
constexpr float pi=3.14159265359F;
constexpr std::string_view vehicleType="dve.prototype.flight_vehicle";
constexpr std::string_view rulesType="dve.prototype.flight_rules";
constexpr std::string_view gateType="dve.prototype.flight_gate";
constexpr std::string_view windType="dve.prototype.flight_wind";
constexpr std::string_view stateType="dve.prototype.flight_state";
const std::array<FlightProfile,6> profiles{{
 {"FLOAT",0,0,0,.3F,.0F,0,0,.995F,9.0F,1.4F,1.8F},
 {"GLIDER",11,4.8F,1.25F,.29F,.035F,.045F,0,0,.13F,2.4F,1.4F},
 {"BIPLANE",13,4.6F,1.35F,.34F,.085F,.06F,2300,0,.23F,3.6F,1.1F},
 {"JET",7,3.6F,1.10F,.26F,.035F,.09F,4200,0,.20F,2.8F,1.5F},
 {"HOVER",0,0,0,.3F,0,0,0,0,1.5F,4.8F,1.8F},
 {"BIRD",14,4.9F,1.35F,.32F,.045F,.04F,0,0,.11F,3.0F,1.4F}
}};
template<class T>T prop(const Component& c,std::string_view key,T fallback){
 const auto i=c.properties.find(key);if(i!=c.properties.end())if(const auto* v=std::get_if<T>(&i->second))return *v;return fallback;
}
bool finite(Float3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
bool finite(Quaternion q){return std::isfinite(q.x)&&std::isfinite(q.y)&&std::isfinite(q.z)&&std::isfinite(q.w);}
Float3 cross3(Float3 a,Float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
Float3 unit(Float3 v,Float3 fallback={0,1,0}){const float l=length(v);return l>1.e-5F?multiply(v,1.F/l):fallback;}
FlightProfile blend(const FlightProfile& a,const FlightProfile& b,float t){
 const auto m=[&](float x,float y){return x+(y-x)*t;};
 return {"MORPH",m(a.wingArea,b.wingArea),m(a.liftSlope,b.liftSlope),m(a.liftLimit,b.liftLimit),
 m(a.stallAngle,b.stallAngle),m(a.parasiteDrag,b.parasiteDrag),m(a.inducedDrag,b.inducedDrag),
 m(a.thrust,b.thrust),m(a.buoyancyFraction,b.buoyancyFraction),m(a.bodyDrag,b.bodyDrag),
 m(a.response,b.response),m(a.transitionSeconds,b.transitionSeconds)};
}
bool flight_role(std::string_view s){return s==vehicleType||s==rulesType||s==gateType||s==windType;}
float clampAxis(float x){return std::isfinite(x)?std::clamp(x,-1.F,1.F):0.F;}
std::string show(float value,unsigned precision=1){std::ostringstream o;o.imbue(std::locale::classic());o<<std::fixed<<std::setprecision(precision)<<value;return o.str();}
bool gate_crossed(Float3 a,Float3 b,const RigidTransform& gate,float width,float height){
 a=inverse_transform_point(gate,a);b=inverse_transform_point(gate,b);
 if(a.y>0||b.y<=0||b.y<=a.y)return false;
 const float f=-a.y/(b.y-a.y);const auto p=add(a,multiply(subtract(b,a),f));
 return std::abs(p.x)<=width&&std::abs(p.z)<=height;
}
}
const FlightProfile& flight_profile(FlightMode m)noexcept{return profiles[(std::min)(unsigned(m),5U)];}
const char* flight_mode_name(FlightMode m)noexcept{return flight_profile(m).name;}
bool scene_has_flying(std::span<const PrototypeObjectView> objects){
 for(const auto& o:objects)for(const auto& c:o.components)if(c.enabled&&flight_role(c.type))return true;
 return false;
}
void register_flying_components(ComponentTypeRegistry& registry){
 auto add=[&](std::string t,std::string label,std::vector<ComponentPropertySchema> fields){
  std::string error;(void)registry.register_type({std::move(t),std::move(label),false,std::move(fields)},&error);
 };
 add(std::string(vehicleType),"Morphing Flight Vehicle (Z up)",{
  {"key",std::string{"flyer"},true,false,"Unique persistent actor key"},
  {"mass_kg",140.0,false,false,"Constant prototype mass (kg)"},
  {"wind_x",3.0,false,false,"Uniform ambient wind X (m/s)"},
  {"wind_y",0.0,false,false,"Uniform ambient wind Y (m/s)"},
  {"starting_mode",std::int64_t{0},false,false,"0 float; 1 glider; 2 biplane; 3 jet; 4 hover; 5 bird"},
  {"assisted",true,false,false,"Flight stabilization and coordinated turns"},
  {"wind_shear",0.0,false,false,"Horizontal shear in m/s per log altitude"},
  {"wind_gust",0.0,false,false,"Deterministic horizontal gust amplitude (m/s)"}});
 add(std::string(rulesType),"Flight Game Rules",{
  {"key",std::string{"flight"},true,false,"Stable key"},
  {"kill_z",-15.0,false,false,"Automatic recovery below this altitude"}});
 add(std::string(gateType),"Ordered Flight Ring",{
  {"key",std::string{"flight-gate"},true,false,"Unique gate key"},
  {"order",std::int64_t{0},false,false,"Ordered gate index from zero"},
  {"mode",std::int64_t{0},false,false,"Required mode: 0 float,1 glider,2 biplane,3 jet,4 hover,5 bird"},
  {"half_width",20.0,false,false,"Gate half width (m)"},
  {"half_height",18.0,false,false,"Gate half height (m)"}});
 add(std::string(windType),"Flight Wind Volume (marker)",{
  {"key",std::string{"wind-zone"},true,false,"Unique volume key"},
  {"kind",std::int64_t{0},false,false,"0 thermal; 1 ridge windward/lee"},
  {"radius",80.0,false,false,"Thermal radius or ridge half-length (m)"},
  {"height",260.0,false,false,"Active height extent (m)"},
  {"strength",6.0,false,false,"Maximum vertical airflow speed (m/s)"},
  {"sink_fraction",0.25,false,false,"Thermal ring sink or ridge lee sink factor"}});
 add(std::string(stateType),"Flight Runtime State",{{"data",std::string{},false,false,"Versioned flight save data"}});
}
bool validate_flying_scene(std::span<const PrototypeObjectView> objects,std::string* error){
 auto fail=[&](std::string s){if(error)*error=std::move(s);return false;};
 auto registry=ComponentTypeRegistry::make_default();
 unsigned vehicles=0,rules=0,winds=0;
 std::set<std::string> keys;std::set<std::int64_t> orders;
 for(const auto& o:objects){
  unsigned count=0;
  for(const auto& c:o.components)if(c.enabled&&c.type.starts_with(kPrototypePrefix)){
   if(c.type==stateType)continue;
   if(!flight_role(c.type))return fail("Cannot mix flight actors with racing or first-person actors");
   if(++count>1)return fail("Only one flight role per object");
   std::string problem;if(!registry.validate(c,&problem))return fail(o.name+": "+problem);
   const auto key=prop(c,"key",std::string{});
   if(key.empty()||key.size()>64||key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-." )!=std::string::npos||!keys.insert(key).second)
    return fail("Missing or duplicate flight actor key");
   if(c.type==vehicleType){
    ++vehicles;
    if(o.marker)return fail("Flight craft requires visible polygon geometry");
    const double mass=prop(c,"mass_kg",140.0);
    if(!std::isfinite(mass)||mass<10||mass>10000)return fail("Flight craft mass must be 10..10000 kg");
    for(const char* axis:{"wind_x","wind_y"}){const double w=prop(c,axis,0.0);if(!std::isfinite(w)||std::abs(w)>100)return fail("Invalid flight wind");}
    for(const auto* field:{"wind_shear","wind_gust"}){
      const double value=prop(c,field,0.0);
      if(!std::isfinite(value)||(std::string_view(field)=="wind_gust"?(value<0||value>20):std::abs(value)>30))
        return fail("Invalid flight shear/gust setting");
    }
    const auto mode=prop(c,"starting_mode",std::int64_t{0});if(mode<0||mode>5)return fail("Invalid flight mode");
   }else if(c.type==rulesType){
    ++rules;if(!o.marker)return fail("Flight rules require a bodyless marker");
    const double z=prop(c,"kill_z",-15.0);if(!std::isfinite(z)||std::abs(z)>1000000)return fail("Invalid recovery altitude");
   }else if(c.type==gateType){
    const auto n=prop(c,"order",std::int64_t{-1}),mode=prop(c,"mode",std::int64_t{-1});
    const double w=prop(c,"half_width",20.0),h=prop(c,"half_height",18.0);
    if(n<0||n>=128||!orders.insert(n).second||mode<0||mode>5||
       !std::isfinite(w)||!std::isfinite(h)||w<1||w>100||h<1||h>100)
      return fail("Invalid flight gate order, mode or dimensions");
   }else if(c.type==windType){
    if(!o.marker||++winds>128)return fail("Wind volumes must be markers; maximum 128");
    const auto kind=prop(c,"kind",std::int64_t{-1});
    FlightWindZone z;
    z.stableId=o.id+1;
    z.kind=kind==1?FlightWindKind::Ridge:FlightWindKind::Thermal;
    z.radiusMeters=float(prop(c,"radius",80.0));
    z.heightMeters=float(prop(c,"height",260.0));
    z.strengthMetersPerSecond=float(prop(c,"strength",6.0));
    z.sinkFraction=float(prop(c,"sink_fraction",0.25));
    if(kind<0||kind>1||!validate_flight_wind_zone(z))
     return fail("Invalid authored thermal or ridge settings");
   }
  }
 }
 if(vehicles!=1||rules!=1)return fail("Flight scene requires one craft and one rules marker");
 if(orders.empty()||orders.size()>128)return fail("Flight scene requires at least one ordered gate");
 for(std::size_t i=0;i<orders.size();++i)if(!orders.contains(std::int64_t(i)))return fail("Flight gate orders must be contiguous");
 return true;
}
struct FlyingGame::Impl {
 struct Gate {GameObjectId id{};FlightMode mode{};float width{},height{};};
 GameWorld& world;
 PrototypeHud hud{};
 FlightTelemetry telemetry{};
 PrototypeCamera cam{};
 GameObjectId craft{},rules{};
 ComponentId stateComponent{};
 Component config{},ruleConfig{};
 std::string craftKey,rulesKey;
 std::vector<Gate> gates;
 FlightWindField windField;
 Float3 position{},velocity{},angular{},startPos{},checkpointPos{};
 Quaternion orientation{},startOrientation{},checkpointRotation{};
 FlightMode mode{FlightMode::Float},target{FlightMode::Float},transitionFrom{FlightMode::Float};
 float morph{},fuel{1},battery{1},stamina{1},spool{};
 float throttle{.65F},elapsed{},cameraLag{};
 float accumulator{};
 float lastSpecificEnergy{};bool energyReady{};
 unsigned nextGate{},crashes{};
 bool lastPause{},lastReset{},lastRecover{},lastView{},lastDebug{},lastNext{},lastPrev{};
 bool lastFlap{},cockpit{},crashed{},active{},finished{};
 explicit Impl(GameWorld& w):world(w){hud.health=hud.maximumHealth=100;hud.active=false;hud.flying=true;}
 void publish(){(void)world.set_position(craft,position);(void)world.set_rotation(craft,orientation);}
 void rebuild_wind(){
   windField.configure({float(prop(config,"wind_x",3.0)),float(prop(config,"wind_y",0.0)),0},
       float(prop(config,"wind_shear",0.0)),float(prop(config,"wind_gust",0.0)));
   std::vector<FlightWindZone> zones;
   for(const auto id:world.object_ids()){
     if(!world.is_enabled(id))continue;
     const auto* cs=world.components(id);
     if(!cs)continue;
     for(const auto& c:*cs)if(c.enabled&&c.type==windType){
       const auto pose=world.transform(id);
       if(!pose)continue;
       FlightWindZone zone;zone.stableId=id+1;
       zone.kind=prop(c,"kind",std::int64_t{0})==1?FlightWindKind::Ridge:FlightWindKind::Thermal;
       zone.transform=*pose;zone.radiusMeters=float(prop(c,"radius",80.0));
       zone.heightMeters=float(prop(c,"height",260.0));
       zone.strengthMetersPerSecond=float(prop(c,"strength",6.0));
       zone.sinkFraction=float(prop(c,"sink_fraction",0.25));
       if(validate_flight_wind_zone(zone))zones.push_back(zone);
     }
   }
   windField.set_zones(std::move(zones));
 }
 void reset(){
  mode=target=transitionFrom=FlightMode(prop(config,"starting_mode",std::int64_t{0}));
  position=startPos;orientation=startOrientation;
  velocity={0,1,0};angular={};morph=0;fuel=battery=stamina=1;spool=0;throttle=.65F;
  elapsed=0;accumulator=0;nextGate=0;crashes=0;crashed=finished=false;
  lastSpecificEnergy=0;energyReady=false;telemetry={};
  checkpointPos=startPos;checkpointRotation=startOrientation;
  hud.phase=PrototypePhase::Playing;publish();cameraLag=0;update_camera(true);
 }
 void recover(){
  ++crashes;crashed=false;position=checkpointPos;orientation=checkpointRotation;
  velocity={};angular={};morph=0;mode=target;spool=0;
  battery=(std::max)(battery,.25F);elapsed+=5;energyReady=false;publish();update_camera(true);
 }
 void request(FlightMode m){
  if(m==target)return;
  // New commands change the target only; no position/orientation/velocity assignment.
  transitionFrom=mode;target=m;morph=0;
 }
 void camera_avoid(Float3 focus,Float3& desired){
  const auto displacement=subtract(desired,focus);
  const float size=length(displacement);if(size<.001F)return;
  const Capsule probe{focus,focus,.28F};
  if(const auto hit=world.capsule_sweep(probe,displacement,craft)){
   desired=add(focus,multiply(displacement,std::clamp(hit->time-.25F/size,0.F,1.F)));
  }
 }
 void update_camera(bool snap){
  const Float3 f=rotate(orientation,{0,1,0}),up=rotate(orientation,{0,0,1});
  const Float3 focus=add(position,multiply(f,4));
  const Float3 desiredTarget=add(position,multiply(f,9));
  Float3 desired=cockpit?add(position,add(multiply(f,1.2F),multiply(up,.6F))):
                          add(add(position,multiply(f,-16)),{0,0,7});
  if(!cockpit)camera_avoid(position,desired);
  const float t=snap?1.F:1.F-std::exp(-8.F*stepSeconds);
  cam.position=add(cam.position,multiply(subtract(desired,cam.position),t));
  cam.target=add(cam.target,multiply(subtract(desiredTarget,cam.target),t));
  cam.up={0,0,1};
  cam.verticalFovRadians=mode==FlightMode::Jet?1.15F:1.02F;
  (void)focus;
 }
 bool intersects(Capsule capsule)const{return world.capsule_overlaps(capsule,craft);}
 Capsule fuselage(Float3 pos,Quaternion rot)const{
  const auto f=rotate(rot,{0,1,0});return {subtract(pos,multiply(f,1.6F)),add(pos,multiply(f,1.6F)),.65F};
 }
 Capsule wingtip(Float3 pos,Quaternion rot,bool right)const{
  const auto r=rotate(rot,{1,0,0});const bool compact=[](FlightMode m){return m==FlightMode::Float||m==FlightMode::Hover;}(mode)&&
     [](FlightMode m){return m==FlightMode::Float||m==FlightMode::Hover;}(target);
  // The sweep must cover unfolding wings from the moment the morph is requested.
  const float span=compact?1.2F:4.2F;
  const auto tip=add(pos,multiply(r,right?span:-span));return {tip,tip,.38F};
 }
 bool blocked(Float3 pos,Quaternion rot)const{
  return intersects(fuselage(pos,rot))||intersects(wingtip(pos,rot,true))||intersects(wingtip(pos,rot,false));
 }
 void advance(float dt){
  const float pitch=clampAxis(world.get_axis("move_y"));
  const float roll=clampAxis(world.get_axis("move_x"));
  const float yaw=clampAxis(world.get_axis("flight_yaw"));
  const float climb=clampAxis(world.get_axis("flight_vertical"));
  const bool flap=world.is_action_pressed("jump");
  const bool airbrake=world.is_action_pressed("flight_airbrake");
  // Throttle changes on the same 120 Hz grid as force integration.
  throttle=std::clamp(throttle+clampAxis(world.get_axis("flight_throttle"))*dt*.55F,0.F,1.F);
  const auto old=position;
  float morphBlend=0;
  if(target!=mode){
   const float duration=flight_profile(target).transitionSeconds;
   morph=(std::min)(1.F,morph+dt/duration);
   morphBlend=morph*morph*(3.F-2.F*morph);
   if(morph>=1){mode=target;transitionFrom=target;morph=0;morphBlend=0;}
  }
  const auto profile=target!=mode?blend(flight_profile(transitionFrom),flight_profile(target),morphBlend):flight_profile(mode);
  const float mass=float(prop(config,"mass_kg",140.0));
  const auto windSample=windField.sample(position,elapsed);
  const auto air=subtract(velocity,windSample.velocity);
  const float density=1.225F*std::exp(-(std::max)(position.z,0.F)/8500.0F);
  const auto aero=flight_aerodynamics(profile,air,orientation,density);
  const float q=aero.dynamicPressure;
  const float speed=aero.airspeed;
  const Float3 f=rotate(orientation,{0,1,0});
  Float3 force=add(Float3{0,0,-mass*gravity},aero.forceNewton);
  force.z+=mass*gravity*profile.buoyancyFraction;
  const float powered=mode==FlightMode::Biplane||mode==FlightMode::Jet||
    target==FlightMode::Biplane||target==FlightMode::Jet?1.F:0.F;
  const float goalSpool=powered&&fuel>0?throttle:0.F;
  spool+=std::clamp(goalSpool-spool,-dt*(mode==FlightMode::Jet?0.65F:1.5F),dt*(mode==FlightMode::Jet?0.65F:1.5F));
  spool=std::clamp(spool,0.F,1.F);
  if(fuel>0&&profile.thrust>0) {
   force=add(force,multiply(f,profile.thrust*spool));
   fuel=(std::max)(0.F,fuel-dt*spool*(mode==FlightMode::Jet?.004F:.002F));
  }
  // Float is buoyant, but the pilot needs a *small, battery-limited trim propulsor*
  // to reach the first ring. It never holds position or cancels wind automatically.
  const float floatMix=mode==FlightMode::Float&&target==mode?1.F:
    mode==FlightMode::Float?1.F-morphBlend:
    target==FlightMode::Float?morphBlend:0.F;
  if(floatMix>0&&battery>0){
    force=add(force,multiply(f,1100.F*floatMix*throttle));
    battery=(std::max)(0.F,battery-dt*.0009F*floatMix*throttle);
  }
  const float hoverMix=mode==FlightMode::Hover&&target==mode?1.F:
   mode==FlightMode::Hover?1.F-morphBlend:
   target==FlightMode::Hover?morphBlend:0.F;
  if(hoverMix>0&&battery>0){
    // Active rotor lift. Position control is limited by energy and acceleration, never teleports.
    const Float3 commanded={roll*8.F,pitch*12.F,climb*7.F};
    const Float3 error=subtract(commanded,velocity);
    Float3 correction=multiply(error,1.8F);
    const float maxA=14.F;
    if(length(correction)>maxA)correction=multiply(correction,maxA/length(correction));
    force=add(force,multiply(add(Float3{0,0,gravity},correction),mass*hoverMix));
    battery=(std::max)(0.F,battery-dt*.006F*hoverMix);
  }
  if((mode==FlightMode::Float||target==FlightMode::Float)&&battery>0&&std::abs(climb)>.01F){
    force.z+=mass*climb*1.5F;
    battery=(std::max)(0.F,battery-dt*.0006F*std::abs(climb));
  }
  if((mode==FlightMode::Bird||target==FlightMode::Bird)&&flap&&!lastFlap&&stamina>.12F){
    force=add(force,multiply(add(f,Float3{0,0,.75F}),mass*120.F));
    stamina=(std::max)(0.F,stamina-.12F);
  }
  lastFlap=flap;
  stamina=(std::min)(1.F,stamina+dt*.03F);
  if(airbrake)force=subtract(force,multiply(air,3.5F*speed));
  const Float3 accel=multiply(force,1.F/mass);
  velocity=add(velocity,multiply(accel,dt));
  // Only numerical protection against runaway non-finite states, not a mode-specific speed cap.
  if(!finite(velocity)||length(velocity)>350.F){crashed=true;recover();return;}
  const float authority=std::clamp(q/140.F,.05F,1.F);
  const float assist=prop(config,"assisted",true)?1.0F:0.65F;
  const float response=profile.response*assist*
      ((mode==FlightMode::Hover||mode==FlightMode::Float)?1.F:authority);
  // Assisted controls add a coordinated bank turn without instantly rotating the velocity.
  const Float3 aircraftUp=rotate(orientation,{0,0,1});
  const Float3 aircraftRight=rotate(orientation,{1,0,0});
  const float bank=std::atan2(aircraftRight.z,aircraftUp.z);
  const float yawAssist=(prop(config,"assisted",true)&&speed>6&&profile.wingArea>0)?
      std::clamp(-gravity*std::tan(std::clamp(bank,-1.0F,1.0F))/(std::max)(speed,6.0F),-0.5F,0.5F):0.0F;
  const Float3 localTarget{pitch*1.15F,roll*1.35F,yaw*.90F+yawAssist};
  const float blendInput=1.F-std::exp(-response*dt);
  angular=add(angular,multiply(subtract(localTarget,angular),blendInput));
  const auto rotatedX=rotate(orientation,{1,0,0});
  const auto rotatedY=rotate(orientation,{0,1,0});
  const auto rotatedZ=rotate(orientation,{0,0,1});
  Quaternion candidate=orientation;
  for(const auto [axis,delta]:std::array<std::pair<Float3,float>,3>{{{rotatedX,angular.x*dt},{rotatedY,angular.y*dt},{rotatedZ,angular.z*dt}}})
   candidate=normalize(multiply(quaternion_from_axis_angle(axis,delta),candidate));
  // Rotation sweeps do not exist; check wings/fuselage after rotation before applying translation.
  if(!blocked(position,candidate))orientation=candidate;else angular={};
  const auto move=multiply(velocity,dt);
  float fraction=1.F;Float3 normal{};
  for(const Capsule p:{fuselage(position,orientation),wingtip(position,orientation,true),wingtip(position,orientation,false)}){
    if(const auto hit=world.capsule_sweep(p,move,craft)){
      if(hit->time<fraction){fraction=hit->time;normal=hit->worldNormal;}
    }
  }
  if(fraction<1.F){
    position=add(position,multiply(move,(std::max)(0.F,fraction-.003F/(std::max)(length(move),.003F))));
    const float inward=dot(velocity,normal);
    if(inward<0){
      if(-inward>14.F){crashed=true;recover();return;}
      velocity=subtract(velocity,multiply(normal,1.05F*inward));
    }
  }else position=add(position,move);
  if(position.z<float(prop(ruleConfig,"kill_z",-15.0))){crashed=true;recover();return;}
  if(nextGate<gates.size()){
    const auto& g=gates[nextGate];const auto pose=world.transform(g.id);
    if(pose&&mode==g.mode&&target==mode&&gate_crossed(old,position,*pose,g.width,g.height)){
      checkpointPos=position;checkpointRotation=orientation;++nextGate;
      if(nextGate==gates.size()){hud.phase=PrototypePhase::Won;finished=true;}
    }
  }
  elapsed+=dt;publish();update_camera(false);
  telemetry.airspeed=speed;telemetry.groundspeed=length(velocity);telemetry.verticalSpeed=velocity.z;
  telemetry.altitude=position.z;telemetry.angleOfAttack=aero.angleOfAttack;
  telemetry.dynamicPressure=q;telemetry.liftCoefficient=aero.liftCoefficient;
  telemetry.dragCoefficient=aero.dragCoefficient;
  telemetry.windVelocity=windSample.velocity;telemetry.windSpeed=length(windSample.velocity);
  telemetry.thermalUpdraft=windSample.thermalVertical;
  telemetry.ridgeUpdraft=windSample.ridgeVertical;
  telemetry.stalled=aero.stalled;telemetry.stallMargin=aero.stallMarginRadians;
  telemetry.spool=spool;telemetry.bankAngle=bank;
  const float specificEnergy=0.5F*dot(velocity,velocity)+gravity*position.z;
  telemetry.energyRatePerKilogram=energyReady?(specificEnergy-lastSpecificEnergy)/dt:0;
  telemetry.mechanicalEnergyPerKilogram=specificEnergy;
  lastSpecificEnergy=specificEnergy;energyReady=true;
  // Var: fixed-step low-pass vertical speed, stable at 60/120 fps.
  telemetry.variometer+=std::clamp(1.0F-std::exp(-2.5F*dt),0.0F,1.0F)*
     (velocity.z-telemetry.variometer);
  const float horizontal=std::sqrt(velocity.x*velocity.x+velocity.y*velocity.y);
  telemetry.glideRatio=velocity.z<-.3F?std::clamp(horizontal/-velocity.z,0.0F,200.F):0.0F;
  telemetry.fuel=fuel;telemetry.battery=battery;telemetry.stamina=stamina;
  telemetry.mode=mode;telemetry.target=target;telemetry.transitioning=target!=mode;
  telemetry.morph=morph;telemetry.nextGate=nextGate;telemetry.gateCount=unsigned(gates.size());
 }
 void hud_update(){
  hud.stats=std::string("FLIGHT ")+flight_mode_name(mode)+(mode!=target?std::string(" > ")+flight_mode_name(target)+" "+show(100.F*morph,0)+"%":"");
  hud.prompt="IAS "+show(telemetry.airspeed)+" M/S  ALT "+show(position.z,0)+" M  VAR "+show(telemetry.variometer)+" M/S";
  hud.message="CLIMB "+show(velocity.z)+"  FUEL "+show(100.F*fuel,0)+"%  BAT "+show(100.F*battery,0)+"%  GATE "+std::to_string(nextGate)+"/"+std::to_string(gates.size());
  if(hud.phase==PrototypePhase::Paused)hud.message="PAUSED - P TO RESUME";
  if(hud.phase==PrototypePhase::Won)hud.message="COURSE COMPLETE - R TO RESTART";
  if(telemetry.stalled&&hud.phase==PrototypePhase::Playing)hud.message="STALL: LOWER NOSE  "+hud.message;
  hud.vehicleTelemetry="AOA "+show(telemetry.angleOfAttack,2)+" BANK "+show(telemetry.bankAngle,2)+
    " GLIDE "+show(telemetry.glideRatio,1)+" E "+show(telemetry.energyRatePerKilogram);
   hud.flightWeather="WIND "+show(telemetry.windVelocity.x)+","+
    show(telemetry.windVelocity.y)+","+show(telemetry.windVelocity.z)+
    " THERM "+show(telemetry.thermalUpdraft)+" RIDGE "+show(telemetry.ridgeUpdraft);
 }
 void persist(){
  std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17);
  out<<"DVE_FLIGHT 1 "<<unsigned(hud.phase)<<' '<<unsigned(mode)<<' '<<unsigned(target)<<' '
    <<unsigned(transitionFrom)<<' '<<morph<<' '<<position.x<<' '<<position.y<<' '<<position.z<<' '
    <<velocity.x<<' '<<velocity.y<<' '<<velocity.z<<' '
    <<orientation.x<<' '<<orientation.y<<' '<<orientation.z<<' '<<orientation.w<<' '
    <<angular.x<<' '<<angular.y<<' '<<angular.z<<' '
    <<fuel<<' '<<battery<<' '<<stamina<<' '<<spool<<' '<<throttle<<' '<<elapsed<<' '
    <<nextGate<<' '<<crashes<<' '<<checkpointPos.x<<' '<<checkpointPos.y<<' '<<checkpointPos.z<<' '
    <<checkpointRotation.x<<' '<<checkpointRotation.y<<' '<<checkpointRotation.z<<' '<<checkpointRotation.w<<' '
    <<accumulator<<' '<<cockpit;
  (void)world.set_component_property(rules,stateComponent,"data",out.str());
 }
};
FlyingGame::FlyingGame(GameWorld& w):impl_(std::make_unique<Impl>(w)){}
FlyingGame::~FlyingGame()=default;
const PrototypeHud& FlyingGame::hud()const noexcept{return impl_->hud;}
PrototypeCamera FlyingGame::camera()const noexcept{return impl_->cam;}
const FlightTelemetry& FlyingGame::telemetry()const noexcept{return impl_->telemetry;}
bool FlyingGame::start(std::string* error){
 auto& s=*impl_;if(s.active){if(error)*error="Flight already started";return false;}
 std::vector<PrototypeObjectView> views;
 for(auto id:s.world.object_ids())views.push_back({id,"Object "+std::to_string(id),*s.world.components(id),s.world.geometry_kind(id)==GameGeometryKind::Marker});
 if(!validate_flying_scene(views,error))return false;
 std::map<std::int64_t,Impl::Gate> ordered;
 for(const auto& o:views)for(const auto& c:o.components)if(c.enabled&&flight_role(c.type)){
  if(!s.world.is_enabled(o.id)){if(error)*error="Flight actors must be enabled";return false;}
  if(c.type==vehicleType){s.craft=o.id;s.config=c;s.craftKey=prop(c,"key",std::string{});}
  else if(c.type==rulesType){s.rules=o.id;s.ruleConfig=c;s.rulesKey=prop(c,"key",std::string{});}
  else if(c.type==gateType)ordered.emplace(prop(c,"order",std::int64_t{0}),Impl::Gate{o.id,FlightMode(prop(c,"mode",std::int64_t{0})),float(prop(c,"half_width",20.0)),float(prop(c,"half_height",18.0))});
 }
 if(s.world.has_collision(s.craft).value_or(true)){if(error)*error="Flight vehicle must disable collision: runtime owns swept proxies";return false;}
 if(!configure_physics3d_gravity(s.world.physics(),{0,0,-9.81F})){if(error)*error="Flight requires configurable Z-up gravity";return false;}
 for(const auto& [n,g]:ordered){(void)n;s.gates.push_back(g);}
 s.startPos=*s.world.position(s.craft);s.startOrientation=s.world.transform(s.craft)->rotation;
  s.rebuild_wind();
 Component state;state.type=std::string(stateType);state.properties["data"]=std::string{};
 auto* added=s.world.add_component(s.rules,std::move(state),error);if(!added)return false;
 s.stateComponent=added->id;s.hud.active=s.active=true;
 s.reset();s.hud_update();s.persist();return true;
}
void FlyingGame::step(float dt){
 auto& s=*impl_;if(!s.active||!std::isfinite(dt)||dt<=0||dt>.25F)return;
 auto edge=[&](const char* action,bool& held){const bool current=s.world.is_action_pressed(action);const bool fired=current&&!held;held=current;return fired;};
 if(edge("prototype_debug",s.lastDebug))s.hud.debug=!s.hud.debug;
 if(edge("restart",s.lastReset)){s.reset();s.hud_update();s.persist();return;}
 if(edge("pause",s.lastPause)&&(s.hud.phase==PrototypePhase::Playing||s.hud.phase==PrototypePhase::Paused))
  s.hud.phase=s.hud.phase==PrototypePhase::Playing?PrototypePhase::Paused:PrototypePhase::Playing;
 if(edge("interact",s.lastView)){s.cockpit=!s.cockpit;s.update_camera(true);}
 if(edge("crouch",s.lastRecover))s.recover();
 const bool next=edge("flight_next",s.lastNext),previous=edge("flight_previous",s.lastPrev);
 if(next)s.request(FlightMode((unsigned(s.target)+1)%6U));
 if(previous)s.request(FlightMode((unsigned(s.target)+5)%6U));
 for(unsigned i=0;i<6;++i)if(s.world.is_action_pressed("flight_mode_"+std::to_string(i+1)))
  s.request(FlightMode(i));
 if(s.hud.phase==PrototypePhase::Playing){
  s.accumulator+=dt;
  for(unsigned n=0;n<31&&s.accumulator+1.e-7F>=stepSeconds;++n){
   s.accumulator-=stepSeconds;if(s.accumulator<0)s.accumulator=0;
   s.advance(stepSeconds);s.world.tick(stepSeconds);
   if(s.hud.phase!=PrototypePhase::Playing){s.accumulator=0;break;}
  }
 }
 s.hud_update();s.persist();
}
bool FlyingGame::after_restore(std::string* error){
 auto& s=*impl_;
 auto fail=[&](){if(error)*error="Invalid DVE_FLIGHT 1 saved state";return false;};
 std::vector<PrototypeObjectView> views;GameObjectId craft=0,rules=0;
 std::map<std::int64_t,Impl::Gate> ordered;
 for(auto id:s.world.object_ids()){
  const auto* cs=s.world.components(id);views.push_back({id,"Saved actor",*cs,s.world.geometry_kind(id)==GameGeometryKind::Marker});
  for(const auto& c:*cs)if(c.enabled){
   if(c.type==vehicleType&&prop(c,"key",std::string{})==s.craftKey)craft=id;
   if(c.type==rulesType&&prop(c,"key",std::string{})==s.rulesKey)rules=id;
   if(c.type==gateType)ordered.emplace(prop(c,"order",std::int64_t{0}),Impl::Gate{id,FlightMode(prop(c,"mode",std::int64_t{0})),float(prop(c,"half_width",20.0)),float(prop(c,"half_height",18.0))});
  }
 }
 if(!validate_flying_scene(views,error)||!craft||!rules||ordered.size()!=s.gates.size()||s.world.has_collision(craft).value_or(true))return fail();
 const auto* c=find_component_by_type(*s.world.components(rules),stateType);if(!c)return fail();
 const auto data=prop(*c,"data",std::string{});if(data.size()>4096)return fail();
 std::istringstream in(data);in.imbue(std::locale::classic());
 std::string magic;unsigned version{},phase{},mode{},target{},from{},next{},crashes{};
 float morph{},fuel{},battery{},stamina{},spool{},throttle{},elapsed{},accum{};
 Float3 pos{},vel{},ang{},checkpoint{};Quaternion rot{},checkRot{};bool cockpit{};
 in>>magic>>version>>phase>>mode>>target>>from>>morph
   >>pos.x>>pos.y>>pos.z>>vel.x>>vel.y>>vel.z
   >>rot.x>>rot.y>>rot.z>>rot.w>>ang.x>>ang.y>>ang.z
   >>fuel>>battery>>stamina>>spool>>throttle>>elapsed>>next>>crashes
   >>checkpoint.x>>checkpoint.y>>checkpoint.z
   >>checkRot.x>>checkRot.y>>checkRot.z>>checkRot.w>>accum>>cockpit;
 if(!in||magic!="DVE_FLIGHT"||version!=1||phase>2||mode>5||target>5||from>5||
    !finite(pos)||!finite(vel)||!finite(ang)||!finite(rot)||!finite(checkpoint)||!finite(checkRot)||
    length(pos)>1000000||length(vel)>350||length(ang)>20||
    !(std::abs(length(Float3{rot.x,rot.y,rot.z})*length(Float3{rot.x,rot.y,rot.z})+rot.w*rot.w-1.F)<.02F)||
    !(std::abs(length(Float3{checkRot.x,checkRot.y,checkRot.z})*length(Float3{checkRot.x,checkRot.y,checkRot.z})+checkRot.w*checkRot.w-1.F)<.02F)||
    !std::isfinite(morph)||morph<0||morph>1||
    !std::isfinite(fuel)||fuel<0||fuel>1||!std::isfinite(battery)||battery<0||battery>1||
    !std::isfinite(stamina)||stamina<0||stamina>1||!std::isfinite(spool)||spool<0||spool>1||
    !std::isfinite(throttle)||throttle<0||throttle>1||!std::isfinite(elapsed)||elapsed<0||
    !std::isfinite(accum)||accum<0||accum>=stepSeconds||next>ordered.size()||crashes>1000000)return fail();
 in>>std::ws;if(!in.eof())return fail();
 s.craft=craft;s.rules=rules;s.stateComponent=c->id;s.gates.clear();
 for(const auto& [n,g]:ordered){(void)n;s.gates.push_back(g);}
 s.position=pos;s.velocity=vel;s.orientation=rot;s.angular=ang;
 s.mode=FlightMode(mode);s.target=FlightMode(target);s.transitionFrom=FlightMode(from);s.morph=morph;
 s.fuel=fuel;s.battery=battery;s.stamina=stamina;s.spool=spool;s.throttle=throttle;
 s.elapsed=elapsed;s.nextGate=next;s.crashes=crashes;s.checkpointPos=checkpoint;s.checkpointRotation=checkRot;
 s.accumulator=accum;s.cockpit=cockpit;s.hud.phase=PrototypePhase(phase);
 if(const auto* cfg=find_component_by_type(*s.world.components(craft),vehicleType))s.config=*cfg;
 s.rebuild_wind();s.energyReady=false;
 s.telemetry={};s.telemetry.mode=s.mode;s.telemetry.target=s.target;
 s.telemetry.fuel=s.fuel;s.telemetry.battery=s.battery;s.telemetry.stamina=s.stamina;
 s.telemetry.airspeed=length(subtract(s.velocity,s.windField.sample(s.position,s.elapsed).velocity));
 s.telemetry.groundspeed=length(s.velocity);s.telemetry.altitude=s.position.z;
 s.update_camera(true);s.hud_update();return true;
}
}
