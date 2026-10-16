#include "dve/editor_native.hpp"
#include "dve/voxel_metric.hpp"
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace dve;
using namespace dve::editor;
namespace {
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void near(Float3 a,Float3 b,const char* message) { check(length(subtract(a,b))<.0002F,message); }
std::unique_ptr<NativeEditorController> controller(bool plane=false) {
    EditorDocument document("Polygon native regression");
    EditorObject object(1,"Mesh"); object.polygon=plane?make_editor_polygon_plane(1):make_editor_polygon_cube(1);
    document.add_object(std::move(object));
    auto c=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));
    c->resize(1600,900); c->workspace().select_object(1);
    c->camera().position={.5F,.5F,5}; c->camera().target={.5F,.5F,.5F};
    return c;
}
std::uint64_t hash(const NativeEditorController& c,EditorObjectId id=1) { return c.workspace().document().find_object(id)->polygon->contentHash; }
void click(NativeEditorController& c,Float3 point,std::uint32_t modifiers=0) {
    const auto screen=project_world_to_screen(c.camera(),c.layout().viewport,point);
    check(screen.visible,"test point visible");
    c.pointer_down(PointerButton::Primary,int(screen.x),int(screen.y),modifiers);
    c.pointer_up(PointerButton::Primary,int(screen.x),int(screen.y),modifiers);
}
void selection_and_visibility() {
    auto owner=controller(); auto& c=*owner;
    check(c.dispatch_action("polygon.vertex_mode"),"enter vertex mode");
    check(c.selection_domain()==SelectionDomain::Polygons,"polygon domain");
    c.key_down("tab",false,false,false); check(c.selection_domain()==SelectionDomain::Objects,"Tab leaves components");
    c.key_down("tab",false,false,false); check(c.selection_domain()==SelectionDomain::Polygons,"Tab reenters selected mesh");
    check(c.dispatch_action("polygon.vertex_mode"),"restore vertex mode");
    click(c,{0,0,1}); check(c.polygon_selection().components.size()==1,"click corner");
    click(c,{1,0,1},1); check(c.polygon_selection().components.size()==2,"shift adds");
    click(c,{1,0,1},2); check(c.polygon_selection().components.size()==1,"ctrl removes");
    check(c.dispatch_action("edit.select_all"),"select all vertices");
    check(c.polygon_selection().components.size()==8,"eight geometric corners across seams");
    check(c.dispatch_action("polygon.select_invert")&&c.polygon_selection().components.empty(),"invert all clears");
    c.key_down("2",false,false,false); check(c.polygon_selection().mode==PolygonSelectionMode::Edge,"polygon context key switches mode");
    c.key_down("3",false,false,false); check(c.polygon_selection().mode==PolygonSelectionMode::Face,"face shortcut");
    click(c,{.7F,.2F,1}); check(c.polygon_selection().components.size()==1,"face click");
    check(c.dispatch_action("polygon.select_shell")&&c.polygon_selection().components.size()==12,"shell selection across seams");
    const auto& r=c.layout().viewport;
    c.viewport_settings().xraySelection=true;
    (void)c.dispatch_action("polygon.select_none");
    c.pointer_down(PointerButton::Primary,r.x+3,r.y+3);
    c.pointer_move(r.x+r.width-3,r.y+r.height-3);
    c.pointer_up(PointerButton::Primary,r.x+r.width-3,r.y+r.height-3);
    check(c.polygon_selection().components.size()==12,"through box includes back faces");
    c.viewport_settings().xraySelection=false;
    EditorObject blocker(2,"Occluder"); blocker.polygon=make_editor_polygon_cube(2);
    blocker.transform.position={0,0,2}; c.workspace().document().add_object(std::move(blocker));
    (void)c.dispatch_action("polygon.select_none"); click(c,{.7F,.2F,1});
    check(c.polygon_selection().components.empty(),"another scene object occludes component selection");
    c.viewport_settings().xraySelection=true; click(c,{.7F,.2F,1});
    check(c.polygon_selection().components.size()==1,"through click bypasses scene occlusion");
    c.workspace().document().find_object(1)->flags.locked=true; c.update(0);
    check(c.selection_domain()==SelectionDomain::Objects,"locking invalidates component domain");
    check(!c.dispatch_action("polygon.vertex_mode"),"locked mesh refuses mode entry");
}
void transforms_and_history() {
    auto owner=controller(); auto& c=*owner;
    auto* object=c.workspace().document().find_object(1);
    object->transform.position={2,3,4}; object->transform.rotation=quaternion_from_axis_angle({0,1,0},.7F); object->voxelScale={2,.5F,3};
    check(c.set_polygon_mode(PolygonSelectionMode::Vertex),"vertex mode on rotated scaled mesh");
    c.select_polygon_components({0},SelectionOperation::Replace);
    const auto original=*object->polygon; const auto originalHash=hash(c);
    const Float3 start=voxel_to_world(object->transform,object->voxelScale,original.vertices[0].position);
    const Float3 offset={.7F,-.4F,.2F};
    check(c.transform_polygon_components(offset,{}, {1,1,1},start),"world component move");
    for (std::size_t i=0;i<original.vertices.size();++i) if (length_squared(original.vertices[i].position)==0)
        near(voxel_to_world(object->transform,object->voxelScale,object->polygon->vertices[i].position),add(start,offset),"all seam copies follow world move");
    check(c.dispatch_action("edit.undo")&&hash(c)==originalHash,"undo restores mesh");
    check(c.selection_domain()==SelectionDomain::Polygons&&c.polygon_selection().components==std::set<std::uint64_t>{0},"undo restores component domain and IDs");
    check(c.dispatch_action("edit.redo")&&hash(c)!=originalHash,"redo component transform");
    (void)c.dispatch_action("edit.undo");
    c.select_polygon_components(polygon_all_components(original,PolygonSelectionMode::Vertex),SelectionOperation::Replace);
    const Float3 pivot={5,2,1}; const auto rotation=quaternion_from_axis_angle({0,0,1},.3F);
    check(c.transform_polygon_components({},rotation,{2,1,1},pivot),"rotation and local scale around world pivot");
    const auto p=voxel_to_world(object->transform,object->voxelScale,original.vertices[6].position);
    auto expected=rotate(conjugate(object->transform.rotation),subtract(p,pivot));
    expected=add(pivot,rotate(rotation,rotate(object->transform.rotation,multiply_components(expected,{2,1,1}))));
    near(voxel_to_world(object->transform,object->voxelScale,object->polygon->vertices[6].position),expected,"world/local transform math");
    check(!c.transform_polygon_components({}, {}, {0,1,1},pivot),"zero scale refused");
}
void gizmo_cancel_and_commit() {
    auto owner=controller(); auto& c=*owner;
    c.set_polygon_mode(PolygonSelectionMode::Vertex); (void)c.dispatch_action("edit.select_all");
    (void)c.dispatch_action("transform.translate");
    const auto originalHash=hash(c); const auto axes=c.gizmo_axes(); check(!axes.empty(),"component gizmo");
    const auto axis=axes.front();
    const int x=int((axis.start.x+axis.end.x)*.5F), y=int((axis.start.y+axis.end.y)*.5F);
    c.pointer_down(PointerButton::Primary,x,y); c.key_down("1",false,false,false);
    check(hash(c)!=originalHash,"numeric component drag previews");
    c.key_down("escape",false,false,false); check(hash(c)==originalHash,"escape restores starting mesh");
    c.pointer_up(PointerButton::Primary,x,y);
    c.pointer_down(PointerButton::Primary,x,y); c.key_down("1",false,false,false);
    c.clear_navigation_input(); check(hash(c)==originalHash,"focus loss restores component preview");
    c.pointer_down(PointerButton::Primary,x,y); c.key_down("1",false,false,false);
    c.key_down("return",false,false,false); check(hash(c)!=originalHash,"numeric component drag commits");
    (void)c.dispatch_action("edit.undo"); check(hash(c)==originalHash,"drag is one undo step");
}
void tools_clipboard_extract() {
    auto owner=controller(true); auto& c=*owner;
    c.set_polygon_mode(PolygonSelectionMode::Face); (void)c.dispatch_action("edit.select_all");
    const auto original=hash(c);
    c.key_down("e",false,false,false); check(c.active_tool()==EditorToolId::Rotate,"E retains Rotate in polygon mode");
    check(c.dispatch_action("polygon.extrude"),"extrude preview starts");
    check(hash(c)!=original&&!c.polygon_preview_lines().empty(),"extrude has live geometry and instructions");
    check(!c.dispatch_action("file.save"),"save refused during unapplied preview");
    c.key_down("escape",false,false,false); check(hash(c)==original,"extrude cancel exact");
    (void)c.dispatch_action("polygon.extrude"); c.key_down("0",false,false,false); c.key_down("period",false,false,false); c.key_down("5",false,false,false);
    c.key_down("return",false,false,false); check(hash(c)!=original,"extrude applies typed amount");
    check(c.dispatch_action("edit.undo")&&hash(c)==original,"extrude undo");
    check(c.polygon_selection().components.size()==2,"undo restores original face selection");
    check(c.dispatch_action("polygon.inset"),"inset preview"); c.key_down("escape",false,false,false);
    check(c.dispatch_action("polygon.bevel"),"face bevel preview"); c.key_down("escape",false,false,false);
    check(c.dispatch_action("polygon.uv_tools"),"planar UV preview"); c.key_down("y",false,false,false); c.key_down("return",false,false,false);
    check(c.dispatch_action("polygon.assign_material"),"active editor material assigned");
    check(c.workspace().document().find_object(1)->polygon->materials.size()==2,"material imported into mesh palette");
    (void)c.dispatch_action("edit.undo"); check(c.workspace().document().find_object(1)->polygon->materials.size()==1,"material undo restores palette");
    const auto editedBaseline=hash(c);
    c.select_polygon_components({0},SelectionOperation::Replace);
    check(c.dispatch_action("edit.copy"),"copy faces");
    check(c.dispatch_action("edit.paste")&&c.workspace().document().objects().size()==2,"paste faces as mesh object");
    (void)c.dispatch_action("edit.undo"); check(c.workspace().document().objects().size()==1&&c.selection_domain()==SelectionDomain::Polygons,"paste undo restores domain");
    check(c.dispatch_action("polygon.extract"),"extract selected face");
    check(c.workspace().document().objects().size()==2,"extract creates object");
    check(c.dispatch_action("edit.undo")&&c.workspace().document().objects().size()==1,"extract atomic undo");
    check(c.polygon_selection().components==std::set<std::uint64_t>{0},"extract undo selection");
    (void)c.dispatch_action("edit.select_all"); check(c.dispatch_action("edit.delete"),"delete all removes object");
    check(c.workspace().document().objects().empty(),"no invalid empty mesh installed");
    check(c.dispatch_action("edit.undo")&&hash(c)==editedBaseline,"delete all undo mesh");
    check(c.polygon_selection().components.size()==2,"delete all undo selection");
}
void join_separate_slice() {
    auto owner=controller(); auto& c=*owner;
    EditorObject second(2,"Other"); second.polygon=make_editor_polygon_cube(2); second.transform.position={3,0,0}; second.voxelScale={2,.5F,1};
    c.workspace().document().add_object(std::move(second)); c.workspace().add_to_selection(2); c.workspace().add_to_selection(1);
    check(c.dispatch_action("polygon.join"),"native join");
    check(c.workspace().document().objects().size()==1&&c.workspace().selected_object()==1,"joined in active frame");
    check(c.dispatch_action("polygon.separate")&&c.workspace().document().objects().size()==2,"native separate");
    check(c.dispatch_action("edit.undo")&&c.workspace().document().objects().size()==1,"separate undo");
    check(c.dispatch_action("edit.undo")&&c.workspace().document().objects().size()==2,"join undo operands");
    check(c.workspace().selection_count()==2,"join undo selection");
    EditorObject child(3,"Child"); child.parent=2; c.workspace().document().add_object(std::move(child));
    check(!c.dispatch_action("polygon.join")&&c.workspace().document().find_object(3),"join refuses operand with descendants");
    c.workspace().select_object(1);
    const auto original=hash(c);
    check(c.dispatch_action("polygon.slice"),"polygon Slice preview"); c.key_down("s",false,false,false); c.key_down("return",false,false,false);
    check(c.workspace().document().objects().size()==4,"Slice separate creates second half");
    check(c.workspace().selection_count()==2,"Slice selects halves");
    check(c.dispatch_action("edit.undo")&&hash(c)==original&&c.workspace().document().objects().size()==3,"Slice atomic undo");
    check(c.workspace().selection_count()==1&&c.workspace().selected_object()==1,"Slice undo selection");
    check(c.dispatch_action("edit.redo")&&c.workspace().document().objects().size()==4,"Slice redo");
}
}
int main() {
    try { selection_and_visibility(); transforms_and_history(); tools_clipboard_extract(); join_separate_slice(); gizmo_cancel_and_commit(); }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    std::cout<<"Native polygon editing regression checks passed\n";
}
