#include "dve/editor_native.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace dve;
using namespace dve::editor;
namespace {
void check(bool b,const std::string& message){if(!b)throw std::runtime_error(message);}
void near(double a,double b,double tolerance=2e-8){check(std::abs(a-b)<tolerance,"numeric mismatch "+std::to_string(a)+" / "+std::to_string(b));}
NurbsSource surface(){
    NurbsSource s;s.columns=4;s.rows=5;s.u={2,{2,2,2,3,6,6,6}};s.v={3,{10,10,10,10,12,20,20,20,20}};
    for(unsigned j=0;j<s.rows;++j)for(unsigned i=0;i<s.columns;++i)s.controls.push_back({double(i),.1*double(i*j),double(j),1+.07*double(i+2*j)});
    return s;
}
void compare(const NurbsSample& a,const NurbsSample& b){for(unsigned i=0;i<3;++i){near(a.position[i],b.position[i]);near(a.du[i],b.du[i]);near(a.dv[i],b.dv[i]);}}
void geometry(){
    std::string error;const auto s=surface();check(validate_nurbs(s,&error),error);
    for(bool v:{false,true})for(double fraction:{.17,.5,.8}){
        const auto& axis=v?s.v:s.u;const unsigned count=v?s.rows:s.columns;
        double t=std::lerp(axis.knots[axis.degree],axis.knots[count],fraction);
        auto pieces=split_nurbs(s,v,t,&error);check(bool(pieces),error);
        check(tessellate_nurbs(pieces->first,1,&error).has_value(),error);check(tessellate_nurbs(pieces->second,2,&error).has_value(),error);
        for(const auto* part:{&pieces->first,&pieces->second}){
            check(decode_nurbs(encode_nurbs(*part))==*part,"split codec");
            for(unsigned j=0;j<=10;++j)for(unsigned i=0;i<=10;++i){double u=std::lerp(part->u.knots[part->u.degree],part->u.knots[part->columns],double(i)/10);double w=std::lerp(part->v.knots[part->v.degree],part->v.knots[part->rows],double(j)/10);auto a=evaluate_nurbs(*part,u,w),b=evaluate_nurbs(s,u,w);check(a&&b,"split evaluation");compare(*a,*b);}
        }
    }
    // Extraction at a boundary, an existing knot and arbitrary interior values.
    for(bool v:{false,true})for(double t:(v?std::vector<double>{10,12,13.7,20}:std::vector<double>{2,3,4.3,6})){
        auto curve=extract_nurbs_isocurve(s,v,t,&error);check(bool(curve),error);check(curve->is_curve(),"isocurve source kind");
        for(unsigned i=0;i<=30;++i){double q=std::lerp(curve->u.knots[curve->u.degree],curve->u.knots[curve->columns],double(i)/30);auto a=evaluate_nurbs(*curve,q),b=evaluate_nurbs(s,v?q:t,v?t:q);check(a&&b,"isocurve evaluation");for(unsigned k=0;k<3;++k){near(a->position[k],b->position[k]);near(a->du[k],v?b->du[k]:b->dv[k]);}}
        check(tessellate_nurbs(*curve,3,&error).has_value(),error);
    }
    for(bool v:{false,true}){
        auto r=reverse_nurbs_axis(s,v,&error);check(bool(r),error);check(reverse_nurbs_axis(*r,v)==s,"reversal involution");
        for(unsigned j=0;j<=8;++j)for(unsigned i=0;i<=8;++i){double u=2+4*double(i)/8,w=10+10*double(j)/8;auto a=evaluate_nurbs(*r,u,w),b=evaluate_nurbs(s,v?u:8-u,v?30-w:w);for(unsigned k=0;k<3;++k){near(a->position[k],b->position[k]);near(a->du[k],(v?1:-1)*b->du[k]);near(a->dv[k],(v?-1:1)*b->dv[k]);}}
    }
    auto transposed=transpose_nurbs(s,&error);check(bool(transposed),error);check(transpose_nurbs(*transposed)==s,"transpose involution");
    auto a=evaluate_nurbs(*transposed,14,4),b=evaluate_nurbs(s,4,14);for(unsigned k=0;k<3;++k){near(a->position[k],b->position[k]);near(a->du[k],b->dv[k]);near(a->dv[k],b->du[k]);}
    // Independent rational Bezier split fixtures across every supported degree.
    for(unsigned degree=1;degree<=7;++degree){
        NurbsSource curve;curve.columns=degree+1;curve.u.degree=degree;curve.u.knots.assign(degree+1,2);curve.u.knots.insert(curve.u.knots.end(),degree+1,5);
        for(unsigned i=0;i<=degree;++i)curve.controls.push_back({double(i),.1*double(i*i),0,1+.2*i});
        auto parts=split_nurbs(curve,false,3.17,&error);check(bool(parts),error);
        for(unsigned i=0;i<=40;++i){double t=2+3*double(i)/40;auto original=evaluate_nurbs(curve,t),piece=evaluate_nurbs(t<=3.17?parts->first:parts->second,t);check(original&&piece,"degree split evaluation");compare(*original,*piece);}
    }
    auto circle=make_nurbs_arc();auto halves=split_nurbs(circle,false,.25,&error);check(bool(halves),error);check(halves->first.columns==3&&halves->second.columns==7,"existing degree-multiplicity split");
    check(!split_nurbs(s,false,2)&&!split_nurbs(s,true,20),"endpoint split refuses");
    check(!split_nurbs(s,false,std::numeric_limits<double>::quiet_NaN()),"NaN refuses");
    check(!extract_nurbs_isocurve(circle,false,.5)&&!transpose_nurbs(circle)&&!reverse_nurbs_axis(circle,true),"curve-only limits");
    check(!extract_nurbs_isocurve(s,true,21),"outside isocurve domain refuses");
    // A source at the control cap may need an over-budget intermediate net.
    auto large=make_nurbs_patch();large.columns=64;large.rows=64;large.u={1,{0,0}};large.v={1,{0,0}};
    for(unsigned i=1;i<63;++i){large.u.knots.push_back(double(i));large.v.knots.push_back(double(i));}large.u.knots.insert(large.u.knots.end(),2,63);large.v.knots.insert(large.v.knots.end(),2,63);large.controls.clear();
    for(unsigned j=0;j<64;++j)for(unsigned i=0;i<64;++i)large.controls.push_back({double(i),0,double(j),1});large.samplesPerSpan=1;
    check(validate_nurbs(large),"budget fixture valid");check(!split_nurbs(large,false,.5),"intermediate net budget refuses");check(!extract_nurbs_isocurve(large,true,.5),"extraction intermediate budget refuses");
}
void native(){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("NURBS topology")));auto& c=*owner;c.resize(1600,1000);
    auto input=make_editor_nurbs(1,surface());check(bool(input),"make native source");input->transform.position={8,3,-2};input->transform.rotation=quaternion_from_axis_angle({0,1,0},.4F);input->voxelScale={2,1,.5F};input->tags={"source"};input->groups={"models"};input->layer=3;
    input->polygon->materials[0].name="Authored material";input->polygon->contentHash=polygon_asset_content_hash(*input->polygon);
    c.workspace().document().add_object(std::move(*input));c.workspace().select_object(1);check(c.dispatch_action("nurbs.edit"),"edit source");check(c.select_nurbs_controls({3,4},SelectionOperation::Replace),"select before reorder");
    const auto original=editor_nurbs_source(*c.workspace().document().find_object(1));
    check(c.dispatch_action("nurbs.source_reverse_u")&&c.selected_nurbs_controls().empty(),"reverse clears obsolete selection");check(c.dispatch_action("edit.undo"),"undo reverse");check(editor_nurbs_source(*c.workspace().document().find_object(1))==original&&c.selected_nurbs_controls()==std::set<std::size_t>{3,4},"undo source and controls");
    auto parameter=[&](const char* text){check(c.dispatch_action("nurbs.construct_settings"),"open operation settings");auto v=c.layout().viewport;c.pointer_down(PointerButton::Primary,v.x+25,v.y+8+125+3*32+12,0);check(c.text_edit().kind==TextEditKind::NurbsControl&&c.text_edit().axis==7,"parameter field opens");c.text_input(text);c.key_down("enter",false,false,false);};
    // Start an independent creation-history fixture after testing reverse undo.
    c.workspace().commands().clear();
    parameter("0.37");const auto beforeCount=c.workspace().document().objects().size(),beforeHistory=c.workspace().commands().size();
    check(c.dispatch_action("nurbs.source_split_v"),"native split");check(c.workspace().document().objects().size()==beforeCount+2&&c.workspace().commands().size()==beforeHistory+1,"two objects one undo");
    const auto first=*c.workspace().selected_object();auto* piece=c.workspace().document().find_object(first);check(piece&&piece->is_nurbs()&&piece->tags==std::vector<std::string>{"source"}&&piece->groups==std::vector<std::string>{"models"}&&piece->layer==3,"piece metadata");near(piece->transform.position.x,8);near(piece->voxelScale.z,.5);near(piece->transform.rotation.y,std::sin(.2),1e-6);check(piece->polygon->materials[0].name=="Authored material","split materials");
    auto pieceSource=editor_nurbs_source(*piece);near(pieceSource->v.knots.back(),13.7);check(editor_nurbs_source(*c.workspace().document().find_object(1))==original,"source retained");
    check(c.dispatch_action("edit.undo")&&c.workspace().document().objects().size()==beforeCount,"undo both pieces");check(c.dispatch_action("edit.redo")&&editor_nurbs_source(*c.workspace().document().find_object(first))==pieceSource,"redo both pieces");
    c.workspace().select_object(1);check(c.dispatch_action("nurbs.source_iso_v"),"native rational isocurve");auto* curve=c.workspace().document().find_object(*c.workspace().selected_object());check(editor_nurbs_source(*curve)->is_curve()&&!curve->flags.collisionEnabled&&curve->polygon->materials[0].name=="Authored material","isocurve resources");
    c.workspace().select_object(1);check(c.dispatch_action("nurbs.source_swap_uv"),"native transpose");check(c.dispatch_action("edit.undo"),"undo transpose");check(c.dispatch_action("nurbs.source_insert_u"),"parameterized insertion");check(editor_nurbs_source(*c.workspace().document().find_object(1))->columns==5,"parameterized refinement");check(c.dispatch_action("edit.undo"),"undo insertion");
    parameter("0");auto history=c.workspace().commands().size(),count=c.workspace().document().objects().size();check(!c.dispatch_action("nurbs.source_split_u")&&c.workspace().document().objects().size()==count&&c.workspace().commands().size()==history,"endpoint split atomic");check(c.dispatch_action("nurbs.source_iso_u"),"endpoint extraction supported");
    c.workspace().select_object(1);c.workspace().document().find_object(1)->flags.locked=true;check(!c.dispatch_action("nurbs.source_reverse_u"),"locked source refuses");c.workspace().document().find_object(1)->flags.locked=false;
    // First half fails its float bake: neither half may enter scene or history.
    auto circle=make_editor_nurbs(100,make_nurbs_arc());check(bool(circle),"tiny split fixture");c.workspace().document().add_object(std::move(*circle));c.workspace().select_object(100);parameter("1e-100");history=c.workspace().commands().size();count=c.workspace().document().objects().size();check(!c.dispatch_action("nurbs.source_split_u"),"float-collapsed half refuses");check(c.workspace().commands().size()==history&&c.workspace().document().objects().size()==count,"failed preparation publishes neither piece");
    NurbsSource longLine;longLine.columns=2;longLine.u={1,{0,0,1,1}};longLine.controls={{0,0,0,1},{0,1e6,0,1}};
    auto secondFailure=make_editor_nurbs(200,longLine);check(bool(secondFailure),"second-half failure fixture");c.workspace().document().add_object(std::move(*secondFailure));c.workspace().select_object(200);parameter("0.999999999999");history=c.workspace().commands().size();count=c.workspace().document().objects().size();check(!c.dispatch_action("nurbs.source_split_u"),"second half float bake refuses");check(c.workspace().commands().size()==history&&c.workspace().document().objects().size()==count,"second-half failure publishes neither piece");
    parameter("2");check(!c.text_edit().error.empty(),"invalid normalized parameter reports error");c.key_down("escape",false,false,false);
    check(c.workspace().document().validate(),"scene source/cache consistency");
}
}
int main(){try{geometry();native();std::cout<<"NURBS split/isocurve/axis tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
