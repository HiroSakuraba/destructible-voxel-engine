#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
namespace dve::editor {
namespace {
UiRect construction_panel(UiRect v){return {v.x+8,v.y+8,std::min(430,std::max(1,v.width-16)),std::min(340,std::max(1,v.height-16))};}
UiRect construction_field(UiRect v,int i){auto r=construction_panel(v);return {r.x+8,r.y+125+32*i,r.width-16,28};}
std::string number(double x){std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(10)<<x;return out.str();}
}
bool NativeEditorController::dispatch_nurbs_construction(std::string_view action){
    if(textEdit_.kind==TextEditKind::NurbsControl)cancel_text_edit();
    if(action=="nurbs.construct_close"){nurbsConstructionPanel_=false;if(textEdit_.kind==TextEditKind::NurbsControl)cancel_text_edit();return true;}
    nurbsConstructionPanel_=true;nurbsEditingObject_=0;nurbsSelected_.clear();
    if(action=="nurbs.construct_settings")return true;
    if(action=="nurbs.construct_clear"){nurbsConstructionInputs_.clear();set_status("Construction snapshots cleared");return true;}
    std::string error;
    if(action=="nurbs.construct_add"){
        auto id=workspace_.selected_object();const auto* o=id?workspace_.document().find_object(*id):nullptr;
        if(!o||workspace_.selection_count()!=1||!o->flags.visible||o->flags.locked||o->prefabLink||is_isolated_out(o->id)||nurbsConstructionInputs_.size()>=64){set_status("Select one visible unlocked owned curve; queue limit 64",true);return false;}
        auto s=editor_nurbs_source(*o,&error);if(!s||!s->is_curve()){set_status("Append requires a native NURBS curve",true);return false;}
        if(nurbsConstructionInputs_.empty())nurbsConstructionOrigin_=o->transform.position;
        const auto origin=o->transform.position,anchor=nurbsConstructionOrigin_;
        std::array<double,3> delta{double(origin.x)-anchor.x,double(origin.y)-anchor.y,double(origin.z)-anchor.z};
        // Use transformed unit axes but accumulate source coordinates in double.
        const auto x=rotate(o->transform.rotation,{1,0,0}),y=rotate(o->transform.rotation,{0,1,0}),z=rotate(o->transform.rotation,{0,0,1});
        for(auto& p:s->controls){const auto q=p;p.x=delta[0]+q.x*o->voxelScale.x*x.x+q.y*o->voxelScale.y*y.x+q.z*o->voxelScale.z*z.x;p.y=delta[1]+q.x*o->voxelScale.x*x.y+q.y*o->voxelScale.y*y.y+q.z*o->voxelScale.z*z.y;p.z=delta[2]+q.x*o->voxelScale.x*x.z+q.y*o->voxelScale.y*y.z+q.z*o->voxelScale.z*z.z;}
        if(!validate_nurbs(*s,&error)){set_status(error,true);return false;}
        nurbsConstructionInputs_.push_back({std::move(*s),delta,o->name});
        set_status("Appended snapshot "+std::to_string(nurbsConstructionInputs_.size())+"; order is append order, inputs are retained");return true;
    }
    if(action=="nurbs.construct_reverse"||action=="nurbs.construct_seam"){
        if(nurbsConstructionInputs_.empty()){set_status("Append a curve snapshot first",true);return false;}
        auto& input=nurbsConstructionInputs_.back();auto r=action=="nurbs.construct_reverse"?reverse_nurbs_curve(input.source,&error):shift_nurbs_seam(input.source,&error);
        if(!r){set_status(error,true);return false;}input.source=std::move(*r);input.name+=action=="nurbs.construct_reverse"?" [reversed]":" [seam +1]";set_status("Changed last snapshot only; scene source retained");return true;
    }
    std::optional<NurbsSource> result;
    if(action=="nurbs.construct_loft"){
        std::vector<NurbsSource> profiles;for(const auto& input:nurbsConstructionInputs_)profiles.push_back(input.source);
        result=loft_nurbs(profiles,&error);
    }else if(action=="nurbs.construct_sweep"){
        if(nurbsConstructionInputs_.size()!=2){set_status("Sweep needs exactly two snapshots: profile, then rail",true);return false;}
        auto options=nurbsSweepOptions_;options.profileOrigin=nurbsConstructionInputs_[0].origin;
        result=sweep_nurbs(nurbsConstructionInputs_[0].source,nurbsConstructionInputs_[1].source,options,&error);
    }else return false;
    if(!result){set_status(error,true);return false;}
    if(!create_nurbs_source(*result,action=="nurbs.construct_loft"?"NURBS Ruled Loft":"NURBS Sampled Sweep",nurbsConstructionOrigin_))return false;
    nurbsConstructionPanel_=false;
    set_status(action=="nurbs.construct_loft"?"Ruled NURBS loft created; input snapshots retained":"Sampled NURBS sweep created (approximate between stations); input snapshots retained");return true;
}
void NativeEditorController::commit_nurbs_construction_field(){
    std::istringstream in(textEdit_.buffer);in.imbue(std::locale::classic());double value;
    if(!(in>>value)||!std::isfinite(value)){textEdit_.error="Enter a finite number";set_status(textEdit_.error,true);return;}in>>std::ws;if(!in.eof()){textEdit_.error="Enter one number";set_status(textEdit_.error,true);return;}
    const int axis=textEdit_.axis;
    if((axis==4&&(value<2||value>64||value!=std::floor(value)))||(axis==5&&std::abs(value)>23040)||(axis==6&&(value<1e-6||value>1e6))){textEdit_.error="Sections 2..64 integer; twist +/-23040 degrees; scale 0.000001..1000000";set_status(textEdit_.error,true);return;}
    if(axis==4)nurbsSweepOptions_.sections=unsigned(value);
    else if(axis==5)nurbsSweepOptions_.twistRadians=value*3.141592653589793/180;
    else if(axis==6)nurbsSweepOptions_.endScale=value;
    textEdit_={};set_status("Sweep settings updated; construct a new result to apply");
}
bool NativeEditorController::nurbs_construction_pointer(PointerButton button,int x,int y){
    if(!nurbsConstructionPanel_||workspace_.mode()!=EditorMode::Edit||playSession_.active()||button!=PointerButton::Primary)return false;
    const auto v=layout_.viewport,r=construction_panel(v);if(!r.contains(x,y))return false;
    focusRegion_=EditorFocusRegion::Viewport;
    const double values[]={double(nurbsSweepOptions_.sections),nurbsSweepOptions_.twistRadians*180/3.141592653589793,nurbsSweepOptions_.endScale};
    for(int i=0;i<3;++i)if(construction_field(v,i).contains(x,y)){begin_text_edit(TextEditKind::NurbsControl,0,number(values[i]));textEdit_.axis=i+4;return true;}
    return true;
}
void NativeEditorController::draw_nurbs_construction(const IEditorCanvas& canvas,UiRect viewport) const{
    if(workspace_.mode()!=EditorMode::Edit||playSession_.active())return;
    auto r=construction_panel(viewport);RectClipCanvas p(canvas,r);constexpr EditorColor white=0xfff5eee0,cyan=0xffe0d060;p.fill(r,0xff28211a);
    p.text(r.x+8,r.y+20,"NURBS construction: ordered snapshots",white);
    p.text(r.x+8,r.y+40,"Inputs: "+std::to_string(nurbsConstructionInputs_.size())+" (append in loft order; sweep: profile, rail)",white);
    for(std::size_t i=0;i<std::min<std::size_t>(3,nurbsConstructionInputs_.size());++i){auto index=nurbsConstructionInputs_.size()>3?nurbsConstructionInputs_.size()-3+i:i;p.text(r.x+8,r.y+62+int(i)*19,elide_text_to_width(p,std::to_string(index+1)+": "+nurbsConstructionInputs_[index].name,r.width-16),white);}
    const char* names[]={"Sweep sections (uniform parameter)","Sweep twist (degrees)","Sweep end scale"};
    double values[]={double(nurbsSweepOptions_.sections),nurbsSweepOptions_.twistRadians*180/3.141592653589793,nurbsSweepOptions_.endScale};
    for(int i=0;i<3;++i){auto f=construction_field(viewport,i);p.outline(f,cyan);auto value=textEdit_.kind==TextEditKind::NurbsControl&&textEdit_.axis==i+4?textEdit_.buffer:number(values[i]);p.text(f.x+5,f.y+18,elide_text_to_width(p,std::string(names[i])+": "+value,f.width-10),white);}
    p.text(r.x+8,r.y+244,"Enter applies; Esc cancels. Build from NURBS menu.",white);
    p.text(r.x+8,r.y+265,"Loft: ruled V; equal degrees, explicit direction/seams.",white);
    p.text(r.x+8,r.y+286,"Sweep: planar section normal to rail start tangent.",white);
    p.text(r.x+8,r.y+307,"Sweep approximates rail; no closed rails or live links.",white);
    p.text(r.x+8,r.y+328,"NURBS > Close Construction Settings hides this panel.",white);
}
}
