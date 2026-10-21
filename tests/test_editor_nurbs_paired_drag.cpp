#include "dve/editor_native.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
using namespace dve;using namespace dve::editor;
namespace {
void check(bool v,const std::string& m){if(!v)throw std::runtime_error(m);}
template<class P> void until(P predicate){auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);while(!predicate()){check(std::chrono::steady_clock::now()<end,"Paired worker timed out");std::this_thread::yield();}}
bool v_edge(NurbsBoundary e){return e==NurbsBoundary::VMin||e==NurbsBoundary::VMax;}
bool min_edge(NurbsBoundary e){return e==NurbsBoundary::UMin||e==NurbsBoundary::VMin;}
NurbsSource patch(bool moving,bool adaptive){auto s=make_nurbs_patch(2);s.samplesPerSpan=moving?5:3;s.adaptiveDisplay=adaptive;s.chordTolerance=.05;s.normalAngle=.3;for(auto& p:s.controls){p.y=.05*p.z*p.z;if(moving)p.x+=2;}return s;}
NurbsSource orient(NurbsSource s,NurbsBoundary e,bool reference,bool reverse){if(reverse)s=*reverse_nurbs_axis(s,true);if(min_edge(e)==reference)s=*reverse_nurbs_axis(s,false);if(v_edge(e))s=*transpose_nurbs(s);return s;}
void add(EditorDocument& d,EditorObjectId id,NurbsSource s){auto o=make_editor_nurbs(id,s);check(bool(o),"Patch fixture");d.add_object(std::move(*o));}
void pair(EditorDocument& d,bool adaptive,NurbsAttachmentOptions options={}){add(d,1,orient(patch(false,adaptive),options.referenceBoundary,true,false));add(d,2,orient(patch(true,!adaptive),options.movingBoundary,false,options.reverseBoundary));auto r=link_nurbs_seam_command(1,2,options)->execute(d);check(r.success,r.message);}
auto boundary_positions(const EditorObject& o,NurbsBoundary e){auto s=editor_nurbs_source(o);auto range=v_edge(e)?s->textureV:s->textureU;float t=float(range[min_edge(e)?0:1]);std::set<std::tuple<float,float,float>> out;for(const auto& p:o.polygon->vertices)if((v_edge(e)?p.texcoord.y:p.texcoord.x)==t)out.emplace(p.position.x,p.position.y,p.position.z);return out;}
void shared(const EditorDocument& d){std::string error;check(d.validate(&error),error);auto* a=d.find_object(1);auto* b=d.find_object(2);auto seam=editor_nurbs_seam(*a);check(boundary_positions(*a,seam->options.referenceBoundary)==boundary_positions(*b,seam->options.movingBoundary),"Both displays share identical boundary vertices");}
void matrix_and_queue(){
    for(unsigned a=0;a<4;++a)for(unsigned b=0;b<4;++b)for(bool reverse:{false,true})for(bool c1:{false,true}){
        EditorDocument d("Paired worker matrix");NurbsAttachmentOptions options{NurbsBoundary(a),NurbsBoundary(b),reverse,c1};pair(d,(a+b)%2,options);
        auto snapshot=capture_nurbs_seam_drag(d,1);check(bool(snapshot),"Capture pair");auto source=*editor_nurbs_source(snapshot->reference);auto after=source;
        std::size_t index=v_edge(options.referenceBoundary)?(min_edge(options.referenceBoundary)?1:after.columns*(after.rows-1)+1):(min_edge(options.referenceBoundary)?after.columns:2*after.columns-1);
        after.controls[index].y+=.03;NurbsTessellationQueue queue;auto generation=queue.submit(snapshot,1,after);std::optional<NurbsTessellationQueue::Result> result;
        until([&]{result=queue.poll();return result.has_value();});check(result->generation==generation&&bool(result->seamCommand),result->error);
        auto& cmd=*result->seamCommand;check(cmd.execute(d).success,"Prepared paired execute");shared(d);check(cmd.undo(d).success&&editor_nurbs_source(*d.find_object(1))==source,"Paired undo");shared(d);check(cmd.execute(d).success,"Paired redo");shared(d);
    }
    EditorDocument d("Queue replacement");pair(d,true,{NurbsBoundary::UMax,NurbsBoundary::UMin,true,true});auto snapshot=capture_nurbs_seam_drag(d,1);auto source=*editor_nurbs_source(snapshot->reference),after=source;
    NurbsTessellationQueue queue;for(unsigned i=0;i<30;++i){after.controls[7].y=.005*i;queue.submit(snapshot,1,after);}after.controls[7].y=.2;auto last=queue.submit(snapshot,1,after);std::optional<NurbsTessellationQueue::Result> result;
    until([&]{result=queue.poll();return result.has_value();});check(result->generation==last&&result->source==after&&bool(result->seamCommand),"Only newest paired result delivered");check(!queue.poll(),"Consume once");
    std::stop_source cancelled;cancelled.request_stop();std::string error;check(!PreparedNurbsSeamEdit::prepare(snapshot,1,after,&error,cancelled.get_token()),"Cancelled pair returns no partial command");
    auto bad=*editor_nurbs_source(snapshot->moving);bad.controls[0].y+=.1;check(!PreparedNurbsSeamEdit::prepare(snapshot,2,bad,&error),"Worker protects driven rows");
    bad=after;bad.controls[0].weight=0;queue.submit(snapshot,1,bad);until([&]{result=queue.poll();return result.has_value();});check(!result->seamCommand&&!result->error.empty(),"Invalid pair has no command");
    queue.submit(snapshot,1,after);queue.cancel();check(!queue.poll(),"Cancel clears pair result");last=queue.submit(snapshot,1,source);until([&]{result=queue.poll();return result.has_value();});check(result->generation==last&&result->seamCommand,"Queue reusable after paired cancellation");
    check(result->seamCommand->execute(d).success&&editor_nurbs_source(*d.find_object(1))==source,"Identity pair preserves exact authored sources");
    auto heavy=source;heavy.adaptiveDisplay=true;heavy.chordTolerance=1e-9;heavy.normalAngle=1e-5;heavy.controls[7].y+=.5;
    for(unsigned i=0;i<3;++i){auto begin=std::chrono::steady_clock::now();{NurbsTessellationQueue dying;dying.submit(snapshot,1,heavy);std::this_thread::sleep_for(std::chrono::milliseconds(20));dying.cancel();check(!dying.poll(),"Cancelled heavy pair delivers nothing");}check(std::chrono::steady_clock::now()-begin<std::chrono::seconds(5),"Heavy paired cancellation and join return promptly");}

}
void native(bool adaptive,bool reverse,bool c1){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Paired native drag")));auto& c=*owner;c.resize(1600,1000);auto& d=c.workspace().document();pair(d,adaptive,{NurbsBoundary::UMax,NurbsBoundary::UMin,reverse,c1});
    c.workspace().select_object(1);check(c.dispatch_action("nurbs.edit")&&c.select_nurbs_controls({7},SelectionOperation::Replace),"Reference controls");
    auto beforeA=*editor_nurbs_source(*d.find_object(1)),beforeB=*editor_nurbs_source(*d.find_object(2));auto hashA=d.find_object(1)->polygon->contentHash,hashB=d.find_object(2)->polygon->contentHash;auto history=c.workspace().commands().size();
    auto grab=[&]{c.set_active_tool(EditorToolId::Translate);auto axes=c.gizmo_axes();check(axes.size()==3,"Axes present");auto axis=axes[0];c.pointer_down(PointerButton::Primary,int(axis.end.x),int(axis.end.y),0);check(c.gizmo_dragging(),c.status().text);};
    auto start=[&]{grab();for(auto key:{"0",".","1"})c.key_down(key,false,false,false);};auto drain=[&]{until([&]{c.update(0);return !c.gizmo_dragging();});};auto preview=[&]{until([&]{c.update(0);return editor_nurbs_source(*d.find_object(1))!=beforeA;});shared(d);};
    grab();c.key_down("enter",false,false,false);check(!c.gizmo_dragging()&&c.workspace().commands().size()==history,"No-op pair adds no history");
    start();check(editor_nurbs_source(*d.find_object(1))==beforeA&&editor_nurbs_source(*d.find_object(2))==beforeB,"Input leaves both previous displays intact");check(!c.dispatch_action("file.save"),"Pending paired preview blocks save");
    auto release=c.gizmo_axes()[0].end;c.pointer_up(PointerButton::Primary,int(release.x),int(release.y),0);check(c.gizmo_dragging(),"Release waits asynchronously");drain();shared(d);check(c.workspace().commands().size()==history+1&&editor_nurbs_source(*d.find_object(2))!=beforeB,"One drag propagates and adds one history step");
    auto afterA=*editor_nurbs_source(*d.find_object(1)),afterB=*editor_nurbs_source(*d.find_object(2));check(c.dispatch_action("edit.undo")&&editor_nurbs_source(*d.find_object(1))==beforeA&&editor_nurbs_source(*d.find_object(2))==beforeB&&d.find_object(1)->polygon->contentHash==hashA&&d.find_object(2)->polygon->contentHash==hashB,"Undo restores both exact sources/caches");shared(d);
    check(c.dispatch_action("edit.redo")&&editor_nurbs_source(*d.find_object(1))==afterA&&editor_nurbs_source(*d.find_object(2))==afterB,"Redo restores pair");shared(d);check(c.dispatch_action("edit.undo"),"Reset");
    check(c.select_nurbs_controls({6,7},SelectionOperation::Replace),"Multiple reference controls");
    for(auto tool:{EditorToolId::Scale,EditorToolId::Rotate}){
        c.set_active_tool(tool);auto axis=c.gizmo_axes()[tool==EditorToolId::Rotate?1:0];c.pointer_down(PointerButton::Primary,int(axis.end.x),int(axis.end.y),0);check(c.gizmo_dragging(),"Paired scale/rotate starts");
        for(char ch:std::string(tool==EditorToolId::Scale?"1.1":"5"))c.key_down(std::string(1,ch),false,false,false);
        c.key_down("enter",false,false,false);drain();shared(d);check(editor_nurbs_source(*d.find_object(1))!=beforeA,"Scale/rotate changes selected controls");
        check(c.dispatch_action("edit.undo")&&editor_nurbs_source(*d.find_object(1))==beforeA&&editor_nurbs_source(*d.find_object(2))==beforeB,"Scale/rotate undo restores pair");
    }
    check(c.select_nurbs_controls({7},SelectionOperation::Replace),"Restore single control selection");
    start();preview();c.key_down("escape",false,false,false);check(!c.gizmo_dragging()&&editor_nurbs_source(*d.find_object(1))==beforeA&&editor_nurbs_source(*d.find_object(2))==beforeB,"Escape restores both published previews");shared(d);
    start();preview();for(unsigned i=0;i<6;++i)c.key_down("backspace",false,false,false);for(auto key:{"0",".","0","5"})c.key_down(key,false,false,false);c.key_down("enter",false,false,false);check(c.gizmo_dragging(),"Older pair cannot satisfy newer release");drain();check(std::abs(editor_nurbs_source(*d.find_object(1))->controls[7].x-beforeA.controls[7].x-.05)<1e-7,"Latest request wins: delta="+std::to_string(editor_nurbs_source(*d.find_object(1))->controls[7].x-beforeA.controls[7].x));shared(d);check(c.dispatch_action("edit.undo"),"Reset latest");
    start();c.key_down("enter",false,false,false);c.key_down("escape",false,false,false);c.update(0);check(!c.gizmo_dragging()&&editor_nurbs_source(*d.find_object(1))==beforeA&&editor_nurbs_source(*d.find_object(2))==beforeB,"Escape while finishing discards late pair");
    start();preview();c.clear_navigation_input();check(!c.gizmo_dragging()&&editor_nurbs_source(*d.find_object(1))==beforeA&&editor_nurbs_source(*d.find_object(2))==beforeB,"Focus loss restores both previews");shared(d);
    start();preview();check(c.dispatch_action("edit.undo")&&!c.gizmo_dragging()&&editor_nurbs_source(*d.find_object(1))==beforeA&&editor_nurbs_source(*d.find_object(2))==beforeB,"Undo during drag cancels pair only");shared(d);
    start();auto* b=d.find_object(2);b->polygon->materials[0].name="Live partner material";b->polygon->contentHash=polygon_asset_content_hash(*b->polygon);preview();c.key_down("escape",false,false,false);check(b->polygon->materials[0].name=="Live partner material","Paired preview/cancel preserves material edits");shared(d);
    start();preview();b->flags.locked=true;c.update(0);check(!c.gizmo_dragging()&&editor_nurbs_source(*b)==beforeB&&editor_nurbs_source(*d.find_object(1))==beforeA,"Locking partner restores entire preview");b->flags.locked=false;shared(d);
    start();b->transform.position.x=1;c.update(0);check(!c.gizmo_dragging()&&b->transform.position.x==1&&editor_nurbs_source(*d.find_object(1))==beforeA,"Partner placement invalidates pending pair");b->transform.position.x=0;shared(d);
    // Moving free controls remain editable; mixed driven/free selections reject atomically.
    c.workspace().select_object(2);check(c.dispatch_action("nurbs.edit")&&c.select_nurbs_controls({0,6},SelectionOperation::Replace),"Moving mixed selection");c.set_active_tool(EditorToolId::Translate);auto axis=c.gizmo_axes()[0];c.pointer_down(PointerButton::Primary,int(axis.end.x),int(axis.end.y),0);check(!c.gizmo_dragging()&&c.status().text.find("driven")!=std::string::npos,"Driven row blocks entire drag");
    check(c.select_nurbs_controls({6},SelectionOperation::Replace),"Free moving control");grab();c.key_down("0",false,false,false);c.key_down(".",false,false,false);c.key_down("2",false,false,false);c.key_down("enter",false,false,false);drain();shared(d);check(editor_nurbs_source(*b)!=beforeB,"Moving free control drag works");
    const auto root=std::filesystem::temp_directory_path()/"dve_paired_drag_test";std::filesystem::remove_all(root);std::filesystem::create_directories(root);auto saved=d.save_transactional(root/"pair.dvescene");check(saved.success,saved.error);std::string error;auto reopened=EditorDocument::load(root/"pair.dvescene",&error);check(bool(reopened),error);shared(*reopened);std::filesystem::remove_all(root);
}
void stale_and_lifetime(){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Stale partner")));auto& c=*owner;c.resize(1600,1000);auto& d=c.workspace().document();pair(d,true);c.workspace().select_object(1);check(c.dispatch_action("nurbs.edit")&&c.select_nurbs_controls({7},SelectionOperation::Replace),"Stale controls");
    auto start=[&]{c.set_active_tool(EditorToolId::Translate);auto axis=c.gizmo_axes()[0];c.pointer_down(PointerButton::Primary,int(axis.end.x),int(axis.end.y),0);check(c.gizmo_dragging(),"Stale drag starts");c.key_down("1",false,false,false);};
    auto original=clone_editor_object(*d.find_object(2));auto sourceA=editor_nurbs_source(*d.find_object(1));start();d.find_object(2)->polygon->vertices.front().position.y+=.01F;auto hash=polygon_asset_content_hash(*d.find_object(2)->polygon);c.update(0);check(!c.gizmo_dragging()&&polygon_asset_content_hash(*d.find_object(2)->polygon)==hash&&editor_nurbs_source(*d.find_object(1))==sourceA,"External partner cache wins over pending result");*d.find_object(2)=clone_editor_object(original);
    start();auto changed=*editor_nurbs_source(*d.find_object(2));changed.controls[6].y+=.01;find_component_by_type(std::span<Component>(d.find_object(2)->components),kNurbsComponent)->properties["source"]=encode_nurbs(changed);c.update(0);check(!c.gizmo_dragging()&&editor_nurbs_source(*d.find_object(2))==changed,"External partner source is preserved");*d.find_object(2)=clone_editor_object(original);
    start();c.workspace().select_object({});c.update(0);check(!c.gizmo_dragging()&&editor_nurbs_source(*d.find_object(1))==sourceA,"Selection change cancels pair");
    c.workspace().select_object(1);check(c.dispatch_action("nurbs.edit")&&c.select_nurbs_controls({7},SelectionOperation::Replace),"Reopen");start();check(d.remove_object(2),"Remove partner fixture");c.update(0);check(!c.gizmo_dragging()&&editor_nurbs_source(*d.find_object(1))==sourceA,"Partner deletion cancels safely");d.add_object(clone_editor_object(original));
    start();auto begin=std::chrono::steady_clock::now();owner.reset();check(std::chrono::steady_clock::now()-begin<std::chrono::seconds(5),"Controller destruction joins paired worker");
}
void review_fixes(){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Partner removed after preview")));auto& c=*owner;c.resize(1600,1000);auto& d=c.workspace().document();pair(d,true);c.workspace().select_object(1);
    check(c.dispatch_action("nurbs.edit")&&c.select_nurbs_controls({7},SelectionOperation::Replace),"Controls");const auto sourceA=*editor_nurbs_source(*d.find_object(1));const auto history=c.workspace().commands().size();
    c.set_active_tool(EditorToolId::Translate);auto axis=c.gizmo_axes()[0];c.pointer_down(PointerButton::Primary,int(axis.end.x),int(axis.end.y),0);check(c.gizmo_dragging(),"Drag starts");c.key_down("1",false,false,false);
    until([&]{c.update(0);return editor_nurbs_source(*d.find_object(1))!=sourceA;});check(d.remove_object(2),"Remove partner after preview");c.update(0);
    check(!c.gizmo_dragging()&&editor_nurbs_source(*d.find_object(1))==sourceA&&c.workspace().commands().size()==history,"Partner deletion after preview restores the survivor and adds no history");
    // Hiding the partner mid-preview restores both patches.
    EditorDocument fresh("Hidden partner");pair(fresh,false);auto owner2=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(fresh)));auto& e=*owner2;e.resize(1600,1000);auto& f=e.workspace().document();e.workspace().select_object(1);
    check(e.dispatch_action("nurbs.edit")&&e.select_nurbs_controls({7},SelectionOperation::Replace),"Controls 2");const auto a0=*editor_nurbs_source(*f.find_object(1)),b0=*editor_nurbs_source(*f.find_object(2));
    e.set_active_tool(EditorToolId::Translate);axis=e.gizmo_axes()[0];e.pointer_down(PointerButton::Primary,int(axis.end.x),int(axis.end.y),0);check(e.gizmo_dragging(),"Drag 2 starts");e.key_down("1",false,false,false);
    until([&]{e.update(0);return editor_nurbs_source(*f.find_object(1))!=a0;});f.find_object(2)->flags.visible=false;e.update(0);
    check(!e.gizmo_dragging()&&editor_nurbs_source(*f.find_object(1))==a0&&editor_nurbs_source(*f.find_object(2))==b0,"Hiding partner mid-drag restores both");f.find_object(2)->flags.visible=true;shared(f);
}
}
int main(){try{matrix_and_queue();for(bool adaptive:{false,true})for(bool reverse:{false,true})for(bool c1:{false,true})native(adaptive,reverse,c1);stale_and_lifetime();review_fixes();std::cout<<"Paired NURBS drag tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
