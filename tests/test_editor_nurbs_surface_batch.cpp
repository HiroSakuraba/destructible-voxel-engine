#include "dve/editor_native.hpp"
#include "dve/editor_nurbs_seam.hpp"
#include "dve/editor_nurbs_surface.hpp"
#include "dve/nurbs_topology.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace dve;using namespace dve::editor;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(#x)+":"+std::to_string(__LINE__));}while(false)
void run(EditorDocument& d,std::unique_ptr<IEditorCommand>& command){auto r=command->execute(d);if(!r.success)throw std::runtime_error(r.message);}
void add(EditorDocument& d,EditorObjectId id,double x=0){auto s=make_nurbs_patch(2);s.samplesPerSpan=4;for(auto& p:s.controls)p.x+=x;auto o=make_editor_nurbs(id,s);CHECK(o);(void)d.add_object(std::move(*o));}
std::string state(const EditorDocument& d){std::string out;for(const auto& [id,o]:d.objects()){if(has_nurbs_source(o)){out+=encode_nurbs(*editor_nurbs_source(o));out+=encode_nurbs_surface_features(*editor_nurbs_surface_features(o));out+=std::to_string(o.polygon->contentHash);}}return out;}
void shape(const NurbsSource& a,const NurbsSource& b){for(unsigned j=0;j<=16;++j)for(unsigned i=0;i<=16;++i){auto p=evaluate_nurbs(a,double(i)/16,double(j)/16),q=evaluate_nurbs(b,double(i)/16,double(j)/16);CHECK(p&&q);for(unsigned k=0;k<3;++k)CHECK(std::abs(p->position[k]-q->position[k])<1e-8);}}
NurbsSurfaceCurve uvloop(std::uint64_t id=1){NurbsSurfaceCurve c;c.id=id;c.samples=17;c.uv.columns=5;c.uv.u={1,{0,0,0.25,0.5,0.75,1,1}};c.uv.controls={{0.2,0.2,0,1},{0.8,0.2,0,1},{0.8,0.8,0,1},{0.2,0.8,0,1},{0.2,0.2,0,1}};return c;}
void network(){
    EditorDocument d("Refinement");add(d,1);add(d,2,2);add(d,3,4);std::string error;
    auto a=link_nurbs_seam_command(1,2,{NurbsBoundary::UMax,NurbsBoundary::UMin,false,true});run(d,a);auto b=link_nurbs_seam_command(2,3,{NurbsBoundary::UMax,NurbsBoundary::UMin,false,true});run(d,b);
    const auto original=state(d);std::vector<NurbsSource> sources;for(auto id:{1U,2U,3U})sources.push_back(*editor_nurbs_source(*d.find_object(id)));
    auto insert=refine_nurbs_network_command(2,true,0.37);run(d,insert);CHECK(d.validate(&error));
    for(unsigned id=1;id<=3;++id){auto source=*editor_nurbs_source(*d.find_object(id));shape(sources[id-1],source);CHECK(std::find(source.v.knots.begin(),source.v.knots.end(),0.37)!=source.v.knots.end());}
    CHECK(insert->undo(d).success&&state(d)==original);run(d,insert);
    auto elevation=refine_nurbs_network_command(3,true);run(d,elevation);CHECK(d.validate(&error));for(unsigned id=1;id<=3;++id){shape(sources[id-1],*editor_nurbs_source(*d.find_object(id)));CHECK(editor_nurbs_source(*d.find_object(id))->v.degree==4);}
    d.find_object(1)->flags.locked=true;const auto before=state(d);auto blocked=refine_nurbs_network_command(3,true,0.45);CHECK(!blocked->execute(d).success&&state(d)==before);d.find_object(1)->flags.locked=false;
    // Interior holes keep sewn boundaries and get a common schedule.
    NurbsTrimRegion hole;hole.loops.push_back({90,{{0.2,0.2},{0.8,0.2},{0.8,0.8},{0.2,0.8}}});hole.inverted=true;
    auto trim=trim_nurbs_surface_command(2,hole);run(d,trim);if(!d.validate(&error))throw std::runtime_error(error);
    auto fine=refine_nurbs_network_command(1,true,0.63);run(d,fine);CHECK(d.validate(&error));CHECK(editor_nurbs_surface_features(*d.find_object(2))->trim.has_value());
    auto snapshot=capture_nurbs_seam_drag(d,1);CHECK(snapshot);auto updated=*editor_nurbs_source(*d.find_object(1));updated.controls[5].y+=0.02;
    auto prepared=PreparedNurbsSeamEdit::prepare(snapshot,1,updated,&error);if(!prepared)throw std::runtime_error(error);CHECK(prepared->execute(d).success);CHECK(d.validate(&error));CHECK(prepared->undo(d).success);CHECK(d.validate(&error));
    auto removed=hole;removed.inverted=false;auto bad=trim_nurbs_surface_command(2,removed);auto old=state(d);CHECK(!bad->execute(d).success&&state(d)==old);
}
void rational_cross_axis(){
    EditorDocument d("Rational reversed axes");add(d,1);add(d,2,2);
    auto a=*editor_nurbs_source(*d.find_object(1));for(std::size_t i=0;i<a.controls.size();++i)a.controls[i].weight=1+0.03*double(i/a.columns);
    CHECK(replace_editor_nurbs_source(*d.find_object(1),a));auto b=*editor_nurbs_source(*d.find_object(2));b=*transpose_nurbs(b);b=*reverse_nurbs_axis(b,false);CHECK(replace_editor_nurbs_source(*d.find_object(2),b));
    auto link=link_nurbs_seam_command(1,2,{NurbsBoundary::UMax,NurbsBoundary::VMin,true,true});run(d,link);a=*editor_nurbs_source(*d.find_object(1));b=*editor_nurbs_source(*d.find_object(2));
    auto insert=refine_nurbs_network_command(2,false,0.31);run(d,insert);CHECK(d.validate());shape(a,*editor_nurbs_source(*d.find_object(1)));shape(b,*editor_nurbs_source(*d.find_object(2)));
    auto elevate=refine_nurbs_network_command(1,true);run(d,elevate);CHECK(d.validate());CHECK(editor_nurbs_source(*d.find_object(2))->u.degree==4);shape(a,*editor_nurbs_source(*d.find_object(1)));shape(b,*editor_nurbs_source(*d.find_object(2)));
}
void curves_trim_history(){
    EditorDocument d("Surface curves");add(d,1);auto baseline=*editor_nurbs_source(*d.find_object(1));const auto originalMesh=*d.find_object(1)->polygon;
    NurbsSurfaceFeatures empty,features;features.curves.push_back(uvloop());auto addCurve=set_nurbs_surface_features_command(1,empty,features);run(d,addCurve);
    auto guide=evaluate_editor_nurbs_surface_curve(d,1,1);CHECK(guide&&guide->size()==17);auto source=baseline;source.controls[5].y=0.4;
    auto edit=std::make_unique<EditNurbsCommand>(1,baseline,source);CHECK(edit->execute(d).success);auto moved=evaluate_editor_nurbs_surface_curve(d,1,1);CHECK(moved&&(*moved)[0].y>(*guide)[0].y);
    auto trim=trim_nurbs_surface_curve_command(1,1);run(d,trim);std::string error;CHECK(d.validate(&error));CHECK(*editor_nurbs_source(*d.find_object(1))==source);
    auto current=*editor_nurbs_surface_features(*d.find_object(1)),changed=current;changed.curves[0].uv.controls[1].x=0.7;
    auto uvEdit=set_nurbs_surface_features_command(1,current,changed);run(d,uvEdit);CHECK(d.validate(&error));CHECK(uvEdit->undo(d).success);
    auto preparedSource=source;preparedSource.controls[6].y+=0.1;auto prepared=EditNurbsCommand::prepare(*d.find_object(1),preparedSource,&error);if(!prepared)throw std::runtime_error(error);CHECK(prepared->execute(d).success);CHECK(d.validate(&error));CHECK(prepared->undo(d).success);
    auto region=select_nurbs_trim_region_command(1,{0.05,0.05});run(d,region);CHECK(d.validate(&error));CHECK(region->undo(d).success);
    auto untrim=trim_nurbs_surface_command(1,std::nullopt);run(d,untrim);CHECK(!editor_nurbs_surface_features(*d.find_object(1))->trim);CHECK(editor_nurbs_surface_features(*d.find_object(1))->curves.size()==1);CHECK(untrim->undo(d).success);
    const auto path=std::filesystem::temp_directory_path()/"dve_surface_batch.dvescene";auto save=d.save_transactional(path);if(!save.success)throw std::runtime_error(save.error);auto loaded=EditorDocument::load(path,&error);if(!loaded)throw std::runtime_error(error);CHECK(state(d)==state(*loaded));std::filesystem::remove(path);
    CHECK(trim->undo(d).success);CHECK(edit->undo(d).success);CHECK(addCurve->undo(d).success);CHECK(nurbs_display_geometry_matches(*d.find_object(1)->polygon,originalMesh));
    CHECK(!decode_nurbs_surface_features("DVE_NURBS_SURFACE_FEATURES 1 999"));
    CHECK(decode_nurbs_surface_features(encode_nurbs_surface_features(features))==features);
}
void projection_native(){
    auto controller=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Persistent project")));auto& c=*controller;c.resize(1600,1000);auto& d=c.workspace().document();add(d,1);
    auto uv=uvloop().uv;for(auto& p:uv.controls){const double x=p.x,z=p.y;p.x=2*x-1;p.y=1;p.z=2*z-1;}auto curve=make_editor_nurbs(2,uv);CHECK(curve);(void)d.add_object(std::move(*curve));
    c.workspace().select_object(2);c.workspace().add_to_selection(1);c.refresh_menu_state();CHECK(std::as_const(c.workspace().menus()).find("nurbs.source_project_persistent")->enabled);
    CHECK(c.dispatch_action("nurbs.source_project_persistent"));CHECK(d.objects().size()==2);auto features=editor_nurbs_surface_features(*d.find_object(1));CHECK(features&&features->curves.size()==1&&features->curves[0].projectionSource==2);
    c.workspace().select_object(1);CHECK(c.edit_nurbs_surface_uv_control(1,1,1,{0.75,0.2}));CHECK(c.dispatch_action("edit.undo"));
    CHECK(c.dispatch_action("nurbs.source_trim_curve"));CHECK(d.validate());
    const auto history=c.workspace().commands().size();CHECK(!c.dispatch_action("nurbs.source_crop"));CHECK(c.workspace().commands().size()==history);
    CHECK(c.dispatch_action("nurbs.source_adaptive"));auto baked=c.workspace().selected_object();CHECK(baked&&*baked!=1&&*baked!=2);const auto* result=d.find_object(*baked);CHECK(result&&result->polygon&&!has_nurbs_source(*result));CHECK(result->polygon->bounds.maximum.x-result->polygon->bounds.minimum.x<1.5F);
    CHECK(c.dispatch_action("edit.undo"));c.workspace().select_object(1);CHECK(c.dispatch_action("nurbs.source_untrim"));CHECK(d.validate());
    CHECK(c.dispatch_action("nurbs.source_curve_refresh"));CHECK(d.validate());
    auto a=*editor_nurbs_source(*d.find_object(1));add(d,3,2);auto link=link_nurbs_seam_command(1,3,{NurbsBoundary::UMax,NurbsBoundary::UMin,false,true});run(d,link);c.refresh_menu_state();CHECK(c.dispatch_action("nurbs.source_insert_v"));CHECK(d.validate());CHECK(c.dispatch_action("nurbs.source_elevate_v"));shape(a,*editor_nurbs_source(*d.find_object(1)));CHECK(d.validate());
}
int main(){try{network();rational_cross_axis();curves_trim_history();projection_native();std::cout<<"NURBS refinement/surface feature tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
