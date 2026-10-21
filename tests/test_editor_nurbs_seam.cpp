#include "dve/editor_native.hpp"
#include "dve/editor_prefab.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <tuple>
using namespace dve;using namespace dve::editor;
namespace {
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
void near(double a,double b,double tolerance=1e-7){check(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<tolerance,"Seam numerical mismatch");}
bool v_edge(NurbsBoundary edge){return edge==NurbsBoundary::VMin||edge==NurbsBoundary::VMax;}
bool min_edge(NurbsBoundary edge){return edge==NurbsBoundary::UMin||edge==NurbsBoundary::VMin;}
NurbsSource fixture(bool moving,bool adaptive=false){auto source=make_nurbs_patch(2);for(auto& p:source.controls){p.y=.1*p.z*p.z;if(moving)p.x+=2;}
    if(!moving)for(unsigned j=0;j<4;++j)source.controls[j*4+2].weight=.9;
    source.samplesPerSpan=moving?5:3;source.adaptiveDisplay=adaptive;source.chordTolerance=moving?.005:.05;source.normalAngle=moving?.15:.3;if(moving&&adaptive)source.controls[6].y+=.4;return source;}
NurbsSource oriented(NurbsSource source,NurbsBoundary edge,bool reference,bool reverse){
    if(reverse)source=*reverse_nurbs_axis(source,true);if(min_edge(edge)==reference)source=*reverse_nurbs_axis(source,false);if(v_edge(edge))source=*transpose_nurbs(source);return source;
}
void add(EditorDocument& document,EditorObjectId id,const NurbsSource& source){std::string error;auto object=make_editor_nurbs(id,source,&error);check(bool(object),error);object->polygon->materials[0].name=id==1?"Reference":"Moving";object->polygon->contentHash=polygon_asset_content_hash(*object->polygon);document.add_object(std::move(*object));}
std::set<std::tuple<float,float,float>> boundary_positions(const EditorObject& object,NurbsBoundary edge){
    auto source=editor_nurbs_source(object);const auto range=v_edge(edge)?source->textureV:source->textureU;const float coordinate=float(range[min_edge(edge)?0:1]);std::set<std::tuple<float,float,float>> positions;
    for(const auto& vertex:object.polygon->vertices)if((v_edge(edge)?vertex.texcoord.y:vertex.texcoord.x)==coordinate)positions.emplace(vertex.position.x,vertex.position.y,vertex.position.z);return positions;
}
void shared(const EditorDocument& document){std::string error;check(document.validate(&error),error);const auto* a=document.find_object(1);const auto* b=document.find_object(2);auto sa=editor_nurbs_seam(*a,&error),sb=editor_nurbs_seam(*b,&error);check(sa&&sb,error);
    auto pa=boundary_positions(*a,sa->options.referenceBoundary),pb=boundary_positions(*b,sa->options.movingBoundary);check(pa==pb&&pa.size()==sa->fractions.size(),"Seam endpoints and all interior samples are bit-identical");
    check(sa->fractions==sb->fractions&&sa->anchors.size()==sb->anchors.size(),"Reciprocal schedule");
}
void grids_and_orientations(){
    for(unsigned a=0;a<4;++a)for(unsigned b=0;b<4;++b)for(bool reverse:{false,true})for(bool c1:{false,true}){
        EditorDocument document("Seam orientation");auto edgeA=static_cast<NurbsBoundary>(a),edgeB=static_cast<NurbsBoundary>(b);add(document,1,oriented(fixture(false),edgeA,true,false));add(document,2,oriented(fixture(true),edgeB,false,reverse));
        auto command=link_nurbs_seam_command(1,2,{edgeA,edgeB,reverse,c1});auto result=command->execute(document);check(result.success,"Orientation "+std::to_string(a)+"/"+std::to_string(b)+": "+result.message);shared(document);
        auto hashA=document.find_object(1)->polygon->contentHash,hashB=document.find_object(2)->polygon->contentHash;
        check(command->undo(document).success&&!has_nurbs_seam(*document.find_object(1))&&!has_nurbs_seam(*document.find_object(2)),"Link undo restores ordinary display");
        check(command->execute(document).success&&document.find_object(1)->polygon->contentHash==hashA&&document.find_object(2)->polygon->contentHash==hashB,"Link redo stable");
    }
    auto source=fixture(false);std::string error;auto grid=nurbs_display_grid(source,nullptr,&error);check(bool(grid),error);auto mesh=tessellate_nurbs_grid(source,1,*grid,&error);check(mesh&&validate_polygon_asset(*mesh),"Explicit grid bakes a production asset");
    auto invalid=*grid;invalid.v.erase(invalid.v.begin());check(!tessellate_nurbs_grid(source,1,invalid),"Incomplete grid refuses");
    std::stop_source stop;stop.request_stop();check(!tessellate_nurbs_grid(source,1,*grid,&error,stop.get_token()),"Explicit grid cancellation returns no mesh");
}
void edits_history_and_storage(){
    EditorDocument document("Adaptive linked pair");add(document,1,fixture(false,true));add(document,2,fixture(true,true));EditorCommandStack history;auto result=history.execute(document,link_nurbs_seam_command(1,2,{NurbsBoundary::UMax,NurbsBoundary::UMin,false,true}));check(result.success,result.message);shared(document);
    auto originalA=*editor_nurbs_source(*document.find_object(1)),originalB=*editor_nurbs_source(*document.find_object(2));auto beforeHash=document.find_object(2)->polygon->contentHash;
    auto changed=originalA;changed.controls[7].y+=.12;result=history.execute(document,std::make_unique<EditNurbsCommand>(1,originalA,changed));check(result.success,result.message);shared(document);
    check(editor_nurbs_source(*document.find_object(1))!=originalA&&document.find_object(2)->polygon->contentHash!=beforeHash,"Reference edit propagates to its partner");
    auto* b=document.find_object(2);b->polygon->materials[0].name="Artist material";b->polygon->contentHash=polygon_asset_content_hash(*b->polygon);
    check(history.undo(document).success&&editor_nurbs_source(*document.find_object(1))==originalA&&editor_nurbs_source(*document.find_object(2))==originalB&&b->polygon->materials[0].name=="Artist material","Pair undo restores sources/cache, retains material");shared(document);check(history.redo(document).success,"Pair redo");shared(document);
    auto before=*editor_nurbs_source(*b),after=before;after.controls[6].y+=.05;result=history.execute(document,std::make_unique<EditNurbsCommand>(2,before,after));check(result.success,result.message);shared(document);
    before=*editor_nurbs_source(*b);after=before;after.controls[0].weight*=1.1;auto count=history.size(),hash=b->polygon->contentHash;
    result=history.execute(document,std::make_unique<EditNurbsCommand>(2,before,after));check(!result.success&&result.message.find("driven")!=std::string::npos&&history.size()==count&&b->polygon->contentHash==hash,"Driven boundary edits refuse atomically");
    after=before;after.adaptiveDisplay=false;after.samplesPerSpan=4;result=history.execute(document,std::make_unique<EditNurbsCommand>(2,before,after));check(result.success,result.message);shared(document);
    check(!replace_editor_nurbs_source(*b,*editor_nurbs_source(*b)),"Standalone replacement cannot bypass pair update");
    std::string error;check(!EditNurbsCommand::prepare(*b,*editor_nurbs_source(*b),&error),"Single-object worker cannot bypass linked update");
    auto rootBefore=*editor_nurbs_source(*document.find_object(1)),rootBad=rootBefore;rootBad.controls[6].weight=4;auto unchangedHash=b->polygon->contentHash;
    result=history.execute(document,std::make_unique<EditNurbsCommand>(1,rootBefore,rootBad));check(!result.success&&editor_nurbs_source(*document.find_object(1))==rootBefore&&b->polygon->contentHash==unchangedHash,"Nonpositive C1 reconstruction weight refuses both edits");
    auto refined=insert_nurbs_knot(*editor_nurbs_source(*b),true,.37);check(bool(refined),"Refinement fixture");result=history.execute(document,std::make_unique<EditNurbsCommand>(2,*editor_nurbs_source(*b),*refined));check(!result.success&&history.size()==count+1,"Parameterization change requires unlink");
    const auto stableWeight=editor_nurbs_source(*b)->controls[3].weight;
    for(unsigned i=0;i<4;++i){result=history.execute(document,refresh_nurbs_seam_command(1));check(result.success,result.message);shared(document);near(editor_nurbs_source(*b)->controls[3].weight,stableWeight,1e-12);}
    const auto root=std::filesystem::temp_directory_path()/"dve_nurbs_seam_test";std::filesystem::remove_all(root);std::filesystem::create_directories(root);
    auto save=document.save_transactional(root/"linked.dvescene");check(save.success,save.error);auto loaded=EditorDocument::load(root/"linked.dvescene",&error);check(bool(loaded),error);shared(*loaded);
    auto encoded=find_component_by_type(std::span<const Component>(b->components),kNurbsComponent)->properties.at("seam");
    find_component_by_type(std::span<Component>(b->components),kNurbsComponent)->properties["seam"]=std::string("DVE_NURBS_SEAM 9");check(!document.validate(&error),"Malformed seam metadata refuses");find_component_by_type(std::span<Component>(b->components),kNurbsComponent)->properties["seam"]=encoded;shared(document);
    auto vertex=b->polygon->vertices.front().position;b->polygon->vertices.front().position.x+=.1F;check(!history.undo(document).success,"Stale partner mesh refuses history atomically");b->polygon->vertices.front().position=vertex;shared(document);
    auto source=editor_nurbs_source(*b);auto bad=*source;bad.controls[6].y+=.01;find_component_by_type(std::span<Component>(b->components),kNurbsComponent)->properties["source"]=encode_nurbs(bad);check(!document.validate(&error),"Stale linked source rejected on save validation");find_component_by_type(std::span<Component>(b->components),kNurbsComponent)->properties["source"]=encode_nurbs(*source);shared(document);
    result=history.execute(document,unlink_nurbs_seam_command(1));check(result.success&&!has_nurbs_seam(*document.find_object(1))&&!has_nurbs_seam(*b)&&document.validate(),"Unlink retains independently valid matched geometry");check(history.undo(document).success,"Unlink undo");shared(document);
    b->flags.locked=true;count=history.size();auto aBefore=*editor_nurbs_source(*document.find_object(1)),aAfter=aBefore;aAfter.controls[3].y+=.1;result=history.execute(document,std::make_unique<EditNurbsCommand>(1,aBefore,aAfter));check(!result.success&&history.size()==count&&editor_nurbs_source(*document.find_object(1))==aBefore,"Locked partner prevents partial edit");b->flags.locked=false;
    auto clone=clone_editor_object(*b);clone.id=9;clone.polygon->objectId=9;AddObjectCommand duplicate(std::move(clone));check(!duplicate.execute(document).success&&!document.find_object(9),"Duplicate requires unlink");
    RemoveObjectsCommand removeOne({1});check(!removeOne.execute(document).success&&document.find_object(1),"Single-patch deletion refuses");RemoveObjectsCommand removePair({1,2});check(removePair.execute(document).success&&document.objects().empty(),"Pair deletion allowed");check(removePair.undo(document).success,"Pair deletion undo");shared(document);
    auto* a=document.find_object(1);auto pose=a->transform;pose.position.x+=1;TransformObjectCommand transform(1,a->transform,pose);check(!transform.execute(document).success,"Independent transform refuses");
    ReparentObjectsCommand reparent({{1,a->parent,std::nullopt}});check(!reparent.execute(document).success,"Independent reparent refuses");
    auto component=find_component_by_type(std::span<const Component>(a->components),kNurbsComponent);RemoveComponentCommand removeSource(1,component->id);SetComponentEnabledCommand disable(1,component->id,true,false);check(!removeSource.execute(document).success&&!disable.execute(document).success,"Source component cannot orphan its partner");
    auto prefab=capture_editor_prefab(document,std::vector<EditorObjectId>{1,2},root/"pair.dveprefab","Pair");check(!prefab.success,"Prefab capture requires unlink");shared(document);std::filesystem::remove_all(root);
}
void refusals(){
    EditorDocument document("Seam refusals");add(document,1,fixture(false));add(document,2,fixture(true));document.find_object(2)->transform.position.x=1;auto result=link_nurbs_seam_command(1,2,{})->execute(document);check(!result.success&&!has_nurbs_seam(*document.find_object(1)),"Different frames refuse before publication");
    document.find_object(2)->transform.position.x=0;check(link_nurbs_seam_command(1,2,{})->execute(document).success,"Refusal pair link");add(document,3,fixture(true));check(!link_nurbs_seam_command(1,3,{})->execute(document).success&&!has_nurbs_seam(*document.find_object(3)),"Second seam on one patch refuses");
    EditorDocument budget("Shared budget");auto a=fixture(false),b=fixture(true);a.samplesPerSpan=b.samplesPerSpan=1;
    for(unsigned i=1;i<8;++i){a=*insert_nurbs_knot(a,false,double(i)/8);b=*insert_nurbs_knot(b,true,double(i)/8);}a.samplesPerSpan=b.samplesPerSpan=64;add(budget,1,a);add(budget,2,b);auto hashA=budget.find_object(1)->polygon->contentHash,hashB=budget.find_object(2)->polygon->contentHash;
    result=link_nurbs_seam_command(1,2,{})->execute(budget);check(!result.success&&!has_nurbs_seam(*budget.find_object(1))&&budget.find_object(1)->polygon->contentHash==hashA&&budget.find_object(2)->polygon->contentHash==hashB&&budget.validate(),"Shared/compatibility budget failure is atomic");
}
void native(){
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(EditorDocument("Native seams")));auto& c=*owner;c.resize(1600,1000);add(c.workspace().document(),1,fixture(false,true));add(c.workspace().document(),2,fixture(true,true));c.workspace().select_object(2);c.workspace().add_to_selection(1);
    check(c.dispatch_action("nurbs.seam_link_c1"),c.status().text);shared(c.workspace().document());c.workspace().select_object(1);check(c.dispatch_action("nurbs.edit"),"Native control edit mode");check(c.select_nurbs_controls({7},SelectionOperation::Replace),"Select reference boundary control");
    auto point=editor_nurbs_source(*c.workspace().document().find_object(1))->controls[7];point.y+=.1;check(c.edit_nurbs_control(7,point),c.status().text);shared(c.workspace().document());
    c.set_active_tool(EditorToolId::Translate);auto axes=c.gizmo_axes();check(axes.size()==3,"Linked control gizmo is visible before attempting a drag");auto axis=axes[0];c.pointer_down(PointerButton::Primary,int(axis.end.x),int(axis.end.y),0);check(!c.gizmo_dragging()&&c.status().text.find("numeric")!=std::string::npos,"Linked CV gizmo refuses with explanation");
    c.refresh_menu_state();const auto& menus=std::as_const(c.workspace().menus());check(!menus.find("nurbs.source_swap_uv")->enabled,"Linked parameterization menu disabled");check(!c.dispatch_action("nurbs.source_reverse_u"),"Linked parameterization command refuses");
    check(c.dispatch_action("nurbs.source_display_fine"),c.status().text);shared(c.workspace().document());check(c.dispatch_action("edit.undo"),"Native paired undo");shared(c.workspace().document());
    check(c.dispatch_action("nurbs.seam_unlink"),c.status().text);check(!has_nurbs_seam(*c.workspace().document().find_object(2))&&c.workspace().document().validate(),"Native unlink both");check(c.dispatch_action("edit.undo"),"Native unlink undo");shared(c.workspace().document());
    c.workspace().document().find_object(2)->flags.visible=false;auto before=editor_nurbs_source(*c.workspace().document().find_object(1));point=before->controls[7];point.y+=.05;check(!c.edit_nurbs_control(7,point)&&editor_nurbs_source(*c.workspace().document().find_object(1))==before,"Hidden partner edit refuses");
}
}
int main(){try{grids_and_orientations();edits_history_and_storage();refusals();native();std::cout<<"Persistent NURBS seam tests passed\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
