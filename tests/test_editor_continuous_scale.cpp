#include "dve/editor_native.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace dve;
using namespace dve::editor;
namespace {
void check(bool v,const char* m) { if(!v) throw std::runtime_error(m); }
void scale_test() {
    EditorDocument doc("Continuous scale");
    for (EditorObjectId id=1;id<=2;++id) {
        EditorObject o(id,"Part"); o.voxelSizeMeters=0.25F;
        o.transform.position={float(id)*2,0,0};
        o.transform.rotation=quaternion_from_axis_angle({0,1,0},0.3F);
        (void)o.voxels->set_voxel({0,0,0},1); (void)o.voxels->set_voxel({2,0,0},2);
        o.anchors={{2,0,0}}; doc.add_object(std::move(o));
    }
    NativeEditorController c{EditorWorkspace(std::move(doc))}; c.resize(1600,900);
    c.workspace().select_object(1); c.workspace().add_to_selection(2);
    c.set_active_tool(EditorToolId::Scale); c.frame_selection();
    const auto pivot=c.current_pivot();
    const auto a=c.workspace().document().find_object(1)->transform;
    const auto revision=c.workspace().document().find_object(1)->voxels->revision();
    const auto h=c.gizmo_axes()[0]; const int x=int(h.end.x),y=int(h.end.y);
    c.pointer_down(PointerButton::Primary,x,y);
    check(c.gizmo_dragging(),"scale handle starts drag");
    c.key_down("1",false,false,false); c.key_down(".",false,false,false); c.key_down("3",false,false,false);
    auto* o=c.workspace().document().find_object(1);
    check(std::abs(o->voxelSizeMeters-0.325F)<0.00001F,"fractional scale previews continuously");
    check(length(subtract(o->transform.position,add(pivot,multiply(subtract(a.position,pivot),1.3F))))<0.0001F,"group pivot positions");
    check(o->voxels->revision()==revision && o->voxels->occupied_voxel_count()==2 && o->anchors.contains({2,0,0}),"preview preserves grid and anchors");
    check(c.workspace().commands().size()==0,"preview has no history");
    c.key_down("escape",false,false,false);
    check(std::abs(o->voxelSizeMeters-0.25F)<0.00001F && length(subtract(o->transform.position,a.position))<0.0001F,"cancel restores size and position");
    c.pointer_down(PointerButton::Primary,x,y);
    c.key_down("2",false,false,false); c.clear_navigation_input();
    check(!c.gizmo_dragging() && std::abs(o->voxelSizeMeters-0.25F)<0.00001F,"focus loss cancels resize");
    c.pointer_down(PointerButton::Primary,x,y); c.pointer_up(PointerButton::Primary,x,y);
    check(c.workspace().commands().size()==0,"no-op drag adds no history");
    c.pointer_down(PointerButton::Primary,x,y);
    c.key_down("1",false,false,false); c.key_down(".",false,false,false); c.key_down("3",false,false,false);
    c.pointer_up(PointerButton::Primary,x,y);
    check(c.workspace().commands().size()==1,"scale commits once");
    for(int i=0;i<3;++i) {
        check(c.dispatch_action("edit.undo") && std::abs(o->voxelSizeMeters-0.25F)<0.00001F,"scale undo");
        check(c.dispatch_action("edit.redo") && std::abs(o->voxelSizeMeters-0.325F)<0.00001F,"scale redo");
        check(o->voxels->revision()==revision && o->anchors.contains({2,0,0}),"history preserves cell state");
    }
    const auto center=transform_point(o->transform,multiply(Float3{0.5F,0.5F,0.5F},o->voxelSizeMeters));
    const auto hit=pick_editor_document(c.workspace().document(),{subtract(center,{0,0,10}),{0,0,1}});
    check(hit.has_value(),"picking uses resized geometry");
    const auto directory=std::filesystem::temp_directory_path()/"dve-continuous-scale-test";
    std::filesystem::create_directories(directory);
    const auto saved=c.workspace().document().save_transactional(directory/"scene.dvescene");
    check(saved.success,"scaled scene saves");
    auto loaded=EditorDocument::load(saved.manifestPath);
    check(loaded && std::abs(loaded->find_object(1)->voxelSizeMeters-0.325F)<0.00001F &&
        loaded->find_object(1)->voxels->occupied_voxel_count()==2,"scale survives round trip");
    std::filesystem::remove_all(directory);
    o->voxelSizeMeters=8;
    c.frame_selection();
    const auto limit=c.gizmo_axes()[0];
    c.pointer_down(PointerButton::Primary,int(limit.end.x),int(limit.end.y));
    c.key_down("2",false,false,false);
    check(std::abs(o->voxelSizeMeters-10)<0.00001F &&
        std::abs(c.workspace().document().find_object(2)->voxelSizeMeters-0.40625F)<0.00001F,"shared factor clamps to size limit");
    c.key_down("escape",false,false,false);
    check(std::abs(o->voxelSizeMeters-8)<0.00001F,"clamped preview cancels");
}
}
int main() { try { scale_test(); std::cout<<"Continuous scale passed\n"; }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} }
