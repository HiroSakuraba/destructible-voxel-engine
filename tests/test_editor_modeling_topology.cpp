#include "dve/editor_modeling_topology.hpp"
#include "dve/editor_subdivision.hpp"
#include "dve/editor_mesh_authoring.hpp"
#include "dve/editor_prefab.hpp"
#ifndef DVE_MODELING_TOPOLOGY_CORE_ONLY
#include "dve/editor_native.hpp"
#endif
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
using namespace dve;
using namespace dve::editor;
namespace {
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
struct Fixture{CookedPolygonAsset mesh;SubdivisionSettings settings;};
Fixture quad() {
    Fixture f;f.mesh=make_editor_polygon_plane(1,1);f.settings.level=0;freeze_subdivision_topology(f.mesh,f.settings);return f;
}
void serialization_and_partitions() {
    auto f=quad();std::string error;auto topo=make_modeling_topology(f.mesh,&f.settings,{},&error);check(bool(topo),error);
    check(topo->faces.size()==1&&topo->corners.size()==6&&topo->attributes.size()==4,"logical quad, render corners and attribute identities are separate");
    std::stringstream stream;write_modeling_topology(stream,*topo);auto decoded=read_modeling_topology(stream,&error);
    check(decoded&&*decoded==*topo&&validate_modeling_topology(f.mesh,&f.settings,*decoded,&error),"identity record round trips exactly");
    auto bad=*topo;bad.attributes[0]=bad.vertices[0];check(!validate_modeling_topology(f.mesh,&f.settings,bad,&error),"cross-domain duplicate identity refuses");
    bad=*topo;bad.nextId=1;check(!validate_modeling_topology(f.mesh,&f.settings,bad,&error),"stale allocation counter refuses");
    std::stringstream oversized("modeling_ids 2\ngeometric_ids 100001\n");check(!read_modeling_topology(oversized,&error),"oversized persisted arrays refuse before allocation");
    auto doubled=f.mesh;const auto n=std::uint32_t(doubled.vertices.size());doubled.vertices.insert(doubled.vertices.end(),f.mesh.vertices.begin(),f.mesh.vertices.end());
    for(auto index:f.mesh.indices)doubled.indices.push_back(index+n);doubled.submeshes[0].indexCount=std::uint32_t(doubled.indices.size());refresh_editor_polygon(doubled);
    check(!make_modeling_topology(doubled,nullptr,{},&error),"coincident disconnected legacy shells require explicit partition");
    std::vector<std::uint32_t> independent;for(std::uint32_t i=0;i<doubled.vertices.size();++i)independent.push_back(i);
    auto distinct=make_modeling_topology(doubled,nullptr,independent,&error);check(bool(distinct),error);
    check(distinct->vertices[0]!=distinct->vertices[n]&&!modeling_matches_legacy_partition(doubled,*distinct),"coincident independent vertices remain distinct and cannot enter position-based tools");
    std::stringstream separate;write_modeling_topology(separate,*distinct);auto restored=read_modeling_topology(separate,&error);
    check(restored&&*restored==*distinct&&validate_modeling_topology(doubled,nullptr,*restored,&error),"explicit independent partitions survive persistence");
}
void movement_reorder_and_retirement() {
    auto f=quad();std::string error;auto topo=make_modeling_topology(f.mesh,&f.settings,{},&error);check(bool(topo),error);
    auto moved=f.mesh;for(auto& v:moved.vertices)v.position=add(v.position,{8,3,-2});refresh_editor_polygon(moved);
    auto unchanged=advance_modeling_topology(f.mesh,moved,&f.settings,*topo,identity_polygon_lineage(f.mesh,&f.settings));
    check(bool(unchanged)&&unchanged.topology.vertices==topo->vertices&&unchanged.topology.edges==topo->edges&&unchanged.topology.faces==topo->faces,"moving geometry preserves IDs without matching old positions");
    auto reordered=f.mesh;std::reverse(reordered.vertices.begin(),reordered.vertices.end());for(auto& i:reordered.indices)i=std::uint32_t(reordered.vertices.size()-1-i);refresh_editor_polygon(reordered);
    auto settings=f.settings;for(auto& face:settings.controlFaces)for(auto& i:face)i=std::uint32_t(reordered.vertices.size()-1-i);
    auto lineage=identity_polygon_lineage(f.mesh,&f.settings);lineage.vertexSurvivors.clear();lineage.edgeDescendants.clear();lineage.afterVertexGroups.clear();
    const auto n=std::uint32_t(f.mesh.vertices.size());for(std::uint32_t i=0;i<n;++i){lineage.vertexSurvivors[i]=n-1-i;lineage.afterVertexGroups.push_back(n-1-i);}
    for(auto key:polygon_all_components(f.mesh,PolygonSelectionMode::Edge)){auto a=n-1-std::uint32_t(key>>32U),b=n-1-std::uint32_t(key);lineage.edgeDescendants[key]={(ModelingId((std::min)(a,b))<<32U)|(std::max)(a,b)};}
    auto changed=advance_modeling_topology(f.mesh,reordered,&settings,*topo,lineage);check(bool(changed),changed.error);
    for(std::uint32_t i=0;i<n;++i)check(changed.topology.vertices[n-1-i]==topo->vertices[i]&&changed.topology.attributes[n-1-i]==topo->attributes[i],"array reorder follows explicit survivors");
    PolygonSelection selected{1,PolygonSelectionMode::Vertex,{0}};const auto ids=modeling_selection_ids(f.mesh,*topo,selected);
    check(resolve_modeling_selection(reordered,changed.topology,0,ids)==std::set<ModelingId>{n-1},"selection ID resolves to reordered array");
    check(resolve_modeling_selection(reordered,changed.topology,0,{topo->nextId+999}).empty(),"retired or missing IDs never select an unrelated index");
}
void cut_and_cleanup() {
    auto f=quad();std::string error;auto topo=make_modeling_topology(f.mesh,&f.settings,{},&error);check(bool(topo),error);
    const auto edges=subdivision_control_edges(f.mesh,&f.settings);const auto seed=*edges.begin();f.settings.edgeCreases[seed]=2;
    const auto cut=cut_cage_edge_ring(f.mesh,f.settings,seed,.3F);check(bool(cut),cut.polygon.error);
    auto changed=advance_modeling_topology(f.mesh,cut.polygon.mesh,&cut.subdivision,*topo,cut.lineage);check(bool(changed),changed.error);
    for(std::uint32_t i=0;i<f.mesh.vertices.size();++i)check(changed.topology.vertices[i]==topo->vertices[i],"loop cut keeps untouched geometric identities");
    const auto endpoints=std::minmax(topo->vertices[std::uint32_t(seed>>32U)],topo->vertices[std::uint32_t(seed)]);
    const auto parent=topo->edges.at({endpoints.first,endpoints.second});
    check(changed.descendants.at(parent).size()==2&&!changed.descendants.at(parent).contains(parent),"split crease edge has two new child identities");
    for(auto child:cut.lineage.edgeDescendants.at(seed))check(cut.subdivision.edgeCreases.at(child)==2,"split children retain crease strength independently of identity allocation");
    auto slid=cut_cage_edge_ring(f.mesh,f.settings,seed,.7F);check(bool(slid),slid.polygon.error);
    auto slideIds=advance_modeling_topology(f.mesh,slid.polygon.mesh,&slid.subdivision,*topo,slid.lineage);
    check(bool(slideIds)&&slideIds.topology==changed.topology,"slide amount does not replace provisional element identities");
    auto dirty=f.mesh;PolygonVertex orphan;orphan.position={99,99,99};dirty.vertices.insert(dirty.vertices.begin(),orphan);for(auto& i:dirty.indices)++i;refresh_editor_polygon(dirty);
    auto saved=f.settings;for(auto& face:saved.controlFaces)for(auto& i:face)++i;saved.edgeCreases.clear();
    auto dirtyIds=make_modeling_topology(dirty,&saved,{},&error);check(bool(dirtyIds),error);
    auto compact=compact_polygon_vertices(dirty,{1,PolygonSelectionMode::Vertex,{1}});check(bool(compact),compact.polygon.error);
    auto remapped=remap_compacted_cage(dirty,compact.polygon.mesh,saved,compact.lineage,&error);check(bool(remapped),error);
    for(std::uint32_t i=0;i<dirtyIds->faces.size();++i)compact.lineage.controlFaceDescendants[i]={i};
    auto cleaned=advance_modeling_topology(dirty,compact.polygon.mesh,&*remapped,*dirtyIds,compact.lineage);check(bool(cleaned),cleaned.error);
    check(cleaned.descendants.at(dirtyIds->vertices[0]).empty()&&cleaned.topology.vertices[0]==dirtyIds->vertices[1],"orphan identity retires while reindexed survivors keep IDs");
    auto floor=changed.topology.nextId+100;
    auto branched=advance_modeling_topology(f.mesh,cut.polygon.mesh,&cut.subdivision,*topo,cut.lineage,floor);check(bool(branched),branched.error);
    for(auto id:branched.created)check(id>=floor,"new history branch never reuses identities below high-water mark");
}
void dissolve_and_merge_identities() {
    Fixture f;f.mesh.objectId=1;f.mesh.materials.resize(1);f.mesh.materialBindings.resize(1);f.settings.level=0;
    for(auto point:{Float3{0,0,0},Float3{.5F,0,0},Float3{1,0,0},Float3{0,1,0}}){PolygonVertex v;v.position=point;v.texcoord={point.x,point.y};f.mesh.vertices.push_back(v);}
    f.mesh.indices={0,1,3,1,2,3};f.mesh.submeshes={{"Fan",0,6,0}};refresh_editor_polygon(f.mesh,true);f.settings.controlFaces={{0,1,3},{1,2,3}};
    std::string error;auto topo=make_modeling_topology(f.mesh,&f.settings,{},&error);check(bool(topo),error);
    auto dissolved=dissolve_polygon_vertices(f.mesh,{1,PolygonSelectionMode::Vertex,{1}});check(bool(dissolved),dissolved.polygon.error);
    auto settings=remap_cleanup_cage(f.mesh,dissolved.polygon.mesh,f.settings,dissolved.lineage,&error);check(bool(settings),error);
    auto changed=advance_modeling_topology(f.mesh,dissolved.polygon.mesh,&*settings,*topo,dissolved.lineage);check(bool(changed),changed.error);
    check(changed.descendants.at(topo->vertices[1]).empty()&&changed.topology.vertices[1]==topo->vertices[2],"dissolve retires the removed vertex and retains surviving geometric IDs");
    check(changed.descendants.at(topo->faces[0])==changed.descendants.at(topo->faces[1])&&changed.topology.faces.size()==1,"dissolved faces explicitly merge to one saved identity");
    auto faceIds=modeling_selection_ids(f.mesh,*topo,{1,PolygonSelectionMode::Face,{0}});
    check(faceIds==std::set<ModelingId>{topo->faces[0]},"face selections store logical face IDs");
    check(resolve_modeling_selection(dissolved.polygon.mesh,changed.topology,2,changed.descendants.at(topo->faces[0]))==std::set<ModelingId>{0},"merged face identity resolves to output triangles");
    auto cracked=f;cracked.mesh.vertices.clear();
    for(auto point:{Float3{0,0,0},Float3{1,0,0},Float3{1,1,0},Float3{1.005F,0,0},Float3{2,1,0},Float3{1.005F,1,0}}){PolygonVertex v;v.position=point;v.texcoord={point.x,point.y};cracked.mesh.vertices.push_back(v);}
    cracked.mesh.indices={0,1,2,3,4,5};refresh_editor_polygon(cracked.mesh,true);cracked.settings.controlFaces={{0,1,2},{3,4,5}};
    topo=make_modeling_topology(cracked.mesh,&cracked.settings,{},&error);check(bool(topo),error);
    auto merged=merge_polygon_vertices(cracked.mesh,{1,PolygonSelectionMode::Vertex,{1,2,3,5}},.01F);check(bool(merged),merged.polygon.error);
    settings=remap_cleanup_cage(cracked.mesh,merged.polygon.mesh,cracked.settings,merged.lineage,&error);check(bool(settings),error);
    changed=advance_modeling_topology(cracked.mesh,merged.polygon.mesh,&*settings,*topo,merged.lineage);check(bool(changed),changed.error);
    check(changed.topology.vertices[1]==changed.topology.vertices[3]&&changed.descendants.at(topo->vertices[3])==std::set<ModelingId>{topo->vertices[1]},"geometric weld records a deterministic vertex survivor");
    check(changed.topology.attributes==topo->attributes&&changed.topology.corners==topo->corners,"geometric merge keeps separate attribute and UV-corner identities");
    auto quadSource=quad();auto quadTopo=make_modeling_topology(quadSource.mesh,&quadSource.settings,{},&error);check(bool(quadTopo),error);
    auto triangulated=quadSource.settings;triangulated.controlFaces={{0,1,2},{0,2,3}};
    // Use actual render triples rather than assuming the plane's diagonal orientation.
    triangulated.controlFaces.clear();for(std::size_t i=0;i<quadSource.mesh.indices.size();i+=3)triangulated.controlFaces.push_back({quadSource.mesh.indices[i],quadSource.mesh.indices[i+1],quadSource.mesh.indices[i+2]});
    auto triangleTopo=make_modeling_topology(quadSource.mesh,&triangulated,{},&error);check(bool(triangleTopo),error);
    auto faces=change_modeling_faces(quadSource.mesh,&triangulated,&quadSource.settings,*triangleTopo);check(bool(faces),faces.error);
    for(auto id:triangleTopo->controlEdges)if(!faces.topology.controlEdges.contains(id))check(resolve_modeling_selection(quadSource.mesh,faces.topology,1,{id}).empty(),"dissolved authored diagonals cannot be recalled as selectable cage edges");
}

#if defined(DVE_MODELING_TOPOLOGY_COMMANDS) || !defined(DVE_MODELING_TOPOLOGY_CORE_ONLY)
void command_history() {
    auto f=quad();std::string error;EditorDocument doc("Identity command regression");EditorObject o(1,"Mesh");o.polygon=f.mesh;o.subdivision=f.settings;
    check(ensure_modeling_topology(o,&error),error);const auto original=*o.modelingTopology;doc.add_object(std::move(o));EditorCommandStack stack;
    const auto seed=*subdivision_control_edges(f.mesh,&f.settings).begin();auto cut=cut_cage_edge_ring(f.mesh,f.settings,seed,.4F);check(bool(cut),cut.polygon.error);
    auto changed=advance_modeling_topology(f.mesh,cut.polygon.mesh,&cut.subdivision,original,cut.lineage,doc.find_object(1)->modelingNextId);check(bool(changed),changed.error);
    check(stack.execute(doc,std::make_unique<ReplacePolygonCommand>(1,f.mesh,cut.polygon.mesh,"Cut",0,cut.subdivision,false,changed.topology)).success,"actual ReplacePolygonCommand commits explicit IDs");
    const auto floor=doc.find_object(1)->modelingNextId;check(stack.undo(doc).success&&*doc.find_object(1)->modelingTopology==original&&doc.find_object(1)->modelingNextId==floor,"actual command Undo restores IDs and keeps high-water");
    check(stack.redo(doc).success&&*doc.find_object(1)->modelingTopology==changed.topology,"actual command Redo restores child IDs");
    check(stack.undo(doc).success,"restore command source");auto next=advance_modeling_topology(f.mesh,cut.polygon.mesh,&cut.subdivision,original,cut.lineage,doc.find_object(1)->modelingNextId);
    for(auto id:next.created)check(id>=floor,"command high-water protects a new history branch");
    auto moved=f.mesh;for(auto& v:moved.vertices)v.position=add(v.position,{4,3,2});refresh_editor_polygon(moved);
    check(stack.execute(doc,std::make_unique<ReplacePolygonCommand>(1,f.mesh,moved,"Move")).success&&*doc.find_object(1)->modelingTopology==original,"actual move command preserves persistent IDs");
    check(stack.undo(doc).success,"move Undo");
    const auto noMap=stack.execute(doc,std::make_unique<ReplacePolygonCommand>(1,f.mesh,cut.polygon.mesh,"Unsupported edit"));
    check(!noMap.success&&*doc.find_object(1)->modelingTopology==original&&doc.find_object(1)->polygon->indices==f.mesh.indices,"unsupported topology change refuses without mutation");
    auto triangles=f.settings;triangles.controlFaces.clear();for(std::size_t i=0;i<f.mesh.indices.size();i+=3)triangles.controlFaces.push_back({f.mesh.indices[i],f.mesh.indices[i+1],f.mesh.indices[i+2]});
    check(stack.execute(doc,std::make_unique<SetSubdivisionSettingsCommand>(1,f.settings,triangles)).success,"actual saved-face split command");
    const auto split=*doc.find_object(1)->modelingTopology;check(split.faces.size()==2&&split.vertices==original.vertices,"saved-face split keeps vertices and allocates face IDs");
    check(stack.undo(doc).success&&*doc.find_object(1)->modelingTopology==original,"saved-face Undo restores exact IDs");
    check(stack.redo(doc).success&&*doc.find_object(1)->modelingTopology==split,"saved-face Redo restores exact IDs");
    auto copy=clone_editor_document(doc);check(copy.find_object(1)->modelingTopology==split&&copy.find_object(1)->modelingNextId==doc.find_object(1)->modelingNextId,"actual document clone retains namespace and floor");
}
#endif
#if defined(DVE_MODELING_TOPOLOGY_SCENE) || !defined(DVE_MODELING_TOPOLOGY_CORE_ONLY)
void scene_and_prefab_persistence() {
    auto f=quad();EditorDocument doc("Topology persistence");EditorObject object(1,"Quad");object.polygon=f.mesh;object.subdivision=f.settings;doc.add_object(std::move(object));std::string error;
    const auto root=std::filesystem::temp_directory_path()/"dve-modeling-record-roundtrip";std::filesystem::remove_all(root);
    const auto path=root/"scene.dvescene";check(doc.save_transactional(path).success,"legacy scene save");
    std::ifstream legacyFile(path);std::string header;std::getline(legacyFile,header);legacyFile.close();check(header=="DVE_EDITOR_SCENE 10","untouched legacy scenes keep their existing format");
    auto legacy=EditorDocument::load(path,&error);check(legacy&&!legacy->find_object(1)->modelingTopology,"old format loads without fabricated IDs");
    check(ensure_modeling_topology(*legacy->find_object(1),&error),error);const auto original=*legacy->find_object(1)->modelingTopology;
    legacy->find_object(1)->modelingNextId+=99;check(legacy->save_transactional(path).success,"save identity scene with higher retired-ID floor");
    std::ifstream identityFile(path);std::getline(identityFile,header);identityFile.close();check(header=="DVE_EDITOR_SCENE 11","persistent IDs use version 11");
    auto loaded=EditorDocument::load(path,&error);check(bool(loaded),error);
    auto expected=original;expected.nextId=legacy->find_object(1)->modelingNextId;
    check(*loaded->find_object(1)->modelingTopology==expected&&loaded->find_object(1)->modelingNextId==expected.nextId,"scene round trip preserves IDs and retired allocation floor");
    const std::array<EditorObjectId,1> roots{1};const auto captured=capture_editor_prefab(*loaded,roots,root/"quad.dveprefab","Quad");check(captured.success,captured.error);
    auto prefab=load_editor_prefab(captured.prefabPath,&error);check(bool(prefab),error);
    check(prefab->templateDocument.find_object(1)->modelingTopology==expected,"prefab template preserves persistent topology");
    EditorDocument instances("Instances");auto instance=instantiate_editor_prefab(instances,*prefab);check(instance.success,instance.error);
    const auto id=instance.objectIds.front();check(instances.find_object(id)->modelingTopology==expected,"instance retains object-scoped local IDs");
    auto* source=prefab->templateDocument.find_object(1);for(auto& v:source->polygon->vertices)v.position=add(v.position,{0,2,0});refresh_editor_polygon(*source->polygon);
    prefab->contentHash=editor_prefab_content_hash(*prefab);auto updated=apply_editor_prefab_instance_update(instances,instance.instanceId,*prefab);
    check(updated.success&&instances.find_object(id)->modelingTopology==expected,"prefab refresh keeps source element identities");
    const auto hash=editor_prefab_content_hash(*prefab);source->modelingNextId+=20;source->modelingTopology->nextId=source->modelingNextId;
    check(editor_prefab_content_hash(*prefab)==hash,"allocator reservations do not create false prefab-content changes");
    std::filesystem::remove_all(root);
}
#endif
#ifndef DVE_MODELING_TOPOLOGY_CORE_ONLY
void legacy_migration_is_not_an_edit() {
    auto f=quad();std::string error;
    {EditorObject retired(2,"Retired");retired.polygon=f.mesh;retired.subdivision=f.settings;retired.modelingNextId=50;
     check(ensure_modeling_topology(retired,&error),"re-migration after Undo removed a record must succeed: "+error);
     std::set<ModelingId> ids(retired.modelingTopology->vertices.begin(),retired.modelingTopology->vertices.end());
     for(auto* list:{&retired.modelingTopology->attributes,&retired.modelingTopology->corners,&retired.modelingTopology->triangles,&retired.modelingTopology->faces})ids.insert(list->begin(),list->end());
     for(auto& [e,id]:retired.modelingTopology->edges)ids.insert(id);
     check(*ids.begin()>=50&&retired.modelingNextId>*ids.rbegin(),"re-migrated IDs stay above the retired high-water mark");}
    {std::istringstream truncated("modeling_ids 9\ngeometric_ids 4 1 2");check(!read_modeling_topology(truncated,&error),"truncated record refuses");
     std::istringstream huge("modeling_ids 9\ngeometric_ids 18446744073709551615");check(!read_modeling_topology(huge,&error),"oversized count refuses before allocation");
     std::istringstream zero("modeling_ids 0\n");check(!read_modeling_topology(zero,&error),"zero allocation counter refuses");}
    EditorDocument document("Legacy");EditorObject object(1,"Cage");object.polygon=f.mesh;object.subdivision=f.settings;document.add_object(std::move(object));
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));auto& c=*owner;c.resize(1600,900);c.workspace().select_object(1);
    check(c.dispatch_action("subdiv.edge_mode"),"edge mode");const auto seed=*subdivision_control_edges(f.mesh,&f.settings).begin();c.select_polygon_components({seed},SelectionOperation::Replace);
    const auto history=c.workspace().commands().size();
    check(c.dispatch_action("subdiv.loop_cut")&&c.workspace().document().find_object(1)->modelingTopology,"legacy preview migrates lazily");
    c.key_down("escape",false,false,false);
    check(!c.workspace().document().find_object(1)->modelingTopology&&c.workspace().commands().size()==history,"Cancel discards the lazy migration and adds no history");
    check(c.dispatch_action("subdiv.loop_cut")&&c.dispatch_action("subdiv.preview_apply")&&c.workspace().document().find_object(1)->modelingTopology,"legacy cut Apply records IDs");
    check(c.dispatch_action("edit.undo")&&!c.workspace().document().find_object(1)->modelingTopology&&c.polygon_selection().components==std::set{seed},"Undo restores the exact ID-less state and selection");
    const auto path=std::filesystem::temp_directory_path()/"dve-modeling-legacy-test"/"scene.dvescene";std::filesystem::remove_all(path.parent_path());
    check(c.workspace().document().save_transactional(path).success,"save legacy after Undo");std::ifstream manifest(path);std::string header;std::getline(manifest,header);manifest.close();
    check(header=="DVE_EDITOR_SCENE 10","no identity field written after Undo removed every edit");
    check(c.dispatch_action("edit.redo")&&c.workspace().document().find_object(1)->modelingTopology,"Redo restores IDs");
    check(c.dispatch_action("edit.undo")&&c.dispatch_action("subdiv.loop_cut"),"re-migrating after Undo still works");c.key_down("escape",false,false,false);
    std::filesystem::remove_all(path.parent_path());
}
void scene_history_and_prefab() {
    auto f=quad();EditorDocument document("Persistent identities");EditorObject object(1,"Cage");object.polygon=f.mesh;object.subdivision=f.settings;std::string error;
    check(ensure_modeling_topology(object,&error),error);const auto initial=*object.modelingTopology;document.add_object(std::move(object));
    const auto path=std::filesystem::temp_directory_path()/"dve-modeling-identities-test"/"scene.dvescene";std::filesystem::remove_all(path.parent_path());
    const auto saved=document.save_transactional(path);check(saved.success,saved.error);std::ifstream manifest(path);std::string header;std::getline(manifest,header);manifest.close();check(header=="DVE_EDITOR_SCENE 11","identity scene uses format 11");
    auto loaded=EditorDocument::load(path,&error);check(bool(loaded),error);check(loaded->find_object(1)->modelingTopology==initial,"scene save/reopen preserves modeling IDs");
    auto cloned=clone_editor_document(*loaded);check(cloned.find_object(1)->modelingTopology==initial,"document clone retains local identity namespace");
    auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(*loaded)));auto& c=*owner;c.resize(1600,900);c.workspace().select_object(1);
    check(c.dispatch_action("subdiv.edge_mode"),"edge mode");const auto seed=*subdivision_control_edges(f.mesh,&f.settings).begin();c.select_polygon_components({seed},SelectionOperation::Replace);
    check(c.dispatch_action("subdiv.loop_cut"),"identity cut preview");auto preview=*c.workspace().document().find_object(1)->modelingTopology;
    check(c.dispatch_action("subdiv.preview_apply"),"identity cut Apply");check(*c.workspace().document().find_object(1)->modelingTopology==preview,"Apply retains preview identities");
    const auto ceiling=c.workspace().document().find_object(1)->modelingNextId;check(c.dispatch_action("edit.undo"),"identity cut Undo");
    check(*c.workspace().document().find_object(1)->modelingTopology==initial&&c.workspace().document().find_object(1)->modelingNextId>=ceiling&&c.polygon_selection().components==std::set{seed},"Undo restores IDs and selection without lowering allocation floor");
    check(c.dispatch_action("edit.redo")&&*c.workspace().document().find_object(1)->modelingTopology==preview,"Redo restores exact child identities");
    check(c.dispatch_action("edit.undo")&&c.dispatch_action("subdiv.loop_cut"),"new history branch preview");auto branch=*c.workspace().document().find_object(1)->modelingTopology;
    std::set<ModelingId> oldCreated(preview.vertices.begin(),preview.vertices.end());for(auto id:initial.vertices)oldCreated.erase(id);
    for(auto id:branch.vertices)check(!oldCreated.contains(id),"new branch does not reuse retired split vertices");c.key_down("escape",false,false,false);
    check(*c.workspace().document().find_object(1)->modelingTopology==initial,"Cancel restores original identity record");
    check(c.workspace().document().save_transactional(path).success,"save after Undo/Cancel");auto reopened=EditorDocument::load(path,&error);check(reopened&&reopened->find_object(1)->modelingNextId>=branch.nextId,"allocation floor survives reopening");
    const auto duplicate=clone_editor_object(*reopened->find_object(1));check(duplicate.modelingTopology==reopened->find_object(1)->modelingTopology,"object clone preserves scoped IDs");
    std::filesystem::remove_all(path.parent_path());
}
#endif
}
int main(){try{serialization_and_partitions();movement_reorder_and_retirement();cut_and_cleanup();dissolve_and_merge_identities();
#if defined(DVE_MODELING_TOPOLOGY_COMMANDS) || !defined(DVE_MODELING_TOPOLOGY_CORE_ONLY)
command_history();
#endif
#if defined(DVE_MODELING_TOPOLOGY_SCENE) || !defined(DVE_MODELING_TOPOLOGY_CORE_ONLY)
scene_and_prefab_persistence();
#endif
#ifndef DVE_MODELING_TOPOLOGY_CORE_ONLY
scene_history_and_prefab();legacy_migration_is_not_an_edit();
#endif
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}std::cout<<"Persistent modeling topology regression checks passed\n";}
