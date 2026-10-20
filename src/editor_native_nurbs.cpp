#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_nurbs.hpp"
#include "dve/voxel_metric.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
namespace dve::editor {
namespace {
constexpr std::string_view actions[]={"nurbs.create_patch","nurbs.create_circle","nurbs.create_arc","nurbs.create_profile","nurbs.create_revolve","nurbs.extrude","nurbs.revolve","nurbs.edit","nurbs.bake_polygon","nurbs.finish","nurbs.previous","nurbs.next"};
UiRect panel(UiRect v){return {v.x+8,v.y+8,std::min(330,std::max(1,v.width-16)),std::min(238,std::max(1,v.height-16))};}
UiRect field(UiRect v,int i){auto p=panel(v);return {p.x+8,p.y+62+i*32,p.width-16,28};}
std::string number(double x){std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(12)<<x;return o.str();}
NurbsSource profile(){NurbsSource s;s.columns=4;s.u.knots={0,0,0,0,1,1,1,1};s.controls={{1,0,0,1},{1.5,1,0,1},{.8,2,0,1},{1,3,0,1}};return s;}
}
void NativeEditorController::refresh_nurbs_menu_state(){
    auto id=workspace_.selected_object();const auto* o=id?workspace_.document().find_object(*id):nullptr;
    bool editing=workspace_.mode()==EditorMode::Edit&&!playSession_.active()&&!longJob_;
    bool ready=editing&&workspace_.selection_count()==1&&o&&o->is_nurbs()&&o->flags.visible&&!o->flags.locked&&!o->prefabLink&&!is_isolated_out(o->id);
    if(o&&o->is_nurbs()) {
        std::vector<std::string> blocked;
        for(const auto& a:std::as_const(workspace_.menus()).actions())
            if(a.id.starts_with("mesh.")||a.id.starts_with("mirror.")||
               (a.id.starts_with("polygon.")&&a.id!="polygon.create_plane"&&a.id!="polygon.import"&&a.id!="polygon.import_exact")||a.id=="subdiv.from_polygon")blocked.push_back(a.id);
        for(const auto& a:blocked)(void)workspace_.menus().set_enabled(a,false,"Bake NURBS to a new polygon before polygon editing");
    }
    std::string error;auto source=ready?editor_nurbs_source(*o,&error):std::nullopt;
    for(auto action:actions) {
        bool enabled=editing;
        if(action=="nurbs.extrude"||action=="nurbs.revolve")enabled=source&&source->is_curve();
        else if(action=="nurbs.edit"||action=="nurbs.bake_polygon")enabled=bool(source);
        else if(action=="nurbs.finish"||action=="nurbs.previous"||action=="nurbs.next")enabled=ready&&nurbsEditingObject_==o->id;
        (void)workspace_.menus().set_enabled(action,enabled,enabled?"":"Stop Play; select one visible unlocked native NURBS source (a curve for extrusion/revolve)");
    }
}
bool NativeEditorController::create_nurbs_source(const NurbsSource& source,std::string name){
    if(workspace_.mode()!=EditorMode::Edit||playSession_.active()||longJob_)return false;
    close_previews_for_voxel_edit("NURBS creation");
    auto& doc=workspace_.document();auto id=doc.allocate_object_id();std::string error;
    auto object=make_editor_nurbs(id,source,&error);if(!object){set_status(error,true);return false;}
    object->name=std::move(name);object->transform.position=camera_.target;
    const auto before=capture_selection_snapshot();
    auto result=workspace_.commands().execute(doc,std::make_unique<AddObjectCommand>(std::move(*object),"Create native NURBS"));
    if(!result.success){set_status(result.message,true);return false;}
    if(!viewportSettings_.isolatedObjects.empty())viewportSettings_.isolatedObjects.insert(id);
    workspace_.select_object(id);selectionDomain_=SelectionDomain::Objects;nurbsEditingObject_=id;nurbsControl_=0;
    workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());
    frame_selection();refresh_menu_state();set_status("Native NURBS created; click controls or use Previous/Next; click X/Y/Z/Weight to edit");return true;
}
bool NativeEditorController::edit_nurbs_control(std::size_t index,NurbsPoint value){
    if(workspace_.mode()!=EditorMode::Edit||playSession_.active()||longJob_||workspace_.selection_count()!=1)return false;
    auto id=workspace_.selected_object();auto* o=id?workspace_.document().find_object(*id):nullptr;
    if(!o||is_isolated_out(o->id))return false;
    std::string error;auto before=editor_nurbs_source(*o,&error);
    if(!before||index>=before->controls.size()){set_status("Select a valid native NURBS control",true);return false;}
    if(before->controls[index]==value)return true;
    auto after=*before;after.controls[index]=value;
    auto selection=capture_selection_snapshot();
    auto result=workspace_.commands().execute(workspace_.document(),std::make_unique<EditNurbsCommand>(*id,*before,std::move(after)));
    if(result.success)workspace_.commands().set_last_selection_transition(selection,capture_selection_snapshot());
    set_status(result.success?"NURBS control updated":result.message,!result.success);return result.success;
}
bool NativeEditorController::dispatch_nurbs_action(std::string_view action){
    if(workspace_.mode()!=EditorMode::Edit||playSession_.active()||longJob_){set_status("Stop Play before NURBS authoring",true);return false;}
    if(action=="nurbs.create_patch")return create_nurbs_source(make_nurbs_patch(),"NURBS Patch");
    if(action=="nurbs.create_circle")return create_nurbs_source(make_nurbs_arc(),"NURBS Circle");
    if(action=="nurbs.create_arc")return create_nurbs_source(make_nurbs_arc(1,1.5707963267948966),"NURBS Quarter Arc");
    if(action=="nurbs.create_profile")return create_nurbs_source(profile(),"NURBS Profile");
    if(action=="nurbs.create_revolve"){auto s=revolve_nurbs(profile());return s&&create_nurbs_source(*s,"NURBS Revolved Surface");}
    auto id=workspace_.selected_object();auto* o=id?workspace_.document().find_object(*id):nullptr;
    if(!o||workspace_.selection_count()!=1||!o->flags.visible||o->flags.locked||o->prefabLink||is_isolated_out(o->id)){
        set_status("Select one visible unlocked NURBS source; detach prefab instances before editing",true);return false;
    }
    std::string error;auto s=editor_nurbs_source(*o,&error);if(!s){set_status(error,true);return false;}
    if(action=="nurbs.edit"){close_previews_for_voxel_edit("NURBS controls");nurbsEditingObject_=*id;nurbsControl_=0;selectionDomain_=SelectionDomain::Objects;return true;}
    if(action=="nurbs.finish"){if(textEdit_.kind==TextEditKind::NurbsControl)cancel_text_edit();nurbsEditingObject_=0;return true;}
    if(action=="nurbs.previous"||action=="nurbs.next"){
        if(nurbsEditingObject_!=*id)return false;
        if(textEdit_.kind==TextEditKind::NurbsControl)cancel_text_edit();
        nurbsControl_=(nurbsControl_+(action=="nurbs.next"?1:s->controls.size()-1))%s->controls.size();return true;
    }
    if(action=="nurbs.extrude"||action=="nurbs.revolve"){
        auto result=action=="nurbs.extrude"?extrude_nurbs(*s,{0,2,0},&error):revolve_nurbs(*s,6.283185307179586,&error);
        if(!result){set_status(error,true);return false;}
        auto pose=o->transform;auto metric=o->voxelScale;
        if(!create_nurbs_source(*result,action=="nurbs.extrude"?"NURBS Extrusion":"NURBS Revolution"))return false;
        auto* created=workspace_.document().find_object(*workspace_.selected_object());created->transform=pose;created->voxelScale=metric;return true;
    }
    if(action=="nurbs.bake_polygon"){
        auto before=capture_selection_snapshot();auto newId=workspace_.document().allocate_object_id();
        EditorObject baked(newId,o->name+" (Polygon)");baked.polygon=*o->polygon;baked.polygon->objectId=newId;
        baked.polygon->contentHash=polygon_asset_content_hash(*baked.polygon);baked.transform=o->transform;baked.voxelScale=o->voxelScale;
        baked.voxelSizeMeters=o->voxelSizeMeters;baked.flags=o->flags;baked.tags=o->tags;baked.groups=o->groups;baked.layer=o->layer;
        auto result=workspace_.commands().execute(workspace_.document(),std::make_unique<AddObjectCommand>(std::move(baked),"Bake NURBS to polygon"));
        if(result.success){workspace_.select_object(newId);selectionDomain_=SelectionDomain::Objects;nurbsEditingObject_=0;if(!viewportSettings_.isolatedObjects.empty())viewportSettings_.isolatedObjects.insert(newId);
            workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());}
        set_status(result.success?"Baked polygon added; native NURBS retained":result.message,!result.success);return result.success;
    }
    return false;
}
bool NativeEditorController::begin_nurbs_field(unsigned axis){
    auto id=workspace_.selected_object();if(!id||nurbsEditingObject_!=*id)return false;
    const auto* o=workspace_.document().find_object(*id);std::string error;auto s=o?editor_nurbs_source(*o,&error):std::nullopt;
    if(!s||nurbsControl_>=s->controls.size()||axis>3)return false;
    nurbsFieldBefore_=s;nurbsFieldControl_=nurbsControl_;
    auto q=s->controls[nurbsControl_];const double values[]={q.x,q.y,q.z,q.weight};
    begin_text_edit(TextEditKind::NurbsControl,*id,number(values[axis]));textEdit_.axis=int(axis);return true;
}
void NativeEditorController::commit_nurbs_field(){
    const auto* o=workspace_.document().find_object(textEdit_.objectId);std::string error;
    auto s=o?editor_nurbs_source(*o,&error):std::nullopt;
    if(!s||!nurbsFieldBefore_||*s!=*nurbsFieldBefore_||workspace_.selected_object()!=textEdit_.objectId||
       nurbsFieldControl_!=nurbsControl_||nurbsControl_>=s->controls.size()) {
        textEdit_.error="NURBS source or control changed; cancel and reopen the field";set_status(textEdit_.error,true);return;
    }
    std::istringstream in(textEdit_.buffer);in.imbue(std::locale::classic());double value=0;
    if(!(in>>value)||!std::isfinite(value)){textEdit_.error="Enter a finite number";set_status(textEdit_.error,true);return;}
    in>>std::ws;if(!in.eof()){textEdit_.error="Enter one finite number";set_status(textEdit_.error,true);return;}
    auto q=s->controls[nurbsControl_];switch(textEdit_.axis){case 0:q.x=value;break;case 1:q.y=value;break;case 2:q.z=value;break;case 3:q.weight=value;break;default:return;}
    if(edit_nurbs_control(nurbsControl_,q)){textEdit_={};nurbsFieldBefore_.reset();}
}
bool NativeEditorController::nurbs_pointer_down(PointerButton button,int x,int y){
    if(button!=PointerButton::Primary||!nurbsEditingObject_||workspace_.mode()!=EditorMode::Edit||playSession_.active()||
       workspace_.selected_object()!=nurbsEditingObject_)return false;
    auto* o=workspace_.document().find_object(nurbsEditingObject_);std::string error;
    if(!o||!o->flags.visible||o->flags.locked||is_isolated_out(o->id))return false;
    auto s=editor_nurbs_source(*o,&error);if(!s)return false;
    auto v=layout_.viewport;if(!v.contains(x,y))return false;
    for(int i=0;i<4;++i)if(field(v,i).contains(x,y))return begin_nurbs_field(unsigned(i));
    auto p=panel(v);if(p.contains(x,y)){
        if(y>=p.y+195){if(x<p.x+p.width/2)return dispatch_nurbs_action("nurbs.previous");return dispatch_nurbs_action("nurbs.next");}return true;
    }
    if(!v.contains(x,y))return false;
    float best=100;std::optional<std::size_t> hit;
    for(std::size_t i=0;i<s->controls.size();++i){
        auto c=s->controls[i];auto screen=project_world_to_screen(camera_,v,voxel_to_world(o->transform,o->voxelScale,{float(c.x),float(c.y),float(c.z)}));
        float d=(screen.x-float(x))*(screen.x-float(x))+(screen.y-float(y))*(screen.y-float(y));
        if(screen.visible&&d<=best){best=d;hit=i;}
    }
    if(hit){nurbsControl_=*hit;return true;}return false;
}
void NativeEditorController::draw_nurbs(const IEditorCanvas& canvas,UiRect viewport) const{
    if(!nurbsEditingObject_||workspace_.mode()!=EditorMode::Edit||playSession_.active()||workspace_.selected_object()!=nurbsEditingObject_)return;
    const auto* o=workspace_.document().find_object(nurbsEditingObject_);std::string error;
    if(!o||!o->flags.visible||is_isolated_out(o->id))return;
    auto s=editor_nurbs_source(*o,&error);if(!s||nurbsControl_>=s->controls.size())return;
    auto screen=[&](std::size_t i){auto q=s->controls[i];return project_world_to_screen(camera_,viewport,voxel_to_world(o->transform,o->voxelScale,{float(q.x),float(q.y),float(q.z)}));};
    constexpr EditorColor cyan=0xffe0d060,bright=0xff70dfff,bg=0xff28211a,white=0xfff5eee0;
    for(unsigned j=0;j<s->rows;++j)for(unsigned i=0;i<s->columns;++i){
        auto k=j*s->columns+i;auto a=screen(k);
        auto line=[&](std::size_t other){auto b=screen(other);if(a.visible&&b.visible)canvas.line(int(a.x),int(a.y),int(b.x),int(b.y),cyan,1);};
        if(i+1<s->columns)line(k+1);
        if(j+1<s->rows)line(k+s->columns);
        if(a.visible)canvas.fill({int(a.x)-3,int(a.y)-3,7,7},k==nurbsControl_?bright:cyan);
    }
    auto rect=panel(viewport);RectClipCanvas painter(canvas,rect);painter.fill(rect,bg);
    painter.text(rect.x+8,rect.y+20,"NURBS controls (X-ray)",white);
    painter.text(rect.x+8,rect.y+43,"CV "+std::to_string(nurbsControl_+1)+" / "+std::to_string(s->controls.size())+"   Enter applies; Esc cancels",white);
    auto q=s->controls[nurbsControl_];double values[]={q.x,q.y,q.z,q.weight};const char* names[]={"X","Y","Z","Weight"};
    for(int i=0;i<4;++i){auto f=field(viewport,i);painter.outline(f,cyan);
        auto value=textEdit_.kind==TextEditKind::NurbsControl&&textEdit_.axis==i?textEdit_.buffer:number(values[i]);
        painter.text(f.x+5,f.y+18,elide_text_to_width(painter,std::string(names[i])+": "+value,f.width-10),white);
    }
    painter.text(rect.x+8,rect.y+215,"< Previous CV              Next CV >",white);
    painter.text(rect.x+8,rect.y+234,"NURBS menu > Finish closes controls",white);
}
}

