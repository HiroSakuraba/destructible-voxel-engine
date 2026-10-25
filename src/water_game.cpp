#include "dve/water_game.hpp"
#include "dve/game_world.hpp"
#include "dve/physics3d_backend.hpp"
#include "dve/water_vessel_dynamics.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <vector>
namespace dve {
namespace {
constexpr std::string_view rulesType="dve.prototype.water_rules";
constexpr std::string_view oceanType="dve.prototype.water_ocean";
constexpr std::string_view vesselType="dve.prototype.water_vessel";
constexpr std::string_view stateType="dve.prototype.water_state";
constexpr float stepDt=1.F/120.F;
template<class T>T get(const Component& c,std::string_view name,T fallback){
 const auto it=c.properties.find(name);
 if(it!=c.properties.end())if(const T* x=std::get_if<T>(&it->second))return *x;
 return fallback;
}
bool finite(Float3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
bool finite(Quaternion q){return std::isfinite(q.x)&&std::isfinite(q.y)&&std::isfinite(q.z)&&std::isfinite(q.w);}
std::string fixed(float x){std::ostringstream out;out.imbue(std::locale::classic());out<<std::fixed<<std::setprecision(1)<<x;return out.str();}
bool is_role(std::string_view name){return name==rulesType||name==oceanType||name==vesselType;}
VesselDynamics propulsion_of(const Component& c){
 VesselDynamics v;
 v.type=WatercraftType(get(c,"craft_type",std::int64_t{0}));
 v.sailArea=float(get(c,"sail_area_m2",0.0));
 v.keelArea=float(get(c,"keel_area_m2",0.0));
 v.mastHeight=float(get(c,"mast_height_m",0.0));
 v.planingFactor=float(get(c,"planing_factor",0.0));
 v.thrustNewtons=float(get(c,"thrust_newtons",1400.0));
 v.rudderNewtons=float(get(c,"rudder_torque",380.0));
 return v;
}
WaterHull hull_of(const Component& c){
 WaterHull h;
 h.massKg=float(get(c,"mass_kg",480.0));h.displacementCubicMetres=float(get(c,"displacement_m3",.55));
 h.lengthMetres=float(get(c,"length_m",4.0));h.beamMetres=float(get(c,"beam_m",1.8));
 h.draftMetres=float(get(c,"draft_m",.8));h.linearDamping=float(get(c,"linear_damping",80.0));
 h.angularDamping=float(get(c,"angular_damping",160.0));
 h.longitudinalProbes=unsigned(get(c,"probes_length",std::int64_t{3}));
 h.lateralProbes=unsigned(get(c,"probes_beam",std::int64_t{3}));
 return h;
}
}
void register_water_components(ComponentTypeRegistry& registry){
 const auto add=[&](std::string id,std::string label,std::vector<ComponentPropertySchema> properties){
  std::string ignored;(void)registry.register_type({std::move(id),std::move(label),false,std::move(properties)},&ignored);
 };
 add(std::string(oceanType),"Ocean Water Field (marker)",{
  {"key",std::string{"ocean"},true,false,"Unique ocean key"},
  {"sea_state",std::int64_t{1},false,false,"0 calm, 1 breeze, 2 storm"},
  {"current_x",0.0,false,false,"Current X (metres/second)"},
  {"current_y",0.0,false,false,"Current Y (metres/second)"},
  {"wind_x",5.0,false,false,"Global ambient wind X (m/s)"},
  {"wind_y",8.0,false,false,"Global ambient wind Y (m/s)"}});
 add(std::string(rulesType),"Water Prototype Rules (marker)",{
  {"key",std::string{"water-rules"},true,false,"Unique water rules key"}});
 add(std::string(vesselType),"Water Vessel (collision-free polygon)",{
  {"key",std::string{"boat"},true,false,"Unique hull key"},
  {"mass_kg",480.0,false,false,"Mass in kilograms"},
  {"displacement_m3",.55,false,false,"Maximum displaced water in cubic metres"},
  {"length_m",4.0,false,false,"Hull length (m)"},
  {"beam_m",1.8,false,false,"Beam (m)"},
  {"draft_m",.8,false,false,"Draft (m)"},
  {"linear_damping",80.0,false,false,"Hull water resistance coefficient"},
  {"angular_damping",160.0,false,false,"Yaw/roll/pitch water damping"},
  {"probes_length",std::int64_t{3},false,false,"Longitudinal buoyancy points"},
  {"probes_beam",std::int64_t{3},false,false,"Lateral buoyancy points"},
  {"thrust_newtons",1400.0,false,false,"W1 demonstration motor force"},
  {"rudder_torque",380.0,false,false,"Rudder force/torque authority"},
  {"craft_type",std::int64_t{0},false,false,"0 dinghy, 1 sailboat, 2 pirate, 3 speedboat, 4 wave runner"},
  {"sail_area_m2",0.0,false,false,"Unreefed sail area square metres"},
  {"keel_area_m2",0.0,false,false,"Keel/centerboard lateral projected area"},
  {"mast_height_m",0.0,false,false,"Sail center of effort above hull COM"},
  {"planing_factor",0.0,false,false,"Dynamic planing lift coefficient 0..2"}});
 add(std::string(stateType),"Water Runtime Save",{
  {"data",std::string{},false,false,"Versioned runtime-only state"}});
}
bool scene_has_water(std::span<const PrototypeObjectView> objects){
 for(const auto& o:objects)for(const auto& c:o.components)if(c.enabled&&is_role(c.type))return true;
 return false;
}
bool validate_water_scene(std::span<const PrototypeObjectView> objects,std::string* error){
 const auto fail=[&](std::string reason){if(error)*error=std::move(reason);return false;};
 const auto registry=ComponentTypeRegistry::make_default();
 unsigned ocean=0,rules=0,vessels=0;
 std::set<std::string> keys;
 for(const auto& o:objects){
  unsigned count=0;
  for(const auto& c:o.components)if(c.enabled&&c.type.starts_with(kPrototypePrefix)){
   if(c.type==stateType)continue;
   if(!is_role(c.type))return fail("Cannot mix water game with other prototype types");
   if(++count>1)return fail("Only one water role per object");
   std::string problem;if(!registry.validate(c,&problem))return fail(o.name+": "+problem);
   const std::string key=get(c,"key",std::string{});
   if(key.empty()||key.size()>64||key.find_first_not_of(
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.")!=std::string::npos||
      !keys.insert(key).second)return fail("Water actor key missing, invalid or duplicated");
   if(c.type==oceanType){
    if(++ocean>1||!o.marker)return fail("Exactly one ocean marker is required");
    const auto state=get(c,"sea_state",std::int64_t{1});
    if(state<0||state>2)return fail("Ocean sea state outside 0..2");
    for(const char* n:{"current_x","current_y","wind_x","wind_y"}){
     const auto v=get(c,n,0.0);if(!std::isfinite(v)||std::abs(v)>25)
      return fail("Ocean current must be finite and within 25 m/s");
    }
   }else if(c.type==rulesType){
    if(++rules>1||!o.marker)return fail("Exactly one water rules marker is required");
   }else if(c.type==vesselType){
    if(++vessels>8||o.marker)return fail("W2 supports 1..8 visible vessels");
    if(!valid_water_hull(hull_of(c))||!valid_vessel_dynamics(propulsion_of(c)))return fail("Invalid vessel mass, propulsion, sails, planing or probes");
    for(const auto* n:{"thrust_newtons","rudder_torque"}){
     const auto v=get(c,n,0.0);if(!std::isfinite(v)||v<0||v>1.e7)
      return fail("Invalid vessel thrust or rudder torque");
    }
   }
  }
 }
 if(ocean!=1||rules!=1||vessels==0||vessels>8)return fail("Water prototype requires one ocean, one rules marker and 1..8 vessels");
 return true;
}
struct WaterGame::Impl {
 struct Boat {
  GameObjectId id{};std::string key;Component component{};
  WaterHull hull{};WaterRigidState state{},origin{};
  VesselDynamics drive{};
  float trim{0.6F},wakeCooldown{};
  WaterHydroLoads loads{};
  VesselDriveLoads propLoads{};
 };
 GameWorld& world;OceanField field{};
 Float3 wind{5,8,0};
 PrototypeHud hud{};PrototypeCamera camera{};
 WaterGameTelemetry telemetry{};
 std::vector<Boat> boats;
 std::vector<WaterSprayEffect> spray;
 GameObjectId rules{};ComponentId stateComponent{};
 std::string rulesKey,oceanKey;
 unsigned selected{};
 double elapsed{};
 float accumulator{};
 bool active{},lastPause{},lastReset{},lastSwitch{},lastDebug{};
 explicit Impl(GameWorld& w):world(w){hud.active=false;hud.health=hud.maximumHealth=100;hud.water=true;}
 void publish(Boat& b){(void)world.set_position(b.id,b.state.position);(void)world.set_rotation(b.id,b.state.orientation);}
 void reset(){
  for(auto& b:boats){b.state=b.origin;b.trim=0.6F;b.wakeCooldown=0;publish(b);}
  field.clear_wakes();spray.clear();
  selected=0;elapsed=0;accumulator=0;hud.phase=PrototypePhase::Playing;
  update_camera(true);update_hud();
 }
 void update_camera(bool snap){
  const auto& s=boats[selected].state;
  const Float3 forward=rotate(s.orientation,{0,1,0});
  const Float3 target=add(s.position,multiply(forward,4.F));
  const float follow=(std::max)(14.F,boats[selected].hull.lengthMetres*1.6F);
  const Float3 wanted=add(add(s.position,multiply(forward,-follow)),Float3{0,0,follow*.48F});
  const float a=snap?1.F:1.F-std::exp(-8.F*stepDt);
  camera.position=add(camera.position,multiply(subtract(wanted,camera.position),a));
  camera.target=add(camera.target,multiply(subtract(target,camera.target),a));
  camera.up={0,0,1};
 }
 void advance(float dt){
  field.prune_wakes(elapsed);
  for(auto& fx:spray){
   fx.age+=dt;
   fx.position=add(fx.position,multiply(fx.velocity,dt));
   fx.velocity.z-=9.81F*dt;
  }
  std::erase_if(spray,[&](const WaterSprayEffect& fx){
    if(fx.age>=fx.lifetime)return true;
    const auto surface=field.sample(fx.position.x,fx.position.y,elapsed);
    return fx.age>.08F&&fx.position.z<surface.height;
  });
  const float throttle=std::clamp(world.get_axis("move_y"),-1.F,1.F);
  const float steer=std::clamp(world.get_axis("move_x"),-1.F,1.F);
  const float trimAxis=std::clamp(world.get_axis("water_trim"),-1.F,1.F);
  for(unsigned i=0;i<boats.size();++i){
   auto& boat=boats[i];const auto old=boat.state.position;
   const Float3 f=rotate(boat.state.orientation,{0,1,0});
   if(i==selected)boat.trim=std::clamp(boat.trim+trimAxis*dt*.65F,-1.4F,1.4F);
   const auto waves=field.sample(boat.state.position.x,boat.state.position.y,elapsed);
   boat.loads=evaluate_water_hull(field,elapsed,boat.hull,boat.state);
   VesselInput input;
   input.throttle=i==selected?throttle:0;
   input.helm=i==selected?steer:0;
   input.sailTrim=boat.trim;
   boat.propLoads=evaluate_vessel_dynamics(boat.drive,boat.hull,boat.state,waves,
                                               wind,input,boat.loads.immersionFraction);
   step_water_hull(boat.state,boat.hull,boat.loads,dt,
                   boat.propLoads.force,boat.propLoads.torque);
   if(!finite(boat.state.position)||!finite(boat.state.linearVelocity)||
      !finite(boat.state.orientation)||length(boat.state.position)>100000.F){
     boat.state=boat.origin;
   }
   // The ocean is not a collision volume. Hulls still hit conventional land.
   const Float3 displacement=subtract(boat.state.position,old);
   const float half=boat.hull.lengthMetres*.32F;
   const Capsule body{add(old,multiply(f,-half)),add(old,multiply(f,half)),
                      boat.hull.beamMetres*.32F};
   if(const auto hit=world.capsule_sweep(body,displacement,boat.id)){
     boat.state.position=add(old,multiply(displacement,
               (std::max)(0.F,hit->time-.002F/(std::max)(length(displacement),.002F))));
     const float v=dot(boat.state.linearVelocity,hit->worldNormal);
     if(v<0)boat.state.linearVelocity=subtract(boat.state.linearVelocity,multiply(hit->worldNormal,1.05F*v));
   }
   publish(boat);
  }
  // Evaluate all boats against the *previous* wake state before emitting new
  // disturbances; this avoids source-order-dependent same-tick reactions.
  for(auto& boat:boats){
   const Float3 forward=rotate(boat.state.orientation,{0,1,0});
   const auto water=field.sample(boat.state.position.x,boat.state.position.y,elapsed);
   const Float3 delta=subtract(boat.state.linearVelocity,water.waterVelocity);
   const float forwardSpeed=dot(delta,forward);
   const float speed=std::abs(forwardSpeed);
   if(speed<2.2F||boat.loads.immersionFraction<.12F){
     boat.wakeCooldown=0;continue;
   }
   boat.wakeCooldown+=dt;
   if(boat.wakeCooldown<.16F)continue;
   boat.wakeCooldown-=.16F;
   WaterWakeEvent wake{};
   // Start the propagating wake behind the stern. This avoids feeding the
   // emitter's own stern probes an artificial buoyancy pulse every 0.16 s.
   const float trailing=boat.hull.lengthMetres*.48F+
                       std::clamp(boat.hull.beamMetres*1.6F,1.0F,12.0F);
   wake.position={boat.state.position.x-forward.x*trailing,
                  boat.state.position.y-forward.y*trailing};
   const float sign=forwardSpeed<0?-1.F:1.F;
   wake.direction={forward.x*sign,forward.y*sign};
   wake.bornSeconds=elapsed+dt;
   wake.amplitude=std::clamp((speed*speed*.009F+boat.loads.impactRate*.03F)*
                             std::sqrt(boat.hull.beamMetres/2.F),.03F,1.45F);
   wake.width=std::clamp(boat.hull.beamMetres*.65F,.7F,10.F);
   wake.wavelength=std::clamp(boat.hull.lengthMetres*.45F,1.1F,16.F);
   wake.speed=std::clamp(1.2F+speed*.18F,1.2F,5.5F);
   wake.lifetime=6.F;
   (void)field.emit_wake(wake);
   // Spray is a visual-only ballistic by-product; it does not feed back into
   // buoyancy and is therefore safely rebuilt after a save restore.
   if(spray.size()<160&&speed>3.5F){
     const auto bow=add(boat.state.position,
          multiply(forward,boat.hull.lengthMetres*.43F));
     for(int side:{-1,1}){
       WaterSprayEffect fx;
       const auto right=rotate(boat.state.orientation,{1,0,0});
       fx.position=add(bow,add(multiply(right,float(side)*boat.hull.beamMetres*.35F),
                                Float3{0,0,.1F}));
       fx.velocity=add(multiply(forward,speed*.15F),
                  add(multiply(right,float(side)*(1.6F+speed*.15F)),
                      Float3{0,0,1.F+speed*.27F}));
       fx.radius=std::clamp(.10F+speed*.015F,.12F,.55F);
       fx.lifetime=std::clamp(.35F+speed*.017F,.4F,1.0F);
       fx.opacity=.72F;
       spray.push_back(fx);
     }
   }
  }
  elapsed+=dt;update_camera(false);
 }
 void update_hud(){
  auto& b=boats[selected];
  const auto e=quaternion_to_euler_xyz(b.state.orientation);
  const auto water=field.sample(b.state.position.x,b.state.position.y,elapsed);
  telemetry.speed=length(b.state.linearVelocity);telemetry.waterHeight=water.height;
  telemetry.immersion=b.loads.immersionFraction;
  telemetry.rollRadians=e.y;telemetry.pitchRadians=e.x;
  telemetry.impactRate=b.loads.impactRate;
  telemetry.apparentWind=b.propLoads.apparentWind;
  telemetry.sailDrive=b.propLoads.sailThrust;
  telemetry.planingLift=b.propLoads.planingLift;
  telemetry.lateralSlip=b.propLoads.lateralWaterSpeed;
  telemetry.sailTrim=b.trim;
  telemetry.localFoam=water.foam;telemetry.activeWakes=unsigned(field.wakes().size());
  telemetry.wakeHeight=field.wakes().empty()?0.F:water.height;
  telemetry.type=b.drive.type;
  telemetry.activeVessel=selected;telemetry.vesselCount=unsigned(boats.size());
  hud.stats=std::string("WATER ")+watercraft_name(b.drive.type)+
      " SEA "+(field.preset()==OceanPreset::Calm?"CALM":field.preset()==OceanPreset::Breeze?"BREEZE":"STORM");
  hud.prompt="SPEED "+fixed(telemetry.speed)+" M/S ROLL "+fixed(e.y)+" PITCH "+fixed(e.x);
  hud.message="IMMERSION "+fixed(100*telemetry.immersion)+"% WATER "+fixed(water.height)+
    " M  1..5 BOATS  Q/E TRIM";
  if(hud.phase==PrototypePhase::Paused)hud.message="PAUSED - P RESUME";
  hud.vehicleTelemetry="WIND "+fixed(telemetry.apparentWind)+" SAIL "+fixed(telemetry.sailDrive)+
    " PLANING "+fixed(telemetry.planingLift)+" TRIM "+fixed(telemetry.sailTrim)+
    " SLIP "+fixed(telemetry.lateralSlip)+" WAKES "+std::to_string(telemetry.activeWakes)+
    " FOAM "+fixed(telemetry.localFoam);
 }
 void persist(){
  std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(17);
  o<<"DVE_WATER 3 "<<selected<<' '<<unsigned(hud.phase)<<' '<<elapsed<<' '<<accumulator<<' '<<boats.size();
  for(const auto& b:boats){
   const auto& s=b.state;
   o<<' '<<b.key<<' '<<b.trim<<' '<<b.wakeCooldown;
   o<<' '<<s.position.x<<' '<<s.position.y<<' '<<s.position.z
    <<' '<<s.orientation.x<<' '<<s.orientation.y<<' '<<s.orientation.z<<' '<<s.orientation.w
    <<' '<<s.linearVelocity.x<<' '<<s.linearVelocity.y<<' '<<s.linearVelocity.z
    <<' '<<s.angularVelocity.x<<' '<<s.angularVelocity.y<<' '<<s.angularVelocity.z;
  }
  const auto wakes=field.wakes();
  o<<' '<<wakes.size();
  for(const auto& w:wakes){
   o<<' '<<w.position.x<<' '<<w.position.y<<' '<<w.direction.x<<' '<<w.direction.y
    <<' '<<w.bornSeconds<<' '<<w.amplitude<<' '<<w.wavelength<<' '<<w.width
    <<' '<<w.speed<<' '<<w.lifetime;
  }
  (void)world.set_component_property(rules,stateComponent,"data",o.str());
 }
};
WaterGame::WaterGame(GameWorld& world):impl_(std::make_unique<Impl>(world)){}
WaterGame::~WaterGame()=default;
const PrototypeHud& WaterGame::hud()const noexcept{return impl_->hud;}
PrototypeCamera WaterGame::camera()const noexcept{return impl_->camera;}
const OceanField& WaterGame::ocean()const noexcept{return impl_->field;}
double WaterGame::ocean_time()const noexcept{return impl_->elapsed;}
std::span<const WaterSprayEffect> WaterGame::spray_effects()const noexcept{
 return impl_->spray;
}
const WaterGameTelemetry& WaterGame::telemetry()const noexcept{return impl_->telemetry;}
bool WaterGame::start(std::string* error){
 auto& s=*impl_;if(s.active)return false;
 std::vector<PrototypeObjectView> views;
 for(auto id:s.world.object_ids())views.push_back({id,s.world.name_of(id),*s.world.components(id),
       s.world.geometry_kind(id)==GameGeometryKind::Marker});
 if(!validate_water_scene(views,error))return false;
 for(const auto& o:views)for(const auto& c:o.components)if(c.enabled&&is_role(c.type)){
  if(!s.world.is_enabled(o.id)){if(error)*error="Water roles must be enabled";return false;}
  if(c.type==oceanType){
   s.oceanKey=get(c,"key",std::string{});
   s.wind={float(get(c,"wind_x",5.0)),float(get(c,"wind_y",8.0)),0};
   if(!s.field.set_preset(OceanPreset(get(c,"sea_state",std::int64_t{1})),
          {float(get(c,"current_x",0.0)),float(get(c,"current_y",0.0)),0})){
    if(error)*error="Invalid ocean wave preset";return false;
   }
  }else if(c.type==rulesType){s.rules=o.id;s.rulesKey=get(c,"key",std::string{});}
  else if(c.type==vesselType){
   if(s.world.has_collision(o.id).value_or(true)){
    if(error)*error="W1 vessels require visual-only geometry; water physics owns hull motion";return false;
   }
   Impl::Boat boat;boat.id=o.id;boat.key=get(c,"key",std::string{});boat.component=c;
   boat.hull=hull_of(c);boat.drive=propulsion_of(c);
   boat.origin.position=*s.world.position(o.id);boat.origin.orientation=s.world.transform(o.id)->rotation;
   boat.state=boat.origin;s.boats.push_back(boat);
  }
 }
 std::sort(s.boats.begin(),s.boats.end(),[](const auto& a,const auto& b){return a.key<b.key;});
 if(!configure_physics3d_gravity(s.world.physics(),{0,0,-9.81F})){
  if(error)*error="Water prototype requires Z-up gravity";return false;
 }
 Component state;state.type=std::string(stateType);state.properties["data"]=std::string{};
 auto* c=s.world.add_component(s.rules,std::move(state),error);
 if(!c)return false;s.stateComponent=c->id;
 s.active=s.hud.active=true;s.reset();s.persist();return true;
}
void WaterGame::step(float dt){
 auto& s=*impl_;if(!s.active||!std::isfinite(dt)||dt<=0||dt>.25F)return;
 const auto edge=[&](const char* key,bool& old){const bool now=s.world.is_action_pressed(key);
   const bool pressed=now&&!old;old=now;return pressed;};
 if(edge("restart",s.lastReset)){s.reset();s.persist();return;}
 if(edge("prototype_debug",s.lastDebug))s.hud.debug=!s.hud.debug;
 if(edge("pause",s.lastPause))s.hud.phase=s.hud.phase==PrototypePhase::Paused?
     PrototypePhase::Playing:PrototypePhase::Paused;
 const bool toggle=edge("interact",s.lastSwitch);
 unsigned selection=s.selected;
 if(toggle)selection=(selection+1)%unsigned(s.boats.size());
 for(unsigned n=0;n<s.boats.size();++n)
   if(s.world.is_action_pressed("water_ship_"+std::to_string(n+1)))selection=n;
 if(selection!=s.selected){s.selected=selection;s.update_camera(true);}
 if(s.hud.phase==PrototypePhase::Playing){
  s.accumulator+=dt;
  for(unsigned i=0;i<31&&s.accumulator+1.e-7F>=stepDt;++i){
    s.accumulator-=stepDt;if(s.accumulator<0)s.accumulator=0;
    s.advance(stepDt);s.world.tick(stepDt);
  }
 }
 s.update_hud();s.persist();
}
bool WaterGame::after_restore(std::string* error){
 auto& s=*impl_;
 const auto fail=[&](){if(error)*error="Invalid or incomplete DVE_WATER 1/2/3 state";return false;};
 std::map<std::string,GameObjectId> vesselIds;GameObjectId rules=0;
 const Component* oceanConfig=nullptr;
 for(auto id:s.world.object_ids())if(const auto* cs=s.world.components(id)){
  for(const auto& c:*cs)if(c.enabled){
   if(c.type==rulesType&&get(c,"key",std::string{})==s.rulesKey)rules=id;
   if(c.type==oceanType&&get(c,"key",std::string{})==s.oceanKey)oceanConfig=&c;
   if(c.type==vesselType)vesselIds[get(c,"key",std::string{})]=id;
  }
 }
 if(!rules||!oceanConfig||vesselIds.size()!=s.boats.size())return fail();
 const auto* c=find_component_by_type(*s.world.components(rules),stateType);
 if(!c)return fail();
 const auto data=get(*c,"data",std::string{});
 if(data.size()>32768)return fail();
 std::istringstream in(data);in.imbue(std::locale::classic());
 std::string magic;unsigned version{},selected{},phase{};double elapsed{};
 float accumulator{};
 in>>magic>>version>>selected>>phase>>elapsed>>accumulator;
 if(!in||magic!="DVE_WATER"||(version<1||version>3)||selected>=s.boats.size()||phase>1||
    !std::isfinite(elapsed)||elapsed<0||!std::isfinite(accumulator)||
    accumulator<0||accumulator>=stepDt)return fail();
 unsigned count=unsigned(s.boats.size());
 if(version>=2){in>>count;if(!in||count!=s.boats.size())return fail();}
 std::vector<WaterRigidState> restored(s.boats.size());
 std::vector<float> trims(s.boats.size(),.6F),wakeTimers(s.boats.size(),0.F);
 for(unsigned i=0;i<restored.size();++i){
   auto& q=restored[i];
   if(version>=2){
     std::string key;float trim{},timer{};
     in>>key>>trim;
     if(version==3)in>>timer;
     if(!in||key!=s.boats[i].key||!std::isfinite(trim)||std::abs(trim)>1.4F||
        !std::isfinite(timer)||timer<0||timer>.161F)return fail();
     trims[i]=trim;wakeTimers[i]=timer;
   }
   in>>q.position.x>>q.position.y>>q.position.z>>q.orientation.x>>q.orientation.y
      >>q.orientation.z>>q.orientation.w>>q.linearVelocity.x>>q.linearVelocity.y
      >>q.linearVelocity.z>>q.angularVelocity.x>>q.angularVelocity.y>>q.angularVelocity.z;
   const auto& rot=q.orientation;
   const float len=rot.x*rot.x+rot.y*rot.y+rot.z*rot.z+rot.w*rot.w;
   if(!in||!finite(q.position)||!finite(q.orientation)||!finite(q.linearVelocity)||
      !finite(q.angularVelocity)||std::abs(len-1.F)>.03F||
      length(q.position)>100000||length(q.linearVelocity)>120||length(q.angularVelocity)>4.1F)return fail();
 }
 std::vector<WaterWakeEvent> wakeEvents;
 if(version==3){
   unsigned n=0;in>>n;
   if(!in||n>WaterWakeField::MaxEvents)return fail();
   for(unsigned i=0;i<n;++i){
     WaterWakeEvent w;
     in>>w.position.x>>w.position.y>>w.direction.x>>w.direction.y
       >>w.bornSeconds>>w.amplitude>>w.wavelength>>w.width
       >>w.speed>>w.lifetime;
     if(!in||!valid_water_wake(w)||w.bornSeconds>elapsed+1.e-4||
         elapsed-w.bornSeconds>=w.lifetime)return fail();
     wakeEvents.push_back(w);
   }
 }
 in>>std::ws;if(!in.eof())return fail();
 const auto state=OceanPreset(get(*oceanConfig,"sea_state",std::int64_t{1}));
 if(!s.field.set_preset(state,{float(get(*oceanConfig,"current_x",0.0)),
                                  float(get(*oceanConfig,"current_y",0.0)),0}))return fail();
 for(unsigned i=0;i<s.boats.size();++i){
  auto found=vesselIds.find(s.boats[i].key);
  if(found==vesselIds.end()||s.world.has_collision(found->second).value_or(true))return fail();
  s.boats[i].id=found->second;s.boats[i].state=restored[i];
  s.boats[i].trim=trims[i];s.boats[i].wakeCooldown=wakeTimers[i];
 }
 s.wind={float(get(*oceanConfig,"wind_x",5.0)),float(get(*oceanConfig,"wind_y",8.0)),0};
 if(!s.field.restore_wakes(wakeEvents,elapsed))return fail();
 s.spray.clear();
 s.rules=rules;s.stateComponent=c->id;s.selected=selected;s.elapsed=elapsed;
 s.accumulator=accumulator;s.hud.phase=PrototypePhase(phase);
 s.update_camera(true);s.update_hud();return true;
}
}
