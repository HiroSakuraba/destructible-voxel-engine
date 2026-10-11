#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
using namespace dve;
using namespace dve::editor;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool near(float a, float b) { return std::abs(a-b) < 1e-4F; }
std::unique_ptr<NativeEditorController> make_controller() {
    EditorDocument doc("Inspector");
    EditorObject a(1,"First"); a.transform.position = {0,1,0}; doc.add_object(std::move(a));
    EditorObject b(2,"Second"); b.transform.position = {4,1,0}; doc.add_object(std::move(b));
    auto c = std::make_unique<NativeEditorController>(EditorWorkspace(std::move(doc)));
    c->resize(1280,720); c->workspace().select_object(2); c->workspace().add_to_selection(1);
    return c;
}
void key(NativeEditorController& c, const char* text) { c.key_down(text,false,false,false); }
void edit(NativeEditorController& c, std::size_t field, const char* value = nullptr) {
    const auto r = c.layout().inspectorFields[field]; require(r.width > 0,"visible edit field");
    c.pointer_down(PointerButton::Primary,r.x+4,r.y+r.height/2);
    c.pointer_up(PointerButton::Primary,r.x+4,r.y+r.height/2);
    if (value) c.text_input(value);
}
std::array<bool,5> flags(const EditorObject& o) {
    return {o.flags.visible,o.flags.locked,o.flags.anchored,o.flags.structural,o.flags.collisionEnabled};
}
void test_mixed_flags() {
    auto owner = make_controller(); auto& c = *owner;
    auto& a = *c.workspace().document().find_object(1);
    auto& b = *c.workspace().document().find_object(2);
    a.flags = {true,true,true,true,true}; b.flags = {false,false,false,false,false};
    const auto originalA = flags(a), originalB = flags(b);
    for (std::size_t i=0;i<5;++i) {
        require(c.inspector_selection_summary().flags[i] == InspectorFlagState::Mixed,"differing flags read Mixed");
        c.workspace().add_to_selection(i%2 == 0 ? 1 : 2);
        require(c.toggle_inspector_flag(i).success,"mixed toggle succeeds");
        require(flags(a)[i] && flags(b)[i],"mixed first click enables all, independent of primary");
        require(c.workspace().commands().undo(c.workspace().document()).success,"one undo restores mixed flags");
        require(flags(a) == originalA && flags(b) == originalB,"undo preserves other policies");
        require(c.workspace().commands().redo(c.workspace().document()).success,"redo sets flags");
        require(c.toggle_inspector_flag(i).success && !flags(a)[i] && !flags(b)[i],"all-on click disables all");
        require(c.workspace().commands().undo(c.workspace().document()).success,"undo all-off");
        require(c.workspace().commands().undo(c.workspace().document()).success,"undo original click");
    }
    require(!c.toggle_inspector_flag(5).success,"invalid flag rejected");
    c.workspace().clear_selection(); require(!c.toggle_inspector_flag(0).success,"empty selection rejected");
}
void test_summary_and_rendering() {
    auto owner = make_controller(); auto& c = *owner;
    auto summary = c.inspector_selection_summary();
    require(summary.count == 2 && summary.positionMixed && !summary.rotationMixed,"position mixed, rotation common");
    auto& b = *c.workspace().document().find_object(2);
    b.transform.rotation.w = -1;
    require(!c.inspector_selection_summary().rotationMixed,"quaternion sign is not a mixed rotation");
    b.transform.rotation = quaternion_from_euler_xyz({0,0.8F,0}); b.flags.visible = false;
    require(c.inspector_selection_summary().rotationMixed,"different rotations read mixed");
    struct Canvas final : IEditorCanvas {
        mutable std::vector<std::string> labels;
        void fill(UiRect,EditorColor) const override {}
        void outline(UiRect,EditorColor) const override {}
        void line(int,int,int,int,EditorColor,int) const override {}
        void text(int,int,std::string_view text,EditorColor) const override { labels.emplace_back(text); }
        int text_width(std::string_view text) const override { return int(text.size())*5; }
        bool has(std::string_view part) const { return std::any_of(labels.begin(),labels.end(),[&](const auto& s){return s.find(part)!=std::string::npos;}); }
    } canvas;
    render_native_editor(canvas,c,1280,720);
    require(canvas.has("2 selected; primary") && canvas.has("Group edits from primary"),"render identifies batch and primary scope");
    require(canvas.has("Mixed; primary") && canvas.has("Visible (Mixed)") && canvas.has("-"),"mixed transforms and flags are visible");
}
void test_relative_transforms_and_noop() {
    auto owner = make_controller(); auto& c = *owner;
    edit(c,0); key(c,"enter"); require(!c.workspace().commands().can_undo(),"unmodified Enter adds no undo");
    edit(c,0,"10 1 0"); key(c,"enter");
    const auto& doc = c.workspace().document();
    require(near(doc.find_object(1)->transform.position.x,10) && near(doc.find_object(2)->transform.position.x,14),"typed primary position preserves relative offset");
    require(c.workspace().commands().undo(c.workspace().document()).success,"single undo restores all positions");
    require(near(doc.find_object(1)->transform.position.x,0) && near(doc.find_object(2)->transform.position.x,4),"exact positions restored");
    edit(c,1,"0 90 0"); key(c,"enter");
    const auto delta = subtract(doc.find_object(2)->transform.position,doc.find_object(1)->transform.position);
    require(near(length(delta),4) && near(delta.x,0),"rotation keeps group spacing");
    require(c.workspace().commands().undo(c.workspace().document()).success,"one undo restores group rotation");
    require(near(doc.find_object(2)->transform.position.x,4),"rotation undo restores position");
}
void test_stale_and_refusals() {
    auto owner = make_controller(); auto& c = *owner;
    edit(c,0,"10 0 0"); c.workspace().select_object(2); key(c,"enter");
    require(near(c.workspace().document().find_object(2)->transform.position.x,4) && !c.workspace().commands().can_undo(),"changed selection cannot receive old value");
    require(c.status().text.find("reopen") != std::string::npos,"stale edit gives a route");
    c.workspace().add_to_selection(1);
    edit(c,0,"10 0 0"); c.workspace().add_to_selection(2); key(c,"enter");
    require(!c.workspace().commands().can_undo(),"primary change with same selection refused");
    c.workspace().add_to_selection(1);
    edit(c,0,"10 0 0"); c.workspace().document().find_object(2)->transform.position.x = 7; key(c,"enter");
    require(near(c.workspace().document().find_object(1)->transform.position.x,0) && !c.workspace().commands().can_undo(),"changed transform refuses stale commit atomically");
    edit(c,0,"10 0 0"); c.workspace().document().find_object(2)->flags.locked = true; key(c,"enter");
    require(!c.workspace().commands().can_undo() && c.status().text.find("Unlock") != std::string::npos,"locked member refuses batch with route");
    c.workspace().document().find_object(2)->flags.locked = false;
    edit(c,1,"0 30 0"); require(c.dispatch_action("transform.pivot_origin"),"change pivot during edit"); key(c,"enter");
    require(!c.workspace().commands().can_undo() && c.status().text.find("reopen") != std::string::npos,"changed rotation pivot refuses stale edit");
    edit(c,0,"nan 0 0"); key(c,"enter"); require(!c.workspace().commands().can_undo(),"nonfinite input refused");
    edit(c,0,"10 0 0"); key(c,"escape"); require(!c.workspace().commands().can_undo(),"Esc cancels without undo");
    edit(c,1,"0 30 0"); require(c.workspace().document().remove_object(2),"delete member"); key(c,"enter");
    require(!c.workspace().commands().can_undo(),"deleted member refuses whole edit");
}
}
int main() {
    try { test_mixed_flags(); test_summary_and_rendering(); test_relative_transforms_and_noop(); test_stale_and_refusals(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "Inspector selection passed\n";
}
