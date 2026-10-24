#include "dve/racing_game.hpp"
#include "dve/game_world.hpp"
#include "dve/physics3d_backend.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <limits>
#include <map>
#include <set>
#include <sstream>
namespace dve {
namespace {
constexpr const char* carType="dve.prototype.race_car";
constexpr const char* rulesType="dve.prototype.race_rules";
constexpr const char* gateType="dve.prototype.race_gate";
constexpr const char* surfaceType="dve.prototype.race_surface";
constexpr const char* stateType="dve.prototype.race_state";
constexpr double fixedStep=1.0/120;
constexpr float kTwoPi=6.283185307F;
struct HandlingProfile { float grip,steer,accel,brake,handbrakeGrip; };
HandlingProfile profile_for(std::string_view name){
 if(name=="arcade")return {1.45F,1.08F,1.15F,1.10F,0.28F};
 if(name=="drift")return {0.48F,1.28F,1.05F,1.0F,0.08F};
 if(name=="offroad")return {0.70F,0.96F,0.85F,0.90F,0.20F};
 if(name=="track")return {1.60F,0.83F,1.18F,1.20F,0.22F};
 return {1.0F,1.0F,1.0F,1.0F,0.15F};
}
template<class T>T get(const Component& c,std::string_view k,T fallback){auto i=c.properties.find(k);if(i!=c.properties.end())if(auto p=std::get_if<T>(&i->second))return *p;return fallback;}
bool finite(Float3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
Float3 forward(float yaw){return {std::sin(yaw),std::cos(yaw),0};}
float heading(Quaternion q){const auto f=rotate(q,{0,1,0});return std::atan2(f.x,f.y);}
std::string time_text(double seconds){std::ostringstream s;s.imbue(std::locale::classic());s<<std::fixed<<std::setprecision(2)<<seconds;return s.str();}
bool racing_type(std::string_view t){return t==carType||t==rulesType||t==gateType||t==surfaceType;}
}
bool scene_has_racing(std::span<const PrototypeObjectView> objects){for(const auto& o:objects)for(const auto& c:o.components)if(c.enabled&&racing_type(c.type))return true;return false;}
void register_racing_components(ComponentTypeRegistry& registry){
 auto add=[&](const char* type,std::string title,std::vector<ComponentPropertySchema> p){std::string error;(void)registry.register_type({type,std::move(title),false,std::move(p)},&error);};
 add(carType,"Arcade Race Car (Z up)",{{"key",std::string{"car"},true,false,"Stable gameplay key"},{"top_speed",38.0,false,false,"Forward speed limit (m/s)"},{"acceleration",12.0,false,false,"Acceleration (m/s2)"},{"braking",30.0,false,false,"Service brake deceleration (m/s2)"},{"grip",7.0,false,false,"Lateral velocity damping (1/s)"},{"wheelbase",2.6,false,false,"Steering wheelbase (m)"},{"steer_angle",.55,false,false,"Maximum steering angle (rad)"},{"handling_profile",std::string{"street"},false,false,"street, arcade, drift, offroad or track"},{"camera_smoothing",8.0,false,false,"Chase camera response (1/s)"}});
 add(rulesType,"Race Time Trial Rules",{{"key",std::string{"race"},true,false,"Stable gameplay key"},{"laps",std::int64_t{3},false,false,"Laps to complete"},{"countdown",3.0,false,false,"Start countdown (s)"},{"kill_z",-15.0,false,false,"Recovery below this Z"}});
 add(surfaceType,"Race Driving Surface",{{"key",std::string{"surface"},true,false,"Stable surface key"},{"grip_multiplier",1.0,false,false,"Tire traction multiplier (0.05..3)"},{"rolling_drag_multiplier",1.0,false,false,"Longitudinal rolling drag multiplier (0.1..8)"}});
 add(gateType,"Ordered Race Gate",{{"key",std::string{"gate"},true,false,"Stable gameplay key"},{"order",std::int64_t{0},false,false,"Contiguous order; gate 0 is finish"},{"half_width",10.0,false,false,"Gate half width (m)"},{"half_height",4.0,false,false,"Gate half height (m)"}});
 add(stateType,"Race Runtime State",{{"data",std::string{},false,false,"Runtime-only versioned save payload"}});
}
bool validate_racing_scene(std::span<const PrototypeObjectView> objects,std::string* error){
 auto fail=[&](std::string s){if(error)*error=std::move(s);return false;};
 const auto registry=ComponentTypeRegistry::make_default();unsigned cars=0,rules=0;std::set<std::int64_t> gates;std::set<std::string> keys;
 for(const auto& o:objects){unsigned roles=0;for(const auto& c:o.components)if(c.enabled&&c.type.starts_with(kPrototypePrefix)){
  if(c.type==stateType)continue;
  if(!racing_type(c.type))return fail("A racing scene cannot also contain first-person roles or event connections");
  if(++roles>1)return fail(o.name+": one racing role per object");
  std::string reason;if(!registry.validate(c,&reason))return fail(o.name+": "+reason);
  const auto key=get(c,"key",std::string{});if(key.empty()||key.size()>64||key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")!=std::string::npos||!keys.insert(key).second)return fail("Missing, invalid or duplicate racing key");
  auto number=[&](const char* n,double lo,double hi){const auto v=get(c,n,std::numeric_limits<double>::quiet_NaN());return std::isfinite(v)&&v>=lo&&v<=hi;};
  if(c.type==carType){++cars;if(o.marker)return fail("Race car requires visible geometry with collision disabled");if(!number("top_speed",1,100)||!number("acceleration",.1,100)||!number("braking",.1,200)||!number("grip",.1,30)||!number("wheelbase",.5,10)||!number("steer_angle",.05,1)||!std::isfinite(get(c,"camera_smoothing",8.0))||get(c,"camera_smoothing",8.0)<1||get(c,"camera_smoothing",8.0)>30)return fail("Invalid race car tuning");
   const auto profile=get(c,"handling_profile",std::string{"street"});
   if(profile!="street"&&profile!="arcade"&&profile!="drift"&&profile!="offroad"&&profile!="track")return fail("Invalid handling profile");}
  if(c.type==rulesType){++rules;if(!o.marker)return fail("Race Rules must be a bodyless marker");const auto laps=get(c,"laps",std::int64_t{0});if(laps<1||laps>100||!number("countdown",0,30)||!number("kill_z",-1000000,1000000))return fail("Invalid race rules");}
  if(c.type==surfaceType){
    if(o.marker||!number("grip_multiplier",.05,3)||!number("rolling_drag_multiplier",.1,8))
      return fail("Driving surfaces require geometry and valid grip/drag multipliers");
  }
  if(c.type==gateType){const auto n=get(c,"order",std::int64_t{-1});if(n<0||n>=128||!gates.insert(n).second||!number("half_width",1,100)||!number("half_height",.5,100))return fail("Duplicate/invalid gate order or size");}
 }}
 if(cars!=1||rules!=1||gates.size()<2||gates.size()>128)return fail("Racing needs one car, one Rules marker and 2..128 ordered gates");
 for(std::size_t n=0;n<gates.size();++n)if(!gates.contains(static_cast<std::int64_t>(n)))return fail("Gate orders must be contiguous from zero");
 return true;
}
bool racing_gate_crossed(Float3 a,Float3 b,const RigidTransform& gate,float width,float height){
 a=inverse_transform_point(gate,a);b=inverse_transform_point(gate,b);
 if(!finite(a)||!finite(b)||a.y>0||b.y<=0||b.y<=a.y)return false;
 const float t=-a.y/(b.y-a.y);const auto p=add(a,multiply(subtract(b,a),t));return std::abs(p.x)<=width&&std::abs(p.z)<=height;
}
struct RacingGame::Impl {
 struct Gate {GameObjectId id{};float width{},height{};};
 struct DrivingSurface {float grip{1.0F},drag{1.0F};};
 GameWorld& world;PrototypeHud hud;RacingStatus race;RacingTelemetry telemetry;
 PrototypeCamera smoothCamera{};bool cameraReady{},lastCameraHood{};
 GameObjectId car{},rules{};ComponentId state{};Component config,ruleConfig;std::vector<Gate> gates;
 std::map<GameObjectId,DrivingSurface> surfaces;
 std::string carKey,rulesKey;Float3 initial{},checkpoint{},velocity{};float yaw{},initialYaw{},checkpointYaw{},verticalSpeed{};double accumulator{};
 bool lastPause{},lastRestart{},lastRecovery{},lastView{},lastDebug{},hood{};
 explicit Impl(GameWorld& w):world(w){hud.racing=true;hud.health=hud.maximumHealth=100;}
 void log(std::string s){if(hud.trace.size()>=64)hud.trace.erase(hud.trace.begin());hud.trace.push_back(std::move(s));}
 void update_hud(){
  race.speedMetersPerSecond=length(velocity);
  hud.vehicleTelemetry="SLIP "+time_text(std::abs(telemetry.lateralSlipMetersPerSecond))+
       "  YAW "+time_text(telemetry.yawRateRadiansPerSecond)+"  HITS "+std::to_string(telemetry.collisionCount);
  hud.stats=std::to_string(int(race.speedMetersPerSecond*3.6F))+" KPH  LAP "+std::to_string((std::min)(race.completedLaps+1,race.targetLaps))+" / "+std::to_string(race.targetLaps)+" LAST "+time_text(race.lastLap);
  hud.prompt="TIME "+time_text(race.elapsed)+"  BEST "+(race.bestLap>0?time_text(race.bestLap):"--")+"  NEXT "+std::to_string(race.nextGate);
  if(race.countdown>0)hud.message="READY "+std::to_string(int(std::ceil(race.countdown)))+" - W/S throttle, A/D steer";
  else if(hud.phase==PrototypePhase::Won)hud.message="FINISHED "+time_text(race.elapsed)+" - R new attempt";
  else hud.message="SPACE brake  CTRL +5s  E camera  P pause  R restart";
 }
 void persist(){std::ostringstream s;s.imbue(std::locale::classic());s<<std::setprecision(17)<<"DVE_RACE 1 "<<unsigned(hud.phase)<<' '<<yaw<<' '<<velocity.x<<' '<<velocity.y<<' '<<verticalSpeed<<' '<<race.elapsed<<' '<<race.lapStarted<<' '<<race.lastLap<<' '<<race.bestLap<<' '<<race.countdown<<' '<<race.completedLaps<<' '<<race.nextGate<<' '<<checkpoint.x<<' '<<checkpoint.y<<' '<<checkpoint.z<<' '<<checkpointYaw<<' '<<hood<<' '<<hud.debug<<' '<<accumulator;
  (void)world.set_component_property(rules,state,"data",s.str());}
 void place(Float3 p,float angle){(void)world.set_position(car,p);(void)world.set_rotation(car,quaternion_from_axis_angle({0,0,1},-angle));}
 void recover(){place(checkpoint,checkpointYaw);yaw=checkpointYaw;velocity={};verticalSpeed=0;telemetry={};update_camera(0,true);if(race.countdown<=0)race.elapsed+=5;log("Recovered; five-second penalty");}
 void restart(){const double best=race.bestLap;race={};race.bestLap=best;race.targetLaps=unsigned(get(ruleConfig,"laps",std::int64_t{3}));race.gateCount=unsigned(gates.size());race.countdown=get(ruleConfig,"countdown",3.0);checkpoint=initial;checkpointYaw=yaw=initialYaw;velocity={};verticalSpeed=0;telemetry={};accumulator=0;hood=false;hud.phase=PrototypePhase::Playing;place(initial,initialYaw);update_camera(0,true);log("New attempt");}
 Capsule capsule(Float3 p,float angle)const{const auto f=forward(angle);const auto center=add(p,{0,0,1.1F});return {subtract(center,multiply(f,.7F)),add(center,multiply(f,.7F)),.96F};}
 Capsule capsule(Float3 p)const{return capsule(p,yaw);}
 void update_camera(float dt,bool snap=false){
   const auto pos=world.position(car).value_or(initial);const auto f=forward(yaw);
   const auto focus=add(pos,{0,0,1});const auto viewTarget=add(focus,multiply(f,5));
   auto avoid=[&](Float3 destination){
     const auto path=subtract(destination,focus);const float distance=length(path);
     if(!(distance>0.001F))return destination;
     const auto hit=world.capsule_sweep(Capsule{focus,focus,0.25F},path,car);
     if(!hit)return destination;
     return add(focus,multiply(path,std::clamp(hit->time-0.12F/distance,0.0F,1.0F)));
   };
   const Float3 desired=hood?add(add(pos,multiply(f,0.8F)),Float3{0,0,1.2F})
                         :avoid(add(add(focus,multiply(f,-7)),Float3{0,0,3.5F}));
   if(!cameraReady||snap||hood!=lastCameraHood){
     smoothCamera.position=desired;smoothCamera.target=viewTarget;cameraReady=true;
   }else{
     const float response=float(get(config,"camera_smoothing",8.0));
     const float blend=1.0F-std::exp(-response*dt);
     smoothCamera.position=add(smoothCamera.position,multiply(subtract(desired,smoothCamera.position),blend));
     smoothCamera.target=add(smoothCamera.target,multiply(subtract(viewTarget,smoothCamera.target),blend));
     if(!hood)smoothCamera.position=avoid(smoothCamera.position);
   }
   lastCameraHood=hood;
 }
 DrivingSurface surface_at(Float3 pos)const{
   const auto hit=world.raycast(add(pos,{0,0,1}),{0,0,-1},4);
   if(!hit||hit->worldNormal.z<0.5F)return {};
   const auto it=surfaces.find(hit->objectId);
   return it==surfaces.end()?DrivingSurface{}:it->second;
 }
 void drive(float dt){
  auto pos=world.position(car).value_or(initial);const auto old=pos;
  auto axis=[&](const char* name){const auto n=world.get_axis(name);return std::isfinite(n)?std::clamp(n,-1.F,1.F):0.F;};
  const float throttle=axis("move_y"),steering=axis("move_x");const bool handbrake=world.is_action_pressed("jump");
  const float serviceBrake=(std::max)(std::clamp(axis("race_brake"),0.0F,1.0F),
                                    world.is_action_pressed("race_brake")?1.0F:0.0F);
  const auto profile=profile_for(get(config,"handling_profile",std::string{"street"}));
  const DrivingSurface surface=surface_at(pos);
  telemetry.surfaceGripMultiplier=surface.grip;
  telemetry.rollingDragMultiplier=surface.drag;
  auto f=forward(yaw);float speed=dot(velocity,f);const float oldSpeed=speed;
  auto lateral=subtract(velocity,multiply(f,speed));
  const float acceleration=float(get(config,"acceleration",12.0))*profile.accel;
  const float brake=float(get(config,"braking",30.0))*profile.brake;
  if(serviceBrake>0.001F||throttle*speed<0||handbrake){
    const float decel=serviceBrake>0.001F?brake*serviceBrake:(handbrake?brake*0.65F:brake);
    speed=std::copysign((std::max)(0.F,std::abs(speed)-decel*dt),speed);
  }else speed+=throttle*acceleration*dt;
  speed*=std::exp(-(std::abs(throttle)<.01F?.45F:.04F)*surface.drag*dt);speed=std::clamp(speed,-10.F,float(get(config,"top_speed",38.0)));
  const auto driven=add(multiply(f,speed),lateral);
  const float priorYaw=yaw;
  const float steerAngle=steering*float(get(config,"steer_angle",.55))*profile.steer;
  const float desiredYaw=std::remainder(yaw+speed/float(get(config,"wheelbase",2.6))*
       std::tan(steerAngle)*dt/(1+.002F*speed*speed),kTwoPi);
  // A translational capsule sweep cannot see overlaps created by rotating the endpoints.
  // Clip the rotation against the world before starting the slide loop.
  if(world.capsule_overlaps(capsule(pos,desiredYaw),car)){
    const float delta=std::remainder(desiredYaw-priorYaw,kTwoPi);
    float freeFraction=0,blockedFraction=1;
    for(unsigned i=0;i<10;++i){
      const float candidate=(freeFraction+blockedFraction)*0.5F;
      if(world.capsule_overlaps(capsule(pos,priorYaw+delta*candidate),car))blockedFraction=candidate;
      else freeFraction=candidate;
    }
    yaw=std::remainder(priorYaw+delta*freeFraction,kTwoPi);
  }else yaw=desiredYaw;
  telemetry.yawRateRadiansPerSecond=std::remainder(yaw-priorYaw,kTwoPi)/dt;
  f=forward(yaw);speed=dot(driven,f);lateral=subtract(driven,multiply(f,speed));
  lateral=multiply(lateral,std::exp(-float(get(config,"grip",7.0))*profile.grip*surface.grip*(handbrake?profile.handbrakeGrip:1.F)*dt));
  velocity=add(multiply(f,speed),lateral);const float maximum=float(get(config,"top_speed",38.0));if(length(velocity)>maximum)velocity=multiply(velocity,maximum/length(velocity));
  float remainingTime=dt;auto remaining=multiply(velocity,remainingTime);bool hitThisStep=false;
  for(unsigned n=0;n<3&&length(remaining)>.000001F;++n){auto hit=world.capsule_sweep(capsule(pos),remaining,car);if(!hit){pos=add(pos,remaining);break;}
   const float fraction=std::clamp(hit->time-.002F/(std::max)(length(remaining),.002F),0.F,1.F);pos=add(pos,multiply(remaining,fraction));
   auto normal=hit->worldNormal;normal.z=0;const float norm=length(normal);if(norm<.1F){velocity={};break;}normal=multiply(normal,1/norm);
   const float into=dot(velocity,normal);
   if(into<0){
     telemetry.impactSpeedMetersPerSecond=-into;
     if(!hitThisStep){++telemetry.collisionCount;hitThisStep=true;}
     velocity=subtract(velocity,multiply(normal,1.05F*into));
   }
   remainingTime*=1-fraction;remaining=multiply(velocity,remainingTime);
  }
  const auto ground=world.raycast(add(pos,{0,0,1}),{0,0,-1},4);
  if(ground&&ground->worldNormal.z>.5F&&pos.z-ground->worldPosition.z<=.35F&&verticalSpeed<=0){pos.z=ground->worldPosition.z+.04F;verticalSpeed=0;telemetry.grounded=true;}
  else {verticalSpeed-=9.81F*dt;pos.z+=verticalSpeed*dt;telemetry.grounded=false;}
  const auto newForward=forward(yaw);const float newSpeed=dot(velocity,newForward);
  telemetry.forwardSpeedMetersPerSecond=newSpeed;
  telemetry.lateralSlipMetersPerSecond=dot(velocity,Float3{newForward.y,-newForward.x,0});
  telemetry.longitudinalAccelerationMetersPerSecondSquared=(newSpeed-oldSpeed)/dt;
  telemetry.skidIntensity=std::clamp(std::abs(telemetry.lateralSlipMetersPerSecond)/8.0F,0.0F,1.0F);
  place(pos,yaw);
  if(pos.z<float(get(ruleConfig,"kill_z",-15.0))){recover();return;}
  const auto& gate=gates[race.nextGate];const auto pose=world.transform(gate.id);
  if(pose&&world.is_enabled(gate.id)&&racing_gate_crossed(old,pos,*pose,gate.width,gate.height)){
   checkpoint=add(pose->position,multiply(rotate(pose->rotation,{0,1,0}),2));checkpoint.z=pos.z;checkpointYaw=heading(pose->rotation);
   log("Gate "+std::to_string(race.nextGate));
   if(race.nextGate==0){++race.completedLaps;race.lastLap=race.elapsed-race.lapStarted;race.lapStarted=race.elapsed;if(race.bestLap<=0||race.lastLap<race.bestLap)race.bestLap=race.lastLap;
    if(race.completedLaps>=race.targetLaps){hud.phase=PrototypePhase::Won;velocity={};log("Finished");}}
   race.nextGate=(race.nextGate+1)%unsigned(gates.size());
  }
 }
};
RacingGame::RacingGame(GameWorld& w):impl_(std::make_unique<Impl>(w)){}RacingGame::~RacingGame()=default;
const PrototypeHud& RacingGame::hud()const{return impl_->hud;}const RacingStatus& RacingGame::status()const{return impl_->race;}
const RacingTelemetry& RacingGame::telemetry()const{return impl_->telemetry;}
bool RacingGame::start(std::string* error){auto& s=*impl_;if(s.hud.active){if(error)*error="Racing runtime already started";return false;}std::vector<PrototypeObjectView> views;for(auto id:s.world.object_ids())views.push_back({id,"Object "+std::to_string(id),*s.world.components(id),s.world.geometry_kind(id)==GameGeometryKind::Marker});if(!validate_racing_scene(views,error))return false;
 std::map<unsigned,Impl::Gate> gates;
 for(const auto& o:views)for(const auto& c:o.components)if(c.enabled&&racing_type(c.type)){
  if(!s.world.is_enabled(o.id)){if(error)*error="Racing actors must be enabled";return false;}
  if(c.type==carType){s.car=o.id;s.config=c;s.carKey=get(c,"key",std::string{});}
  if(c.type==rulesType){s.rules=o.id;s.ruleConfig=c;s.rulesKey=get(c,"key",std::string{});}
  if(c.type==gateType)gates.emplace(unsigned(get(c,"order",std::int64_t{0})),Impl::Gate{o.id,float(get(c,"half_width",10.0)),float(get(c,"half_height",4.0))});
  if(c.type==surfaceType)s.surfaces.emplace(o.id,Impl::DrivingSurface{float(get(c,"grip_multiplier",1.0)),float(get(c,"rolling_drag_multiplier",1.0))});
 }
 if(s.world.has_collision(s.car).value_or(true)){if(error)*error="Arcade race car must have collision disabled; runtime owns the swept capsule";return false;}
 if(!configure_physics3d_gravity(s.world.physics(),{0,0,-9.81F})){if(error)*error="Racer requires configurable Z-up gravity";return false;}
 for(const auto& [n,g]:gates){(void)n;s.gates.push_back(g);}s.initial=s.checkpoint=*s.world.position(s.car);s.initialYaw=heading(s.world.transform(s.car)->rotation);
 Component state;state.type=stateType;state.properties["data"]=std::string{};auto* added=s.world.add_component(s.rules,std::move(state),error);if(!added)return false;s.state=added->id;s.hud.active=true;s.restart();s.update_hud();s.persist();return true;
}
void RacingGame::step(float dt){auto& s=*impl_;if(!s.hud.active||!std::isfinite(dt)||dt<=0||dt>.25F)return;
 auto edge=[&](const char* name,bool& last){const bool pressed=s.world.is_action_pressed(name);const bool fire=pressed&&!last;last=pressed;return fire;};
 if(edge("prototype_debug",s.lastDebug))s.hud.debug=!s.hud.debug;
 if(edge("restart",s.lastRestart)){s.restart();s.update_hud();s.persist();return;}
 if(edge("pause",s.lastPause)&&(s.hud.phase==PrototypePhase::Playing||s.hud.phase==PrototypePhase::Paused)){s.hud.phase=s.hud.phase==PrototypePhase::Paused?PrototypePhase::Playing:PrototypePhase::Paused;s.accumulator=0;}
 const bool recover=edge("crouch",s.lastRecovery),view=edge("interact",s.lastView);
 if(s.hud.phase!=PrototypePhase::Playing){s.update_hud();s.persist();return;}
 if(recover)s.recover();
 if(view){s.hood=!s.hood;s.update_camera(0,true);}
 s.accumulator+=dt;
 for(unsigned steps=0;s.accumulator+1e-9>=fixedStep&&steps<31;++steps){s.accumulator-=fixedStep;if(s.accumulator<0)s.accumulator=0;
  if(s.race.countdown>0)s.race.countdown=(std::max)(0.0,s.race.countdown-fixedStep);
  else {s.race.elapsed+=fixedStep;s.drive(float(fixedStep));}
  s.world.tick(float(fixedStep));s.update_camera(float(fixedStep));if(s.hud.phase==PrototypePhase::Won){s.accumulator=0;break;}
 }
 s.update_hud();s.persist();
}
PrototypeCamera RacingGame::camera()const{return impl_->smoothCamera;}
bool RacingGame::after_restore(std::string* error){auto& s=*impl_;auto fail=[&](){if(error)*error="Invalid or incompatible racing save state";return false;};
 GameObjectId car=0,rules=0;std::map<unsigned,Impl::Gate> gates;std::vector<PrototypeObjectView> views;
 for(auto id:s.world.object_ids()){const auto* cs=s.world.components(id);views.push_back({id,"Saved actor",*cs,s.world.geometry_kind(id)==GameGeometryKind::Marker});for(const auto& c:*cs)if(c.enabled){if(racing_type(c.type)&&!s.world.is_enabled(id))return fail();if(c.type==carType&&get(c,"key",std::string{})==s.carKey)car=id;if(c.type==rulesType&&get(c,"key",std::string{})==s.rulesKey)rules=id;if(c.type==gateType)gates.emplace(unsigned(get(c,"order",std::int64_t{0})),Impl::Gate{id,float(get(c,"half_width",10.0)),float(get(c,"half_height",4.0))});}}
 if(!validate_racing_scene(views,error)||!car||!rules||gates.size()!=s.gates.size()||s.world.has_collision(car).value_or(true))return fail();
 const auto* c=find_component_by_type(*s.world.components(rules),stateType);if(!c)return fail();const auto data=get(*c,"data",std::string{});if(data.size()>4096)return fail();std::istringstream in(data);in.imbue(std::locale::classic());std::string magic;unsigned version{},phase{};RacingStatus race=s.race;Float3 velocity{},checkpoint{};float yaw{},vertical{},checkpointYaw{};bool hood{},debug{};double accumulator{};
 in>>magic>>version>>phase>>yaw>>velocity.x>>velocity.y>>vertical>>race.elapsed>>race.lapStarted>>race.lastLap>>race.bestLap>>race.countdown>>race.completedLaps>>race.nextGate>>checkpoint.x>>checkpoint.y>>checkpoint.z>>checkpointYaw>>hood>>debug>>accumulator;
 if(!in||magic!="DVE_RACE"||version!=1||phase>2||!finite(velocity)||!finite(checkpoint)||!std::isfinite(yaw)||!std::isfinite(vertical)||!std::isfinite(checkpointYaw)||length(velocity)>101||std::abs(vertical)>100000||length(checkpoint)>1000000||race.completedLaps>race.targetLaps||race.nextGate>=gates.size()||!std::isfinite(accumulator)||accumulator<0||accumulator>=fixedStep)return fail();
 for(double n:{race.elapsed,race.lapStarted,race.lastLap,race.bestLap,race.countdown})if(!std::isfinite(n)||n<0||n>1e9)return fail();
 if(race.lapStarted>race.elapsed||race.countdown>30||(phase==2&&race.completedLaps!=race.targetLaps)||(phase!=2&&race.completedLaps==race.targetLaps))return fail();
 in>>std::ws;if(!in.eof())return fail();
 s.car=car;s.rules=rules;s.state=c->id;s.surfaces.clear();
 for(auto id:s.world.object_ids())for(const auto& component:*s.world.components(id))
  if(component.enabled&&component.type==surfaceType)
   s.surfaces.emplace(id,Impl::DrivingSurface{float(get(component,"grip_multiplier",1.0)),float(get(component,"rolling_drag_multiplier",1.0))});
 s.gates.clear();for(const auto& [n,g]:gates){(void)n;s.gates.push_back(g);}s.race=race;s.velocity=velocity;s.yaw=yaw;s.verticalSpeed=vertical;s.checkpoint=checkpoint;s.checkpointYaw=checkpointYaw;s.hood=hood;s.hud.debug=debug;s.accumulator=accumulator;s.hud.phase=PrototypePhase(phase);s.telemetry={};s.update_camera(0,true);s.update_hud();return true;
}
}
