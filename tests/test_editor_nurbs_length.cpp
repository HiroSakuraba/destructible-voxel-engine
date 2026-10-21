#include "dve/editor_native.hpp"
#include "dve/nurbs_queries.hpp"
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
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
    o->polygon->vertices[0].position.x+=1;check(!c.dispatch_action("nurbs.source_length"),"Stale source cache refuses");
}
NurbsSource corner(){auto s=line();s.columns=3;s.u={1,{2,2,3,5,5}};s.controls={{0,0,0,1},{3,0,0,1},{3,4,0,1}};return s;}
void inversion(){
    std::string error;auto s=line();s.controls[1].weight=20;
    auto parameter=nurbs_parameter_at_length_fraction(s,.25,1e-8,{1,1,1},&error);check(bool(parameter),error);
    near(parameter->parameter,2+3*(.25/(20-19*.25)),1e-8);near(parameter->lengthBefore,1.25,1e-8);near(parameter->total.length,5,1e-8);
    check(parameter->estimatedResidual<=1e-8&&parameter->evaluations<=200000,"Bounded inverse diagnostics");
    auto c=corner();parameter=nurbs_parameter_at_length_fraction(c,.5,1e-8,{2,.5,1},&error);check(bool(parameter),error);near(parameter->parameter,2+2.0/3,1e-8);near(parameter->lengthBefore,4,1e-8);
    near(nurbs_parameter_at_length_fraction(c,0)->parameter,2);near(nurbs_parameter_at_length_fraction(c,1)->parameter,5);
    auto arc=make_nurbs_arc(3,std::numbers::pi/2);
    auto original=nurbs_parameter_at_length_fraction(arc,.25,1e-8);check(bool(original),"Rational arc inverse");
    auto sample=evaluate_nurbs(arc,original->parameter);near(sample->position[0],3*std::cos(std::numbers::pi/8),1e-8);near(sample->position[2],3*std::sin(std::numbers::pi/8),1e-8);
    for(auto other:{*insert_nurbs_knot(arc,false,.3),*elevate_nurbs_degree(arc,false)}){
        auto p=nurbs_parameter_at_length_fraction(other,.25,1e-8);check(bool(p),"Refined/elevated inverse");auto q=evaluate_nurbs(other,p->parameter);for(unsigned k=0;k<3;++k)near(q->position[k],sample->position[k],1e-8);
    }
    auto reversed=reverse_nurbs_axis(arc,false);auto reverseParameter=nurbs_parameter_at_length_fraction(*reversed,.75,1e-8);check(bool(reverseParameter),"Reverse inverse");auto reversePoint=evaluate_nurbs(*reversed,reverseParameter->parameter);for(unsigned k=0;k<3;++k)near(reversePoint->position[k],sample->position[k],1e-8);
    auto cusp=arc;cusp.columns=3;cusp.u={2,{0,0,0,1,1,1}};cusp.controls={{0,0,0,1},{1,0,0,1},{0,0,0,1}};near(nurbs_parameter_at_length_fraction(cusp,.5)->parameter,.5);
    for(auto& p:c.controls)p.weight=1e250;near(nurbs_parameter_at_length_fraction(c,.5,1e-7,{2,.5,1})->parameter,2+2.0/3);
    check(!nurbs_parameter_at_length_fraction(c,-.1)&&!nurbs_parameter_at_length_fraction(c,1.1)&&!nurbs_parameter_at_length_fraction(c,NAN),"Invalid fractions refuse");
    check(!nurbs_parameter_at_length_fraction(c,.5,0)&&!nurbs_parameter_at_length_fraction(c,.5,INFINITY),"Invalid inverse tolerance refuses");
    check(!nurbs_parameter_at_length_fraction(c,.5,1e-7,{0,0,0})&&!nurbs_parameter_at_length_fraction(c,.5,1e-7,{NAN,1,1}),"Zero/nonfinite inverse metric refuses");
    check(!nurbs_parameter_at_length_fraction(make_nurbs_patch(),.5),"Surface inverse refuses");
    check(!nurbs_parameter_at_length_fraction(arc,.37,1e-300),"Unattainable inverse tolerance refuses");
    for(auto& p:c.controls){p.x=0;p.y=0;}check(!nurbs_parameter_at_length_fraction(c,.5),"Constant inverse refuses");
}
void native_length_split(){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Length split")));auto& c=*owner;c.resize(1600,1000);
    auto source=corner();source.textureU={2,7};auto object=make_editor_nurbs(1,source);check(bool(object),"Split fixture");object->voxelScale={2,.5F,1};object->transform.position={8,3,-2};object->transform.rotation=quaternion_from_axis_angle({0,1,0},.4F);
    object->polygon->materials[0].name="Retained";object->polygon->contentHash=polygon_asset_content_hash(*object->polygon);
    c.workspace().document().add_object(std::move(*object));c.workspace().select_object(1);check(c.dispatch_action("nurbs.edit")&&c.select_nurbs_controls({1},SelectionOperation::Replace),"Split selection");
    auto fraction=[&](const char* text){check(c.dispatch_action("nurbs.construct_settings"),"Fraction settings");auto v=c.layout().viewport;c.pointer_down(PointerButton::Primary,v.x+25,v.y+8+125+3*32+12,0);check(c.text_edit().axis==7,"Length fraction field");c.text_input(text);c.key_down("enter",false,false,false);};
    fraction("0.5");
    const auto history=c.workspace().commands().size();check(c.dispatch_action("nurbs.source_split_length"),"Native half-length split");check(c.workspace().document().objects().size()==3&&c.workspace().commands().size()==history+1,"Two pieces one history item");
    const auto first=*c.workspace().selected_object();auto* a=c.workspace().document().find_object(first);auto part=editor_nurbs_source(*a);check(bool(part),"First split source");near(part->u.knots.back(),2+2.0/3,1e-6);near(part->textureU[1],2+5*(2.0/3)/3,1e-6);
    near(measure_nurbs_curve(*part,part->u.knots.front(),part->u.knots.back(),1e-7,{2,.5,1})->length,4,1e-6);check(a->polygon->materials[0].name=="Retained"&&a->transform.position.x==8&&a->voxelScale.y==.5F,"Split appearance/placement");
    check(editor_nurbs_source(*c.workspace().document().find_object(1))==source,"Original retained");
    check(c.dispatch_action("edit.undo")&&c.workspace().document().objects().size()==1&&c.workspace().selected_object()==1&&c.selected_nurbs_controls()==std::set<std::size_t>{1},"Undo split and selection");
    check(c.dispatch_action("edit.redo")&&editor_nurbs_source(*c.workspace().document().find_object(first))==part,"Redo source/cache");
    c.workspace().select_object(1);auto* o=c.workspace().document().find_object(1);o->flags.locked=true;const auto count=c.workspace().document().objects().size();const auto commands=c.workspace().commands().size();check(!c.dispatch_action("nurbs.source_split_length")&&c.workspace().commands().size()==commands&&c.workspace().document().objects().size()==count,"Locked split atomic");o->flags.locked=false;
    for(const char* endpoint:{"0","1"}){fraction(endpoint);check(!c.dispatch_action("nurbs.source_split_length")&&c.workspace().commands().size()==commands&&c.workspace().document().objects().size()==count,"Endpoint split atomic");}
    fraction("0.5");o->polygon->vertices[0].position.x+=1;check(!c.dispatch_action("nurbs.source_split_length")&&c.workspace().commands().size()==commands&&c.workspace().document().objects().size()==count,"Stale length split refuses");o->polygon->vertices[0].position.x-=1;
    check(c.workspace().document().validate(),"Split scene valid");
}
}
int main(){try{core();native();inversion();native_length_split();std::cout<<"NURBS length and inversion tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
