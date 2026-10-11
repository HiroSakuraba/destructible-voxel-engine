#include "dve/editor_native.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace dve;
using namespace dve::editor;
namespace {
void require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
std::unique_ptr<NativeEditorController> controller() {
    EditorDocument doc("Selection undo");
    for (EditorObjectId id=1;id<=3;++id) {
        EditorObject object(id,"Object "+std::to_string(id)); object.voxelSizeMeters=1;
        object.transform.position={float(id-1),0,0};
        for (int x=0;x<3;++x) object.voxels->set_voxel({x,0,0},MaterialId(id));
        object.anchors.insert({0,0,0}); doc.add_object(std::move(object));
    }
    auto c=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(doc)));
    c->resize(1280,720); c->workspace().select_object(2); c->workspace().add_to_selection(1); return c;
}
void check_original(NativeEditorController& c) {
    require(c.workspace().selected_objects()==std::set<EditorObjectId>({1,2}) && c.workspace().selected_object()==1,"original objects and primary restored");
}
void test_boolean(const char* policy) {
    auto c=controller(); require(c->dispatch_action(policy),"choose operand policy");
    require(c->dispatch_action("voxel.boolean_union") && c->dispatch_action("voxel.boolean_commit"),"commit Boolean");
    const auto after=c->workspace().selected_objects(); const auto primary=c->workspace().selected_object();
    for (int i=0;i<3;++i) {
        c->workspace().select_object(3);
        require(c->dispatch_action("edit.undo"),"Boolean undo"); check_original(*c);
        require(c->workspace().document().find_object(2) && c->workspace().document().find_object(2)->flags.visible,"operand restored");
        c->workspace().select_object(3);
        require(c->dispatch_action("edit.redo"),"Boolean redo");
        require(c->workspace().selected_objects()==after && c->workspace().selected_object()==primary,"Boolean redo restores result selection");
    }
}
void test_join_and_branch() {
    auto c=controller(); require(c->begin_voxel_join() && c->commit_voxel_join().success,"join");
    require(!c->workspace().document().find_object(2),"operand deleted");
    for (int i=0;i<3;++i) {
        c->workspace().select_object(3); require(c->dispatch_action("edit.undo"),"join undo"); check_original(*c);
        require(c->dispatch_action("edit.redo"),"join redo");
        require(c->workspace().selected_objects()==std::set<EditorObjectId>({1}) && c->workspace().selected_object()==1,"join result selection");
    }
    require(c->dispatch_action("edit.undo"),"undo before branch");
    c->workspace().select_object(3); require(c->nudge_selection({1,0,0}).success,"new edit creates branch");
    require(!c->workspace().commands().can_redo(),"redo branch discarded on success");
    require(c->dispatch_action("edit.undo") && c->workspace().selected_object()==3,"unannotated edit retains current selection");
}
void test_voxel_move() {
    auto c=controller(); c->workspace().select_object(1); c->set_selection_domain(SelectionDomain::Voxels);
    const std::set<Int3> before{{0,0,0},{1,0,0}}, after{{1,0,0},{2,0,0}};
    c->select_voxels(1,before,SelectionOperation::Replace);
    require(c->move_selected_voxels({1,0,0}),"move onto occupied cells");
    require(c->voxel_selection()==after,"moved selection");
    for (int i=0;i<3;++i) {
        c->set_selection_domain(SelectionDomain::Objects); c->workspace().select_object(3);
        require(c->dispatch_action("edit.undo"),"move undo");
        require(c->selection_domain()==SelectionDomain::Voxels && c->voxel_selection()==before && c->workspace().selected_object()==1,"undo restores domain, owner and cells");
        require(c->workspace().document().find_object(1)->anchors.contains({0,0,0}),"anchor restored");
        require(c->dispatch_action("edit.redo") && c->voxel_selection()==after,"redo restores moved cells");
    }
    require(c->dispatch_action("edit.undo"),"undo before failed command");
    require(!c->move_selected_voxels({0,0,0}),"zero move refused");
    require(c->workspace().commands().can_redo() && c->dispatch_action("edit.redo"),"failed edit preserves redo selection");
}
void test_visibility_and_failed_undo() {
    auto c=controller(); require(c->begin_voxel_join() && c->commit_voxel_join().success,"join for isolation");
    c->workspace().select_object(3); require(c->toggle_isolation(),"isolate other object");
    const auto isolated=c->isolated_objects(); require(c->dispatch_action("edit.undo"),"undo in different isolation");
    require(c->isolated_objects()==isolated && c->workspace().selection_count()==0,"undo keeps isolation and excludes isolated-out selections");
    require(c->toggle_isolation(),"exit isolation"); require(c->dispatch_action("edit.redo"),"redo visible again");
    c->workspace().document().find_object(1)->flags.locked=true;
    c->workspace().select_object(3); const auto before=c->workspace().selected_objects();
    require(!c->dispatch_action("edit.undo") && c->workspace().selected_objects()==before,"failed undo preserves current selection");
    require(!c->workspace().document().find_object(2),"failed compound undo rolls back restored operand");
    c->workspace().document().find_object(1)->flags.locked=false;
    require(c->dispatch_action("edit.undo"),"unlock and undo"); check_original(*c);
    c->workspace().document().find_object(3)->flags.visible=false;
    require(c->dispatch_action("edit.redo"),"redo ignores unrelated hidden object");
}
struct Noop final:IEditorCommand {
    bool fail{}; explicit Noop(bool f=false):fail(f){}
    std::string_view label() const noexcept override{return "noop";}
    CommandResult execute(EditorDocument&) override{return fail?CommandResult::fail("refused"):CommandResult::ok();}
    CommandResult undo(EditorDocument&) override{return CommandResult::ok();}
};
void test_history_metadata() {
    EditorDocument doc; EditorCommandStack stack(2);
    auto add=[&](EditorObjectId id) {
        require(stack.execute(doc,std::make_unique<Noop>()).success,"execute history fixture");
        EditorSelectionSnapshot before; before.objects.insert(id); before.primary=id;
        auto after=before; after.objects={id+10}; after.primary=id+10;
        stack.set_last_selection_transition(std::move(before),std::move(after));
    };
    add(1); add(2); add(3);
    require(stack.size()==2 && stack.undo_selection()->primary==3,"trimmed history retains newest metadata");
    require(stack.undo(doc).success && stack.redo_selection()->primary==13,"redo metadata stays with entry");
    require(!stack.execute(doc,std::make_unique<Noop>(true)).success && stack.redo_selection()->primary==13,"failed execute preserves redo and metadata");
    require(stack.undo(doc).success && !stack.can_undo(),"trimmed oldest entry gone");
    stack.clear(); require(!stack.undo_selection() && !stack.redo_selection(),"clear removes metadata");
}
}
int main(){try{test_boolean("voxel.boolean_operands_hide");test_boolean("voxel.boolean_operands_delete");test_boolean("voxel.boolean_operands_keep");test_join_and_branch();test_voxel_move();test_visibility_and_failed_undo();test_history_metadata();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}std::cout<<"Selection undo passed\n";}
