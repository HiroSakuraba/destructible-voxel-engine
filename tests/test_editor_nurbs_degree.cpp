#include "dve/editor_native.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace dve;
using namespace dve::editor;
namespace {
void check(bool b,const std::string& e){if(!b)throw std::runtime_error(e);}
void near(double a,double b,double tolerance=3e-8){check(std::abs(a-b)<tolerance,"degree elevation numeric mismatch");}
void compare(const NurbsSource& a,const NurbsSource& b){
    for(unsigned j=0;j<=(a.is_curve()?0U:10U);++j)for(unsigned i=0;i<=36;++i){double u=std::lerp(a.u.knots[a.u.degree],a.u.knots[a.columns],double(i)/36);double v=a.is_curve()?0:std::lerp(a.v.knots[a.v.degree],a.v.knots[a.rows],double(j)/10);auto x=evaluate_nurbs(a,u,v),y=evaluate_nurbs(b,u,v);check(x&&y,"evaluate elevation");for(unsigned k=0;k<3;++k){near(x->position[k],y->position[k]);near(x->du[k],y->du[k],3e-7);near(x->dv[k],y->dv[k],3e-7);}}
}
NurbsSource surface(){NurbsSource s;s.columns=5;s.rows=3;s.u={2,{2,2,2,3,4.5,6,6,6}};s.v={1,{10,10,12,20,20}};s.textureU={2,5};s.textureV={-1,2};for(unsigned j=0;j<3;++j)for(unsigned i=0;i<5;++i)s.controls.push_back({double(i),.1*double(i*j),double(j),1+.1*i+.2*j});return s;}
void geometry(){
    std::string error;
    for(unsigned degree=1;degree<7;++degree){NurbsSource a;a.columns=degree+1;a.u.degree=degree;a.u.knots.assign(degree+1,2);a.u.knots.insert(a.u.knots.end(),degree+1,5);for(unsigned i=0;i<=degree;++i)a.controls.push_back({double(i),.2*std::sin(double(i)),0,1e250*(1+.1*i)});auto b=elevate_nurbs_degree(a,false,&error);check(bool(b),error);check(b->u.degree==degree+1&&b->columns==degree+2,"Bezier degree/count");check(b->controls.front()==a.controls.front()&&b->controls.back()==a.controls.back(),"endpoints remain exact");compare(a,*b);check(decode_nurbs(encode_nurbs(*b))==b,"elevated codec");}
    auto circle=make_nurbs_arc();auto cubic=elevate_nurbs_degree(circle,false,&error);check(bool(cubic),error);check(cubic->columns==13&&cubic->u.degree==3,"circle Bezier decomposition");compare(circle,*cubic);check(tessellate_nurbs(*cubic,1,&error).has_value(),error);
    auto a=surface();for(bool v:{false,true}){auto b=elevate_nurbs_degree(a,v,&error);check(bool(b),error);compare(a,*b);check(b->textureU==a.textureU&&b->textureV==a.textureV&&b->samplesPerSpan==a.samplesPerSpan,"display metadata retained");check(tessellate_nurbs(*b,2,&error).has_value(),error);}
    auto repeated=a;for(unsigned p=3;p<=7;++p){auto next=elevate_nurbs_degree(repeated,false,&error);check(bool(next),error);check(next->u.degree==p,"successive elevations");compare(a,*next);repeated=*next;}check(!elevate_nurbs_degree(repeated,false),"degree cap refuses");check(!elevate_nurbs_degree(circle,true),"curve V refuses");
    auto bad=a;bad.controls[0].weight=0;check(!elevate_nurbs_degree(bad,false),"invalid weights refuse");
    NurbsSource large;large.columns=large.rows=64;large.samplesPerSpan=1;large.u={1,{0,0}};large.v={1,{0,0}};for(unsigned i=1;i<63;++i){large.u.knots.push_back(double(i));large.v.knots.push_back(double(i));}large.u.knots.insert(large.u.knots.end(),2,63);large.v.knots.insert(large.v.knots.end(),2,63);for(unsigned j=0;j<64;++j)for(unsigned i=0;i<64;++i)large.controls.push_back({double(i),0,double(j),1});check(validate_nurbs(large),"control budget fixture");check(!elevate_nurbs_degree(large,false),"elevated control budget refuses");
    // Mixed-degree input can be made compatible explicitly, then joined/lofted.
    NurbsSource line;line.columns=2;line.u={1,{0,0,1,1}};line.controls={{0,0,0,1},{1,0,0,1}};auto quadratic=elevate_nurbs_degree(line,false);check(bool(quadratic),"raise line");auto second=*quadratic;for(auto& p:second.controls)p.x+=1;check(!join_nurbs_curves(std::array{line,second}),"mixed join initially refuses");auto joined=join_nurbs_curves(std::array{*quadratic,second},&error);check(bool(joined),error);
    for(auto& p:second.controls)p.y+=2;check(loft_nurbs(std::array{*quadratic,second},&error).has_value(),error);
}
void native(){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Degree elevation")));auto& c=*owner;c.resize(1600,1100);auto original=surface();auto object=make_editor_nurbs(1,original);check(bool(object),"make native surface");object->transform.position={5,3,1};object->voxelScale={2,1,.5F};object->polygon->materials[0].name="Retained";object->polygon->contentHash=polygon_asset_content_hash(*object->polygon);c.workspace().document().add_object(std::move(*object));c.workspace().select_object(1);check(c.dispatch_action("nurbs.edit")&&c.select_nurbs_controls({1,2},SelectionOperation::Replace),"native selection");
    auto* o=c.workspace().document().find_object(1);const auto hash=o->polygon->contentHash;check(c.dispatch_action("nurbs.source_elevate_u"),"native raise U");auto elevated=editor_nurbs_source(*o);check(elevated&&elevated->u.degree==3&&c.selected_nurbs_controls().empty(),"new net clears selection");compare(original,*elevated);near(o->transform.position.x,5);near(o->voxelScale.z,.5);check(o->polygon->materials[0].name=="Retained","materials retained");check(c.dispatch_action("edit.undo")&&editor_nurbs_source(*o)==original&&o->polygon->contentHash==hash&&c.selected_nurbs_controls()==std::set<std::size_t>{1,2},"undo source/cache/selection");check(c.dispatch_action("edit.redo")&&editor_nurbs_source(*o)==elevated,"redo elevated net");check(c.dispatch_action("nurbs.source_elevate_v"),"native raise V");
    const auto root=std::filesystem::temp_directory_path()/"dve_nurbs_degree_regression";std::filesystem::remove_all(root);std::filesystem::create_directories(root);std::string error;auto save=c.workspace().document().save_transactional(root/"scene.dvescene");check(save.success,save.error);auto loaded=EditorDocument::load(root/"scene.dvescene",&error);check(bool(loaded),error);check(editor_nurbs_source(*loaded->find_object(1))==editor_nurbs_source(*o),"reopened elevated source");std::filesystem::remove_all(root);
    o->flags.locked=true;auto history=c.workspace().commands().size();check(!c.dispatch_action("nurbs.source_elevate_u")&&c.workspace().commands().size()==history,"locked elevation refuses atomically");o->flags.locked=false;check(c.workspace().document().validate(),"final native cache valid");
}
}
int main(){try{geometry();native();std::cout<<"NURBS degree elevation tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
