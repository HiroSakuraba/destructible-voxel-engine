#include "dve/editor_native.hpp"
#include "dve/nurbs_modeling.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace dve;using namespace dve::editor;
namespace {
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
template<class Predicate> void until(Predicate predicate){
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while(!predicate()){check(std::chrono::steady_clock::now()<end,"Background work timed out");std::this_thread::yield();}
}
NurbsSource surface(){auto source=make_nurbs_patch(2);source.adaptiveDisplay=true;source.controls[5].y=.2;return source;}
void queue_and_commands(){
    std::string error;auto source=surface();auto object=make_editor_nurbs(1,source,&error);check(bool(object),error);
    std::stop_source cancel;cancel.request_stop();
    check(!tessellate_nurbs(source,1,&error,cancel.get_token()),"Adaptive cancellation returns no partial mesh");
    auto fixed=source;fixed.adaptiveDisplay=false;check(!tessellate_nurbs(fixed,1,&error,cancel.get_token()),"Fixed cancellation returns no partial mesh");
    check(!EditNurbsCommand::prepare(*object,source,&error,cancel.get_token()),"Prepared edit cancellation");
    auto snapshot=std::make_shared<const EditorObject>(clone_editor_object(*object));
    NurbsTessellationQueue queue;auto after=source;
    for(unsigned i=0;i<24;++i){after.controls[5].y=.2+.005*i;queue.submit(snapshot,after);}
    after.controls[5].y=.4;const auto newest=queue.submit(snapshot,after);
    std::optional<NurbsTessellationQueue::Result> result;
    until([&]{result=queue.poll();return result.has_value();});
    check(result->generation==newest&&result->source==after&&bool(result->command),"Only newest complete result is delivered");
    check(!queue.poll(),"Result consumed once");
    EditorDocument document("Prepared NURBS");document.add_object(clone_editor_object(*object));auto* current=document.find_object(1);
    auto& command=*result->command;check(command.matches_before(*current),"Prepared baseline matches");
    check(command.execute(document).success&&editor_nurbs_source(*current)==after&&validate_editor_nurbs(*current),"Prepared execute has coherent source/cache");
    current->polygon->materials[0].name="Artist material";current->polygon->contentHash=polygon_asset_content_hash(*current->polygon);
    check(command.undo(document).success&&editor_nurbs_source(*current)==source&&current->polygon->materials[0].name=="Artist material","Cached undo preserves material changes");
    check(command.execute(document).success&&command.undo(document).success,"Cached redo/undo");
    current->polygon->vertices[0].position.x+=.1F;check(!command.matches_before(*current)&&!command.execute(document).success,"Prepared result rejects stale mesh");
    *current=clone_editor_object(*object);current->flags.locked=true;check(!command.execute(document).success,"Prepared result respects locked state");current->flags.locked=false;
    auto invalid=after;invalid.controls[0].weight=0;queue.submit(snapshot,invalid);until([&]{result=queue.poll();return result.has_value();});check(!result->command&&!result->error.empty(),"Failure has no partial edit");
    queue.submit(snapshot,after);queue.cancel();check(!queue.poll(),"Cancel discards completed and pending results");
    auto generation=queue.submit(snapshot,source);until([&]{result=queue.poll();return result.has_value();});check(result->generation==generation&&bool(result->command),"Queue reusable after cancel");
    for(unsigned i=0;i<8;++i){NurbsTessellationQueue dying;dying.submit(snapshot,after);} // destruction cancels and joins owned worker
}
void native(){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Background display")));auto& controller=*owner;
    controller.resize(1600,1000);auto source=surface();auto object=make_editor_nurbs(1,source);check(bool(object),"Native fixture");
    controller.workspace().document().add_object(clone_editor_object(*object));controller.workspace().select_object(1);
    check(controller.dispatch_action("nurbs.edit")&&controller.select_nurbs_controls({5,6},SelectionOperation::Replace),"Edit controls");
    auto* current=controller.workspace().document().find_object(1);const auto history=controller.workspace().commands().size();
    auto grab=[&]{controller.set_active_tool(EditorToolId::Translate);auto axis=controller.gizmo_axes()[0];check(axis.end.visible,"Gizmo visible");controller.pointer_down(PointerButton::Primary,int(axis.end.x),int(axis.end.y),0);check(controller.gizmo_dragging(),"Adaptive drag starts");};
    auto start=[&]{grab();controller.key_down("1",false,false,false);};
    auto drain=[&]{until([&]{controller.update(0);return !controller.gizmo_dragging();});};
    grab();controller.key_down("enter",false,false,false);check(!controller.gizmo_dragging()&&controller.workspace().commands().size()==history,"No-op adaptive drag adds no history");
    start();check(editor_nurbs_source(*current)==source,"Request does not mutate authored source on input event");
    check(!controller.dispatch_action("file.save"),"Pending drag blocks saving preview");
    controller.key_down("enter",false,false,false);check(controller.gizmo_dragging(),"Release waits without blocking editor thread");
    drain();auto moved=editor_nurbs_source(*current);check(moved&&std::abs(moved->controls[5].x-source.controls[5].x-1)<1e-12&&std::abs(moved->controls[6].x-source.controls[6].x-1)<1e-12,"Newest drag result applied");
    check(controller.workspace().commands().size()==history+1&&validate_editor_nurbs(*current),"One coherent transform command");
    check(controller.dispatch_action("edit.undo")&&editor_nurbs_source(*current)==source,"Undo restores adaptive source/cache");
    check(controller.dispatch_action("edit.redo")&&editor_nurbs_source(*current)==moved,"Redo restores adaptive source/cache");
    check(controller.dispatch_action("edit.undo"),"Restore baseline");
    start();until([&]{controller.update(0);return editor_nurbs_source(*current)!=source;});
    check(validate_editor_nurbs(*current)&&controller.workspace().commands().size()==history+1,"Completed preview is coherent and adds no history");
    controller.key_down("escape",false,false,false);check(!controller.gizmo_dragging()&&editor_nurbs_source(*current)==source&&validate_editor_nurbs(*current),"Escape restores source/cache after preview");
    start();until([&]{controller.update(0);return editor_nurbs_source(*current)!=source;});
    controller.key_down("backspace",false,false,false);for(auto key:{"0",".","5"})controller.key_down(key,false,false,false);
    controller.key_down("enter",false,false,false);check(controller.gizmo_dragging(),"Older completed preview cannot finish newer request");drain();
    check(std::abs(editor_nurbs_source(*current)->controls[5].x-source.controls[5].x-.5)<1e-12,"Only latest native request committed");check(controller.dispatch_action("edit.undo"),"Restore after replacement request");
    start();controller.key_down("enter",false,false,false);controller.key_down("escape",false,false,false);controller.update(0);
    check(!controller.gizmo_dragging()&&editor_nurbs_source(*current)==source,"Escape while finishing discards late result");
    start();current->polygon->materials[0].name="Live material";current->polygon->contentHash=polygon_asset_content_hash(*current->polygon);
    until([&]{controller.update(0);return editor_nurbs_source(*current)!=source;});controller.key_down("escape",false,false,false);
    check(current->polygon->materials[0].name=="Live material"&&validate_editor_nurbs(*current),"Preview and cancellation preserve separate material edits");
    start();current->transform.position.x+=1;controller.update(0);check(!controller.gizmo_dragging()&&editor_nurbs_source(*current)==source&&current->transform.position.x==1,"Changed placement invalidates pending job");current->transform.position.x=0;
    start();current->flags.locked=true;controller.update(0);check(!controller.gizmo_dragging()&&editor_nurbs_source(*current)==source,"Locking cancels pending job");current->flags.locked=false;
    start();auto external=source;external.controls[7].y=.1;check(replace_editor_nurbs_source(*current,external),"External edit fixture");controller.update(0);
    check(!controller.gizmo_dragging()&&editor_nurbs_source(*current)==external,"External source wins over worker result");
    check(replace_editor_nurbs_source(*current,source),"Reset external source");
    start();current->polygon->vertices[0].position.x+=.1F;auto externalHash=polygon_asset_content_hash(*current->polygon);controller.update(0);
    check(!controller.gizmo_dragging()&&polygon_asset_content_hash(*current->polygon)==externalHash,"External cache edit is preserved");
    check(replace_editor_nurbs_source(*current,source),"Reset cache");
    start();controller.workspace().select_object({});controller.update(0);check(!controller.gizmo_dragging()&&editor_nurbs_source(*current)==source,"Selection change cancels pending transform");
    controller.workspace().select_object(1);check(controller.dispatch_action("nurbs.edit"),"Reopen controls");check(controller.select_nurbs_controls({5},SelectionOperation::Replace),"Reselect");
    const auto root=std::filesystem::temp_directory_path()/"dve_nurbs_background_test";std::filesystem::remove_all(root);std::filesystem::create_directories(root);
    auto saved=controller.workspace().document().save_transactional(root/"scene.dvescene");check(saved.success,saved.error);std::string error;auto loaded=EditorDocument::load(root/"scene.dvescene",&error);check(loaded&&validate_editor_nurbs(*loaded->find_object(1)),"Saved scene has no transient job state");std::filesystem::remove_all(root);
    start();check(controller.workspace().document().remove_object(1),"Delete fixture");controller.update(0);check(!controller.gizmo_dragging(),"Deletion cancels without touching freed object");
}
}
int main(){try{queue_and_commands();native();std::cout<<"Background NURBS tests passed\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
