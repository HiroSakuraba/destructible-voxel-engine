#include "dve/editor_native.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace dve;
using namespace dve::editor;
namespace {
void check(bool b,const std::string& e){if(!b)throw std::runtime_error(e);}
void near(double a,double b,double tol=1e-8){check(std::abs(a-b)<tol,"numeric mismatch: "+std::to_string(a)+" / "+std::to_string(b));}
NurbsSource line(){NurbsSource s;s.columns=2;s.u={1,{0,0,1,1}};s.controls={{0,0,0,1},{0,5,0,1}};return s;}
void geometry(){
    std::string e;auto a=make_nurbs_arc(1),b=make_nurbs_arc(2),c=make_nurbs_arc(.75);
    for(auto& p:b.controls)p.y=2;for(auto& p:c.controls)p.y=5;
    // Same geometry with a different domain, weight scale and refinement.
    b=*insert_nurbs_knot(b,false,.13);for(auto& k:b.u.knots)k=4+2*k;for(auto& p:b.controls)p.weight*=1e30;
    const std::vector<NurbsSource> ordered{a,b,c};auto loft=loft_nurbs(ordered,&e);check(bool(loft),e);
    check(loft->v.degree==1&&loft->rows==3,"ruled loft rows");
    for(unsigned j=0;j<3;++j)for(unsigned i=0;i<=40;++i){auto u=double(i)/40;auto q=evaluate_nurbs(*loft,u,double(j)/2);auto expected=evaluate_nurbs(ordered[j],j==1?4+2*u:u);check(q&&expected,"evaluate sections");for(unsigned k=0;k<3;++k)near(q->position[k],expected->position[k]);}
    check(tessellate_nurbs(*loft,1,&e).has_value(),e);check(decode_nurbs(encode_nurbs(*loft))==loft,"loft serialization");
    auto reversed=reverse_nurbs_curve(b,&e);check(bool(reversed),e);for(unsigned i=0;i<=30;++i){double u=4+2*double(i)/30;auto q=evaluate_nurbs(*reversed,u),r=evaluate_nurbs(b,10-u);check(q&&r,"reverse evaluate");for(unsigned k=0;k<3;++k){near(q->position[k],r->position[k]);near(q->du[k],-r->du[k],1e-7);}}
    auto shifted=shift_nurbs_seam(a,&e);check(bool(shifted),e);for(unsigned i=0;i<40;++i){double u=double(i)/40;auto q=evaluate_nurbs(*shifted,u),r=evaluate_nurbs(a,std::fmod(u+.25,1));for(unsigned k=0;k<3;++k)near(q->position[k],r->position[k]);}
    auto restored=a;for(unsigned i=0;i<4;++i)restored=*shift_nurbs_seam(restored);check(restored==a,"four seam shifts restore exact net");
    check(!shift_nurbs_seam(b),"nonuniform seam shift refuses");check(!shift_nurbs_seam(line()),"open seam refuses");
    auto wrong=ordered;wrong[1]=line();check(!loft_nurbs(wrong),"mixed degree refuses");check(!loft_nurbs(std::span<const NurbsSource>{}),"empty loft refuses");
    wrong.assign(65,a);check(!loft_nurbs(wrong),"profile count cap");
    auto rail=line();NurbsSweepOptions options;options.sections=9;options.endScale=2;options.twistRadians=1.5707963267948966;
    auto sweep=sweep_nurbs(a,rail,options,&e);check(bool(sweep),e);
    for(unsigned j=0;j<options.sections;++j){const double f=double(j)/(options.sections-1),angle=f*options.twistRadians;auto q=evaluate_nurbs(*sweep,0,f);near(q->position[0],(1+f)*std::cos(angle));near(q->position[1],5*f);near(q->position[2],-(1+f)*std::sin(angle));}
    check(tessellate_nurbs(*sweep,2,&e).has_value(),e);
    // Cubic inflection: finite transport without a Frenet-normal flip.
    rail.columns=4;rail.u={3,{0,0,0,0,1,1,1,1}};rail.controls={{0,0,0,1},{0,2,0,1},{2,3,0,1},{2,5,0,1}};
    options={};options.sections=33;sweep=sweep_nurbs(a,rail,options,&e);check(bool(sweep),e);
    for(unsigned j=0;j<options.sections;++j){double f=double(j)/(options.sections-1);auto center=evaluate_nurbs(rail,f);auto q=evaluate_nurbs(*sweep,0,f);std::array<double,3> d;double len=0,projection=0;for(unsigned k=0;k<3;++k){d[k]=q->position[k]-center->position[k];len+=d[k]*d[k];projection+=d[k]*center->du[k];}near(len,1);near(projection,0,1e-7);if(j){auto prev=sweep->controls[(j-1)*a.columns];auto now=sweep->controls[j*a.columns];check(std::hypot(now.x-prev.x,now.y-prev.y,now.z-prev.z)<.5,"transport frame continuity");}}
    check(tessellate_nurbs(*sweep,3,&e).has_value(),e);
    check(!sweep_nurbs(a,a),"closed rail refuses");auto stationary=line();stationary.controls[1]=stationary.controls[0];check(!sweep_nurbs(a,stationary),"stationary rail refuses");
    auto nonplanar=a;nonplanar.controls[2].y=.1;check(!sweep_nurbs(nonplanar,line()),"nonplanar profile refuses");
    options.sections=1;check(!sweep_nurbs(a,line(),options),"section cap");options={};options.endScale=0;check(!sweep_nurbs(a,line(),options),"zero scale refuses");options={};options.twistRadians=std::numeric_limits<double>::quiet_NaN();check(!sweep_nurbs(a,line(),options),"NaN refuses");
    auto dense=a;dense.samplesPerSpan=1;
    for(unsigned i=0;i<56;++i){auto refined=insert_nurbs_knot(dense,false,.001*(i+1));check(bool(refined),"dense profile fixture");dense=*refined;}
    options={};options.sections=64;check(!sweep_nurbs(dense,line(),options),"sweep control budget");
    wrong.assign(64,dense);check(!loft_nurbs(wrong),"loft control budget");
    auto tinyDomain=line();tinyDomain.u.knots={0,0,std::numeric_limits<double>::denorm_min(),std::numeric_limits<double>::denorm_min()};
    check(!sweep_nurbs(a,tinyDomain),"unevaluable rail endpoints refuse without dereference");
    {   // Codex P1: equal knots from different domains must coalesce (no artificial near-zero span).
        auto p0=*insert_nurbs_knot(make_nurbs_arc(1),false,.1);auto p1=p0;for(auto& k:p1.u.knots)k=k==.1?1.2:1+2*k;for(auto& p:p1.controls)p.y=2;
        const std::vector<NurbsSource> pair{p0,p1};auto coalesced=loft_nurbs(pair,&e);check(bool(coalesced),e);
        check(coalesced->columns==p0.columns,"loft inserted an artificial near-duplicate knot");
        check(tessellate_nurbs(*coalesced,4,&e).has_value(),e);
    }
    {   // Codex P2: a valid rail over a huge knot domain is not stationary.
        auto wide=line();for(auto& k:wide.u.knots)k*=1e13;auto ok=sweep_nurbs(make_nurbs_arc(1),wide,{},&e);check(bool(ok),"large-domain rail refused: "+e);
    }
    auto reversal=line();reversal.columns=3;reversal.u={1,{0,0,.5,1,1}};reversal.controls={{0,0,0,1},{0,2,0,1},{0,1,0,1}};options={};options.sections=3;check(!sweep_nurbs(a,reversal,options),"antiparallel tangent refuses");
}
void native(){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Construction")));auto& c=*owner;c.resize(1600,1000);
    auto add=[&](EditorObjectId id,NurbsSource s,Float3 position,Float3 scale,Quaternion rotation={}){auto o=make_editor_nurbs(id,s);check(bool(o),"make input");o->transform.position=position;o->transform.rotation=rotation;o->voxelScale=scale;c.workspace().document().add_object(std::move(*o));c.workspace().select_object(id);check(c.dispatch_action("nurbs.construct_add"),"append snapshot");};
    const auto circle=make_nurbs_arc();add(1,circle,{10,0,0},{2,1,3});add(2,circle,{10,4,0},{1,1,1},quaternion_from_axis_angle({0,1,0},.7F));
    auto history=c.workspace().commands().size();check(c.dispatch_action("nurbs.construct_loft"),"native loft");auto id=*c.workspace().selected_object();auto* o=c.workspace().document().find_object(id);auto s=editor_nurbs_source(*o);check(bool(s),"native loft source");near(o->transform.position.x,10);auto q=evaluate_nurbs(*s,0,0);near(q->position[0],2);q=evaluate_nurbs(*s,0,1);near(q->position[0],std::cos(.7),1e-6);near(q->position[1],4);near(q->position[2],-std::sin(.7),1e-6);
    check(c.workspace().commands().size()==history+1,"one creation undo");check(c.dispatch_action("edit.undo")&&!c.workspace().document().find_object(id),"undo loft");check(c.dispatch_action("edit.redo")&&editor_nurbs_source(*c.workspace().document().find_object(id))==s,"redo loft source");near(c.workspace().document().find_object(id)->transform.position.x,10);
    check(editor_nurbs_source(*c.workspace().document().find_object(1))==circle,"input source retained");
    check(c.dispatch_action("nurbs.construct_clear"),"clear");check(!c.dispatch_action("nurbs.construct_sweep"),"missing sweep inputs refused");
    c.workspace().select_object(1);check(c.dispatch_action("nurbs.construct_add"),"sweep section");add(20,line(),{10,0,0},{1,1,1});
    check(c.dispatch_action("nurbs.construct_settings"),"settings");auto v=c.layout().viewport;
    auto enter=[&](int i,const char* value){c.pointer_down(PointerButton::Primary,v.x+25,v.y+8+125+32*i+12,0);check(c.text_edit().kind==TextEditKind::NurbsControl&&c.text_edit().axis==i+4,"construction field opens");c.text_input(value);c.key_down("enter",false,false,false);};
    enter(0,"8");enter(1,"90");enter(2,"2");check(c.dispatch_action("nurbs.construct_sweep"),"native sweep");o=c.workspace().document().find_object(*c.workspace().selected_object());s=editor_nurbs_source(*o);check(s&&s->rows==8,"section count field");q=evaluate_nurbs(*s,0,1);near(q->position[0],0,1e-7);near(q->position[1],5);near(q->position[2],-4);
    check(c.workspace().document().validate(),"constructed scene valid");
    // Snapshot is independent of later edits to the retained input curve.
    auto changed=circle;changed.controls[0].x=1.1;check(replace_editor_nurbs_source(*c.workspace().document().find_object(1),changed),"change input");check(c.dispatch_action("nurbs.construct_sweep"),"rebuild snapshot");check(editor_nurbs_source(*c.workspace().document().find_object(*c.workspace().selected_object()))==s,"snapshot result deterministic");
    check(c.dispatch_action("nurbs.construct_settings"),"reopen");enter(0,"1");c.key_down("escape",false,false,false);check(c.dispatch_action("nurbs.construct_sweep"),"invalid sections leave settings unchanged");check(editor_nurbs_source(*c.workspace().document().find_object(*c.workspace().selected_object()))->rows==8,"invalid setting atomic");
}
}
int main(){try{geometry();native();std::cout<<"NURBS construction tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
