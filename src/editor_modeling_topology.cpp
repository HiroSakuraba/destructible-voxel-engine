#include "dve/editor_modeling_topology.hpp"
#include "dve/editor_polygon.hpp"
#include <algorithm>
#include <array>
#include <istream>
#include <ostream>
#include <limits>
#include <stdexcept>
#include <tuple>
namespace dve::editor {
namespace {
using Position=std::tuple<float,float,float>;
using Pair=std::pair<ModelingId,ModelingId>;
Pair pair(ModelingId a,ModelingId b){return {(std::min)(a,b),(std::max)(a,b)};}
std::vector<std::uint32_t> position_groups(const CookedPolygonAsset& mesh) {
    std::map<Position,std::uint32_t> first;std::vector<std::uint32_t> groups;
    for(std::uint32_t i=0;i<mesh.vertices.size();++i){auto p=mesh.vertices[i].position;groups.push_back(first.emplace(Position{p.x,p.y,p.z},i).first->second);}return groups;
}
std::vector<std::vector<std::uint32_t>> face_groups(const CookedPolygonAsset& mesh,const SubdivisionSettings* settings) {
    if(settings)return subdivision_control_face_triangles(mesh,settings);
    std::vector<std::vector<std::uint32_t>> result;for(std::uint32_t t=0;t<mesh.indices.size()/3;++t)result.push_back({t});return result;
}
std::set<Pair> mesh_edges(const CookedPolygonAsset& mesh,const std::vector<ModelingId>& vertices) {
    std::set<Pair> edges;for(std::size_t i=0;i<mesh.indices.size();i+=3)for(unsigned j=0;j<3;++j)
        edges.insert(pair(vertices[mesh.indices[i+j]],vertices[mesh.indices[i+(j+1)%3]]));return edges;
}
std::set<ModelingId> control_edges(const CookedPolygonAsset& mesh,const SubdivisionSettings* settings,const ModelingTopology& topo) {
    std::set<ModelingId> result;
    if(!settings){for(auto& [edge,id]:topo.edges)result.insert(id);return result;}
    for(auto key:subdivision_control_edges(mesh,settings)) {
        auto edge=pair(topo.vertices.at(std::uint32_t(key>>32U)),topo.vertices.at(std::uint32_t(key)));
        if(auto at=topo.edges.find(edge);at!=topo.edges.end())result.insert(at->second);
    }
    return result;
}
std::set<ModelingId> all_ids(const ModelingTopology& topo) {
    std::set<ModelingId> ids(topo.vertices.begin(),topo.vertices.end());
    for(auto* list:{&topo.attributes,&topo.corners,&topo.triangles,&topo.faces})ids.insert(list->begin(),list->end());
    for(auto& [edge,id]:topo.edges)ids.insert(id);return ids;
}
ModelingId allocate(ModelingId& next) {
    if(!next||next==(std::numeric_limits<ModelingId>::max)())throw std::runtime_error("Modeling identity space exhausted");return next++;
}
// Legacy positions cannot distinguish coincident disconnected fans or coincident faces.
bool unambiguous_legacy(const CookedPolygonAsset& mesh,const std::vector<std::uint32_t>& groups) {
    std::map<std::uint32_t,std::set<std::uint32_t>> incident;
    std::map<std::pair<std::uint32_t,std::uint32_t>,std::vector<std::uint32_t>> edges;
    std::set<std::array<std::uint32_t,3>> triangles;
    for(std::uint32_t t=0;t<mesh.indices.size()/3;++t){std::array<std::uint32_t,3> v;
        for(unsigned j=0;j<3;++j){v[j]=groups[mesh.indices[t*3+j]];incident[v[j]].insert(t);}
        auto sorted=v;std::sort(sorted.begin(),sorted.end());if(sorted[0]==sorted[1]||sorted[1]==sorted[2]||!triangles.insert(sorted).second)return false;
        for(unsigned j=0;j<3;++j){auto a=v[j],b=v[(j+1)%3];auto& list=edges[{(std::min)(a,b),(std::max)(a,b)}];list.push_back(t);if(list.size()>2)return false;}}
    std::map<std::uint32_t,std::map<std::uint32_t,std::set<std::uint32_t>>> links;
    for(auto& [edge,faces]:edges)if(faces.size()==2)for(auto v:{edge.first,edge.second}){links[v][faces[0]].insert(faces[1]);links[v][faces[1]].insert(faces[0]);}
    for(auto& [v,faces]:incident){std::set<std::uint32_t> seen;std::vector<std::uint32_t> todo{*faces.begin()};
        while(!todo.empty()){auto f=todo.back();todo.pop_back();if(!seen.insert(f).second)continue;for(auto n:links[v][f])todo.push_back(n);}
        if(seen.size()!=faces.size())return false;}
    return true;
}
}
std::optional<ModelingTopology> make_modeling_topology(const CookedPolygonAsset& mesh,const SubdivisionSettings* settings,
    std::span<const std::uint32_t> partition,std::string* error) {
    const auto fail=[&](std::string message)->std::optional<ModelingTopology>{if(error)*error=std::move(message);return {};};if(error)error->clear();
    if(mesh.vertices.size()>100000||mesh.indices.size()>150000)return fail("Modeling identity budget exceeded");
    if(auto valid=validate_polygon_asset(mesh);!valid)return fail(valid.message);
    auto groups=position_groups(mesh);
    if(!partition.empty()){if(partition.size()!=mesh.vertices.size())return fail("Invalid geometric partition size");groups.assign(partition.begin(),partition.end());}
    else if(!unambiguous_legacy(mesh,groups))return fail("Coincident or disconnected legacy geometry needs an explicit geometric identity partition");
    ModelingTopology result;std::map<std::uint32_t,ModelingId> geometric;
    try {
        for(auto g:groups){auto at=geometric.find(g);if(at==geometric.end())at=geometric.emplace(g,allocate(result.nextId)).first;result.vertices.push_back(at->second);result.attributes.push_back(allocate(result.nextId));}
        for(std::size_t i=0;i<mesh.indices.size();++i)result.corners.push_back(allocate(result.nextId));
        for(std::size_t i=0;i<mesh.indices.size()/3;++i)result.triangles.push_back(allocate(result.nextId));
        for(auto e:mesh_edges(mesh,result.vertices))result.edges.emplace(e,allocate(result.nextId));
        for(const auto& face:face_groups(mesh,settings)){result.faces.push_back(allocate(result.nextId));std::vector<ModelingId> triangles;
            for(auto t:face)triangles.push_back(result.triangles.at(t));result.faceTriangles.push_back(std::move(triangles));}
    }catch(const std::exception& e){return fail(e.what());}
    result.controlEdges=control_edges(mesh,settings,result);
    if(!validate_modeling_topology(mesh,settings,result,error))return {};return result;
}
bool validate_modeling_topology(const CookedPolygonAsset& mesh,const SubdivisionSettings* settings,const ModelingTopology& topo,std::string* error) {
    const auto fail=[&](const char* message){if(error)*error=message;return false;};if(error)error->clear();
    if(mesh.vertices.size()>100000||mesh.indices.size()>150000||topo.vertices.size()!=mesh.vertices.size()||topo.attributes.size()!=mesh.vertices.size()||
        topo.corners.size()!=mesh.indices.size()||topo.triangles.size()!=mesh.indices.size()/3||topo.faces.size()!=topo.faceTriangles.size())return fail("Modeling identity arrays do not match the mesh");
    if(!validate_polygon_asset(mesh))return fail("Invalid mesh for modeling identities");
    std::map<ModelingId,Position> geometric;
    for(std::size_t i=0;i<topo.vertices.size();++i){auto p=mesh.vertices[i].position;Position point{p.x,p.y,p.z};auto [at,added]=geometric.emplace(topo.vertices[i],point);
        if(!added&&at->second!=point)return fail("One geometric identity spans different positions");}
    std::set<ModelingId> used;for(auto& [id,p]:geometric)used.insert(id);
    for(auto* list:{&topo.attributes,&topo.corners,&topo.triangles,&topo.faces})for(auto id:*list)if(!used.insert(id).second)return fail("Duplicate modeling identity");
    const auto expected=mesh_edges(mesh,topo.vertices);if(expected.size()!=topo.edges.size())return fail("Modeling edges do not match connectivity");
    for(auto& [edge,id]:topo.edges)if(edge.first==edge.second||!expected.contains(edge)||!used.insert(id).second)return fail("Invalid modeling edge identity");
    if(topo.controlEdges!=control_edges(mesh,settings,topo))return fail("Authored edge identities do not match control faces");
    const auto groups=face_groups(mesh,settings);if(groups.size()!=topo.faces.size() || (groups.empty()&&!topo.triangles.empty()))return fail("Saved face identities do not match control faces");
    for(std::size_t i=0;i<groups.size();++i){std::vector<ModelingId> ids;for(auto t:groups[i])ids.push_back(topo.triangles.at(t));
        auto recorded=topo.faceTriangles[i];std::sort(ids.begin(),ids.end());std::sort(recorded.begin(),recorded.end());if(ids!=recorded)return fail("Saved face identity has different triangle membership");}
    if(used.empty()||used.contains(0)||!topo.nextId||*used.rbegin()>=topo.nextId)return fail("Invalid modeling identity allocation counter");return true;
}
bool modeling_matches_legacy_partition(const CookedPolygonAsset& mesh,const ModelingTopology& topo) {
    if(topo.vertices.size()!=mesh.vertices.size())return false;const auto positions=position_groups(mesh);
    std::map<std::uint32_t,ModelingId> forward;std::map<ModelingId,std::uint32_t> back;
    for(std::size_t i=0;i<positions.size();++i){if(auto [at,newKey]=forward.emplace(positions[i],topo.vertices[i]);!newKey&&at->second!=topo.vertices[i])return false;
        if(auto [at,newKey]=back.emplace(topo.vertices[i],positions[i]);!newKey&&at->second!=positions[i])return false;}return true;
}
PolygonTopologyLineage identity_polygon_lineage(const CookedPolygonAsset& mesh,const SubdivisionSettings* settings) {
    PolygonTopologyLineage result;result.afterVertexGroups=position_groups(mesh);
    for(std::uint32_t i=0;i<mesh.vertices.size();++i)result.vertexSurvivors[i]=i;
    for(std::uint32_t i=0;i<mesh.indices.size();++i)result.cornerDescendants[i]={i};
    for(std::uint32_t i=0;i<mesh.indices.size()/3;++i)result.triangleDescendants[i]={i};
    for(auto key:polygon_all_components(mesh,PolygonSelectionMode::Edge))result.edgeDescendants[key]={key};
    const auto groups=face_groups(mesh,settings);for(std::uint32_t i=0;i<groups.size();++i)result.controlFaceDescendants[i]={i};return result;
}
ModelingChange advance_modeling_topology(const CookedPolygonAsset& before,const CookedPolygonAsset& after,const SubdivisionSettings* settings,
    const ModelingTopology& source,const PolygonTopologyLineage& lineage,ModelingId floor) {
    ModelingChange result;const auto fail=[&](std::string message){result.error=std::move(message);return result;};
    if(before.vertices.size()>100000||after.vertices.size()>100000||before.indices.size()>150000||after.indices.size()>150000||
        lineage.afterVertexGroups.size()!=after.vertices.size()||source.vertices.size()!=before.vertices.size()||source.attributes.size()!=before.vertices.size()||
        source.corners.size()!=before.indices.size()||source.triangles.size()!=before.indices.size()/3)return fail("Incomplete modeling geometric lineage");
    auto& topo=result.topology;topo.nextId=(std::max)(source.nextId,floor);const auto oldIds=all_ids(source);for(auto id:oldIds)result.descendants[id];
    try {
        // Each target collects known parents. Splits get fresh IDs; a many-to-one merge
        // retains the smallest participating identity. There is no position-based parent lookup.
        const auto assign=[&](const auto& old,const auto& mapping,std::size_t size) {
            std::vector<std::set<ModelingId>> candidates(size);std::vector<ModelingId> output(size);std::set<ModelingId> taken;
            for(auto& [index,next]:mapping){if(index>=old.size())throw std::runtime_error("Stale modeling lineage source");for(auto n:next){if(n>=size)throw std::runtime_error("Stale modeling lineage destination");if(next.size()==1)candidates[n].insert(old[index]);}}
            for(std::size_t i=0;i<size;++i){for(auto id:candidates[i])if(taken.insert(id).second){output[i]=id;break;}if(!output[i])output[i]=allocate(topo.nextId);}
            for(auto& [index,next]:mapping)for(auto n:next)result.descendants[old[index]].insert(output[n]);return output;
        };
        std::map<std::uint32_t,std::set<std::uint32_t>> attributeMap;for(auto& [old,next]:lineage.vertexSurvivors)attributeMap[old]={next};
        topo.attributes=assign(source.attributes,attributeMap,after.vertices.size());
        std::map<std::uint32_t,std::set<ModelingId>> parents;
        for(auto& [old,next]:lineage.vertexSurvivors){if(old>=source.vertices.size()||next>=after.vertices.size())throw std::runtime_error("Stale vertex survivor");parents[lineage.afterVertexGroups[next]].insert(source.vertices[old]);}
        std::map<std::uint32_t,ModelingId> groups;std::set<ModelingId> taken;
        for(auto label:lineage.afterVertexGroups)if(!groups.contains(label)){ModelingId chosen=0;for(auto id:parents[label])if(taken.insert(id).second){chosen=id;break;}groups[label]=chosen?chosen:allocate(topo.nextId);}
        for(auto label:lineage.afterVertexGroups)topo.vertices.push_back(groups.at(label));
        for(auto& [old,next]:lineage.vertexSurvivors)result.descendants[source.vertices[old]].insert(topo.vertices[next]);
        topo.corners=assign(source.corners,lineage.cornerDescendants,after.indices.size());
        topo.triangles=assign(source.triangles,lineage.triangleDescendants,after.indices.size()/3);
        const auto faces=face_groups(after,settings);std::map<std::uint32_t,std::set<std::uint32_t>> faceMap;
        for(auto& [old,next]:lineage.controlFaceDescendants)faceMap[old].insert(next.begin(),next.end());
        topo.faces=assign(source.faces,faceMap,faces.size());
        for(auto& face:faces){std::vector<ModelingId> ids;for(auto t:face)ids.push_back(topo.triangles.at(t));topo.faceTriangles.push_back(std::move(ids));}
        std::map<Pair,std::set<ModelingId>> edgeParents;
        const auto endpoint=[&](std::uint64_t key,const ModelingTopology& t){auto a=std::uint32_t(key>>32U),b=std::uint32_t(key);if(a>=t.vertices.size()||b>=t.vertices.size())throw std::runtime_error("Stale edge lineage");return pair(t.vertices[a],t.vertices[b]);};
        for(auto& [old,next]:lineage.edgeDescendants){auto at=source.edges.find(endpoint(old,source));if(at==source.edges.end())throw std::runtime_error("Unknown source edge identity");if(next.size()==1)edgeParents[endpoint(*next.begin(),topo)].insert(at->second);}
        taken.clear();for(auto e:mesh_edges(after,topo.vertices)){ModelingId chosen=0;for(auto id:edgeParents[e])if(taken.insert(id).second){chosen=id;break;}topo.edges[e]=chosen?chosen:allocate(topo.nextId);}
        for(auto& [old,next]:lineage.edgeDescendants){auto id=source.edges.at(endpoint(old,source));for(auto n:next){auto at=topo.edges.find(endpoint(n,topo));if(at==topo.edges.end())throw std::runtime_error("Missing descendant edge");result.descendants[id].insert(at->second);}}
    }catch(const std::exception& e){return fail(e.what());}
    topo.controlEdges=control_edges(after,settings,topo);
    if(!validate_modeling_topology(after,settings,topo,&result.error))return result;
    for(auto id:all_ids(topo))if(!oldIds.contains(id))result.created.insert(id);return result;
}
ModelingChange change_modeling_faces(const CookedPolygonAsset& mesh,const SubdivisionSettings* before,const SubdivisionSettings* after,
    const ModelingTopology& topo,ModelingId floor,const CookedPolygonAsset* output) {
    const auto& target=output?*output:mesh;
    if(mesh.indices!=target.indices||mesh.vertices.size()!=target.vertices.size()) {ModelingChange failed;failed.error="Saved-face remap requires unchanged render connectivity";return failed;}
    auto lineage=identity_polygon_lineage(mesh,before);lineage.controlFaceDescendants.clear();
    const auto oldGroups=face_groups(mesh,before),newGroups=face_groups(target,after);std::map<std::uint32_t,std::uint32_t> owner;
    for(std::uint32_t f=0;f<newGroups.size();++f)for(auto t:newGroups[f])owner[t]=f;
    for(std::uint32_t f=0;f<oldGroups.size();++f){std::set<std::uint32_t> next;for(auto t:oldGroups[f])if(owner.contains(t))next.insert(owner.at(t));lineage.controlFaceDescendants[f]={next.begin(),next.end()};}
    // Geometry is unchanged; use explicit existing groups even when positions coincide.
    std::map<ModelingId,std::uint32_t> labels;lineage.afterVertexGroups.clear();for(auto id:topo.vertices)lineage.afterVertexGroups.push_back(labels.emplace(id,std::uint32_t(labels.size())).first->second);
    std::map<ModelingId,std::uint32_t> first;for(std::uint32_t i=0;i<topo.vertices.size();++i)first.emplace(topo.vertices[i],i);
    lineage.edgeDescendants.clear();for(const auto& [edge,id]:topo.edges) {
        const auto a=first.at(edge.first),b=first.at(edge.second);const auto key=(ModelingId((std::min)(a,b))<<32U)|(std::max)(a,b);lineage.edgeDescendants[key]={key};
    }
    return advance_modeling_topology(mesh,target,after,topo,lineage,floor);
}
bool ensure_modeling_topology(EditorObject& object,std::string* error) {
    if(!object.polygon){if(error)*error="Modeling identities need a polygon mesh";return false;}
    if(object.modelingTopology)return validate_modeling_topology(*object.polygon,object.subdivision?&*object.subdivision:nullptr,*object.modelingTopology,error);
    auto topo=make_modeling_topology(*object.polygon,object.subdivision?&*object.subdivision:nullptr,{},error);if(!topo)return false;
    // Re-migrating after Undo removed an earlier record: shift the fresh IDs above the
    // allocation high-water mark so retired IDs held by history/selection are never reused.
    if(object.modelingNextId>1) {
        const ModelingId offset=object.modelingNextId-1;
        if(topo->nextId>(std::numeric_limits<ModelingId>::max)()-offset){if(error)*error="Modeling identity space exhausted";return false;}
        const auto shift=[&](std::vector<ModelingId>& ids){for(auto& id:ids)id+=offset;};
        shift(topo->vertices);shift(topo->attributes);shift(topo->corners);shift(topo->triangles);shift(topo->faces);
        for(auto& face:topo->faceTriangles)shift(face);
        std::map<Pair,ModelingId> edges;for(auto& [e,id]:topo->edges)edges.emplace(Pair{e.first+offset,e.second+offset},id+offset);topo->edges=std::move(edges);
        std::set<ModelingId> control;for(auto id:topo->controlEdges)control.insert(id+offset);topo->controlEdges=std::move(control);
        topo->nextId+=offset;
        if(!validate_modeling_topology(*object.polygon,object.subdivision?&*object.subdivision:nullptr,*topo,error))return false;
    }
    object.modelingNextId=topo->nextId;object.modelingTopology=std::move(topo);return true;
}
std::set<ModelingId> modeling_selection_ids(const CookedPolygonAsset&,const ModelingTopology& topo,const PolygonSelection& selection) {
    std::set<ModelingId> ids,selectedTriangles;for(auto value:selection.components){if(selection.mode==PolygonSelectionMode::Vertex){if(value<topo.vertices.size())ids.insert(topo.vertices[value]);}
        else if(selection.mode==PolygonSelectionMode::Edge){auto a=std::uint32_t(value>>32U),b=std::uint32_t(value);if(a<topo.vertices.size()&&b<topo.vertices.size())if(auto at=topo.edges.find(pair(topo.vertices[a],topo.vertices[b]));at!=topo.edges.end())ids.insert(at->second);}
        else if(value<topo.triangles.size())selectedTriangles.insert(topo.triangles[value]);
    }
    if(selection.mode==PolygonSelectionMode::Face)for(std::size_t i=0;i<topo.faces.size();++i)
        if(std::any_of(topo.faceTriangles[i].begin(),topo.faceTriangles[i].end(),[&](auto id){return selectedTriangles.contains(id);}))ids.insert(topo.faces[i]);
    return ids;
}
std::set<std::uint64_t> resolve_modeling_selection(const CookedPolygonAsset&,const ModelingTopology& topo,std::uint8_t mode,const std::set<ModelingId>& ids) {
    std::set<std::uint64_t> selected;std::map<ModelingId,std::uint32_t> first;for(std::uint32_t i=0;i<topo.vertices.size();++i)first.emplace(topo.vertices[i],i);
    if(mode==std::uint8_t(PolygonSelectionMode::Vertex)){for(auto id:ids)if(first.contains(id))selected.insert(first.at(id));}
    else if(mode==std::uint8_t(PolygonSelectionMode::Edge)){for(auto& [e,id]:topo.edges)if(ids.contains(id)&&topo.controlEdges.contains(id)){auto a=first.at(e.first),b=first.at(e.second);selected.insert((ModelingId((std::min)(a,b))<<32U)|(std::max)(a,b));}}
    else {std::map<ModelingId,std::uint32_t> triangles;for(std::uint32_t i=0;i<topo.triangles.size();++i)triangles[topo.triangles[i]]=i;
        for(std::size_t i=0;i<topo.faces.size();++i)if(ids.contains(topo.faces[i]))for(auto triangle:topo.faceTriangles[i])if(triangles.contains(triangle))selected.insert(triangles.at(triangle));
    }return selected;
}
void write_modeling_topology(std::ostream& out,const ModelingTopology& topo) {
    out<<"modeling_ids "<<topo.nextId<<'\n';const auto list=[&](const char* key,const auto& values){out<<key<<' '<<values.size();for(auto id:values)out<<' '<<id;out<<'\n';};
    list("geometric_ids",topo.vertices);list("attribute_ids",topo.attributes);list("corner_ids",topo.corners);list("triangle_ids",topo.triangles);list("face_ids",topo.faces);
    list("control_edge_ids",topo.controlEdges);
    out<<"modeling_edges "<<topo.edges.size()<<'\n';for(auto& [e,id]:topo.edges)out<<"modeling_edge "<<e.first<<' '<<e.second<<' '<<id<<'\n';
    out<<"modeling_faces "<<topo.faceTriangles.size()<<'\n';for(auto& face:topo.faceTriangles)list("modeling_face",face);
}
std::optional<ModelingTopology> read_modeling_topology(std::istream& in,std::string* error) {
    const auto fail=[&](const char* message)->std::optional<ModelingTopology>{if(error)*error=message;return {};};if(error)error->clear();ModelingTopology topo;std::string key;in>>key>>topo.nextId;if(!in||key!="modeling_ids"||!topo.nextId)return fail("Invalid modeling identity header");
    const auto list=[&](const char* expected,std::vector<ModelingId>& values,std::size_t maximum){std::size_t count{};in>>key>>count;if(!in||key!=expected||count>maximum)return false;values.resize(count);for(auto& id:values)in>>id;return bool(in);};
    if(!list("geometric_ids",topo.vertices,100000)||!list("attribute_ids",topo.attributes,100000)||!list("corner_ids",topo.corners,150000)||!list("triangle_ids",topo.triangles,50000)||!list("face_ids",topo.faces,50000))return fail("Invalid modeling identity arrays");
    std::vector<ModelingId> control;if(!list("control_edge_ids",control,150000))return fail("Invalid authored edge IDs");
    topo.controlEdges.insert(control.begin(),control.end());if(topo.controlEdges.size()!=control.size())return fail("Duplicate authored edge IDs");
    std::size_t count{};in>>key>>count;if(!in||key!="modeling_edges"||count>150000)return fail("Invalid modeling edge count");
    for(std::size_t i=0;i<count;++i){ModelingId a{},b{},id{};in>>key>>a>>b>>id;if(!in||key!="modeling_edge"||a>=b||!topo.edges.emplace(Pair{a,b},id).second)return fail("Invalid modeling edge record");}
    in>>key>>count;if(!in||key!="modeling_faces"||count>50000)return fail("Invalid modeling face count");
    for(std::size_t i=0;i<count;++i){std::vector<ModelingId> face;if(!list("modeling_face",face,2)||face.empty())return fail("Invalid modeling face record");topo.faceTriangles.push_back(std::move(face));}return topo;
}
std::uint64_t modeling_topology_hash(const ModelingTopology& topo) noexcept {
    std::uint64_t hash=1469598103934665603ULL;const auto add=[&](ModelingId n){for(unsigned i=0;i<8;++i){hash^=(n>>(i*8))&255;hash*=1099511628211ULL;}};
    for(auto* list:{&topo.vertices,&topo.attributes,&topo.corners,&topo.triangles,&topo.faces}){add(list->size());for(auto id:*list)add(id);}
    add(topo.controlEdges.size());for(auto id:topo.controlEdges)add(id);
    add(topo.edges.size());for(auto& [e,id]:topo.edges){add(e.first);add(e.second);add(id);}add(topo.faceTriangles.size());for(auto& face:topo.faceTriangles){add(face.size());for(auto id:face)add(id);}return hash;
}
}
