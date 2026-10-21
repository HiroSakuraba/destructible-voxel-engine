#include "dve/editor_native.hpp"
#include "dve/nurbs_modeling.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>
using namespace dve;using namespace dve::editor;
namespace {
void check(bool b,const std::string& message){if(!b)throw std::runtime_error(message);}
void near(double a,double b,double t=1e-7){check(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=t,"Numeric mismatch: "+std::to_string(a)+" vs "+std::to_string(b));}
NurbsSource line(double y=0){NurbsSource s;s.columns=2;s.u={1,{0,0,1,1}};s.controls={{-1,y,0,1},{1,y,0,1}};return s;}
void loft_fit(){
    std::vector<NurbsSource> profiles;for(unsigned j=0;j<4;++j){auto s=make_nurbs_arc(1,std::numbers::pi/2);for(auto& p:s.controls)p.y=.2*j*j;profiles.push_back(s);}
    std::string error;auto loft=smooth_loft_nurbs(profiles,&error);check(bool(loft),error);check(loft->v.degree==3&&loft->rows==10,"Cubic loft shape");
    for(unsigned j=0;j<4;++j)for(unsigned i=0;i<=8;++i){auto a=evaluate_nurbs(*loft,double(i)/8,double(j)),b=evaluate_nurbs(profiles[j],double(i)/8);check(a&&b,"Loft evaluation");for(unsigned k=0;k<3;++k)near(a->position[k],b->position[k]);}
    for(unsigned j=1;j<3;++j){auto a=evaluate_nurbs(*loft,.3,j-1e-7),b=evaluate_nurbs(*loft,.3,j+1e-7);for(unsigned k=0;k<3;++k)near(a->dv[k],b->dv[k],1e-6);}
    for(unsigned j=1;j<3;++j){const double h=1e-4;auto a=evaluate_nurbs(*loft,.3,j-h),aa=evaluate_nurbs(*loft,.3,j-2*h),b=evaluate_nurbs(*loft,.3,j+h),bb=evaluate_nurbs(*loft,.3,j+2*h);for(unsigned k=0;k<3;++k)near((a->dv[k]-aa->dv[k])/h,(bb->dv[k]-b->dv[k])/h,1e-3);}
    check(tessellate_nurbs(*loft,1,&error).has_value(),error);
    std::vector<NurbsSource> bad;for(double w:{1.,.001,.001,1.}){auto s=line();s.controls[0].weight=w;bad.push_back(s);}check(!smooth_loft_nurbs(bad),"Nonpositive smooth weights refuse");check(!smooth_loft_nurbs({}),"Insufficient profiles refuse");
    std::vector<std::array<double,3>> points;for(unsigned i=0;i<65;++i){double t=double(i)/64;points.push_back({t,t*t,t*t*t});}
    auto fit=fit_nurbs_curve(points,4,1e-8,&error);check(bool(fit),error);check(fit->maxSampledError<1e-8,"Polynomial fit error");for(unsigned i=0;i<17;++i){double t=double(i)/16;auto p=evaluate_nurbs(fit->source,t);near(p->position[0],t);near(p->position[1],t*t);near(p->position[2],t*t*t);}
    auto circle=make_nurbs_arc();circle.textureU={2,5};auto rebuilt=rebuild_nurbs_curve(circle,24,.01,&error);check(bool(rebuilt),error);check(rebuilt->source.textureU==circle.textureU&&rebuilt->maxSampledError<=.01,"Rebuild metadata/error");
    check(!rebuild_nurbs_curve(circle,2,.001),"Excess rebuild error refuses");check(!rebuild_nurbs_curve(make_nurbs_patch(),4,.01),"Surface rebuild refuses");check(!fit_nurbs_curve(points,65,.01)&&!fit_nurbs_curve(points,4,NAN),"Fit argument guards");
}
void attachment_crop_fillet(){
    auto ref=make_nurbs_patch(2),moving=ref;for(auto& p:moving.controls)p.x+=3;for(unsigned j=0;j<ref.rows;++j)ref.controls[j*ref.columns+ref.columns-2].weight=1+.01*j;
    std::string error;auto c0=attach_nurbs_u(ref,moving,false,&error),c1=attach_nurbs_u(ref,moving,true,&error);check(c0&&c1,error);
    for(unsigned i=0;i<=16;++i){double v=double(i)/16;auto a=evaluate_nurbs(ref,1,v),b=evaluate_nurbs(*c0,0,v),c=evaluate_nurbs(*c1,0,v);for(unsigned k=0;k<3;++k){near(a->position[k],b->position[k]);near(a->position[k],c->position[k]);near(a->du[k],c->du[k]);}}
    {auto huge=ref,tiny=moving;for(auto& p:huge.controls)p.weight*=1e250;for(auto& p:tiny.controls)p.weight*=1e-200;auto matched=attach_nurbs_u(huge,tiny,true);check(bool(matched),"Attachment ignores independent common weight scales");for(unsigned i=0;i<=8;++i){auto a=evaluate_nurbs(ref,1,double(i)/8),b=evaluate_nurbs(*matched,0,double(i)/8);for(unsigned k=0;k<3;++k){near(a->position[k],b->position[k]);near(a->du[k],b->du[k]);}}}
    moving.v.knots.back()=2;check(!attach_nurbs_u(ref,moving,true),"Invalid/mismatched axes refuse");check(!attach_nurbs_u(line(),ref,false),"Curve attach refuses");
    auto patch=make_nurbs_patch();patch.textureU={2,6};patch.textureV={-2,8};for(auto& p:patch.controls)p.y=.1*p.x*p.z;
    auto crop=crop_nurbs_surface(patch,.2,.8,.1,.9,&error);check(bool(crop),error);near(crop->textureU[0],2.8);near(crop->textureU[1],5.2);near(crop->textureV[0],-1);
    for(unsigned j=0;j<=5;++j)for(unsigned i=0;i<=5;++i){double u=std::lerp(.2,.8,double(i)/5),v=std::lerp(.1,.9,double(j)/5);auto a=evaluate_nurbs(patch,u,v),b=evaluate_nurbs(*crop,u,v);for(unsigned k=0;k<3;++k)near(a->position[k],b->position[k]);}
    check(decode_nurbs(encode_nurbs(*crop))==crop,"Crop codec");check(!crop_nurbs_surface(patch,.8,.2,0,1)&&!crop_nurbs_surface(patch,0,1,-.1,1),"Crop invalid domains refuse");
    auto corner=line();corner.columns=3;corner.u={1,{0,0,.5,1,1}};corner.controls={{0,2,0,1},{0,0,0,1},{2,0,0,1}};auto arc=fillet_nurbs_corner(corner,.5,&error);check(bool(arc),error);
    for(unsigned i=0;i<=20;++i){auto p=evaluate_nurbs(*arc,double(i)/20);near(std::hypot(p->position[0]-.5,p->position[1]-.5),.5);}
    auto a=evaluate_nurbs(*arc,0),b=evaluate_nurbs(*arc,1);near(a->position[1],.5);near(b->position[0],.5);near(a->du[0],0);near(b->du[1],0);
    check(!fillet_nurbs_corner(corner,3)&&!fillet_nurbs_corner(corner,0),"Excess/zero fillet radius refuses");corner.controls[2]={0,-2,0,1};check(!fillet_nurbs_corner(corner,.1),"Straight corner refuses");
}
void projection_intersection_adaptive(){
    auto plane=make_nurbs_patch(2);std::string error;auto q=project_nurbs_point(plane,{.2,3,.4},1e-7,&error);check(bool(q),error);near(q->position[0],.2);near(q->position[1],0);near(q->position[2],.4);near(q->distance,3);
    q=project_nurbs_point(plane,{3,2,0});check(bool(q),"Boundary projection");near(q->position[0],1);near(q->distance,std::sqrt(8.));check(!project_nurbs_point(plane,{NAN,0,0})&&!project_nurbs_point(line(),{0,0,0}),"Projection guards");
    auto curve=line(2);auto projected=project_nurbs_curve(curve,plane,17,&error);check(bool(projected),error);for(auto p:projected->controls)near(p.y,0);check(projected->u.degree==1&&projected->columns==17,"Projection is sample guide");
    auto vertical=plane;for(auto& p:vertical.controls){p.y=p.z;p.z=0;}auto segment=intersect_nurbs_planes(plane,vertical,1e-8,&error);check(bool(segment),error);for(auto p:segment->controls){near(p.y,0);near(p.z,0);}near(std::abs(segment->controls[1].x-segment->controls[0].x),2);
    check(!intersect_nurbs_planes(plane,plane),"Coplanar intersection refuses");auto far=vertical;for(auto& p:far.controls)p.x+=4;check(!intersect_nurbs_planes(plane,far),"Disjoint intersection refuses");auto curved=plane;curved.controls[5].y=1;check(!intersect_nurbs_planes(plane,curved),"Curved intersection explicitly refuses");
    auto flat=tessellate_nurbs_adaptive(plane,1,.01,.15,&error);check(bool(flat),error);check(flat->vertices.size()==4&&flat->indices.size()==6,"Flat patch has minimal adaptive bake");
    auto coarse=tessellate_nurbs_adaptive(curved,2,.05,.3,&error),fine=tessellate_nurbs_adaptive(curved,3,.005,.15,&error);check(coarse&&fine,error);check(coarse->vertices.size()>4&&fine->vertices.size()>=coarse->vertices.size()&&fine->vertices.size()<=65536,"Adaptive density/budget");check(bool(validate_polygon_asset(*fine)),"Adaptive production asset valid");
    NurbsSource ridge;ridge.columns=3;ridge.rows=2;ridge.u={1,{0,0,.5,1,1}};ridge.v={1,{0,0,1,1}};for(unsigned j=0;j<2;++j)for(unsigned i=0;i<3;++i)ridge.controls.push_back({double(i),i==1?.7:0,double(j),1});auto crease=tessellate_nurbs_adaptive(ridge,4,.01,.15,&error);check(bool(crease),error);check(crease->vertices.size()==8&&crease->vertices[1].position.x==crease->vertices[4].position.x&&crease->vertices[1].normal.x*crease->vertices[4].normal.x<0,"Adaptive knot-side crease normals");
    check(!tessellate_nurbs_adaptive(curved,1,1e-20,.001),"Adaptive exhaustion refuses partial result");check(!tessellate_nurbs_adaptive(plane,1,0)&&!tessellate_nurbs_adaptive(line(),1),"Adaptive argument guards");
}
void native(){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Modeling")));auto& c=*owner;c.resize(1600,1000);
    auto add=[&](EditorObjectId id,const NurbsSource& s){auto o=make_editor_nurbs(id,s);check(bool(o),"Native fixture");o->polygon->materials[0].name="Retained";o->polygon->contentHash=polygon_asset_content_hash(*o->polygon);c.workspace().document().add_object(std::move(*o));};
    auto plane=make_nurbs_patch(2);add(1,plane);auto vertical=plane;for(auto& p:vertical.controls){p.y=p.z;p.z=0;}add(2,vertical);add(3,line(2));auto moving=plane;for(auto& p:moving.controls)p.x+=3;add(4,moving);
    auto corner=line();corner.columns=3;corner.u={1,{0,0,.5,1,1}};corner.controls={{0,2,0,1},{0,0,0,1},{2,0,0,1}};add(5,corner);add(6,line(4));
    auto run=[&](std::string action,std::set<EditorObjectId> selected,EditorObjectId primary,bool polygon=false){c.workspace().select_object(*selected.begin());for(auto id:selected)if(id!=*selected.begin())c.workspace().add_to_selection(id);if(c.workspace().selected_object()!=primary){c.workspace().toggle_selection(primary);c.workspace().add_to_selection(primary);}const auto count=c.workspace().document().objects().size(),history=c.workspace().commands().size();check(c.dispatch_action(action),action+": "+c.status().text);auto id=*c.workspace().selected_object();auto* result=c.workspace().document().find_object(id);check(c.workspace().document().objects().size()==count+1&&c.workspace().commands().size()==history+1,"New object one undo");check(result->is_nurbs()!=polygon&&result->polygon->materials[0].name=="Retained","Source type/material retained");auto hash=result->polygon->contentHash;check(c.dispatch_action("edit.undo")&&c.workspace().document().objects().size()==count&&c.workspace().selected_objects()==selected,"Undo result/selection");check(c.dispatch_action("edit.redo")&&c.workspace().document().find_object(id)->polygon->contentHash==hash,"Redo cache");};
    run("nurbs.source_adaptive",{1},1,true);run("nurbs.source_rebuild",{3},3);run("nurbs.source_attach_c0",{1,4},1);run("nurbs.source_attach_c1",{1,4},1);run("nurbs.source_project",{1,3},1);run("nurbs.source_intersect_planes",{1,2},1);run("nurbs.source_fillet_corner",{5},5);
    {auto* a=c.workspace().document().find_object(1);auto* b=c.workspace().document().find_object(3);a->transform.position={5,1,-4};a->transform.rotation=quaternion_from_axis_angle({0,1,0},.4F);a->voxelScale={2,.5F,1};b->transform=a->transform;b->voxelScale={1,.5F,1};}
    run("nurbs.source_project",{1,3},1);{auto* result=c.workspace().document().find_object(*c.workspace().selected_object());auto projected=editor_nurbs_source(*result);check(bool(projected),"Transformed projection source");near(projected->controls.front().x,-.5,1e-6);near(projected->controls.front().y,0,1e-6);near(result->transform.position.x,5);near(result->voxelScale.x,2);}
    for(auto id:{1U,3U}){auto* o=c.workspace().document().find_object(id);o->transform={};o->voxelScale={1,1,1};}
    c.workspace().select_object(1);check(c.dispatch_action("nurbs.construct_settings"),"Crop settings");auto v=c.layout().viewport;c.pointer_down(PointerButton::Primary,v.x+25,v.y+8+125+3*32+12,0);c.text_input("0.1");c.key_down("enter",false,false,false);run("nurbs.source_crop",{1},1);
    for(auto id:{3U,6U}){c.workspace().select_object(id);check(c.dispatch_action("nurbs.construct_add"),"Loft snapshots");}check(c.dispatch_action("nurbs.construct_smooth_loft"),"Native smooth loft");
    c.workspace().select_object(1);auto* source=c.workspace().document().find_object(1);source->flags.locked=true;auto count=c.workspace().document().objects().size(),history=c.workspace().commands().size();check(!c.dispatch_action("nurbs.source_adaptive")&&c.workspace().commands().size()==history&&c.workspace().document().objects().size()==count,"Locked modeling refuses atomically");source->flags.locked=false;
    const auto position=source->polygon->vertices[0].position;source->polygon->vertices[0].position.x+=.123F;check(!c.dispatch_action("nurbs.source_adaptive")&&c.workspace().commands().size()==history&&c.workspace().document().objects().size()==count,"Stale modeling cache refuses atomically");source->polygon->vertices[0].position=position;
    check(editor_nurbs_source(*source)==plane&&c.workspace().document().validate(),"Originals/cache valid");
    const auto root=std::filesystem::temp_directory_path()/"dve_surface_modeling_test";std::filesystem::remove_all(root);std::filesystem::create_directories(root);auto save=c.workspace().document().save_transactional(root/"scene.dvescene");check(save.success,save.error);std::string error;auto loaded=EditorDocument::load(root/"scene.dvescene",&error);check(bool(loaded),error);check(loaded->validate(),"Reopened modeling scene");std::filesystem::remove_all(root);
}
}
int main(){try{loft_fit();attachment_crop_fillet();projection_intersection_adaptive();native();std::cout<<"NURBS surface modeling tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
