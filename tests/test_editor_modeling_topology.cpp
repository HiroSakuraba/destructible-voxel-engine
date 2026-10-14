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

void authored_face_tool_lineage() {
    auto f=quad();for(std::size_t i=0;i<f.mesh.vertices.size();++i){f.mesh.vertices[i].texcoord1={.25F+float(i),.75F-float(i)};f.mesh.vertices[i].color={.2F,.3F,.4F,.5F};}refresh_editor_polygon(f.mesh);
    std::string error;const auto seed=*subdivision_control_edges(f.mesh,&f.settings).begin();f.settings.edgeCreases[seed]=2;
    auto topo=make_modeling_topology(f.mesh,&f.settings,{},&error);check(bool(topo),error);
    PolygonUvAuthoring flags;flags.enabled=false;flags.layoutHash=polygon_uv_layout_hash(f.mesh);flags.seams[0]={seed};flags.seams[1]={seed};flags.pins[1]={1};
    const PolygonSelection all{1,PolygonSelectionMode::Face,{0,1}};
    for(auto operation:{PolygonEdit::Extrude,PolygonEdit::Inset,PolygonEdit::Bevel}) {
        PolygonEditSettings settings;settings.amount=.3F;settings.inset=.2F;
        auto edit=edit_polygon_authored_faces(f.mesh,all,operation,settings,&f.settings);check(bool(edit),edit.polygon.error);
        check(edit.subdivision&&edit.subdivision->controlFaces.size()==5&&edit.polygon.mesh.indices.size()==30,"face cap plus four explicit side quads");
        auto ids=advance_modeling_topology(f.mesh,edit.polygon.mesh,&*edit.subdivision,*topo,edit.lineage);check(bool(ids),ids.error);
        check(ids.topology.faces[0]==topo->faces[0]&&ids.topology.triangles[0]==topo->triangles[0]&&ids.topology.corners[1]==topo->corners[1],"cap retains face triangle and corner IDs");
        for(std::uint32_t i=0;i<f.mesh.vertices.size();++i) {
            check(ids.topology.vertices[i]==topo->vertices[i]&&ids.topology.attributes[i]==topo->attributes[i],"base vertices and attributes retain identity");
            const auto copy=*edit.lineage.attributeCopies.at(i).begin();
            check(ids.topology.vertices[copy]!=topo->vertices[i]&&ids.topology.attributes[copy]!=topo->attributes[i],"cap has separate created geometry and attribute IDs");
            check(edit.polygon.mesh.vertices[copy].texcoord.x==f.mesh.vertices[i].texcoord.x&&edit.polygon.mesh.vertices[copy].texcoord1.x==f.mesh.vertices[i].texcoord1.x&&edit.polygon.mesh.vertices[copy].texcoord1.y==f.mesh.vertices[i].texcoord1.y&&edit.polygon.mesh.vertices[copy].color.w==f.mesh.vertices[i].color.w,"cap copies retain both UV channels and color");
        }
        const auto descendants=edit.lineage.edgeDescendants.at(seed);check(descendants.size()==2,"source boundary crease maps to base and cap edges");
        for(auto edge:descendants)check(edit.subdivision->edgeCreases.at(edge)==2,"base and cap inherit crease strength");
        auto mapped=remap_polygon_uv_authoring(flags,edit.polygon.mesh,edit.lineage,&error);
        check(mapped&&!mapped->enabled&&mapped->seams[0]==descendants&&mapped->seams[1]==descendants&&mapped->pins[1]==flags.pins[1],"both seam channels follow edge copies while pins stay on cap corners");
        const auto pair=std::minmax(topo->vertices[std::uint32_t(seed>>32U)],topo->vertices[std::uint32_t(seed)]);const auto parent=topo->edges.at({pair.first,pair.second});
        check(ids.topology.edges.at({pair.first,pair.second})==parent&&ids.descendants.at(parent).size()==2,"copied edge retains the untouched base ID and identifies its cap child");
        for(auto edge:subdivision_control_edges(edit.polygon.mesh,&*edit.subdivision))if(!descendants.contains(edge)&&!subdivision_control_edges(f.mesh,&f.settings).contains(edge))
            check(!edit.subdivision->edgeCreases.contains(edge),"unrelated connecting edges default smooth");
        auto againSettings=settings;againSettings.amount=.6F;againSettings.inset=.35F;
        auto again=edit_polygon_authored_faces(f.mesh,all,operation,againSettings,&f.settings);check(bool(again),again.polygon.error);
        auto againIds=advance_modeling_topology(f.mesh,again.polygon.mesh,&*again.subdivision,*topo,again.lineage);
        check(againIds&&againIds.topology==ids.topology,"changing preview values preserves provisional IDs");
        check(!edit_polygon_authored_faces(f.mesh,{1,PolygonSelectionMode::Face,{0}},operation,settings,&f.settings),"partial authored quad refuses cap edits");
    }
    auto materialSplit=f.mesh;materialSplit.materials.resize(2);materialSplit.materialBindings.resize(2);materialSplit.submeshes={{"A",0,3,0},{"B",3,3,1}};refresh_editor_polygon(materialSplit);
    auto multimat=edit_polygon_authored_faces(materialSplit,all,PolygonEdit::Extrude);check(bool(multimat),multimat.polygon.error);
    std::vector<std::uint32_t> materialByFace(multimat.polygon.mesh.indices.size()/3);
    for(const auto& sub:multimat.polygon.mesh.submeshes)for(auto f=sub.firstIndex/3;f<(sub.firstIndex+sub.indexCount)/3;++f)materialByFace[f]=sub.materialIndex;
    for(std::size_t i=materialSplit.indices.size();i<multimat.polygon.mesh.indices.size();i+=6) {
        const auto a=multimat.polygon.mesh.indices[i],b=multimat.polygon.mesh.indices[i+1];std::optional<std::uint32_t> owner;
        for(std::uint32_t t=0;t<2;++t)for(unsigned j=0;j<3;++j)if(materialSplit.indices[t*3+j]==a&&materialSplit.indices[t*3+(j+1)%3]==b)owner=t;
        check(owner&&materialByFace[i/3]==*owner&&materialByFace[i/3+1]==*owner,"side walls inherit the material of their own adjacent cap face");
    }
    PolygonEditSettings noOp;noOp.amount=0;noOp.inset=0;
    check(!edit_polygon_authored_faces(f.mesh,all,PolygonEdit::Extrude,noOp,&f.settings)&&!edit_polygon_authored_faces(f.mesh,all,PolygonEdit::Inset,noOp,&f.settings),"degenerate zero-value face edits refuse");
    PolygonEditSettings uv;uv.uvAxis=2;uv.amount=2;
    auto projected=edit_polygon_authored_faces(f.mesh,{1,PolygonSelectionMode::Face,{0}},PolygonEdit::ProjectUV,uv,&f.settings);check(bool(projected),projected.polygon.error);
    auto projectionIds=advance_modeling_topology(f.mesh,projected.polygon.mesh,&*projected.subdivision,*topo,projected.lineage);check(bool(projectionIds),projectionIds.error);
    check(projected.polygon.mesh.vertices.size()>f.mesh.vertices.size()&&projectionIds.topology.corners==topo->corners,"UV projection detaches shared attributes without replacing corner IDs");
    for(const auto& [old,copies]:projected.lineage.attributeCopies)for(auto copy:copies)check(projectionIds.topology.vertices[copy]==topo->vertices[old]&&projectionIds.topology.attributes[copy]!=topo->attributes[old],"UV copy keeps geometry identity but gets a distinct attribute ID");
    auto cube=make_editor_polygon_cube(1);SubdivisionSettings cubeSettings;cubeSettings.level=0;freeze_subdivision_topology(cube,cubeSettings);
    auto cubeIds=make_modeling_topology(cube,&cubeSettings,{},&error);check(bool(cubeIds),error);
    auto flipped=edit_polygon_authored_faces(cube,{1,PolygonSelectionMode::Face,polygon_all_components(cube,PolygonSelectionMode::Face)},PolygonEdit::FlipNormals,{},&cubeSettings);check(bool(flipped),flipped.polygon.error);
    auto flipIds=advance_modeling_topology(cube,flipped.polygon.mesh,&*flipped.subdivision,*cubeIds,flipped.lineage);check(bool(flipIds),flipIds.error);
    check(flipIds.topology.faces==cubeIds->faces&&flipIds.topology.corners[2]==cubeIds->corners[1],"whole-shell reversal preserves faces and permutes stable corner identities");
    flags={};flags.layoutHash=polygon_uv_layout_hash(cube);flags.pins[0]={1};auto reversedFlags=remap_polygon_uv_authoring(flags,flipped.polygon.mesh,flipped.lineage,&error);
    check(reversedFlags&&reversedFlags->pins[0]==std::set<std::uint32_t>{2},"normal reversal moves pinned corner to its new array index");
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
    for(auto operation:{PolygonEdit::Extrude,PolygonEdit::Inset,PolygonEdit::Bevel}) {
        EditorDocument patchDoc("Face patch history");EditorObject target(1,"Quad");target.polygon=f.mesh;target.subdivision=f.settings;check(ensure_modeling_topology(target,&error),error);patchDoc.add_object(std::move(target));
        const auto before=*patchDoc.find_object(1)->modelingTopology;
        PolygonUvAuthoring flags;flags.layoutHash=polygon_uv_layout_hash(f.mesh);flags.pins[1]={1};flags.seams[0]={seed};
        auto flagsCommand=make_polygon_uv_authoring_command(patchDoc,*patchDoc.find_object(1),flags,&error);check(flagsCommand&&flagsCommand->execute(patchDoc).success,"save face command UV flags");
        auto edit=edit_polygon_authored_faces(f.mesh,{1,PolygonSelectionMode::Face,{0,1}},operation,{},&f.settings);check(bool(edit),edit.polygon.error);
        auto ids=advance_modeling_topology(f.mesh,edit.polygon.mesh,&*edit.subdivision,before,edit.lineage,patchDoc.find_object(1)->modelingNextId);check(bool(ids),ids.error);
        auto record=remap_polygon_uv_authoring_record(*patchDoc.find_object(1),edit.polygon.mesh,edit.lineage,&error);check(bool(record),error);
        const auto* component=find_component_by_type(std::span<const Component>(patchDoc.find_object(1)->components),kUvAuthoringComponent);
        auto compound=std::make_unique<CompoundCommand>("Authored face edit");
        compound->add(std::make_unique<ReplacePolygonCommand>(1,f.mesh,edit.polygon.mesh,"Face edit",0,edit.subdivision,false,ids.topology));
        compound->add(std::make_unique<SetComponentPropertyCommand>(1,component->id,"state",component->properties.at("state"),ComponentValue{*record},"Map face UV flags"));
        EditorCommandStack history;check(history.execute(patchDoc,std::move(compound)).success,"real compound face edit commits");
        auto current=read_polygon_uv_authoring(*patchDoc.find_object(1),&error);check(current&&current->seams[0].size()==2&&*patchDoc.find_object(1)->modelingTopology==ids.topology,"real face command keeps copied seams and prepared identities");
        check(history.undo(patchDoc).success,"real compound face Undo");current=read_polygon_uv_authoring(*patchDoc.find_object(1),&error);
        check(current&&current->seams==flags.seams&&current->pins==flags.pins&&*patchDoc.find_object(1)->modelingTopology==before,"real compound Undo restores flags and identities");
        check(history.redo(patchDoc).success&&*patchDoc.find_object(1)->modelingTopology==ids.topology,"real compound face Redo restores exact IDs");
    }

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
    EditorDocument retired("Retired namespace");EditorObject emptyRecord(1,"Quad");emptyRecord.polygon=f.mesh;emptyRecord.subdivision=f.settings;emptyRecord.modelingNextId=500;retired.add_object(std::move(emptyRecord));
    const auto retiredPath=root/"retired.dvescene";check(retired.save_transactional(retiredPath).success,"save an ID-less retired namespace");
    auto retiredReload=EditorDocument::load(retiredPath,&error);check(retiredReload&&!retiredReload->find_object(1)->modelingTopology&&retiredReload->find_object(1)->modelingNextId==500,"retired allocation floor survives save/reopen without inventing a live record");
    check(ensure_modeling_topology(*retiredReload->find_object(1),&error),error);for(auto id:retiredReload->find_object(1)->modelingTopology->vertices)check(id>=500,"post-reopen migration never reuses retired IDs");
    std::filesystem::remove_all(root);
}
#endif
#ifndef DVE_MODELING_TOPOLOGY_CORE_ONLY
void native_authored_face_tools() {
    for(const auto* action:{"subdiv.extrude","subdiv.inset","subdiv.bevel","polygon.uv_tools"}) {
        auto f=quad();std::string error;const auto seed=*subdivision_control_edges(f.mesh,&f.settings).begin();f.settings.edgeCreases[seed]=2;
        EditorDocument document("Authored face tools");EditorObject object(1,"Quad");object.polygon=f.mesh;object.subdivision=f.settings;check(ensure_modeling_topology(object,&error),error);
        const auto beforeIds=*object.modelingTopology;document.add_object(std::move(object));auto owner=std::make_unique<NativeEditorController>(EditorWorkspace(std::move(document)));auto& c=*owner;
        c.resize(1600,900);c.workspace().select_object(1);check(c.dispatch_action("subdiv.face_mode"),"face tool mode");c.select_polygon_components({0,1},SelectionOperation::Replace);
        PolygonUvAuthoring flags;flags.enabled=false;flags.layoutHash=polygon_uv_layout_hash(f.mesh);flags.seams[1]={seed};flags.pins[0]={1};
        auto uv=make_polygon_uv_authoring_command(c.workspace().document(),*c.workspace().document().find_object(1),flags,&error);check(uv&&c.workspace().commands().execute(c.workspace().document(),std::move(uv)).success,"save face flags");
        const auto history=c.workspace().commands().size(),hash=polygon_asset_content_hash(f.mesh);
        check(c.dispatch_action(action),std::string("authored preview: ")+action);c.key_down("escape",false,false,false);
        auto* target=c.workspace().document().find_object(1);check(*target->modelingTopology==beforeIds&&polygon_asset_content_hash(*target->polygon)==hash&&c.workspace().commands().size()==history,"face Cancel restores geometry and IDs without history");
        check(c.dispatch_action(action),"face preview reopens");const auto preview=*c.workspace().document().find_object(1)->modelingTopology;
        check(c.dispatch_action("subdiv.preview_apply"),"face tool Apply");target=c.workspace().document().find_object(1);const auto afterHash=polygon_asset_content_hash(*target->polygon);
        check(*target->modelingTopology==preview&&c.workspace().commands().size()==history+1,"Apply uses preview IDs and one history transaction");
        check(c.dispatch_action("edit.undo"),"face tool Undo");target=c.workspace().document().find_object(1);const auto restored=read_polygon_uv_authoring(*target,&error);
        check(*target->modelingTopology==beforeIds&&*target->subdivision==f.settings&&polygon_asset_content_hash(*target->polygon)==hash&&restored&&restored->seams==flags.seams&&restored->pins==flags.pins,"face Undo restores geometry settings IDs and flags");
        check(c.dispatch_action("edit.redo")&&polygon_asset_content_hash(*c.workspace().document().find_object(1)->polygon)==afterHash,"face Redo restores exact output");
    }
}
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
    check(header=="DVE_EDITOR_SCENE 12","Undo retires IDs but persists their allocation floor");
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
int main(){try{serialization_and_partitions();movement_reorder_and_retirement();cut_and_cleanup();dissolve_and_merge_identities();authored_face_tool_lineage();
#if defined(DVE_MODELING_TOPOLOGY_COMMANDS) || !defined(DVE_MODELING_TOPOLOGY_CORE_ONLY)
command_history();
#endif
#if defined(DVE_MODELING_TOPOLOGY_SCENE) || !defined(DVE_MODELING_TOPOLOGY_CORE_ONLY)
scene_and_prefab_persistence();
#endif
#ifndef DVE_MODELING_TOPOLOGY_CORE_ONLY
scene_history_and_prefab();legacy_migration_is_not_an_edit();native_authored_face_tools();
#endif
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}std::cout<<"Persistent modeling topology regression checks passed\n";}
