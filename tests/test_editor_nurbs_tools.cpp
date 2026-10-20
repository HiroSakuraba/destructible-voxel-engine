#include "dve/editor_native.hpp"
#include "dve/voxel_metric.hpp"
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace dve;
using namespace dve::editor;
namespace {
void check(bool x,const char* message){if(!x)throw std::runtime_error(message);}
void near(double a,double b,double tolerance=1e-9){check(std::abs(a-b)<tolerance,"numeric mismatch");}
void insertion(){
    std::string error;auto arc=make_nurbs_arc(2);
    for(auto t:{.1,.3,.62,.9}){
        auto refined=insert_nurbs_knot(arc,false,t,&error);check(bool(refined),error.c_str());
        for(unsigned i=0;i<=40;++i){
            auto a=evaluate_nurbs(arc,double(i)/40),b=evaluate_nurbs(*refined,double(i)/40);
            check(a&&b,"evaluate inserted curve");
            for(unsigned c=0;c<3;++c){near(a->position[c],b->position[c]);near(a->du[c],b->du[c],1e-8);}
        }
    }
    auto s=make_nurbs_patch();s.controls[5].y=.8;s.controls[9].weight=2;
    for(bool v:{false,true}){
        auto refined=insert_nurbs_knot(s,v,.37);check(bool(refined),"surface insertion");
        check((v?refined->rows:refined->columns)==5,"net refinement");
        for(unsigned i=0;i<=10;++i)for(unsigned j=0;j<=10;++j){
            auto a=evaluate_nurbs(s,double(i)/10,double(j)/10),b=evaluate_nurbs(*refined,double(i)/10,double(j)/10);
            check(a&&b,"surface evaluation");
            for(unsigned c=0;c<3;++c){near(a->position[c],b->position[c]);near(a->du[c],b->du[c],1e-8);near(a->dv[c],b->dv[c],1e-8);}
        }
        auto encoded=encode_nurbs(*refined);check(decode_nurbs(encoded)==refined,"refined source codec");
    }
    auto repeated=s;
    for(unsigned i=0;i<3;++i){auto next=insert_nurbs_knot(repeated,false,.5);check(bool(next),"repeated insertion");repeated=*next;}
    check(!insert_nurbs_knot(repeated,false,.5),"multiplicity limit");
    check(!insert_nurbs_knot(s,false,0)&&!insert_nurbs_knot(s,false,1),"endpoint insertion refuses");
    check(!insert_nurbs_knot(arc,true,.5),"curve V refuses");
}
std::unique_ptr<NativeEditorController> controller(){
    auto c=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("NURBS tools")));
    c->resize(1600,1000);check(c->dispatch_action("nurbs.create_patch"),"create");
    c->camera().position={4,5,11};c->camera().target={0,0,0};c->update(0);return c;
}
NurbsSource source(NativeEditorController& c){
    auto* o=c.workspace().document().find_object(*c.workspace().selected_object());
    auto s=editor_nurbs_source(*o);check(bool(s),"native source");return *s;
}
void grab(NativeEditorController& c,EditorToolId tool,int axis){
    c.set_active_tool(tool);auto axes=c.gizmo_axes();check(axes.size()==3,"gizmo axes");
    auto a=axes[std::size_t(axis-1)];check(a.end.visible,"gizmo visible");
    c.pointer_down(PointerButton::Primary,int(a.end.x),int(a.end.y),0);
    check(c.gizmo_dragging(),"NURBS gizmo starts");
}
void type(NativeEditorController& c,std::string_view s){for(char ch:s)c.key_down(std::string(1,ch),false,false,false);}
void native(){
    auto owner=controller();auto& c=*owner;
    auto id=*c.workspace().selected_object();auto* o=c.workspace().document().find_object(id);auto pose=o->transform;auto metric=o->voxelScale;
    check(c.select_nurbs_controls({5,6},SelectionOperation::Replace),"multi select");
    check(c.selected_nurbs_controls().size()==2,"two controls");
    check(!c.select_nurbs_controls({999},SelectionOperation::Replace),"invalid selection refuses");
    auto original=source(c);auto history=c.workspace().commands().size();
    grab(c,EditorToolId::Translate,1);type(c,"1");
    auto moved=source(c);near(moved.controls[5].x,original.controls[5].x+1);near(moved.controls[6].x,original.controls[6].x+1);
    check(moved.controls[4]==original.controls[4],"unselected control unchanged");
    check(length_squared(subtract(o->transform.position,pose.position))==0&&o->transform.rotation.x==pose.rotation.x&&o->transform.rotation.y==pose.rotation.y&&o->transform.rotation.z==pose.rotation.z&&o->transform.rotation.w==pose.rotation.w&&length_squared(subtract(o->voxelScale,metric))==0,"object placement untouched");
    check(c.workspace().commands().size()==history,"preview has no history");
    c.key_down("escape",false,false,false);check(source(c)==original&&!c.gizmo_dragging(),"Escape restores exact source");
    grab(c,EditorToolId::Translate,1);type(c,"1");c.key_down("enter",false,false,false);
    check(c.workspace().commands().size()==history+1,"one transform undo step");
    check(c.dispatch_action("edit.undo")&&source(c)==original,"undo controls");
    check(c.selected_nurbs_controls()==std::set<std::size_t>{5,6},"undo restores control selection");
    check(c.dispatch_action("edit.redo"),"redo controls");
    check(c.dispatch_action("edit.undo"),"restore before scale");
    grab(c,EditorToolId::Scale,1);type(c,"2");auto scaled=source(c);
    near(scaled.controls[6].x-scaled.controls[5].x,2*(original.controls[6].x-original.controls[5].x),1e-7);
    c.clear_navigation_input();check(source(c)==original&&!c.gizmo_dragging(),"focus loss cancels scale");
    grab(c,EditorToolId::Rotate,2);type(c,"90");auto rotated=source(c);check(rotated.controls[5]!=original.controls[5],"rotate preview");
    c.key_down("escape",false,false,false);check(source(c)==original,"rotate cancel");
    // No-op drag must preserve full double-precision source and history.
    history=c.workspace().commands().size();grab(c,EditorToolId::Translate,1);
    auto end=c.gizmo_axes()[0].end;c.pointer_up(PointerButton::Primary,int(end.x),int(end.y),0);
    check(source(c)==original&&c.workspace().commands().size()==history,"no-op drag exact");
    check(c.dispatch_action("nurbs.select_row")&&c.selected_nurbs_controls().size()==4,"U row");
    check(c.dispatch_action("nurbs.select_column")&&c.selected_nurbs_controls().size()==4,"V column");
    check(c.dispatch_action("nurbs.select_all")&&c.selected_nurbs_controls().size()==16,"all");
    check(c.dispatch_action("nurbs.select_invert")&&c.selected_nurbs_controls().empty(),"invert");
    check(c.gizmo_axes().empty(),"empty controls have no gizmo");
    c.set_active_tool(EditorToolId::Select);
    auto point=project_world_to_screen(c.camera(),c.layout().viewport,voxel_to_world(o->transform,o->voxelScale,{float(original.controls[15].x),0,float(original.controls[15].z)}));
    c.pointer_down(PointerButton::Primary,int(point.x),int(point.y),0);
    check(c.selected_nurbs_controls()==std::set<std::size_t>{15},"click selects control");
    auto point2=project_world_to_screen(c.camera(),c.layout().viewport,voxel_to_world(o->transform,o->voxelScale,{float(original.controls[14].x),0,float(original.controls[14].z)}));
    c.pointer_down(PointerButton::Primary,int(point2.x),int(point2.y),1);
    check(c.selected_nurbs_controls()==std::set<std::size_t>{14,15},"Shift adds control");
    c.pointer_down(PointerButton::Primary,int(point2.x),int(point2.y),1);
    check(c.selected_nurbs_controls()==std::set<std::size_t>{15},"Shift-click on a selected control removes it");
    c.pointer_down(PointerButton::Primary,int(point2.x),int(point2.y),1);
    check(c.selected_nurbs_controls()==std::set<std::size_t>{14,15},"Shift re-adds control");
    check(c.dispatch_action("nurbs.select_none"),"clear before box");
    const auto viewport=c.layout().viewport;
    c.pointer_down(PointerButton::Primary,viewport.x+viewport.width-5,viewport.y+5,0);
    c.pointer_move(viewport.x+5,viewport.y+viewport.height-5,0);
    c.pointer_up(PointerButton::Primary,viewport.x+5,viewport.y+viewport.height-5,0);
    check(c.selected_nurbs_controls().size()==16,"box selects controls");
    check(c.dispatch_action("nurbs.select_none"),"clear before Ctrl+A");
    c.key_down("a",true,false,false);check(c.selected_nurbs_controls().size()==16,"Ctrl+A selects controls, not scene objects");
    check(c.select_nurbs_controls({5,6},SelectionOperation::Replace),"restore multi select");
    EditorObject target(100,"Snap target");target.polygon=make_editor_polygon_cube(100);target.transform.position={2,2,0};
    c.workspace().document().add_object(std::move(target));
    auto snapBefore=source(c);grab(c,EditorToolId::Translate,1);c.key_down("v",false,false,false);
    auto targetPixel=project_world_to_screen(c.camera(),viewport,{2,3,1});
    c.pointer_move(int(targetPixel.x),int(targetPixel.y),0);
    check(c.drag_snap_target()&&c.drag_snap_target()->object==100,"NURBS controls use existing held-V snap targets");
    check(source(c).controls[5]!=snapBefore.controls[5],"snap moves selected controls");
    check(length_squared(subtract(o->transform.position,pose.position))==0,"snap does not move object");
    c.key_down("escape",false,false,false);c.key_up("v",false,false,false);
    check(source(c)==snapBefore,"snap cancel restores controls");
    auto beforeInsertion=source(c);
    check(c.dispatch_action("nurbs.insert_u"),"insert U");
    check(source(c).columns==5&&c.selected_nurbs_controls().empty(),"refine clears obsolete control selection");
    check(c.dispatch_action("edit.undo")&&source(c)==beforeInsertion,"undo insertion");
    check(c.selected_nurbs_controls()==std::set<std::size_t>{5,6},"undo insertion selection");
    check(c.dispatch_action("edit.redo")&&source(c).columns==5,"redo insertion");
    check(c.dispatch_action("nurbs.insert_v")&&source(c).rows==5,"insert V");
    check(c.workspace().document().validate(),"source cache valid");
    check(c.select_nurbs_controls({0,1},SelectionOperation::Replace),"select for transformed object");
    o->transform.rotation=quaternion_from_axis_angle({0,1,0},.4F);o->voxelScale={2,3,.5F};
    auto transformedBefore=source(c);auto objectPose=o->transform;
    grab(c,EditorToolId::Translate,1);type(c,"1");
    auto transformed=source(c);
    const auto delta=divide_components(rotate(conjugate(objectPose.rotation),{1,0,0}),o->voxelScale);
    near(transformed.controls[0].x,transformedBefore.controls[0].x+delta.x,1e-6);
    near(transformed.controls[0].z,transformedBefore.controls[0].z+delta.z,1e-6);
    check(transformed.controls[2]==transformedBefore.controls[2],"nonuniform unselected control unchanged");
    c.key_down("escape",false,false,false);check(source(c)==transformedBefore,"nonuniform cancel exact");
    check(!c.dispatch_action("edit.delete"),"control Delete cannot delete whole surface");
    check(c.workspace().document().find_object(id)!=nullptr,"surface survives component Delete");
    // An external source update during a drag must not be overwritten on cancel.
    grab(c,EditorToolId::Translate,1);type(c,"1");
    auto preview=source(c),foreign=preview;foreign.controls[2].y=.2;
    auto changed=c.workspace().commands().execute(c.workspace().document(),std::make_unique<EditNurbsCommand>(id,preview,foreign));
    check(changed.success,"external source change");c.key_down("escape",false,false,false);
    check(source(c)==foreign,"cancel preserves changed external source");
    check(c.workspace().document().validate(),"final source cache valid");
}
}
int main(){try{insertion();native();std::cout<<"NURBS selection/transform/refinement tests passed\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
