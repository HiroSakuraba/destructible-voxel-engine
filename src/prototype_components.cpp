#include "dve/prototype_game.hpp"
#include "dve/racing_game.hpp"
#include "dve/flying_game.hpp"
#include "dve/water_game.hpp"
#include "dve/gameplay_runtime.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
namespace dve {
namespace {
std::string type(std::string_view role){return std::string(kPrototypePrefix)+std::string(role);}
template<class T> T value(const Component& c,std::string_view key,T fallback){auto it=c.properties.find(key);
 if(it==c.properties.end())return fallback;
 if(auto* p=std::get_if<T>(&it->second))return *p;
 return fallback;}
bool key_valid(std::string_view s){return !s.empty()&&s.size()<=64&&std::all_of(s.begin(),s.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';});}
}
std::vector<std::string> prototype_role_types(){return {type("player_start"),type("rules"),type("switch"),type("door"),type("pickup"),type("checkpoint"),type("damage_volume"),type("goal"),type("race_car"),type("race_rules"),type("race_gate"),type("race_surface"),type("flight_vehicle"),type("flight_rules"),type("flight_gate"),type("flight_wind"),type("water_rules"),type("water_ocean"),type("water_vessel"),type("water_coast")};}
std::vector<std::string> prototype_events(std::string_view r){
 if(r==type("player_start"))return {"died","respawned"};
 if(r==type("door"))return {"opened","closed"};
 if(r==type("switch"))return {"used"};
 if(r==type("pickup"))return {"collected"};
 if(r==type("checkpoint"))return {"entered"};
 if(r==type("damage_volume"))return {"entered","damaged","exited"};
 if(r==type("goal"))return {"entered","exited"};
 if(r==type("rules"))return {"won","lost","restarted"};
 return {};
}
std::vector<std::string> prototype_actions(std::string_view r){if(r==type("door"))return {"open","close","toggle"};
 if(r==type("rules"))return {"win","lose","restart"};
 if(r==type("player_start"))return {"heal","damage","respawn"};
 return {"enable","disable"};}
std::string prototype_object_key(std::span<const Component> cs){for(const auto& c:cs)if(c.enabled&&c.type.starts_with(kPrototypePrefix)&&c.type!=type("link")&&c.type!=type("state"))return value(c,"key",std::string{});
 return {};}
void register_prototype_components(ComponentTypeRegistry& r){
 register_racing_components(r);
 register_flying_components(r);
 register_water_components(r);
 auto add=[&](std::string role,std::string title,std::vector<ComponentPropertySchema> properties){properties.insert(properties.begin(),{"key",role,true,false,"Stable gameplay key"});
 std::string ignored;(void)r.register_type({type(role),std::move(title),false,std::move(properties)},&ignored);};
 add("player_start","Prototype Player Start (Z up)",{{"speed",5.5,false,false,"Run speed (m/s)"},{"jump_speed",7.0,false,false,"Jump speed (m/s)"},{"height",1.8,false,false,"Standing height (m)"},{"radius",.35,false,false,"Capsule radius (m)"},{"health",100.0,false,false,"Maximum health"},{"look_speed",2.2,false,false,"Look speed (rad/s)"}});
 add("rules","Prototype Game Rules",{{"required_items",std::int64_t{3},false,false,"Items needed to win"},{"auto_respawn",true,false,false,"Respawn at checkpoint on death"},{"kill_z",-20.0,false,false,"Fall death below Z"}});
 add("switch","Prototype Switch",{{"reach",2.0,false,false,"Interaction reach (m)"},{"prompt",std::string{"E: use switch"},false,false,"Interaction prompt"}});
 add("door","Prototype Door",{});
 add("pickup","Prototype Pickup",{{"radius",.45,false,false,"Collection radius (m)"},{"amount",std::int64_t{1},false,false,"Collection value"}});
 add("checkpoint","Prototype Checkpoint",{{"radius",.7,false,false,"Activation radius (m)"}});
 add("damage_volume","Prototype Damage Volume",{{"half_extents",Float3{.5F,.5F,.5F},false,false,"Box half extents (m)"},{"damage",25.0,false,false,"Damage per interval"},{"interval",.5,false,false,"Interval (s)"}});
 add("goal","Prototype Goal",{{"half_extents",Float3{.75F,.75F,1},false,false,"Box half extents (m)"}});
 std::string ignored;(void)r.register_type({type("link"),"Prototype Event Connection",true,{{"event",std::string{"used"},true,false,"Source event"},{"target",std::string{},true,false,"Target gameplay key"},{"action",std::string{"open"},true,false,"Target action"},{"amount",10.0,false,false,"Damage/heal amount"}}},&ignored);
 (void)r.register_type({type("state"),"Prototype Runtime State",false,{{"data",std::string{},false,false,"Runtime-only save payload"}}},&ignored);
}
bool validate_prototype_scene(std::span<const PrototypeObjectView> objects,std::string* error){
 auto fail=[&](std::string text){if(error)*error=std::move(text);
 return false;};
 if(scene_has_racing(objects))return validate_racing_scene(objects,error);
 if(scene_has_flying(objects))return validate_flying_scene(objects,error);
 if(scene_has_water(objects))return validate_water_scene(objects,error);
 const auto roles=prototype_role_types();
 std::map<std::string,const Component*,std::less<>> keys;
 unsigned players=0,rules=0,links=0;
 const auto registry=ComponentTypeRegistry::make_default();
 for(const auto& o:objects){const Component* role=nullptr;
 for(const auto& c:o.components)if(c.enabled&&c.type.starts_with(kPrototypePrefix)){
   std::string reason;
 if(!registry.validate(c,&reason))return fail(o.name+": "+reason);
   if(c.type==type("link")){if(++links>4096)return fail("Prototype scene exceeds 4096 connections");continue;}if(c.type==type("state"))continue;
   if(std::find(roles.begin(),roles.end(),c.type)==roles.end())return fail(o.name+": unknown prototype role");
 if(role)return fail(o.name+": one prototype role per object is supported");
   role=&c;
   const auto key=value(c,"key",std::string{});
 if(!key_valid(key)||!keys.emplace(key,&c).second)return fail(o.name+": missing, invalid or duplicate gameplay key '"+key+"'");
   if(keys.size()>1024)return fail("Prototype scene exceeds 1024 actors");
   auto number=[&](std::string_view name,double min,double max,double fallback){double n=value(c,name,fallback);
 return std::isfinite(n)&&n>=min&&n<=max;};
   if(c.type==type("player_start")){++players;
 if(!o.marker)return fail("Player Start must be a marker without physics geometry");
 CharacterControllerConfig cfg;cfg.maximumGroundSpeedMetersPerSecond=float(value(c,"speed",5.5));cfg.jumpSpeedMetersPerSecond=float(value(c,"jump_speed",7.0));cfg.standingHeightMeters=float(value(c,"height",1.8));cfg.radiusMeters=float(value(c,"radius",.35));cfg.crouchedHeightMeters=cfg.standingHeightMeters*.64F;
 if(!cfg.validate()||!number("health",1,1000000,100)||!number("look_speed",.01,20,2.2)||!number("speed",.01,100,5.5)||!number("jump_speed",0,100,7))return fail(o.name+": invalid player configuration");}
   if(c.type==type("rules")){++rules;
 auto n=value(c,"required_items",std::int64_t{3});
 if(n<0||n>1000000||!number("kill_z",-1000000,1000000,-20))return fail(o.name+": invalid game rules");}
   if(c.type==type("switch")&&(!number("reach",.1,20,2)||value(c,"prompt",std::string{}).size()>160))return fail(o.name+": invalid interaction settings");
   if((c.type==type("checkpoint")||c.type==type("pickup"))&&!number("radius",.01,100,.5))return fail(o.name+": invalid trigger radius");
   if(c.type==type("pickup")){auto n=value(c,"amount",std::int64_t{1});
 if(n<1||n>1000000)return fail(o.name+": invalid pickup value");}
   if(c.type==type("damage_volume")&&(!number("damage",0,1000000,25)||!number("interval",.01,3600,.5)))return fail(o.name+": invalid damage settings");
   if(c.type==type("goal")||c.type==type("damage_volume")){auto v=value(c,"half_extents",Float3{.5F,.5F,.5F});
 if(!(v.x>0&&v.y>0&&v.z>0&&v.x<=1000&&v.y<=1000&&v.z<=1000))return fail(o.name+": invalid trigger extents");}
 }}
 if(keys.empty())return links?fail("Prototype connections have no source actors"):true;
 if(players!=1||rules!=1)return fail("Prototype scene requires exactly one Player Start and one Game Rules marker");
 for(const auto& o:objects){auto key=prototype_object_key(o.components);
 for(const auto& c:o.components)if(c.enabled&&c.type==type("link")){
   if(key.empty())return fail(o.name+": connection needs a source prototype actor");
 auto events=prototype_events(keys.at(key)->type);
 auto event=value(c,"event",std::string{});
 if(std::find(events.begin(),events.end(),event)==events.end())return fail(o.name+": unsupported source event '"+event+"'");
   auto target=value(c,"target",std::string{});
 auto found=keys.find(target);
 if(found==keys.end())return fail(o.name+": connection target '"+target+"' is missing");
 auto actions=prototype_actions(found->second->type);
 auto action=value(c,"action",std::string{});
 if(std::find(actions.begin(),actions.end(),action)==actions.end())return fail(o.name+": action '"+action+"' is incompatible with target");
 double n=value(c,"amount",10.0);
 if(!std::isfinite(n)||n<0||n>1000000)return fail(o.name+": invalid connection amount");
 }}return true;
}
}
