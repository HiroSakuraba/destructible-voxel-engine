#include "dve/editor_native.hpp"
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace dve;using namespace dve::editor;
namespace {
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
void near(double a,double b,double tolerance=2e-7){check(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<tolerance,"Boundary numeric mismatch: "+std::to_string(a)+" / "+std::to_string(b));}
bool v_edge(NurbsBoundary edge){return edge==NurbsBoundary::VMin||edge==NurbsBoundary::VMax;}
bool min_edge(NurbsBoundary edge){return edge==NurbsBoundary::UMin||edge==NurbsBoundary::VMin;}
NurbsBoundary opposite(NurbsBoundary edge){return static_cast<NurbsBoundary>(static_cast<unsigned>(edge)^1U);}
NurbsSample sample(const NurbsSource& source,NurbsBoundary edge,double t){
    const auto& cross=v_edge(edge)?source.v:source.u;const auto& tangent=v_edge(edge)?source.u:source.v;
    const double a=min_edge(edge)?cross.knots.front():cross.knots.back(),b=std::lerp(tangent.knots.front(),tangent.knots.back(),t);
    auto value=evaluate_nurbs(source,v_edge(edge)?b:a,v_edge(edge)?a:b);check(bool(value),"Boundary evaluation");return *value;
}
std::array<double,3> cross_derivative(const NurbsSource& source,NurbsBoundary edge,const NurbsSample& value,bool reference){
    auto derivative=v_edge(edge)?value.dv:value.du;const auto& axis=v_edge(edge)?source.v:source.u;
    const double sign=(min_edge(edge)?-1.:1.)*(reference?1.:-1.);for(auto& x:derivative)x*=sign*(axis.knots.back()-axis.knots.front());return derivative;
}
NurbsSource low(){NurbsSource source;source.columns=source.rows=2;source.u={1,{2,2,6,6}};source.v={1,{10,10,16,16}};
    source.controls={{-1,0,1,1},{1,0,1,1.02},{-1,.05,-1,1.01},{1,.1,-1,1.03}};source.textureU={2,7};source.textureV={-3,5};return source;}
NurbsSource high(){auto source=make_nurbs_patch(2);source.controls[5].y=.08;for(unsigned j=0;j<4;++j)for(unsigned i=0;i<4;++i)source.controls[j*4+i].weight=1+.01*i+.015*j;
    source=*insert_nurbs_knot(source,false,.23);source=*insert_nurbs_knot(source,true,.61);source.textureU={3,8};source.textureV={4,9};
    for(auto& x:source.u.knots)x=std::lerp(-3.,7.,x);for(auto& x:source.v.knots)x=std::lerp(4.,9.,x);return source;}
void geometry(){
    const auto reference=high(),moving=low();const auto referenceBytes=encode_nurbs(reference),movingBytes=encode_nurbs(moving);
    for(unsigned a=0;a<4;++a)for(unsigned b=0;b<4;++b)for(bool reverse:{false,true})for(bool c1:{false,true}){
        NurbsAttachmentOptions options{static_cast<NurbsBoundary>(a),static_cast<NurbsBoundary>(b),reverse,c1};std::string error;
        auto result=attach_nurbs_boundaries(reference,moving,options,&error);check(bool(result),error);
        check(result->moving.textureU==moving.textureU&&result->moving.textureV==moving.textureV,"Moving texture ranges retained");
        check(result->moving.u.knots.front()==moving.u.knots.front()&&result->moving.u.knots.back()==moving.u.knots.back()&&result->moving.v.knots.front()==moving.v.knots.front()&&result->moving.v.knots.back()==moving.v.knots.back(),"Moving parameter domains retained");
        check((v_edge(options.movingBoundary)?result->moving.u.degree:result->moving.v.degree)==3,"Tangential degree prepared");
        for(unsigned i=0;i<=20;++i){const double t=double(i)/20;auto expected=sample(reference,options.referenceBoundary,reverse?1-t:t),actual=sample(result->moving,options.movingBoundary,t);
            auto far=sample(moving,opposite(options.movingBoundary),t),farResult=sample(result->moving,opposite(options.movingBoundary),t);
            for(unsigned k=0;k<3;++k){near(expected.position[k],actual.position[k]);near(far.position[k],farResult.position[k]);}
            if(c1){auto da=cross_derivative(reference,options.referenceBoundary,expected,true),db=cross_derivative(result->moving,options.movingBoundary,actual,false);for(unsigned k=0;k<3;++k)near(da[k],db[k]);}
        }
        for(unsigned j=0;j<=5;++j)for(unsigned i=0;i<=5;++i){auto u=std::lerp(reference.u.knots.front(),reference.u.knots.back(),double(i)/5),v=std::lerp(reference.v.knots.front(),reference.v.knots.back(),double(j)/5);
            auto before=evaluate_nurbs(reference,u,v),after=evaluate_nurbs(result->reference,u,v);for(unsigned k=0;k<3;++k){near(before->position[k],after->position[k]);near(before->du[k],after->du[k]);near(before->dv[k],after->dv[k]);}}
        check(decode_nurbs(encode_nurbs(result->moving))==result->moving,"Matched source codec");
    }
    check(encode_nurbs(reference)==referenceBytes&&encode_nurbs(moving)==movingBytes,"Input sources never changed");
    auto a=make_nurbs_patch(2),b=a;a=*insert_nurbs_knot(a,true,.25);b=*insert_nurbs_knot(b,true,.75);b=*insert_nurbs_knot(b,true,.75);b=*insert_nurbs_knot(b,true,.2500000001);
    auto result=attach_nurbs_boundaries(a,b);check(bool(result),"Knot multiplicity union");
    for(auto x:{.25,.75,.2500000001})check(std::count(result->reference.v.knots.begin(),result->reference.v.knots.end(),x)==std::count(result->moving.v.knots.begin(),result->moving.v.knots.end(),x),"Distinct knots/multiplicities retained exactly");
    check(std::count(result->moving.v.knots.begin(),result->moving.v.knots.end(),.75)==2,"Repeated knots kept");
    auto displayReference=reference,displayMoving=moving;displayReference.adaptiveDisplay=true;displayMoving.adaptiveDisplay=true;displayMoving.chordTolerance=.025;displayMoving.normalAngle=.2;
    auto display=attach_nurbs_boundaries(displayReference,displayMoving);check(display&&display->reference.adaptiveDisplay&&display->moving.adaptiveDisplay&&display->moving.chordTolerance==.025&&display->moving.normalAngle==.2,"Adaptive policy retained");
    auto huge=reference,tiny=moving;for(auto& p:huge.controls)p.weight*=1e250;for(auto& p:tiny.controls)p.weight*=1e-200;
    check(attach_nurbs_boundaries(huge,tiny,{NurbsBoundary::VMin,NurbsBoundary::UMax,true,true}).has_value(),"Independent common weight scales");
    check(!attach_nurbs_boundaries(make_nurbs_arc(),moving),"Curve input refuses");check(!attach_nurbs_boundaries(reference,moving,{static_cast<NurbsBoundary>(9),NurbsBoundary::UMin}),"Invalid boundary refuses");
    auto precision=*insert_nurbs_knot(make_nurbs_patch(),false,.5);
    for(auto& knot:precision.u.knots)knot=knot==0?-1e308:knot==1?1e-10:0;
    std::string precisionError;check(validate_nurbs(precision)&&!attach_nurbs_boundaries(precision,moving,{},&precisionError)&&precisionError.find("lose precision")!=std::string::npos,"Unrepresentable normalized spans refuse");
    auto bad=moving;bad.controls[0].weight=0;check(!attach_nurbs_boundaries(reference,bad),"Invalid source refuses");
    auto weights=make_nurbs_patch();for(unsigned j=0;j<weights.rows;++j)weights.controls[j*weights.columns+weights.columns-2].weight=4;
    check(!attach_nurbs_boundaries(weights,make_nurbs_patch(),{NurbsBoundary::UMax,NurbsBoundary::UMin,false,true}),"Nonpositive C1 weight refuses");
    NurbsSource grid;grid.columns=grid.rows=64;grid.samplesPerSpan=1;grid.u={1,{0,0}};grid.v=grid.u;
    for(unsigned i=1;i<63;++i){grid.u.knots.push_back(double(i)/63);grid.v.knots.push_back(double(i)/63);}grid.u.knots.insert(grid.u.knots.end(),2,1);grid.v.knots.insert(grid.v.knots.end(),2,1);
    for(unsigned j=0;j<64;++j)for(unsigned i=0;i<64;++i)grid.controls.push_back({double(i),0,double(j),1});
    std::string error;check(validate_nurbs(grid),"Budget fixture valid");check(!attach_nurbs_boundaries(reference,grid,{},&error)&&error.find("4096")!=std::string::npos,"Compatibility budget refuses before publication");
}
void native(){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Boundary attachment")));auto& c=*owner;c.resize(1600,1000);
    auto reference=make_nurbs_patch(2);auto moving=low();moving=*transpose_nurbs(moving);for(auto& p:moving.controls)p.z+=4;
    auto add=[&](EditorObjectId id,const NurbsSource& source){auto object=make_editor_nurbs(id,source);check(bool(object),"Native fixture");object->transform.position={3,2,-4};object->transform.rotation=quaternion_from_axis_angle({0,1,0},.3F);object->voxelScale={id==1?2.F:1.F,.5F,1};object->polygon->materials[0].name=id==1?"Reference":"Moving material";object->polygon->contentHash=polygon_asset_content_hash(*object->polygon);c.workspace().document().add_object(std::move(*object));};
    add(1,reference);add(2,moving);
    const auto history=c.workspace().commands().size();
    check(c.dispatch_action("nurbs.attach_reference_vmin")&&c.dispatch_action("nurbs.attach_moving_umax"),"Boundary settings actions");
    check(c.dispatch_action("nurbs.attach_reverse")&&c.dispatch_action("nurbs.attach_reverse"),"Twist toggle");
    const auto& menus=std::as_const(c.workspace().menus());check(menus.find("nurbs.attach_reference_vmin")->checked&&!menus.find("nurbs.attach_reference_umax")->checked&&menus.find("nurbs.attach_moving_umax")->checked&&!menus.find("nurbs.attach_reverse")->checked,"Boundary radio/toggle state");
    check(c.workspace().commands().size()==history,"Options do not edit geometry/history");
    auto select=[&]{c.workspace().select_object(2);c.workspace().add_to_selection(1);};
    for(auto action:{"nurbs.source_attach_c0","nurbs.source_attach_c1"}){
        select();const auto count=c.workspace().document().objects().size();auto* a=c.workspace().document().find_object(1);auto* b=c.workspace().document().find_object(2);const auto oldA=encode_nurbs(*editor_nurbs_source(*a)),oldB=encode_nurbs(*editor_nurbs_source(*b));
        check(c.dispatch_action(action),std::string(action)+": "+c.status().text);auto id=*c.workspace().selected_object();auto* result=c.workspace().document().find_object(id);auto source=editor_nurbs_source(*result);check(source&&validate_editor_nurbs(*result),"Native source/cache valid");
        check(result->polygon->materials[0].name=="Moving material"&&result->transform.position.x==3&&result->voxelScale.x==2,"Moving appearance / active placement retained");
        for(unsigned i=0;i<=16;++i){auto expected=sample(reference,NurbsBoundary::VMin,double(i)/16),actual=sample(*source,NurbsBoundary::UMax,double(i)/16);for(unsigned k=0;k<3;++k)near(expected.position[k],actual.position[k],1e-6);}
        check(encode_nurbs(*editor_nurbs_source(*a))==oldA&&encode_nurbs(*editor_nurbs_source(*b))==oldB,"Native originals retained");
        auto hash=result->polygon->contentHash;check(c.workspace().document().objects().size()==count+1,"One generated patch");
        check(c.dispatch_action("edit.undo")&&c.workspace().selected_objects()==std::set<EditorObjectId>{1,2}&&c.workspace().document().objects().size()==count,"Undo restores inputs/selection");
        check(c.dispatch_action("edit.redo")&&c.workspace().document().find_object(id)->polygon->contentHash==hash,"Redo cache stable");
    }
    select();c.workspace().document().find_object(2)->flags.locked=true;auto count=c.workspace().document().objects().size(),size=c.workspace().commands().size();c.refresh_menu_state();check(!menus.find("nurbs.source_attach_c0")->enabled&&!c.dispatch_action("nurbs.source_attach_c0")&&c.workspace().commands().size()==size&&c.workspace().document().objects().size()==count,"Locked input refuses atomically");c.workspace().document().find_object(2)->flags.locked=false;
    const auto root=std::filesystem::temp_directory_path()/"dve_boundary_attachment_test";std::filesystem::remove_all(root);std::filesystem::create_directories(root);auto saved=c.workspace().document().save_transactional(root/"scene.dvescene");check(saved.success,saved.error);std::string error;auto loaded=EditorDocument::load(root/"scene.dvescene",&error);check(loaded&&loaded->validate(),"Generated patches save/reopen");std::filesystem::remove_all(root);
}
}
int main(){try{geometry();native();std::cout<<"NURBS boundary attachment tests passed\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
