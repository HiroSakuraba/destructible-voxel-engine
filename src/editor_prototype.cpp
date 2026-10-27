#include "dve/editor_prototype.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/editor_project.hpp"
#include "dve/editor_scene_export.hpp"
#include "dve/game_manifest.hpp"
#include "dve/prototype_game.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
namespace dve::editor {
namespace {
Component component(std::string role,ComponentId id,std::string key){auto c=make_component_from_schema("dve.prototype."+role,id);
 if(!c)throw std::runtime_error("Prototype schema missing");
 if(c->properties.contains("key"))c->properties["key"]=std::move(key);
 return *c;}
void link(EditorObject& o,std::string event,std::string target,std::string action){auto c=component("link",100+o.components.size(),{});c.properties["event"]=std::move(event);c.properties["target"]=std::move(target);c.properties["action"]=std::move(action);o.components.push_back(std::move(c));}
}
EditorDocument make_prototype_starter(){
 EditorDocument d("First Person Prototype");
 auto marker=[&](std::string name,Float3 pos,std::string r,std::string key)->EditorObject&{EditorObject o(d.allocate_object_id(),std::move(name));o.flags.anchored=true;o.flags.collisionEnabled=false;o.transform.position=pos;o.components.push_back(component(std::move(r),1,std::move(key)));
 return d.add_object(std::move(o));};
 auto box=[&](std::string name,Float3 pos,Float3 size,Float4 color,bool collision=true)->EditorObject&{EditorObject o(d.allocate_object_id(),std::move(name));o.flags.anchored=true;o.flags.collisionEnabled=collision;o.transform.position=pos;o.polygon=make_editor_polygon_cube(o.id,1);
 for(auto& v:o.polygon->vertices)v.position={(v.position.x-.5F)*size.x,(v.position.y-.5F)*size.y,(v.position.z-.5F)*size.z};
 o.polygon->materials[0].baseColor=color;refresh_editor_polygon(*o.polygon,true);
 return d.add_object(std::move(o));};
 marker("Player Start",{0,0,.05F},"player_start","player");marker("Game Rules",{0,0,0},"rules","game");
 box("Floor",{0,5,-.25F},{8,14,.5F},{.25F,.29F,.34F,1});box("Left wall",{-4,5,1.5F},{.2F,14,3},{.35F,.39F,.44F,1});box("Right wall",{4,5,1.5F},{.2F,14,3},{.35F,.39F,.44F,1});
 box("Gate left",{-2.75F,3,1.2F},{2.5F,.25F,2.4F},{.3F,.35F,.4F,1});box("Gate right",{2.75F,3,1.2F},{2.5F,.25F,2.4F},{.3F,.35F,.4F,1});
 auto& door=box("Door",{0,3,1.2F},{3,.25F,2.4F},{.7F,.35F,.1F,1});door.components.push_back(component("door",1,"gate"));
 auto& button=box("Switch",{0,1.4F,1.5F},{.25F,.25F,.3F},{.1F,.8F,.6F,1},false);button.components.push_back(component("switch",1,"button"));link(button,"used","gate","toggle");
 for(unsigned n=0;n<3;++n){const Float3 p[]{ {1,1,.6F},{-.8F,5,.6F},{.8F,7,.6F}};
 auto& o=box("Collectible "+std::to_string(n+1),p[n],{.35F,.35F,.35F},{.95F,.75F,.1F,1},false);o.components.push_back(component("pickup",1,"item"+std::to_string(n+1)));}
 marker("Checkpoint",{0,5,.05F},"checkpoint","checkpoint");
 auto& hazard=box("Damage strip",{0,8,.06F},{2.8F,1.2F,.1F},{.8F,.1F,.1F,1},false);hazard.components.push_back(component("damage_volume",1,"hazard"));hazard.components[0].properties["half_extents"]=Float3{1.4F,.6F,.4F};
 auto& goal=box("Goal",{0,10,1},{3,.2F,2},{.1F,.5F,.9F,1},false);goal.components.push_back(component("goal",1,"goal"));link(goal,"entered","game","win");
 box("Jump block",{-2,5,.3F},{1,1,.6F},{.45F,.45F,.5F,1});
 return d;
}
bool prototype_object_is_marker(const EditorObject& o){
 std::string extension=o.sourceAsset.extension().string();
 std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
 return !o.polygon&&!o.text3d&&!o.gaborVolume&&extension!=".dmesh"&&o.voxels&&o.voxels->occupied_voxel_count()==0;}
bool validate_editor_prototype(const EditorDocument& d,std::string* error){for(const auto& [id,o]:d.objects()){(void)id;
 if(!o.flags.visible&&!prototype_object_key(o.components).empty()){if(error)*error=o.name+": hidden prototype actors cannot be exported; disable the role or show the actor";
 return false;}}std::vector<PrototypeObjectView> views;
 for(const auto& [id,o]:d.objects())views.push_back({id,o.name,o.components,prototype_object_is_marker(o)});
 return validate_prototype_scene(views,error);}
bool export_prototype_project(const EditorDocument& d,const EditorMaterialLibrary& materials,const std::filesystem::path& requestedRoot,std::string* error){
 // "out/" and "out" name the same new folder; a trailing separator must not read as an empty name.
 std::filesystem::path root=requestedRoot.lexically_normal();
 if(!root.empty()&&!root.has_filename())root=root.parent_path();
 auto fail=[&](std::string s){if(error)*error=std::move(s);
 return false;};
 std::string reason;
 if(!d.validate(&reason))return fail(reason);
 if(std::none_of(d.objects().begin(),d.objects().end(),[](const auto& p){return !prototype_object_key(p.second.components).empty();}))return fail("Add a Player Start and Game Rules first");
 if(!validate_editor_prototype(d,&reason))return fail(reason);
 if(root.empty()||root.filename().empty())return fail("Choose a fresh project directory");
 std::error_code ec;
 if(!root.parent_path().empty())std::filesystem::create_directories(root.parent_path(),ec);
 if(ec)return fail(ec.message());
 if(!std::filesystem::create_directory(root,ec))return fail("Output project directory already exists or cannot be created");
 struct Cleanup{std::filesystem::path path;
 bool keep{};~Cleanup(){if(!keep){std::error_code ec;
 std::filesystem::remove_all(path,ec);}}} cleanup{root};
 const bool racing=std::any_of(d.objects().begin(),d.objects().end(),[](const auto& p){return find_component_by_type(p.second.components,"dve.prototype.race_car")!=nullptr;});
 const bool mecha=std::any_of(d.objects().begin(),d.objects().end(),[](const auto& p){return find_component_by_type(p.second.components,"dve.prototype.mech_player")!=nullptr;});
 const bool submarine=std::any_of(d.objects().begin(),d.objects().end(),
     [](const auto& p){for(const auto& c:p.second.components)
        if(c.type=="dve.prototype.water_vessel"){
          const auto it=c.properties.find("craft_type");
          if(it!=c.properties.end())if(const auto type=std::get_if<std::int64_t>(&it->second))
             return *type==6;
        }return false;});
 const bool water=std::any_of(d.objects().begin(),d.objects().end(),[](const auto& p){return find_component_by_type(p.second.components,"dve.prototype.water_ocean")!=nullptr;});
 const bool flying=std::any_of(d.objects().begin(),d.objects().end(),[](const auto& p){return find_component_by_type(p.second.components,"dve.prototype.flight_vehicle")!=nullptr;});
 EditorProject project;project.name=mecha?"Iron Yard - Mecha Combat":submarine?"Deep Explorer Mini-Sub":water?"Water Ships Test Range":flying?"Wind Garden Flight":racing?"Racing Time Trial":"First Person Prototype";project.root=root;project.recentScenes={"scenes/prototype.dvescene"};
 if(!project.initialize(&reason))return fail(reason);
 auto copy=clone_editor_document(d);
 auto saved=copy.save_transactional(root/"scenes"/"prototype.dvescene");
 if(!saved.success)return fail(saved.error);
 auto exported=export_editor_scene(d,materials,root/"scenes"/"prototype.dvoxscene.json");
 if(!exported.success)return fail(exported.error);
 GameManifest manifest;manifest.name=project.name;manifest.version="0.1.0";manifest.entryScene="scenes/prototype.dvoxscene.json";
 manifest.inputBindings={{"move_x","key:a/-1,key:d/+1,gamepad:leftx"},{"move_y","key:s/-1,key:w/+1,gamepad:lefty/-1"},{"look_x","key:j/-1,key:l/+1,gamepad:rightx"},{"look_y","key:i/-1,key:k/+1,gamepad:righty"},{"jump","key:space,gamepad:south"},{"crouch","key:left ctrl,gamepad:east"},{"interact","key:e,gamepad:west"},{"pause","key:p,gamepad:start"},{"restart","key:r,gamepad:back"},{"prototype_debug","key:f3"}};
 if(mecha)manifest.inputBindings={
  {"move_x","key:a/-1,key:d/+1,gamepad:leftx"},
  {"move_y","key:s/-1,key:w/+1,gamepad:lefty/-1"},
  {"look_x","key:j/-1,key:l/+1,gamepad:rightx"},
  {"look_y","key:k/-1,key:i/+1,gamepad:righty/-1"},
  {"mech_primary","mouse:left,gamepad:righttrigger"},
  {"mech_secondary","mouse:right,gamepad:lefttrigger"},
  {"mech_boost","key:left shift,gamepad:south"},
  {"pause","key:p,gamepad:start"},{"restart","key:r,gamepad:back"},
  {"prototype_debug","key:f3"}};
 if(racing)manifest.inputBindings["race_brake"]="key:b/+1,gamepad:lefttrigger";
 if(submarine){manifest.inputBindings={
   {"move_y","key:s/-1,key:w/+1,gamepad:lefty/-1"},
   {"move_x","key:a/-1,key:d/+1,gamepad:leftx"},
   {"sub_heave","key:left ctrl/-1,key:space/+1,gamepad:righty/-1"},
   {"sub_strafe","key:q/-1,key:e/+1,gamepad:rightx"},
   {"sub_ballast","key:z/-1,key:x/+1"},
   {"sub_pitch","key:down/-1,key:up/+1"},
   {"sub_depth_hold","key:h,gamepad:south"},
   {"pause","key:p,gamepad:start"},{"restart","key:r,gamepad:back"},
   {"prototype_debug","key:f3"}
 };}else if(water){manifest.inputBindings={{"move_x","key:a/-1,key:d/+1,gamepad:leftx"},
  {"move_y","key:s/-1,key:w/+1,gamepad:lefty/-1"},
  {"water_ship_1","key:1"},{"water_ship_2","key:2"},
  {"water_ship_3","key:3"},{"water_ship_4","key:4"},{"water_ship_5","key:5"},{"water_ship_6","key:6"},
  {"water_trim","key:q/-1,key:e/+1,gamepad:rightx"},
  {"interact","key:tab,gamepad:west"},{"pause","key:p,gamepad:start"},
  {"restart","key:r,gamepad:back"},{"prototype_debug","key:f3"}};}
 if(flying){
   manifest.inputBindings={{"move_x","key:a/-1,key:d/+1,gamepad:leftx"},
     {"move_y","key:s/-1,key:w/+1,gamepad:lefty/-1"},
     {"flight_yaw","key:q/-1,key:e/+1,gamepad:rightx"},
     {"flight_vertical","key:up/+1,key:down/-1,gamepad:righty/-1"},
     {"flight_throttle","key:left shift/+1,key:left ctrl/-1,gamepad:righttrigger,gamepad:lefttrigger/-1"},
     {"flight_next","key:tab,gamepad:rightshoulder"},
     {"flight_previous","key:backspace,gamepad:leftshoulder"},
     {"flight_mode_1","key:1"},{"flight_mode_2","key:2"},{"flight_mode_3","key:3"},
     {"flight_mode_4","key:4"},{"flight_mode_5","key:5"},{"flight_mode_6","key:6"},
     {"jump","key:space,gamepad:south"},{"flight_airbrake","key:b,gamepad:east"},
     {"crouch","key:x,gamepad:west"},{"interact","key:c,gamepad:north"},
     {"pause","key:p,gamepad:start"},{"restart","key:r,gamepad:back"},
     {"prototype_debug","key:f3"}};
 }
 std::ofstream game(root/"game.dvegame",std::ios::binary);game<<manifest.serialize();game.close();
 if(!game)return fail("Cannot write game manifest");
 std::ofstream readme(root/"README.md");readme<<"# First-person prototype\n\nOpen project.dveproject and scenes/prototype.dvescene in the editor, or run dve_player with this directory.\n\nWASD: move; mouse or IJKL: look; Space: jump; Ctrl: crouch; E: use; P: pause; R: restart; F3: event trace. Gamepad sticks, South/East/West/Start/Back are bound too.\n\nCollect three yellow objects. Use the green switch to open the door. Reach the checkpoint, cross the red damage strip and enter the blue goal. Z is up. Lua is optional.\n\nPackage with dve_pack NEW_PROJECT NEW_GAME.dvepak --all, then launch dve_player NEW_GAME.dvepak.\n";if(racing){readme.close();readme.open(root/"README.md",std::ios::trunc);readme<<"# Racing time trial\n\nOpen project.dveproject and scenes/prototype.dvescene in the editor, or launch dve_player with this directory.\n\nW/S: throttle and reverse (opposite direction brakes). B / left trigger: independent service brake. A/D: steer. Space: handbrake. Ctrl: recover to last ordered gate (+5 seconds). E: chase/hood camera. P: pause. R: new attempt. Gamepad left stick drives, South is handbrake, East recovery, West camera, Start pause, Back restart.\n\nWait for the countdown, drive counterclockwise around the central barrier, and pass gates 1, 2, 3, then finish gate 0 in the arrow direction. Complete three laps. Shortcut and reverse crossings do not advance the lap. Best lap survives restart within the session and save/load. Z is up. This is portable arcade capsule collision, not Jolt suspension simulation.\n\nPackage with dve_pack NEW_PROJECT NEW_GAME.dvepak --all; launch dve_player NEW_GAME.dvepak.\n";}if(flying){readme.close();readme.open(root/"README.md",std::ios::trunc);
 readme<<"# Wind Garden: Six Form Flight\n\nOpen project.dveproject in the editor or run dve_player with this project folder.\n\nW/S pitch or hover forward/back; A/D bank or hover strafe; Q/E rudder; Shift/Ctrl throttle; Arrow Up/Down hover climb; Space bird flap; B airbrake; Tab/Backspace mode cycle; 1..6 direct mode requests (Float, Glider, Biplane, Jet, Hover, Bird); C camera; X recovery; P pause; R restart; F3 flight diagnostics. Gamepad sticks, shoulders, triggers and face buttons are bound.\n\nPass the six colored sky gates in order, matching the form written in the HUD. Glide has no motor; Float is buoyant; Hover consumes battery. Fly through the orange updraft around X=50, Y=280 to soar. Transitions preserve position and momentum. Jet cannot instantly stop when changing to Hover.\n\nThis is a portable Z-up arcade aerodynamic prototype, not a certified or high-fidelity simulator. Deeper weather, vehicle morph meshes and flight audio are later batches. Package with dve_pack NEW_PROJECT NEW_GAME.dvepak --all.\n";
 }if(mecha){readme.close();readme.open(root/"README.md",std::ios::trunc);
 readme<<"# Iron Yard: M1-M5 Mecha Combat\\n\\nRun dve_player with this project directory.\\nWASD move; mouse or J/L rotate cannon turret; I/K plus J/L aim with gamepad right stick.\\nShift boost, mouse left autocannon, mouse right rockets. P pause, R restart, F3 debug.\\nDestroy three moving enemies, then enter the green extraction zone. Chassis and turret rotate independently. The reference renderer is CPU.\\nVoxel barricades can be destroyed with rockets; surviving enemies use cover and can shoot back. F5 quicksaves, F9 quickloads, and R fully restarts the scene including destructible cover. A procedural audio mix plays in builds with audio enabled.\\n";
 }if(submarine){readme.close();readme.open(root/"README.md",std::ios::trunc);
 readme<<"# Deep Explorer: Mini-Submarine\n\nW/S forward/aft; A/D yaw; Space/Ctrl rise/dive; Q/E lateral thrusters; Z/X pump ballast out/in; Up/Down pitch; H toggle depth hold; P pause; R reset; F3 instruments.\n\nDescend toward the wreck at Y=80, Z=-30. The sea and pressure use the shared OceanField plus underwater volume; the seafloor is real collision at Z=-40. Use ballast for neutral trim, thrusters for active movement, and depth hold for station keeping. This S1 prototype does not include depth-dependent rendered fog, submarine headlamps or mission scoring; those are S2/S3.\n";
 }else if(water){readme.close();readme.open(root/"README.md",std::ios::trunc);
 readme<<"# W2 Water Ships Test Range\n\nRun dve_player with this generated project.\n\n1 Dinghy, 2 Pirate Ship, 3 Sailboat, 4 Speedboat, 5 Wave Runner, 6 Surfboard; Tab cycles; W/S engine throttle or sail exposure; A/D helm; Q/E trim sails; P pause; R restart; F3 hydro diagnostics. Gamepad left stick, West, Start and Back are bound.\n\nThe Gerstner ocean surface is rendered by the CPU player and sampled by buoyancy at a fixed 120 Hz physics step. Calm/breeze/storm is an authorable ocean marker property. Ships are visual-only geometry; the portable water controller owns their multi-probe motion and solid-land capsule collisions. W2 implements simplified apparent-wind sails, keels, planing and pump jets. Wakes and breaking surf are W3/W4.\n";
 }readme.close();
 if(!readme)return fail("Cannot write starter guide");
 cleanup.keep=true;
 return true;
}
}
