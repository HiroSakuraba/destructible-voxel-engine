#include "dve/editable_mesh.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace dve {
namespace {
constexpr std::uint32_t kInvalid = 0xFFFFFFFFU;

template <typename H, typename V>
bool valid_handle(H h, const V& values) noexcept {
    return h.index < values.size() && values[h.index].alive && values[h.index].generation == h.generation;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) noexcept {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
    return h;
}
std::uint64_t float_bits(float value) noexcept { return std::bit_cast<std::uint32_t>(value); }
Float3 sub(Float3 a, Float3 b) noexcept { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Float3 cross(Float3 a, Float3 b) noexcept { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
float length(Float3 v) noexcept { return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); }
Float3 normalized(Float3 v) noexcept { const float l=length(v); return l>1.0e-20F?Float3{v.x/l,v.y/l,v.z/l}:Float3{0,1,0}; }
}

std::size_t EditableMesh::AttributeKeyHash::operator()(const AttributeKey& key) const noexcept {
    return std::hash<std::string>{}(key.name) ^ (static_cast<std::size_t>(key.domain) << 1U);
}
std::size_t EditableMesh::DirectedEdgeHash::operator()(const DirectedEdgeKey& key) const noexcept {
    return (static_cast<std::size_t>(key.from) << 32U) ^ static_cast<std::size_t>(key.to);
}

VertexHandle EditableMesh::add_vertex(Float3 position) {
    const std::uint32_t index=static_cast<std::uint32_t>(vertices_.size());
    vertices_.push_back(Vertex{position,{},1U,true});
    ++topologyRevision_;
    return {index,1U};
}

std::optional<FaceHandle> EditableMesh::add_face(const std::vector<VertexHandle>& verts, std::uint32_t material) {
    if(verts.size()<3U) return std::nullopt;
    std::set<std::uint32_t> unique;
    for(const auto h:verts) if(!valid(h)||!unique.insert(h.index).second) return std::nullopt;
    for(std::size_t i=0;i<verts.size();++i){
        const DirectedEdgeKey key{verts[i].index,verts[(i+1U)%verts.size()].index};
        if(openDirectedEdges_.contains(key)) return std::nullopt;
        const auto reverse=openDirectedEdges_.find({key.to,key.from});
        if(reverse!=openDirectedEdges_.end() && valid(reverse->second) && halfEdges_[reverse->second.index].twin) return std::nullopt;
    }
    const FaceHandle faceHandle{static_cast<std::uint32_t>(faces_.size()),1U};
    const std::uint32_t firstHalf=static_cast<std::uint32_t>(halfEdges_.size());
    faces_.push_back(Face{{firstHalf,1U},material,1U,true});
    for(std::size_t i=0;i<verts.size();++i){
        const HalfEdgeHandle he{static_cast<std::uint32_t>(halfEdges_.size()),1U};
        const CornerHandle corner{static_cast<std::uint32_t>(corners_.size()),1U};
        const HalfEdgeHandle next{firstHalf+static_cast<std::uint32_t>((i+1U)%verts.size()),1U};
        halfEdges_.push_back(HalfEdge{verts[i],{},next,faceHandle,{},corner,1U,true});
        corners_.push_back(Corner{he,1U,true});
        if(!vertices_[verts[i].index].outgoing) vertices_[verts[i].index].outgoing=he;
    }
    for(std::size_t i=0;i<verts.size();++i){
        const HalfEdgeHandle he{firstHalf+static_cast<std::uint32_t>(i),1U};
        const DirectedEdgeKey key{verts[i].index,verts[(i+1U)%verts.size()].index};
        const auto reverse=openDirectedEdges_.find({key.to,key.from});
        if(reverse!=openDirectedEdges_.end() && valid(reverse->second)){
            const HalfEdgeHandle twin=reverse->second;
            halfEdges_[he.index].twin=twin;
            halfEdges_[twin.index].twin=he;
            halfEdges_[he.index].edge=halfEdges_[twin.index].edge;
            openDirectedEdges_.erase(reverse);
        }else{
            const EdgeHandle edge{static_cast<std::uint32_t>(edges_.size()),1U};
            edges_.push_back(Edge{he,1U,true});
            halfEdges_[he.index].edge=edge;
            openDirectedEdges_[key]=he;
        }
    }
    ++topologyRevision_;
    return faceHandle;
}

bool EditableMesh::remove_face(FaceHandle handle){
    if(!valid(handle)) return false;
    auto he=faces_[handle.index].halfEdge;
    const auto start=he;
    do{
        auto& value=halfEdges_[he.index];
        const auto next=value.next;
        const auto origin=value.origin;
        const auto destination=halfEdges_[next.index].origin;
        openDirectedEdges_.erase({origin.index,destination.index});
        if(value.twin && valid(value.twin)){
            auto& twin=halfEdges_[value.twin.index];
            twin.twin={};
            openDirectedEdges_[{twin.origin.index,origin.index}]=value.twin;
            edges_[value.edge.index].halfEdge=value.twin;
        }else if(valid(value.edge)){
            edges_[value.edge.index].alive=false;
            ++edges_[value.edge.index].generation;
        }
        if(valid(value.corner)){corners_[value.corner.index].alive=false;++corners_[value.corner.index].generation;}
        value.alive=false;++value.generation;
        he=next;
    }while(he && he!=start);
    faces_[handle.index].alive=false;++faces_[handle.index].generation;
    ++topologyRevision_;
    return true;
}

bool EditableMesh::valid(VertexHandle h) const noexcept{return valid_handle(h,vertices_);} bool EditableMesh::valid(EdgeHandle h) const noexcept{return valid_handle(h,edges_);} bool EditableMesh::valid(HalfEdgeHandle h) const noexcept{return valid_handle(h,halfEdges_);} bool EditableMesh::valid(FaceHandle h) const noexcept{return valid_handle(h,faces_);} bool EditableMesh::valid(CornerHandle h) const noexcept{return valid_handle(h,corners_);}
const EditableMesh::Vertex* EditableMesh::vertex(VertexHandle h) const noexcept{return valid(h)?&vertices_[h.index]:nullptr;} EditableMesh::Vertex* EditableMesh::vertex(VertexHandle h) noexcept{return valid(h)?&vertices_[h.index]:nullptr;} const EditableMesh::Face* EditableMesh::face(FaceHandle h) const noexcept{return valid(h)?&faces_[h.index]:nullptr;} EditableMesh::Face* EditableMesh::face(FaceHandle h) noexcept{return valid(h)?&faces_[h.index]:nullptr;}

std::vector<VertexHandle> EditableMesh::face_vertices(FaceHandle f) const{
    std::vector<VertexHandle> out; if(!valid(f)) return out; auto he=faces_[f.index].halfEdge; const auto start=he; do{if(!valid(he))break;out.push_back(halfEdges_[he.index].origin);he=halfEdges_[he.index].next;}while(he&&he!=start);return out;
}
std::vector<FaceHandle> EditableMesh::faces() const{std::vector<FaceHandle> out;for(std::uint32_t i=0;i<faces_.size();++i)if(faces_[i].alive)out.push_back({i,faces_[i].generation});return out;}
std::vector<VertexHandle> EditableMesh::vertices() const{std::vector<VertexHandle> out;for(std::uint32_t i=0;i<vertices_.size();++i)if(vertices_[i].alive)out.push_back({i,vertices_[i].generation});return out;}
std::vector<HalfEdgeHandle> EditableMesh::vertex_halfedges(VertexHandle v) const{std::vector<HalfEdgeHandle> out;if(!valid(v))return out;for(std::uint32_t i=0;i<halfEdges_.size();++i)if(halfEdges_[i].alive&&halfEdges_[i].origin==v)out.push_back({i,halfEdges_[i].generation});return out;}
std::size_t EditableMesh::vertex_count() const noexcept{return std::count_if(vertices_.begin(),vertices_.end(),[](const auto& v){return v.alive;});} std::size_t EditableMesh::edge_count() const noexcept{return std::count_if(edges_.begin(),edges_.end(),[](const auto& v){return v.alive;});} std::size_t EditableMesh::face_count() const noexcept{return std::count_if(faces_.begin(),faces_.end(),[](const auto& v){return v.alive;});} std::size_t EditableMesh::corner_count() const noexcept{return std::count_if(corners_.begin(),corners_.end(),[](const auto& v){return v.alive;});}

bool EditableMesh::validate(std::string* error) const{
    auto fail=[&](std::string m){if(error)*error=std::move(m);return false;};
    for(const auto f:faces()){
        const auto verts=face_vertices(f);if(verts.size()<3U)return fail("face has fewer than three vertices");
        auto he=faces_[f.index].halfEdge;const auto start=he;std::size_t count=0;do{if(!valid(he))return fail("face references invalid half-edge");const auto& h=halfEdges_[he.index];if(h.face!=f||!valid(h.origin)||!valid(h.edge)||!valid(h.corner))return fail("half-edge references invalid topology");if(h.twin){if(!valid(h.twin)||halfEdges_[h.twin.index].twin!=he)return fail("half-edge twin relation is not symmetric");}he=h.next;if(++count>halfEdges_.size())return fail("half-edge loop does not terminate");}while(he!=start);
    }
    return true;
}

PolygonBounds EditableMesh::bounds() const noexcept{
    PolygonBounds b{};bool first=true;for(const auto& v:vertices_)if(v.alive){if(first){b.minimum=b.maximum=v.position;first=false;}else{b.minimum.x=std::min(b.minimum.x,v.position.x);b.minimum.y=std::min(b.minimum.y,v.position.y);b.minimum.z=std::min(b.minimum.z,v.position.z);b.maximum.x=std::max(b.maximum.x,v.position.x);b.maximum.y=std::max(b.maximum.y,v.position.y);b.maximum.z=std::max(b.maximum.z,v.position.z);}}return b;
}
std::size_t EditableMesh::domain_size(MeshAttributeDomain d) const noexcept{switch(d){case MeshAttributeDomain::Point:return vertices_.size();case MeshAttributeDomain::Edge:return edges_.size();case MeshAttributeDomain::Face:return faces_.size();case MeshAttributeDomain::Corner:return corners_.size();case MeshAttributeDomain::Detail:return 1U;}return 0U;}
bool EditableMesh::set_attribute(MeshAttributeDomain d,std::string name,std::uint32_t index,MeshAttributeValue value){if(d==MeshAttributeDomain::Detail)return set_detail_attribute(std::move(name),std::move(value));if(index>=domain_size(d)||name.empty())return false;AttributeKey key{d,std::move(name)};auto& values=attributes_[key];values.resize(domain_size(d));values[index]=std::move(value);++attributeRevision_;return true;}
const MeshAttributeValue* EditableMesh::attribute(MeshAttributeDomain d,std::string_view name,std::uint32_t index) const{const auto it=attributes_.find(AttributeKey{d,std::string(name)});if(it==attributes_.end()||index>=it->second.size()||!it->second[index])return nullptr;return &*it->second[index];}
bool EditableMesh::set_detail_attribute(std::string name,MeshAttributeValue value){if(name.empty())return false;detailAttributes_[std::move(name)]=std::move(value);++attributeRevision_;return true;}
const MeshAttributeValue* EditableMesh::detail_attribute(std::string_view name) const{const auto it=detailAttributes_.find(std::string(name));return it==detailAttributes_.end()?nullptr:&it->second;}

VertexHandle MeshBuilder::add_vertex(Float3 p){return mesh_.add_vertex(p);} std::optional<FaceHandle> MeshBuilder::add_triangle(VertexHandle a,VertexHandle b,VertexHandle c,std::uint32_t m){return mesh_.add_face({a,b,c},m);} std::optional<FaceHandle> MeshBuilder::add_quad(VertexHandle a,VertexHandle b,VertexHandle c,VertexHandle d,std::uint32_t m){return mesh_.add_face({a,b,c,d},m);} std::optional<FaceHandle> MeshBuilder::add_polygon(const std::vector<VertexHandle>& v,std::uint32_t m){return mesh_.add_face(v,m);}

EditableMesh editable_mesh_from_cooked(const CookedPolygonAsset& asset,std::string* error){
    EditableMesh mesh;std::vector<VertexHandle> handles;handles.reserve(asset.vertices.size());for(const auto& v:asset.vertices)handles.push_back(mesh.add_vertex(v.position));
    for(const auto& sub:asset.submeshes){for(std::uint32_t i=0;i<sub.indexCount;i+=3U){const auto base=sub.firstIndex+i;if(base+2U>=asset.indices.size()){if(error)*error="submesh index range is invalid";return {};}const auto a=asset.indices[base],b=asset.indices[base+1U],c=asset.indices[base+2U];if(a>=handles.size()||b>=handles.size()||c>=handles.size()||!mesh.add_face({handles[a],handles[b],handles[c]},sub.materialIndex)){if(error)*error="cooked mesh contains non-manifold or invalid triangles";return {};}}}
    return mesh;
}

CookedPolygonAsset cooked_polygon_from_editable(const EditableMesh& mesh,std::uint64_t objectId,std::string* error){
    CookedPolygonAsset out;out.objectId=objectId;std::string validation;if(!mesh.validate(&validation)){if(error)*error=validation;return out;}
    std::unordered_map<std::uint32_t,std::uint32_t> remap;for(const auto h:mesh.vertices()){remap[h.index]=static_cast<std::uint32_t>(out.vertices.size());PolygonVertex v;v.position=mesh.vertex(h)->position;out.vertices.push_back(v);}
    std::map<std::uint32_t,std::vector<std::uint32_t>> byMaterial;
    for(const auto f:mesh.faces()){const auto verts=mesh.face_vertices(f);if(verts.size()<3U)continue;const auto material=mesh.face(f)->material;for(std::size_t i=1;i+1U<verts.size();++i){byMaterial[material].push_back(remap[verts[0].index]);byMaterial[material].push_back(remap[verts[i].index]);byMaterial[material].push_back(remap[verts[i+1U].index]);}}
    const std::uint32_t materialCount=byMaterial.empty()?1U:(byMaterial.rbegin()->first+1U);out.materials.resize(materialCount);out.materialBindings.resize(materialCount);for(std::uint32_t i=0;i<materialCount;++i)out.materials[i].name="Material_"+std::to_string(i);
    for(const auto& [material,indices]:byMaterial){PolygonSubmesh s;s.name="Material_"+std::to_string(material);s.firstIndex=static_cast<std::uint32_t>(out.indices.size());s.indexCount=static_cast<std::uint32_t>(indices.size());s.materialIndex=material;out.indices.insert(out.indices.end(),indices.begin(),indices.end());out.submeshes.push_back(std::move(s));}
    out.bounds=mesh.bounds();
    for(auto& v:out.vertices)v.normal={0,0,0};
    for(std::size_t i=0;i+2U<out.indices.size();i+=3U){const auto ia=out.indices[i],ib=out.indices[i+1U],ic=out.indices[i+2U];const auto n=cross(sub(out.vertices[ib].position,out.vertices[ia].position),sub(out.vertices[ic].position,out.vertices[ia].position));for(const auto idx:{ia,ib,ic}){out.vertices[idx].normal.x+=n.x;out.vertices[idx].normal.y+=n.y;out.vertices[idx].normal.z+=n.z;}}
    for(auto& v:out.vertices)v.normal=normalized(v.normal);
    out.contentHash=polygon_asset_content_hash(out);const auto result=validate_polygon_asset(out);if(!result){if(error)*error=result.message;return {};}
    return out;
}

std::uint64_t editable_mesh_content_hash(const EditableMesh& mesh) noexcept{
    std::uint64_t h=1469598103934665603ULL;for(const auto v:mesh.vertices()){const auto p=mesh.vertex(v)->position;h=mix(h,float_bits(p.x));h=mix(h,float_bits(p.y));h=mix(h,float_bits(p.z));}for(const auto f:mesh.faces()){h=mix(h,mesh.face(f)->material);for(const auto v:mesh.face_vertices(f))h=mix(h,v.index);}h=mix(h,mesh.topology_revision());h=mix(h,mesh.attribute_revision());return h;
}

} // namespace dve
