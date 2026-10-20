#include "dve/editor_native.hpp"
#include "dve/voxel_metric.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace dve;
using namespace dve::editor;
namespace {
void check(bool b,const std::string& e){if(!b)throw std::runtime_error(e);}
void near(double a,double b,double t=2e-8){check(std::abs(a-b)<t,"numeric mismatch "+std::to_string(a)+" / "+std::to_string(b));}
void compare_piece(const NurbsSource& source,const NurbsSource& joined,unsigned segment){
    const double lo=source.u.knots[source.u.degree],hi=source.u.knots[source.columns];
    for(unsigned j=0;j<=30;++j){const double f=double(j)/30;auto a=evaluate_nurbs(source,std::lerp(lo,hi,f)),b=evaluate_nurbs(joined,segment+f);check(a&&b,"joined evaluation");for(unsigned k=0;k<3;++k){near(a->position[k],b->position[k]);if(j&&j<30)near(a->du[k]*(hi-lo),b->du[k],2e-7);}}
}
void geometry(){
    std::string error;auto patch=make_nurbs_patch();patch.controls[5].y=.7;patch.controls[10].weight=1.5;
    auto edges=extract_nurbs_boundaries(patch,&error);check(bool(edges),error);
    for(unsigned i=0;i<4;++i){const auto& edge=(*edges)[i];auto next=(*edges)[(i+1)%4].controls.front();const auto last=edge.controls.back();check(last.x==next.x&&last.y==next.y&&last.z==next.z,"oriented boundary loop endpoints");
        for(unsigned j=0;j<=20;++j){double f=double(j)/20;double u=i==0?f:i==1?1:i==2?1-f:0;double v=i==0?0:i==1?f:i==2?1:1-f;auto a=evaluate_nurbs(edge,f),b=evaluate_nurbs(patch,u,v);for(unsigned k=0;k<3;++k)near(a->position[k],b->position[k]);}
    }
    auto loop=join_nurbs_curves(*edges,&error);check(bool(loop),error);check(loop->controls.front()==loop->controls.back(),"joined boundary loop geometrically closes");check(tessellate_nurbs(*loop,1,&error).has_value(),error);
    for(unsigned i=0;i<4;++i)compare_piece((*edges)[i],*loop,i);
    for(unsigned degree=1;degree<=7;++degree){
        std::vector<NurbsSource> curves;
        for(unsigned segment=0;segment<3;++segment){NurbsSource s;s.columns=degree+1;s.u.degree=degree;const double lo=10+10*segment,hi=lo+2+segment;s.u.knots.assign(degree+1,lo);s.u.knots.insert(s.u.knots.end(),degree+1,hi);
            for(unsigned i=0;i<=degree;++i)s.controls.push_back({segment+double(i)/degree,(i==0||i==degree)?0:.1*std::sin(double(i)),0,(segment%2?1e-100:1e100)*(1+.1*i)});curves.push_back(s);
        }
        auto joined=join_nurbs_curves(curves,&error);check(bool(joined),error);check(joined->columns==3*degree+1,"shared join controls");check(joined->u.knots.back()==3&&joined->u.degree==degree,"unit interval per input");check(decode_nurbs(encode_nurbs(*joined))==joined,"joined codec");
        for(unsigned i=0;i<curves.size();++i)compare_piece(curves[i],*joined,i);
    }
    auto circle=make_nurbs_arc();auto first=split_nurbs(circle,false,.37,&error);check(bool(first),error);auto second=split_nurbs(first->second,false,.81,&error);check(bool(second),error);
    std::vector<NurbsSource> pieces{first->first,second->first,second->second};auto joined=join_nurbs_curves(pieces,&error);check(bool(joined),error);for(unsigned i=0;i<3;++i)compare_piece(pieces[i],*joined,i);
    auto wrong=pieces;wrong[1].controls.front().x=std::nextafter(wrong[1].controls.front().x,1e8);check(!join_nurbs_curves(wrong),"even tiny endpoint gaps refuse without snapping");
    wrong=pieces;wrong[1]=*reverse_nurbs_curve(wrong[1]);check(!join_nurbs_curves(wrong),"wrong direction refuses");
    check(!join_nurbs_curves(std::span<const NurbsSource>{})&&!extract_nurbs_boundaries(circle),"input kind and count refuse");
    auto mixed=std::vector<NurbsSource>{(*edges)[0],make_nurbs_arc()};check(!join_nurbs_curves(mixed),"degree mismatch refuses");
    NurbsSource chain;chain.columns=2;chain.u={1,{0,0,1,1}};chain.controls={{0,0,0,1},{0,0,0,1e8}};
    std::vector<NurbsSource> ill{chain,chain};check(!join_nurbs_curves(ill),"cross-join weight conditioning refuses");
    auto tooMany=std::vector<NurbsSource>(65,chain);check(!join_nurbs_curves(tooMany),"curve count budget refuses");
    NurbsSource many;many.columns=66;many.samplesPerSpan=1;many.u={1,{0,0}};for(unsigned i=1;i<65;++i)many.u.knots.push_back(double(i));many.u.knots.insert(many.u.knots.end(),2,65);many.controls.resize(66);check(validate_nurbs(many),"large join input valid");auto excessive=std::vector<NurbsSource>(64,many);check(!join_nurbs_curves(excessive),"joined control budget refuses");
    // Boundary extraction supports unequal U/V degrees and domains.
    auto surface=extrude_nurbs(make_nurbs_arc(),{0,2,0});check(bool(surface),"boundary fixture");auto nonuniformEdges=extract_nurbs_boundaries(*surface,&error);check(bool(nonuniformEdges),error);check((*nonuniformEdges)[0].u.degree==2&&(*nonuniformEdges)[1].u.degree==1,"boundary axes retain degrees");check(!join_nurbs_curves(*nonuniformEdges),"mixed-degree boundary loop requires explicit elevation");
}
void boundaries_native(){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Boundary extraction")));auto& c=*owner;c.resize(1600,1100);
    auto original=make_nurbs_patch();auto object=make_editor_nurbs(1,original);check(bool(object),"make patch");object->transform.position={3,4,5};object->voxelScale={2,1,.5F};object->flags.collisionEnabled=true;object->tags={"surface"};object->groups={"shapes"};object->layer=2;object->polygon->materials[0].name="Authored";object->polygon->contentHash=polygon_asset_content_hash(*object->polygon);c.workspace().document().add_object(std::move(*object));c.workspace().select_object(1);
    check(c.dispatch_action("nurbs.source_boundaries"),"native four boundaries");check(c.workspace().document().objects().size()==5&&c.workspace().commands().size()==1,"four objects one undo");check(*c.workspace().selected_object()==2,"select first boundary");
    for(EditorObjectId id=2;id<=5;++id){const auto* edge=c.workspace().document().find_object(id);check(edge&&editor_nurbs_source(*edge)->is_curve()&&!edge->flags.collisionEnabled,"native curve source");near(edge->transform.position.x,3);near(edge->voxelScale.z,.5);check(edge->polygon->materials[0].name=="Authored"&&edge->tags==std::vector<std::string>{"surface"}&&edge->groups==std::vector<std::string>{"shapes"}&&edge->layer==2,"boundary resources and membership");}
    check(editor_nurbs_source(*c.workspace().document().find_object(1))==original,"surface retained");check(c.dispatch_action("edit.undo")&&c.workspace().document().objects().size()==1,"undo four boundaries");check(c.dispatch_action("edit.redo")&&c.workspace().document().objects().size()==5,"redo four boundaries");
    // A valid surface whose second guide ribbon collapses at float precision.
    NurbsSource large;large.columns=large.rows=2;large.samplesPerSpan=1;large.u={1,{0,0,1,1}};large.v=large.u;large.controls={{1e8-16,0,0,1},{1e8,0,0,1},{1e8-16,4,0,1},{1e8,4,0,1}};
    auto badEdges=extract_nurbs_boundaries(large);check(bool(badEdges),"large boundary source");check(tessellate_nurbs((*badEdges)[0],10).has_value()&&!tessellate_nurbs((*badEdges)[1],11),"second boundary fails after first prepares");
    auto big=make_editor_nurbs(100,large);check(bool(big),"valid large surface");c.workspace().document().add_object(std::move(*big));c.workspace().select_object(100);auto count=c.workspace().document().objects().size(),history=c.workspace().commands().size();check(!c.dispatch_action("nurbs.source_boundaries"),"bad boundary bake refuses");check(c.workspace().document().objects().size()==count&&c.workspace().commands().size()==history,"boundary preparation publishes none on failure");check(c.workspace().document().validate(),"boundary scene validates");
}
void join_native(){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Curve joining")));auto& c=*owner;c.resize(1600,1100);
    auto halves=split_nurbs(make_nurbs_arc(),false,.5);check(bool(halves),"split join input");std::array<NurbsSource,2> inputs{halves->first,halves->second};
    for(unsigned i=0;i<2;++i){auto object=make_editor_nurbs(i+1,inputs[i]);check(bool(object),"make join source");object->transform.position={8,3,5};object->transform.rotation=quaternion_from_axis_angle({0,1,0},.4F);object->voxelScale={2,1,.5F};c.workspace().document().add_object(std::move(*object));c.workspace().select_object(i+1);check(c.dispatch_action("nurbs.construct_add"),"append ordered input");}
    auto count=c.workspace().document().objects().size();check(c.dispatch_action("nurbs.construct_join"),"native join");const auto id=*c.workspace().selected_object();auto* object=c.workspace().document().find_object(id);auto joined=editor_nurbs_source(*object);check(joined&&joined->is_curve(),"joined editable source");near(object->transform.position.x,8);check(c.workspace().document().objects().size()==count+1&&c.workspace().commands().size()==1,"joined object one undo");
    for(unsigned segment=0;segment<2;++segment)for(unsigned j=0;j<=20;++j){double f=double(j)/20;const auto& input=inputs[segment];auto a=evaluate_nurbs(input,std::lerp(input.u.knots[input.u.degree],input.u.knots[input.columns],f));auto world=voxel_to_world(c.workspace().document().find_object(segment+1)->transform,{2,1,.5F},{float(a->position[0]),float(a->position[1]),float(a->position[2])});auto b=evaluate_nurbs(*joined,segment+f);near(b->position[0]+8,world.x,2e-6);near(b->position[1]+3,world.y,2e-6);near(b->position[2]+5,world.z,2e-6);}
    check(editor_nurbs_source(*c.workspace().document().find_object(1))==inputs[0]&&editor_nurbs_source(*c.workspace().document().find_object(2))==inputs[1],"join inputs retained");check(c.dispatch_action("edit.undo")&&!c.workspace().document().find_object(id),"undo join");check(c.dispatch_action("edit.redo")&&editor_nurbs_source(*c.workspace().document().find_object(id))==joined,"redo joined source");
    check(c.dispatch_action("nurbs.construct_clear"),"clear snapshots");for(unsigned i=0;i<2;++i){c.workspace().select_object(i+1);check(c.dispatch_action("nurbs.construct_add"),"append for direction refusal");}check(c.dispatch_action("nurbs.construct_reverse"),"reverse last snapshot");count=c.workspace().document().objects().size();auto history=c.workspace().commands().size();check(!c.dispatch_action("nurbs.construct_join"),"wrong endpoint direction refuses");check(c.workspace().document().objects().size()==count&&c.workspace().commands().size()==history,"failed join leaves scene and history");check(c.dispatch_action("nurbs.construct_reverse")&&c.dispatch_action("nurbs.construct_join"),"correct orientation recovers without changing sources");check(c.workspace().document().validate(),"joined scene validates");
}
}
int main(){try{geometry();boundaries_native();join_native();std::cout<<"NURBS boundary/join tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
