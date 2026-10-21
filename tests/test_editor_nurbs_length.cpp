#include "dve/editor_native.hpp"
#include "dve/nurbs_queries.hpp"
#include <cmath>
#include <iostream>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
using namespace dve;
using namespace dve::editor;
namespace {
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
void near(double a,double b,double tolerance=1e-7){check(std::abs(a-b)<tolerance,"Length mismatch");}
NurbsSource line(){NurbsSource s;s.columns=2;s.u={1,{2,2,5,5}};s.controls={{0,0,0,1},{3,4,0,1}};return s;}
void core(){
    std::string error;auto s=line();auto length=measure_nurbs_curve(s,2,5,1e-8,{1,1,1},&error);check(bool(length),error);near(length->length,5);near(length->estimatedError,0);check(length->evaluations>0,"Derivative sampled");
    near(measure_nurbs_curve(s,3,4)->length,5.0/3);near(measure_nurbs_curve(s,2,5,1e-7,{-2,.5,0})->length,std::sqrt(40.0));
    s.controls[1].weight=20;near(measure_nurbs_curve(s,2,5)->length,5);s=line();
    near(measure_nurbs_curve(s,2,5,1e-7,{0,0,0})->length,0);
    NurbsSource cusp;cusp.columns=3;cusp.u={2,{0,0,0,1,1,1}};cusp.controls={{0,0,0,1},{1,0,0,1},{0,0,0,1}};near(measure_nurbs_curve(cusp,0,1)->length,1);
    auto circle=make_nurbs_arc(3);auto arc=measure_nurbs_curve(circle,0,1,1e-8);check(bool(arc),"Circle converges");near(arc->length,6*std::numbers::pi);check(arc->estimatedError<=1e-8,"Summed estimate respects tolerance");
    auto refined=insert_nurbs_knot(circle,false,.13);check(bool(refined),"Refine circle");near(measure_nurbs_curve(*refined,0,1)->length,arc->length);
    circle.samplesPerSpan=1;circle.creaseNormals=false;near(measure_nurbs_curve(circle,0,1)->length,arc->length);
    auto elevated=elevate_nurbs_degree(circle,false);check(bool(elevated),"Elevate circle");near(measure_nurbs_curve(*elevated,0,1)->length,arc->length);
    auto reversed=reverse_nurbs_axis(circle,false);near(measure_nurbs_curve(*reversed,0,1)->length,arc->length);
    // A full-multiplicity corner has unequal left/right speeds: both spans count.
    s.columns=3;s.u={1,{2,2,3,5,5}};s.controls={{0,0,0,1},{3,0,0,1},{3,4,0,1}};
    near(measure_nurbs_curve(s,2,5)->length,7);near(measure_nurbs_curve(s,2.5,4)->length,3.5);
    for(auto& p:s.controls)p.weight=1e250;near(measure_nurbs_curve(s,2,5)->length,7);
    for(auto& p:s.controls){p.x=1;p.y=2;}near(measure_nurbs_curve(s,2,5)->length,0);
    check(measure_nurbs_curve(s,3,3)->evaluations==0,"Empty interval");
    check(!measure_nurbs_curve(make_nurbs_patch(),0,1),"Surface refuses");
    check(!measure_nurbs_curve(s,5,2)&&!measure_nurbs_curve(s,1,3)&&!measure_nurbs_curve(s,2,6),"Bad interval refuses");
    check(!measure_nurbs_curve(s,2,5,0)&&!measure_nurbs_curve(s,2,5,-1),"Bad tolerance refuses");
    check(!measure_nurbs_curve(s,2,5,1e-7,{NAN,1,1})&&!measure_nurbs_curve(s,NAN,5),"Nonfinite arguments refuse");
    s.controls[0].weight=0;check(!measure_nurbs_curve(s,2,5),"Invalid source refuses");
    check(!measure_nurbs_curve(circle,0,1,1e-300),"Unattainable tolerance refuses");
}
void native(){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Length")));auto& c=*owner;c.resize(1600,1100);
    auto object=make_editor_nurbs(1,line());check(bool(object),"Native curve");object->voxelScale={2,.5F,1};object->transform.position={100,20,3};
    c.workspace().document().add_object(std::move(*object));c.workspace().select_object(1);check(c.dispatch_action("nurbs.edit")&&c.select_nurbs_controls({0},SelectionOperation::Replace),"Select control");
    auto* o=c.workspace().document().find_object(1);const auto before=editor_nurbs_source(*o);const auto hash=o->polygon->contentHash;const auto history=c.workspace().commands().size();
    check(c.dispatch_action("nurbs.source_length"),"Native measurement");check(!c.status().error&&c.status().text.find("6.32455532")!=std::string::npos,"Scaled length status");
    check(c.workspace().commands().size()==history&&editor_nurbs_source(*o)==before&&o->polygon->contentHash==hash&&c.selected_nurbs_controls()==std::set<std::size_t>{0},"Read-only source/cache/history/selection");
    o->flags.locked=true;check(!c.dispatch_action("nurbs.source_length"),"Locked source refuses");o->flags.locked=false;
    o->flags.visible=false;check(!c.dispatch_action("nurbs.source_length")&&c.workspace().commands().size()==history,"Hidden source refuses");o->flags.visible=true;
    o->prefabLink=EditorPrefabLink{};check(!c.dispatch_action("nurbs.source_length")&&c.workspace().commands().size()==history,"Prefab source refuses");o->prefabLink.reset();
    auto patch=make_editor_nurbs(2,make_nurbs_patch());check(bool(patch),"Native patch");c.workspace().document().add_object(std::move(*patch));c.workspace().select_object(2);c.refresh_menu_state();
    auto* item=std::as_const(c.workspace().menus()).find("nurbs.source_length");check(item&&!item->enabled&&item->disabledReason.find("curves only")!=std::string::npos,"Surface menu says why");
    check(!c.dispatch_action("nurbs.source_length")&&c.workspace().commands().size()==history,"Surface refuses without history");c.workspace().select_object(1);
    o->polygon->vertices[0].position.x+=1;check(!c.dispatch_action("nurbs.source_length"),"Stale source cache refuses");
}
}
int main(){try{core();native();std::cout<<"NURBS length tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
