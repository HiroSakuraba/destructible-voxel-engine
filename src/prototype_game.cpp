#include "dve/prototype_game.hpp"
#include "dve/racing_game.hpp"
#include "dve/flying_game.hpp"
#include "dve/water_game.hpp"
#include "dve/game_world.hpp"
#include "dve/gameplay_runtime.hpp"
#include "dve/physics3d_backend.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <iomanip>
#include <locale>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
namespace dve {
namespace {
std::string role(std::string_view s){return std::string(kPrototypePrefix)+std::string(s);}
template<class T>T get(const Component& c,std::string_view k,T fallback){auto i=c.properties.find(k);
 if(i!=c.properties.end())if(auto p=std::get_if<T>(&i->second))return *p;
 return fallback;}
bool finite(Float3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
struct Inbox {std::vector<TriggerEvent> events;
 bool overflow{};};
}
struct PrototypeGame::Impl {
 struct Actor {GameObjectId id{};
 Component config;
 bool initialEnabled{};
 double nextDamage{};};
 struct Link {std::string source,event,target,action;
 float amount{};};
 GameWorld& world;PrototypeHud hud;std::unique_ptr<RacingGame> racing;std::unique_ptr<FlyingGame> flying;std::unique_ptr<WaterGame> water;
 std::map<std::string,Actor,std::less<>> actors;
 std::vector<Link> links;
 std::map<GameTriggerId,std::string> triggers;
 std::shared_ptr<Inbox> inbox=std::make_shared<Inbox>();
 std::deque<std::pair<std::string,std::string>> events;
 std::set<std::string> picked;
 std::string playerKey,rulesKey;GameObjectId pawn{};GamePlayerId player{};ComponentId stateComponent{};
 Float3 initial{},checkpoint{};
 float yaw{},initialYaw{},pitch{},lookYaw{},lookPitch{},lookSpeed{2.2F},height{1.8F};
 bool lastInteract{},lastJump{},lastPause{},lastRestart{},lastDebug{},started{};
 explicit Impl(GameWorld& w):world(w){}
 void log(std::string s){if(hud.trace.size()>=64)hud.trace.erase(hud.trace.begin());hud.trace.push_back(std::move(s));}
 void emit(const std::string& source,std::string event){if(events.size()>=512){hud.phase=PrototypePhase::Lost;hud.message="Event queue limit exceeded";
 return;}log(source+"."+event);events.emplace_back(source,std::move(event));}
 void persist(){
  if(!started)return;
 std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<"DVE_PROTOTYPE 1 "<<unsigned(hud.phase)<<' '<<hud.health<<' '<<hud.collected<<' '<<yaw<<' '<<pitch<<' '<<checkpoint.x<<' '<<checkpoint.y<<' '<<checkpoint.z<<' '<<hud.debug<<' '<<std::quoted(hud.prompt)<<' '<<std::quoted(hud.message)<<' '<<hud.trace.size();
 for(const auto& text:hud.trace)out<<' '<<std::quoted(text);
 unsigned volumes=0;
 for(const auto& [key,a]:actors){(void)key;
 if(a.config.type==role("damage_volume"))++volumes;}out<<' '<<volumes;
 for(const auto& [key,a]:actors)if(a.config.type==role("damage_volume"))out<<' '<<std::quoted(key)<<' '<<a.nextDamage;
  (void)world.set_component_property(actors.at(rulesKey).id,stateComponent,"data",out.str());
 }
 void respawn(){
  (void)world.set_position(pawn,checkpoint);
 if(auto c=world.gameplay().character(pawn)){c->velocity={};
 c->grounded=false;
 c->supportObject=0;
 c->coyoteRemainingSeconds=0;
 c->jumpBufferRemainingSeconds=0;
 c->stance=CharacterStance::Standing;}
  hud.health=hud.maximumHealth;hud.phase=PrototypePhase::Playing;hud.message="Checkpoint restored";emit(playerKey,"respawned");
 }
 void damage(float amount){if(hud.phase!=PrototypePhase::Playing||amount<=0)return;hud.health=(std::max)(0.F,hud.health-amount);
 if(hud.health==0){emit(playerKey,"died");
 if(get(actors.at(rulesKey).config,"auto_respawn",true))respawn();else {hud.phase=PrototypePhase::Lost;hud.message="You lost - R to restart";emit(rulesKey,"lost");}}}
 void restart(){
  events.clear();inbox->events.clear();picked.clear();checkpoint=initial;yaw=initialYaw;pitch=lookYaw=lookPitch=0;hud.collected=0;hud.phase=PrototypePhase::Playing;
  for(auto& [key,a]:actors){(void)key;(void)world.set_enabled(a.id,a.initialEnabled);a.nextDamage=0;}
  for(const auto& [id,key]:triggers)(void)world.gameplay().set_trigger_enabled(id,actors.at(key).initialEnabled);
  respawn();hud.message="New round";emit(rulesKey,"restarted");
 }
 void action(Actor& target,const Link& l){
  log(l.source+"."+l.event+" -> "+l.target+"."+l.action);
  if(target.config.type==role("door")){
   bool open=!world.is_enabled(target.id);
 bool desired=l.action=="open"?true:l.action=="close"?false:!open;
   if(open!=desired){(void)world.set_enabled(target.id,!desired);emit(l.target,desired?"opened":"closed");}
  }else if(target.config.type==role("rules")){
   if(l.action=="restart")restart();else if(l.action=="win"){
    if(hud.collected<hud.required){hud.message="Collect "+std::to_string(hud.required)+" items first";
 return;}
    if(hud.phase==PrototypePhase::Playing){hud.phase=PrototypePhase::Won;hud.message="You win - R to restart";emit(rulesKey,"won");}
   }else if(hud.phase==PrototypePhase::Playing){hud.phase=PrototypePhase::Lost;hud.message="You lost - R to restart";emit(rulesKey,"lost");}
  }else if(target.config.type==role("player_start")){
   if(l.action=="respawn")respawn();else if(l.action=="damage")damage(l.amount);else hud.health=(std::min)(hud.maximumHealth,hud.health+l.amount);
  }else (void)world.set_enabled(target.id,l.action=="enable");
 }
 void drain(){unsigned budget=256;while(!events.empty()&&budget--){auto e=std::move(events.front());events.pop_front();
 for(const auto& l:links)if(l.source==e.first&&l.event==e.second){action(actors.at(l.target),l);}}
  if(!events.empty()){events.clear();hud.phase=PrototypePhase::Lost;hud.message="Event cycle: 256-event budget exceeded";log(hud.message);}
 }
 PrototypeCamera camera()const{
  PrototypeCamera c;
 auto p=world.position(pawn).value_or(initial);
 float h=height;
 if(auto state=world.gameplay().character(pawn);state&&state->stance==CharacterStance::Crouched)h*=.64F;
  c.position=add(p,{0,0,h*.9F});Float3 forward{std::sin(yaw)*std::cos(pitch),std::cos(yaw)*std::cos(pitch),std::sin(pitch)};c.target=add(c.position,forward);
 return c;
 }
 void interact(){
  hud.prompt.clear();
 const auto c=camera();
 const auto forward=normalize(subtract(c.target,c.position));
 float best=1e30F;Actor* selected=nullptr;
 std::string key;
  for(auto& [k,a]:actors)if(a.config.type==role("switch")&&world.is_enabled(a.id)){
   const auto pos=world.position(a.id);
 if(!pos)continue;
 const auto d=subtract(*pos,c.position);
 const float distance=length(d);
 if(distance>get(a.config,"reach",2.0)||distance<1e-5F||dot(multiply(d,1/distance),forward)<.65F||distance>=best)continue;
   auto hit=world.raycast(c.position,multiply(d,1/distance),distance);
 if(hit&&hit->objectId!=a.id&&hit->objectId!=pawn&&hit->distance<distance-.05F)continue;
   best=distance;selected=&a;key=k;
  }
  const bool pressed=world.is_action_pressed("interact");
 if(selected){hud.prompt=get(selected->config,"prompt",std::string{"E: use switch"});
 if(pressed&&!lastInteract)emit(key,"used");}lastInteract=pressed;
 }
};
PrototypeGame::PrototypeGame(GameWorld& w):impl_(std::make_unique<Impl>(w)){}
PrototypeGame::~PrototypeGame()=default;
bool PrototypeGame::active()const noexcept{return impl_->water?impl_->water->hud().active:impl_->flying?impl_->flying->hud().active:impl_->racing?impl_->racing->hud().active:impl_->hud.active;}
const PrototypeHud& PrototypeGame::hud()const noexcept{return impl_->water?impl_->water->hud():impl_->flying?impl_->flying->hud():impl_->racing?impl_->racing->hud():impl_->hud;}
PrototypeCamera PrototypeGame::camera()const{return impl_->water?impl_->water->camera():impl_->flying?impl_->flying->camera():impl_->racing?impl_->racing->camera():impl_->camera();}
const OceanField* PrototypeGame::ocean_field()const noexcept{return impl_->water?&impl_->water->ocean():nullptr;}
double PrototypeGame::ocean_time()const noexcept{return impl_->water?impl_->water->ocean_time():0;}
std::span<const RacingVisualEffect> PrototypeGame::racing_visual_effects()const noexcept {
 return impl_->racing?impl_->racing->visuals():std::span<const RacingVisualEffect>{};
}
void PrototypeGame::look(float x,float y){if(impl_->racing||impl_->flying||impl_->water)return;if(std::isfinite(x)&&std::isfinite(y)&&impl_->hud.phase==PrototypePhase::Playing){impl_->lookYaw+=std::clamp(x,-1.F,1.F);impl_->lookPitch+=std::clamp(y,-1.F,1.F);}}
bool PrototypeGame::start(std::string* error){
 auto& s=*impl_;
 if(s.started){if(error)*error="Prototype runtime already started";
 return false;}
 std::vector<PrototypeObjectView> views;
 for(auto id:s.world.object_ids())views.push_back({id,"Object "+std::to_string(id),*s.world.components(id),s.world.geometry_kind(id)==GameGeometryKind::Marker});
 if(!validate_prototype_scene(views,error))return false;
 if(scene_has_water(views)){s.water=std::make_unique<WaterGame>(s.world);if(!s.water->start(error)){s.water.reset();return false;}s.started=true;return true;}
 if(scene_has_flying(views)){s.flying=std::make_unique<FlyingGame>(s.world);if(!s.flying->start(error)){s.flying.reset();return false;}s.started=true;return true;}
 if(scene_has_racing(views)){s.racing=std::make_unique<RacingGame>(s.world);if(!s.racing->start(error)){s.racing.reset();return false;}s.started=true;return true;}
 for(const auto& o:views){const auto key=prototype_object_key(o.components);
 if(key.empty())continue;
 for(const auto& c:o.components)if(c.enabled&&c.type.starts_with(kPrototypePrefix)&&c.type!=role("link")&&c.type!=role("state")){
   s.actors.emplace(key,Impl::Actor{o.id,c,s.world.is_enabled(o.id),0});
 if(c.type==role("player_start"))s.playerKey=key;else if(c.type==role("rules"))s.rulesKey=key;
  }for(const auto& c:o.components)if(c.enabled&&c.type==role("link"))s.links.push_back({key,get(c,"event",std::string{}),get(c,"target",std::string{}),get(c,"action",std::string{}),float(get(c,"amount",10.0))});
 }
 if(s.actors.empty()){s.started=true;
 return true;}
 auto fail=[&](std::string why){if(error)*error=std::move(why);
 return false;};
 s.pawn=s.actors.at(s.playerKey).id;
 if(!s.world.is_enabled(s.pawn)||!s.world.is_enabled(s.actors.at(s.rulesKey).id))return fail("Player Start and Game Rules must be enabled");
 const auto& config=s.actors.at(s.playerKey).config;
 CharacterControllerConfig cfg;cfg.maximumGroundSpeedMetersPerSecond=float(get(config,"speed",5.5));cfg.jumpSpeedMetersPerSecond=float(get(config,"jump_speed",7.0));cfg.standingHeightMeters=float(get(config,"height",1.8));cfg.crouchedHeightMeters=cfg.standingHeightMeters*.64F;cfg.radiusMeters=float(get(config,"radius",.35));
 if(!configure_physics3d_gravity(s.world.physics(),{0,0,-9.81F}))return fail("Prototype kit requires a physics backend with configurable Z-up gravity");
 if(!s.world.gameplay().add_character(s.pawn,cfg,error))return false;
 s.player=s.world.gameplay().create_player("Prototype Player");
 if(!s.world.gameplay().possess(s.player,s.pawn,error))return false;
 s.height=cfg.standingHeightMeters;
 s.lookSpeed=float(get(config,"look_speed",2.2));
 s.hud.maximumHealth=s.hud.health=float(get(config,"health",100.0));
 s.hud.required=unsigned(get(s.actors.at(s.rulesKey).config,"required_items",std::int64_t{3}));
 s.initial=s.checkpoint=s.world.position(s.pawn).value();
 const auto f=rotate(s.world.transform(s.pawn)->rotation,{0,1,0});
 s.yaw=s.initialYaw=std::atan2(f.x,f.y);
 const std::weak_ptr<Inbox> weak=s.inbox;
 s.world.gameplay().on_trigger([weak](const TriggerEvent& e){if(auto p=weak.lock()){if(p->events.size()<4096)p->events.push_back(e);else p->overflow=true;}});
 for(const auto& [key,a]:s.actors)if(a.config.type==role("pickup")||a.config.type==role("checkpoint")||a.config.type==role("damage_volume")||a.config.type==role("goal")){
  TriggerVolumeDesc t;t.name="prototype:"+key;t.transform=*s.world.transform(a.id);t.enabled=a.initialEnabled;
  t.shape=(a.config.type==role("pickup")||a.config.type==role("checkpoint"))?TriggerShape::Sphere:TriggerShape::Box;t.radiusMeters=float(get(a.config,"radius",.5));t.halfExtents=get(a.config,"half_extents",Float3{.5F,.5F,.5F});
  auto id=s.world.gameplay().create_trigger(t,error);
 if(!id)return false;
 s.triggers.emplace(id,key);
 }
 Component state;state.type=role("state");state.properties["data"]=std::string{};
 auto* added=s.world.add_component(s.actors.at(s.rulesKey).id,std::move(state),error);
 if(!added)return false;
 s.stateComponent=added->id;
 s.hud.active=s.started=true;
 s.hud.message="Collect items, use the switch and reach the goal";
 s.persist();
 return true;
}
void PrototypeGame::step(float dt){
 auto& s=*impl_;
 if(s.water){s.water->step(dt);return;}
 if(s.flying){s.flying->step(dt);return;}
 if(s.racing){s.racing->step(dt);return;}
 if(!s.hud.active){s.world.tick(dt);
 return;}if(!std::isfinite(dt)||dt<=0||dt>.25F)return;
 auto edge=[&](const char* name,bool& held){bool now=s.world.is_action_pressed(name);
 bool fire=now&&!held;held=now;
 return fire;};
 if(edge("prototype_debug",s.lastDebug))s.hud.debug=!s.hud.debug;
 if(edge("restart",s.lastRestart)){s.restart();
 s.drain();
 s.persist();
 return;}
 if(edge("pause",s.lastPause)&&(s.hud.phase==PrototypePhase::Playing||s.hud.phase==PrototypePhase::Paused))s.hud.phase=s.hud.phase==PrototypePhase::Playing?PrototypePhase::Paused:PrototypePhase::Playing;
 if(s.hud.phase!=PrototypePhase::Playing){s.lookYaw=s.lookPitch=0;
 s.persist();
 return;}
 s.yaw=std::remainder(s.yaw+s.lookYaw+s.world.get_axis("look_x")*s.lookSpeed*dt,6.283185307F);
 s.pitch=std::clamp(s.pitch-s.lookPitch-s.world.get_axis("look_y")*s.lookSpeed*dt,-1.45F,1.45F);
 s.lookYaw=s.lookPitch=0;
 float x=s.world.get_axis("move_x"),y=s.world.get_axis("move_y");CharacterInput input;input.move={std::cos(s.yaw)*x+std::sin(s.yaw)*y,-std::sin(s.yaw)*x+std::cos(s.yaw)*y,0};input.jumpPressed=edge("jump",s.lastJump);input.crouchHeld=s.world.is_action_pressed("crouch");(void)s.world.gameplay().set_player_input(s.player,input);
 for(const auto& [id,key]:s.triggers)(void)s.world.gameplay().set_trigger_enabled(id,s.world.is_enabled(s.actors.at(key).id));
 s.world.tick(dt);
 s.interact();
 std::stable_sort(s.inbox->events.begin(),s.inbox->events.end(),[](const TriggerEvent& a,const TriggerEvent& b){return std::tuple{a.trigger,a.object,unsigned(a.kind)}<std::tuple{b.trigger,b.object,unsigned(b.kind)};});
 for(const auto& e:s.inbox->events){if(e.object!=s.pawn||!s.triggers.contains(e.trigger))continue;
 const auto& key=s.triggers.at(e.trigger);
 auto& a=s.actors.at(key);
 if(!s.world.is_enabled(a.id)||s.hud.phase!=PrototypePhase::Playing)continue;
  if(a.config.type==role("pickup")&&e.kind!=TriggerEventKind::Exit&&!s.picked.contains(key)){s.picked.insert(key);
 s.hud.collected=(std::min)(1000000U,s.hud.collected+unsigned(get(a.config,"amount",std::int64_t{1})));(void)s.world.set_enabled(a.id,false);
 s.emit(key,"collected");}
  else if(a.config.type==role("checkpoint")&&e.kind==TriggerEventKind::Enter){s.checkpoint=s.world.position(a.id).value();
 s.hud.message="Checkpoint reached";
 s.emit(key,"entered");}
  else if(a.config.type==role("damage_volume")){if(e.kind==TriggerEventKind::Enter)s.emit(key,"entered");
 if(e.kind==TriggerEventKind::Exit)s.emit(key,"exited");else if(s.world.elapsed_seconds()>=a.nextDamage){a.nextDamage=s.world.elapsed_seconds()+get(a.config,"interval",.5);
 s.damage(float(get(a.config,"damage",25.0)));
 s.emit(key,"damaged");}}
  else if(a.config.type==role("goal")&&e.kind==TriggerEventKind::Enter)s.emit(key,"entered");else if(a.config.type==role("goal")&&e.kind==TriggerEventKind::Exit)s.emit(key,"exited");
 }
 s.inbox->events.clear();
 if(s.inbox->overflow){s.hud.phase=PrototypePhase::Lost;
 s.hud.message="Trigger queue limit exceeded";
 s.inbox->overflow=false;}
 if(auto p=s.world.position(s.pawn);p&&p->z<get(s.actors.at(s.rulesKey).config,"kill_z",-20.0))s.damage(s.hud.maximumHealth);
 s.drain();
 s.persist();
}
bool PrototypeGame::after_restore(std::string* error){
 auto& s=*impl_;
 if(s.water)return s.water->after_restore(error);
 if(s.flying)return s.flying->after_restore(error);
 if(s.racing)return s.racing->after_restore(error);
 if(!s.hud.active)return true;
 std::map<std::string,GameObjectId,std::less<>> restoredIds;
 for(auto id:s.world.object_ids()){const auto* components=s.world.components(id);auto key=prototype_object_key(*components);if(!key.empty()&&!restoredIds.emplace(key,id).second){if(error)*error="Duplicate saved prototype key";return false;}}
 if(restoredIds.size()!=s.actors.size()){if(error)*error="Saved prototype actors do not match the scene";return false;}
 for(auto& [key,actor]:s.actors){if(!restoredIds.contains(key)){if(error)*error="Saved prototype actor is missing";return false;}actor.id=restoredIds.at(key);}
 s.pawn=s.actors.at(s.playerKey).id;auto controller=s.world.gameplay().controller_of(s.pawn);if(!controller||!s.world.gameplay().has_character(s.pawn)){if(error)*error="Saved prototype player is not possessed";return false;}s.player=*controller;
 const auto* cs=s.world.components(s.actors.at(s.rulesKey).id);
 const auto* c=cs?find_component_by_type(*cs,role("state")):nullptr;
 auto fail=[&](){if(error)*error="Invalid prototype save state";
 return false;};
 if(!c)return fail();
 auto text=get(*c,"data",std::string{});
 if(text.size()>128U*1024)return fail();
 std::istringstream in(text);in.imbue(std::locale::classic());
 std::string magic;
 unsigned version{},phase{},debug{},count{};
 float health{},yaw{},pitch{};Float3 checkpoint;in>>magic>>version>>phase>>health>>count>>yaw>>pitch>>checkpoint.x>>checkpoint.y>>checkpoint.z>>debug;
 if(!in||magic!="DVE_PROTOTYPE"||version!=1||phase>3||debug>1||!std::isfinite(health)||health<0||health>s.hud.maximumHealth||count>1000000||!std::isfinite(yaw)||!std::isfinite(pitch)||std::abs(pitch)>1.45F||!finite(checkpoint))return fail();
 std::string prompt,message;
 unsigned countTrace{},volumes{};in>>std::quoted(prompt)>>std::quoted(message)>>countTrace;
 if(!in||prompt.size()>160||message.size()>512||countTrace>64)return fail();
 std::vector<std::string> trace(countTrace);
 for(auto& line:trace){in>>std::quoted(line);
 if(line.size()>512)return fail();}in>>volumes;
 if(!in||volumes>s.actors.size())return fail();
 std::map<std::string,double> cooldowns;
 for(unsigned n=0;n<volumes;++n){std::string key;
 double next{};in>>std::quoted(key)>>next;
 if(!in||!s.actors.contains(key)||s.actors.at(key).config.type!=role("damage_volume")||!std::isfinite(next)||next<0||!cooldowns.emplace(key,next).second)return fail();}in>>std::ws;
 if(!in.eof())return fail();
 std::map<GameTriggerId,std::string> triggers;
 for(auto id:s.world.gameplay().trigger_ids())if(const auto* t=s.world.gameplay().trigger(id);t&&t->name.starts_with("prototype:")){auto key=t->name.substr(10);
 if(!s.actors.contains(key))return fail();
 triggers.emplace(id,key);}if(triggers.size()!=s.triggers.size())return fail();
 for(const auto& [key,a]:s.actors)if(a.config.type==role("damage_volume")&&!cooldowns.contains(key))return fail();
 s.triggers=std::move(triggers);
 s.hud.phase=PrototypePhase(phase);
 s.hud.health=health;
 s.hud.collected=count;
 s.hud.debug=debug!=0;
 s.yaw=yaw;
 s.pitch=pitch;
 s.checkpoint=checkpoint;
 s.stateComponent=c->id;
 s.picked.clear();
 for(const auto& [key,a]:s.actors)if(a.config.type==role("pickup")&&!s.world.is_enabled(a.id)&&a.initialEnabled)s.picked.insert(key);
 s.inbox->events.clear();
 s.events.clear();
 s.hud.prompt=std::move(prompt);
 s.hud.message=std::move(message);
 s.hud.trace=std::move(trace);
 for(auto& [key,a]:s.actors)if(a.config.type==role("damage_volume")){if(!cooldowns.contains(key))return fail();a.nextDamage=cooldowns.at(key);}
 return true;
}
}
