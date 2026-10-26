#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_prototype.hpp"
#include "dve/prototype_game.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <system_error>
#include <variant>
#include <vector>
namespace dve::editor {
namespace {
class PrototypeComponentsCommand final:public IEditorCommand {
 EditorObjectId id_;
 std::vector<Component> before_,after_;
 CommandResult apply(EditorDocument& d,bool forward){auto* o=d.find_object(id_);
 if(!o||o->flags.locked)return CommandResult::fail("Prototype object is missing or locked");
 auto copy=forward?after_:before_;o->components.swap(copy);d.mark_dirty();
 return CommandResult::ok();}
public:PrototypeComponentsCommand(const EditorObject& o,std::vector<Component> after):id_(o.id),before_(o.components),after_(std::move(after)){}
 std::string_view label()const noexcept override{return "Edit prototype component";}CommandResult execute(EditorDocument& d)override{return apply(d,true);}CommandResult undo(EditorDocument& d)override{return apply(d,false);}
};
// Adds the starter objects to the (empty) open scene instead of replacing the document, so the
// scene keeps its name, file path, revision and selection sets, and Undo removes exactly them.
class PrototypeStarterCommand final:public IEditorCommand {
 std::vector<EditorObjectId> added_;int kind_{};
public:explicit PrototypeStarterCommand(int kind=0):kind_(kind){}
 std::string_view label()const noexcept override{return kind_==5?"Create mecha combat starter":kind_==4?"Create mini-submarine":kind_==3?"Create water test range":kind_==2?"Create flight starter":kind_==1?"Create racing starter":"Create prototype starter";}
 CommandResult execute(EditorDocument& d)override{if(!d.objects().empty())return CommandResult::fail("Use File > New Scene before creating the starter");
 auto starter=kind_==5?make_mecha_starter():kind_==4?make_submarine_starter():kind_==3?make_water_starter():kind_==2?make_flying_starter():kind_==1?make_racing_starter():make_prototype_starter();added_.clear();
 for(const auto& entry:starter.objects()){auto copy=clone_editor_object(entry.second);added_.push_back(copy.id);d.add_object(std::move(copy));}
 return CommandResult::ok();}
 CommandResult undo(EditorDocument& d)override{for(const auto id:added_)(void)d.remove_object(id);added_.clear();d.mark_dirty();
 return CommandResult::ok();}
};
// Property text of a component, or empty when the property is missing or not text.
std::string text_property(const Component& c,std::string_view name){const auto it=c.properties.find(name);
 if(it==c.properties.end())return {};
 if(const auto* text=std::get_if<std::string>(&it->second))return *text;
 return {};}
const Component* selected_component(const EditorObject* o,std::size_t index){if(!o)return nullptr;
 std::size_t n=0;
 for(const auto& c:o->components)if(c.type.starts_with(kPrototypePrefix)&&c.type!="dve.prototype.state"){if(n++==index)return &c;}
 return nullptr;}
const Component* actor_component(const EditorObject* o){if(!o)return nullptr;
 for(const auto& c:o->components)if(c.enabled&&c.type.starts_with(kPrototypePrefix)&&c.type!="dve.prototype.link"&&c.type!="dve.prototype.state")return &c;
 return nullptr;}
}
PrototypePanelLayout NativeEditorController::prototype_panel_layout()const{
 PrototypePanelLayout l;
 const int w=(std::max)(280,(std::min)(760,width_-32)),h=(std::max)(220,(std::min)(640,height_-48));l.panel={(width_-w)/2,(height_-h)/2,w,h};l.close={l.panel.x+w-36,l.panel.y+8,26,24};l.previous={l.panel.x+12,l.panel.y+39,90,26};l.next={l.panel.x+110,l.panel.y+39,90,26};
 for(int y=l.panel.y+73;y+28<l.panel.y+h-65;y+=29)l.rows.push_back({l.panel.x+12,y,w-24,26});
 return l;
}
std::vector<std::pair<std::string,std::string>> NativeEditorController::prototype_panel_controls()const{
 const auto& p=prototypePanel_;
 const auto* o=workspace_.selected_object()?workspace_.document().find_object(*workspace_.selected_object()):nullptr;
 std::vector<std::pair<std::string,std::string>> controls;
 if(p.picking){controls.push_back({"cancelpick","Cancel target selection"});
 for(const auto& [id,target]:workspace_.document().objects()){auto key=prototype_object_key(target.components);
 if(!key.empty())controls.push_back({"target:"+std::to_string(id),target.name+" ["+key+"]"});}return controls;}
 auto roles=prototype_role_types();controls={{"starter","Create first-person starter (empty scene)"},{"racing","Create racing time trial (empty scene)"},{"flying","Create flying transformation course (empty scene)"},{"water","Create water/ships test range (empty scene)"},{"submarine","Create mini-submarine test range (empty scene)"},{"mecha","Create mecha combat range (empty scene)"},{"export","Export playable project to new folder"},{"validate","Validate gameplay keys and connections"},{"role","Role to add: "+roles[p.role%roles.size()].substr(kPrototypePrefix.size())},{"marker","Create marker with this role"},{"addrole","Add role to selected object"},{"link","Add event connection to selected actor"},{"component","Next component on selected actor"}};
 if(const auto* c=selected_component(o,p.component)){
  controls.push_back({"enabled",std::string(c->enabled?"Disable ":"Enable ")+c->type});
  controls.push_back({"remove","Remove "+c->type});
  if(c->type=="dve.prototype.link"){controls.push_back({"event","Cycle source event"});controls.push_back({"action","Cycle target action"});}
  for(const auto& [name,v]:c->properties)controls.push_back({"edit:"+std::to_string(c->id)+":"+name,name+": "+format_component_value(v)});
 }
 return controls;
}
void NativeEditorController::open_prototype_panel(){prototypePanel_.open=true;prototypePanel_.first=0;prototypePanel_.message="Select an actor to edit. Event targets are picked from scene objects. Z is up.";}
bool NativeEditorController::prototype_panel_action(std::string_view id){
 auto& p=prototypePanel_;
 auto fail=[&](std::string s){p.message=std::move(s);set_status(p.message,true);
 return false;};
 if(playSession_.active()||longJob_)return fail("Stop Play and background work before authoring gameplay");
 auto& d=workspace_.document();
 auto* o=workspace_.selected_object()?d.find_object(*workspace_.selected_object()):nullptr;
 // Editing/picking state belongs to one object: any unrelated action (including ones that change
 // the selection) abandons it so a later Enter cannot write into a different object's component.
 if(!id.starts_with("edit:")&&!id.starts_with("target:")&&id!="cancelpick"){p.picking=false;p.editingProperty.clear();p.editingComponent=0;p.editingObject=0;p.typed.clear();}
 auto result=[&](CommandResult r){p.message=r.success?"Gameplay edit saved in scene; Undo is available":r.message;
 if(!r.success)set_status(r.message,true);
 return r.success;};
 if(id=="cancelpick"){p.picking=false;p.editingProperty.clear();p.editingComponent=0;p.editingObject=0;
 p.first=0;
 return true;}
 if(id=="starter"||id=="racing"||id=="flying"||id=="water"||id=="submarine"||id=="mecha"){auto r=workspace_.commands().execute(d,std::make_unique<PrototypeStarterCommand>(id=="mecha"?5:id=="submarine"?4:id=="water"?3:id=="flying"?2:id=="racing"?1:0));
 if(r.success){workspace_.clear_selection();workspace_.select_object(d.objects().begin()->first);camera_.position=id=="mecha"?Float3{45,-60,80}:id=="submarine"?Float3{16,-58,18}:id=="water"?Float3{60,-100,58}:id=="flying"?Float3{130,-170,190}:id=="racing"?Float3{60,-65,65}:Float3{8,-7,6};camera_.target=id=="mecha"?Float3{0,15,0}:id=="submarine"?Float3{0,-10,-12}:id=="water"?Float3{0,35,0}:id=="flying"?Float3{0,200,60}:id=="racing"?Float3{0,0,0}:Float3{0,4,0};camera_.worldUp={0,0,1};}return result(r);}
 if(id=="validate"){std::string error;
 bool valid=validate_editor_prototype(d,&error);
 p.message=valid?"Prototype validation passed":error;
 return valid;}
 if(id=="export"){if(projectRoot_.empty())return fail("Open a project first; export creates a new prototype folder inside it");
 const auto output=projectRoot_/("prototype-build-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
 std::string error;
 if(!export_prototype_project(d,materials_,output,&error))return fail(error);
 p.message="Playable project: "+output.string();set_status(p.message,false,15);
 return true;}
 if(id=="role"){++p.role;
 return true;}
 if(id=="marker"){EditorObject marker(d.allocate_object_id(),"Prototype marker");marker.transform.position=camera_.target;marker.flags.anchored=true;marker.flags.collisionEnabled=false;
 const auto roles=prototype_role_types();
 auto c=make_component_from_schema(roles[p.role%roles.size()],1);
 c->properties["key"]=roles[p.role%roles.size()].substr(kPrototypePrefix.size())+"-"+std::to_string(marker.id);marker.components.push_back(*c);
 auto objectId=marker.id;
 auto r=workspace_.commands().execute(d,std::make_unique<AddObjectCommand>(std::move(marker),"Create gameplay marker"));
 if(r.success)workspace_.select_object(objectId);
 p.component=0;
 return result(r);}
 if(!o||o->flags.locked)return fail("Select an unlocked object first");
 if(id=="addrole"){if(actor_component(o))return fail("This object already has a prototype role");
 auto roles=prototype_role_types();
 const auto& type=roles[p.role%roles.size()];
 if(type=="dve.prototype.player_start"&&!prototype_object_is_marker(*o))return fail("Use an empty Player Start marker");
 auto c=make_component_from_schema(type,d.allocate_component_id(o->id));
 c->properties["key"]=type.substr(kPrototypePrefix.size())+"-"+std::to_string(o->id);
 p.component=0;
 return result(workspace_.commands().execute(d,std::make_unique<AddComponentCommand>(o->id,*c)));}
 if(id=="component"){std::size_t count=0;
 for(const auto& c:o->components)if(c.type.starts_with(kPrototypePrefix)&&c.type!="dve.prototype.state")++count;
 p.component=count?(p.component+1)%count:0;
 return true;}
 if(id=="link"){const auto* role=actor_component(o);
 if(!role)return fail("Give the selected object a prototype role first");
 auto events=prototype_events(role->type);
 if(events.empty())return fail("This actor has no source events");
 auto c=make_component_from_schema("dve.prototype.link",d.allocate_component_id(o->id));
 c->properties["event"]=events.front();
 auto after=o->components;after.push_back(*c);
 p.component=0;
 for(const auto& entry:o->components)if(entry.type.starts_with(kPrototypePrefix)&&entry.type!="dve.prototype.state")++p.component;
 return result(workspace_.commands().execute(d,std::make_unique<PrototypeComponentsCommand>(*o,std::move(after))));}
 const auto* current=selected_component(o,p.component);
 if(!current)return fail("Select a prototype component first");
 if(id=="enabled")return result(set_component_enabled_on_primary(current->id,!current->enabled));
 if(id=="remove"){auto r=remove_component_from_primary(current->id);
 p.component=0;
 return result(r);}
 if(id=="event"||id=="action"){
  if(current->type!="dve.prototype.link")return fail("Select an event connection first");
 std::vector<std::string> choices;
  const char* property=id=="event"?"event":"action";
  if(id=="event"){auto* role=actor_component(o);
 if(!role)return fail("Source role is disabled");
 choices=prototype_events(role->type);}else {auto target=text_property(*current,"target");
 for(const auto& [key,obj]:d.objects()){(void)key;
 if(prototype_object_key(obj.components)==target)if(auto* role=actor_component(&obj))choices=prototype_actions(role->type);}}
  if(choices.empty())return fail("Pick a target actor first");
 auto old=text_property(*current,property);
 auto i=std::find(choices.begin(),choices.end(),old);
 auto n=i==choices.end()?0:(std::size_t(i-choices.begin())+1)%choices.size();
 return result(set_component_property_text_on_primary(current->id,property,choices[n]));
 }
 if(id.starts_with("target:")){
  EditorObjectId targetId{};
 auto s=id.substr(7);
 auto parsed=std::from_chars(s.data(),s.data()+s.size(),targetId);
 auto* target=d.find_object(targetId);
 if(parsed.ec!=std::errc{}||!target||!actor_component(target))return fail("Target no longer exists");
 if(p.editingObject!=o->id)return fail("Selection changed; pick the connection target again");
 auto after=o->components;
 auto* c=find_component(std::span<Component>(after),p.editingComponent);
 if(!c)return fail("Connection no longer exists");
 c->properties["target"]=prototype_object_key(target->components);
 auto choices=prototype_actions(actor_component(target)->type);
 auto old=text_property(*c,"action");
 if(choices.empty())return fail("Target has no actions");
 if(std::find(choices.begin(),choices.end(),old)==choices.end())c->properties["action"]=choices.front();
 p.picking=false;
 p.first=0;
 p.editingProperty.clear();
 p.editingComponent=0;p.editingObject=0;
 return result(workspace_.commands().execute(d,std::make_unique<PrototypeComponentsCommand>(*o,std::move(after))));
 }
 if(id.starts_with("edit:")){auto start=id.find(':',5);ComponentId component{};
 auto s=id.substr(5,start-5);
 auto parsed=std::from_chars(s.data(),s.data()+s.size(),component);
 if(parsed.ec!=std::errc{}||start==id.npos)return false;
 p.editingComponent=component;p.editingObject=o->id;
 p.editingProperty=std::string(id.substr(start+1));
 const auto* c=d.find_component(o->id,component);
 if(!c||!c->properties.contains(p.editingProperty))return false;
 if(p.editingProperty=="target"&&c->type=="dve.prototype.link"){p.picking=true;
 p.first=0;
 return true;}p.typed=format_component_value(c->properties.at(p.editingProperty));
 p.replaceText=true;
 return true;}
 return false;
}
bool NativeEditorController::prototype_panel_pointer(int x,int y){auto l=prototype_panel_layout();
 auto& p=prototypePanel_;
 if(l.close.contains(x,y)){p.open=false;
 return true;}if(l.previous.contains(x,y)){p.first=p.first>l.rows.size()?p.first-l.rows.size():0;
 return true;}if(l.next.contains(x,y)){auto controls=prototype_panel_controls();
 if(p.first+l.rows.size()<controls.size())p.first+=l.rows.size();
 return true;}auto controls=prototype_panel_controls();
 for(std::size_t n=0;n<l.rows.size()&&p.first+n<controls.size();++n)if(l.rows[n].contains(x,y))return prototype_panel_action(controls[p.first+n].first);
 return l.panel.contains(x,y);}
void NativeEditorController::prototype_panel_text(std::string_view text){auto& p=prototypePanel_;
 if(!p.editingProperty.empty()){if(p.replaceText)p.typed.clear();
 p.replaceText=false;
 if(p.typed.size()+text.size()<=256)p.typed+=text;}}
bool NativeEditorController::prototype_panel_key(std::string_view key){auto& p=prototypePanel_;
 if(key=="escape"){if(!p.editingProperty.empty()){p.editingProperty.clear();
 p.editingComponent=0;p.editingObject=0;p.picking=false;}else if(p.picking){p.picking=false;
 p.first=0;}else p.open=false;
 return true;}if(key=="backspace"&&!p.editingProperty.empty()){if(p.replaceText)p.typed.clear();else if(!p.typed.empty())p.typed.pop_back();
 p.replaceText=false;
 return true;}if((key=="enter"||key=="return")&&!p.editingProperty.empty()){
 if(workspace_.selected_object()!=std::optional<EditorObjectId>(p.editingObject)){p.message="Selection changed; edit cancelled";p.editingProperty.clear();p.editingComponent=0;p.editingObject=0;return true;}
 auto r=set_component_property_text_on_primary(p.editingComponent,p.editingProperty,p.typed);
 p.message=r.success?"Property updated":r.message;
 if(r.success){p.editingProperty.clear();
 p.editingComponent=0;p.editingObject=0;}return true;}return true;}
void render_prototype_panel(const IEditorCanvas& canvas,NativeEditorController& controller){
 const auto& p=controller.prototype_panel();
 if(!p.open)return;
 auto l=controller.prototype_panel_layout();RectClipCanvas painter(canvas,l.panel);painter.fill(l.panel,0x172133);painter.outline(l.panel,0x66cddd);painter.text(l.panel.x+12,l.panel.y+26,"Gameplay Prototype Kit",0xe2e9f2);
 auto button=[&](UiRect r,std::string label){painter.fill(r,0x26374d);painter.text(r.x+6,r.y+18,elide_text_to_width(painter,label,r.width-12),0xe2e9f2);};
 button(l.close,"X");
 button(l.previous,"Previous");
 button(l.next,"Next");
 auto controls=controller.prototype_panel_controls();
 for(std::size_t n=0;n<l.rows.size()&&p.first+n<controls.size();++n){auto label=controls[p.first+n].second;
 if(controls[p.first+n].first=="edit:"+std::to_string(p.editingComponent)+":"+p.editingProperty&&!p.picking)label=p.editingProperty+": "+p.typed+" (Enter to save)";
 button(l.rows[n],label);}painter.text(l.panel.x+12,l.panel.y+l.panel.height-38,elide_text_to_width(painter,p.message,l.panel.width-24),0x9eafc3);painter.text(l.panel.x+12,l.panel.y+l.panel.height-16,"Use Next for component fields. Connections pick scene objects by stable key.",0x9eafc3);
}
}
