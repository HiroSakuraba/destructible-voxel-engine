#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace dve;
using namespace dve::editor;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool near(Float3 a, Float3 b, float tolerance=0.001F) { return length(subtract(a,b)) < tolerance; }
std::unique_ptr<NativeEditorController> fixture(EditorProjection projection=EditorProjection::Perspective) {
    EditorDocument doc("Slice interaction");
    EditorObject object(1,"Cube");
    object.voxelSizeMeters=0.25F;
    for (int x=0; x<8; ++x) for (int y=0; y<8; ++y) for (int z=0; z<8; ++z)
        (void)object.voxels->set_voxel({x,y,z},1);
    object.anchors={{0,0,0},{7,7,7}};
    doc.add_object(std::move(object));
    EditorObject other(2,"Other");
    (void)other.voxels->set_voxel({0,0,0},2);
    doc.add_object(std::move(other));
    auto c=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(doc)));
    c->resize(1920,1080);
    c->camera().position={6,5,8}; c->camera().target={1,1,1};
    c->camera().projection=projection; c->camera().orthographicHeight=8;
    c->workspace().select_object(1);
    check(c->begin_voxel_slice(),"open Slice");
    return c;
}
void click(NativeEditorController& c,UiRect r) {
    const int x=r.x+r.width/2,y=r.y+r.height/2;
    c.pointer_down(PointerButton::Primary,x,y); c.pointer_up(PointerButton::Primary,x,y);
}
struct Canvas final : IEditorCanvas {
    mutable bool point{}, normal{}, front{}, back{}, separate{}, apply{}, cancel{};
    void fill(UiRect,EditorColor) const override {}
    void outline(UiRect,EditorColor) const override {}
    void line(int,int,int,int,EditorColor,int) const override {}
    int text_width(std::string_view text) const override { return static_cast<int>(text.size())*7; }
    void text(int,int,std::string_view text,EditorColor) const override {
        point |= text.starts_with("Point (m):"); normal |= text.starts_with("Normal:");
        front |= text=="Keep Front"; back |= text=="Keep Back"; separate |= text=="Separate";
        apply |= text=="Apply"; cancel |= text=="Cancel";
    }
};
void panel_and_numeric() {
    auto c=fixture(); auto p=c->voxel_slice_panel_layout();
    Canvas canvas; render_native_editor(canvas,*c,1920,1080);
    check(canvas.point && canvas.normal && canvas.front && canvas.back && canvas.separate && canvas.apply && canvas.cancel,"panel controls rendered");
    click(*c,p.point); c->text_input("1.25 1 1"); c->key_down("enter",false,false,false);
    check(near(c->voxel_slice().point(),{1.25F,1,1}),"numeric point");
    click(*c,p.normal); c->text_input("0 0 0"); c->key_down("enter",false,false,false);
    check(c->text_edit().kind==TextEditKind::SliceNormal && !c->text_edit().error.empty(),"invalid normal stays editable");
    check(near(c->voxel_slice().normal(),{1,0,0}),"invalid normal unchanged");
    c->key_down("escape",false,false,false); check(c->voxel_slice().active(),"escape field keeps preview");
    click(*c,p.normal); c->text_input("0 2 0"); c->key_down("enter",false,false,false);
    check(near(c->voxel_slice().normal(),{0,1,0}),"normal normalized");
    click(*c,p.axes[2]); check(near(c->voxel_slice().normal(),{0,0,1}),"axis button");
    click(*c,p.outputs[2]); check(c->voxel_slice().output()==VoxelSliceOutput::Separate,"output button");
    click(*c,p.snap); check(!c->voxel_slice_snap(),"snap toggle");
    const auto revision=c->workspace().document().find_object(1)->voxels->revision();
    const auto viewport=c->layout().viewport;
    c->pointer_down(PointerButton::Primary,viewport.x+viewport.width-10,viewport.y+viewport.height-10);
    c->pointer_up(PointerButton::Primary,viewport.x+viewport.width-10,viewport.y+viewport.height-10);
    check(c->workspace().selected_object()==1 && c->workspace().document().find_object(1)->voxels->revision()==revision,"ordinary viewport clicks do not edit or select behind preview");
    click(*c,p.cancel);
    check(!c->voxel_slice().active() && c->workspace().commands().size()==0,"cancel leaves no geometry history");
    check(c->workspace().document().find_object(1)->voxels->occupied_voxel_count()==512,"cancel preserves cells");
}
void translate(EditorProjection projection) {
    auto c=fixture(projection);
    const auto start=c->voxel_slice().point(), normal=c->voxel_slice().normal();
    const auto handle=c->voxel_slice_move_handle();
    check(handle.start.visible && handle.end.visible,"visible move handle");
    const float size=projection==EditorProjection::Orthographic ? 8*0.13F : length(subtract(c->camera().position,start))*0.12F;
    const auto destination=project_world_to_screen(c->camera(),c->layout().viewport,add(start,multiply(normal,size+0.26F)));
    c->pointer_down(PointerButton::Primary,int(std::lround(handle.end.x)),int(std::lround(handle.end.y)));
    check(c->voxel_slice_drag_handle()==-1,"shaft starts drag");
    c->pointer_move(int(std::lround(destination.x)),int(std::lround(destination.y)));
    check(near(c->voxel_slice().point(),add(start,multiply(normal,0.25F)),0.01F),"move snaps by voxel size");
    check(c->workspace().commands().size()==0,"drag only previews");
    c->key_down("escape",false,false,false);
    check(c->voxel_slice().active() && near(c->voxel_slice().point(),start),"escape restores drag start");
    c->pointer_up(PointerButton::Primary,int(destination.x),int(destination.y));
    check(near(c->voxel_slice().point(),start),"release after escape cannot reapply drag");
    c->pointer_down(PointerButton::Primary,int(std::lround(handle.end.x)),int(std::lround(handle.end.y)));
    c->pointer_up(PointerButton::Primary,int(std::lround(destination.x)),int(std::lround(destination.y)));
    check(c->voxel_slice_drag_handle()==0 && c->voxel_slice().active() && !near(c->voxel_slice().point(),start),"release retains preview");
}
void translation_modifiers() {
    for (int mode=0; mode<3; ++mode) {
        auto c=fixture(); click(*c,c->voxel_slice_panel_layout().snap);
        const auto start=c->voxel_slice().point(), normal=c->voxel_slice().normal();
        const float size=length(subtract(c->camera().position,start))*0.12F;
        const float travel=mode==1 ? 0.26F : 0.37F;
        const float expected=mode==1 ? 0.25F : mode==2 ? 0.037F : 0.37F;
        const std::uint32_t modifiers=mode==1 ? 2U : mode==2 ? 1U : 0U;
        const auto h=c->voxel_slice_move_handle();
        const auto destination=project_world_to_screen(c->camera(),c->layout().viewport,add(start,multiply(normal,size+travel)));
        c->pointer_down(PointerButton::Primary,int(std::lround(h.end.x)),int(std::lround(h.end.y)),modifiers);
        c->pointer_up(PointerButton::Primary,int(std::lround(destination.x)),int(std::lround(destination.y)),modifiers);
        check(near(c->voxel_slice().point(),add(start,multiply(normal,expected)),0.015F),"free, Ctrl snap and Shift fine translation");
    }
}
void rotate_plane() {
    auto c=fixture();
    const auto original=c->voxel_slice().normal();
    bool tested=false;
    for (const auto& ring:c->voxel_slice_rotate_handles()) {
        if (ring.axis!=2) continue;
        for (std::size_t i=0; i<ring.points.size(); ++i) {
            const auto a=ring.points[i], b=ring.points[(i+8)%ring.points.size()];
            const auto panel=c->voxel_slice_panel_layout().panel;
            if (!a.visible || !b.visible || panel.contains(int(a.x),int(a.y)) || panel.contains(int(b.x),int(b.y))) continue;
            c->pointer_down(PointerButton::Primary,int(std::lround(a.x)),int(std::lround(a.y)));
            if (c->voxel_slice_drag_handle()!=2) {
                if (c->voxel_slice_drag_handle()!=0) c->key_down("escape",false,false,false);
                continue;
            }
            c->pointer_move(int(std::lround(b.x)),int(std::lround(b.y)));
            const auto normal=c->voxel_slice().normal();
            check(std::abs(std::abs(dot(normal,original))-std::sqrt(0.5F))<0.03F,"ring rotates plane about 45 degrees with snap");
            check(std::abs(length(normal)-1)<0.001F,"rotated normal stays unit length");
            c->key_down("escape",false,false,false); check(near(c->voxel_slice().normal(),original),"rotation cancel");
            tested=true; break;
        }
    }
    check(tested,"reachable rotation ring");
}
void history(VoxelSliceOutput output) {
    auto c=fixture();
    c->set_selection_domain(SelectionDomain::Voxels);
    const std::set<Int3> selected{{0,0,0},{7,7,7}};
    c->select_voxels(1,selected,SelectionOperation::Replace);
    check(c->configure_voxel_slice({1,1,1},{1,0,0},output),"configure output");
    click(*c,c->voxel_slice_panel_layout().apply);
    check(!c->voxel_slice().active(),"apply closes preview");
    const auto after=c->workspace().selected_objects(); const auto cells=c->voxel_selection();
    check(c->workspace().document().find_object(1)->voxels->occupied_voxel_count()==256,"half retained");
    if (output==VoxelSliceOutput::Separate) check(after.size()==2 && c->workspace().selected_object()==1,"both halves selected, primary kept");
    for(int i=0; i<3; ++i) {
        c->set_selection_domain(SelectionDomain::Objects); c->workspace().select_object(2);
        check(c->dispatch_action("edit.undo"),"Slice undo");
        check(c->workspace().selected_objects()==std::set<EditorObjectId>{1} && c->voxel_selection()==selected && c->selection_domain()==SelectionDomain::Voxels,"original domain and cells restored");
        check(c->workspace().document().find_object(1)->anchors.size()==2,"anchors restored");
        check(c->dispatch_action("edit.redo") && c->workspace().selected_objects()==after && c->voxel_selection()==cells,"redo restores result selection");
    }
}
void end_on_axis() {
    auto c=fixture(); c->camera().position={8,1,1}; c->camera().target={1,1,1};
    const auto start=c->voxel_slice().point(); const auto h=c->voxel_slice_move_handle();
    c->pointer_down(PointerButton::Primary,int(h.start.x),int(h.start.y));
    check(c->voxel_slice_drag_handle()==-1,"end-on shaft still pickable");
    c->pointer_move(int(h.start.x),int(h.start.y)-40);
    check(!near(start,c->voxel_slice().point()),"end-on vertical fallback moves plane");
    c->clear_navigation_input();
    check(c->voxel_slice_drag_handle()==0 && near(start,c->voxel_slice().point()),"focus loss restores drag start");
    c->pointer_down(PointerButton::Primary,int(h.start.x),int(h.start.y));
    c->workspace().document().find_object(1)->flags.locked=true;
    c->pointer_move(int(h.start.x),int(h.start.y)-50);
    check(!c->voxel_slice().active() && c->voxel_slice_drag_handle()==0,"stale target closes drag safely");
}
}
int main() {
    try {
        panel_and_numeric(); translate(EditorProjection::Perspective); translate(EditorProjection::Orthographic);
        translation_modifiers(); rotate_plane(); end_on_axis();
        history(VoxelSliceOutput::Front); history(VoxelSliceOutput::Back); history(VoxelSliceOutput::Separate);
        std::cout << "Slice interaction checks passed\n";
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
