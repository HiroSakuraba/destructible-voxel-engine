#include "dve/editor_subdivision.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <tuple>
namespace dve::editor {
namespace {
using Edge = std::pair<std::uint32_t,std::uint32_t>;
Edge edge(std::uint32_t a,std::uint32_t b) { return std::minmax(a,b); }
Float3 cross(Float3 a,Float3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Float3 normal(Float3 v) { const float n=length(v); return n>1e-15F && std::isfinite(n) ? multiply(v,1/n) : Float3{}; }
struct AttributeTriangle {
    std::array<Float2,3> domain;
    std::array<PolygonVertex,3> attributes;
};
struct Face {
    std::vector<std::uint32_t> vertices;
    std::vector<PolygonVertex> attributes;
    std::uint32_t material{},triangle{};
    std::vector<std::uint32_t> triangles;
    std::vector<Float2> parameters;
    std::shared_ptr<const std::vector<AttributeTriangle>> rootAttributes;
};
struct Cage { std::vector<Float3> points; std::vector<Face> faces; };
Float3 face_normal(const Cage& cage,const Face& face) {
    Float3 result{};
    for (std::size_t i=1;i+1<face.vertices.size();++i)
        result=add(result,cross(subtract(cage.points[face.vertices[i]],cage.points[face.vertices[0]]),
                                subtract(cage.points[face.vertices[i+1]],cage.points[face.vertices[0]])));
    return result;
}
bool quad_pair(const Cage& cage,const Face& a,const Face& b,Face& result) {
    if (a.material!=b.material) return false;
    std::map<Edge,std::vector<Edge>> edges;
    std::map<std::uint32_t,PolygonVertex> attributes;
    for (const auto* face : {&a,&b}) for (std::size_t i=0;i<3;++i) {
        const auto v=face->vertices[i], next=face->vertices[(i+1)%3];
        edges[edge(v,next)].push_back({v,next});
        attributes.emplace(v,face->attributes[i]);
    }
    if (attributes.size()!=4) return false;
    std::map<std::uint32_t,std::uint32_t> boundary;
    bool shared=false;
    for (const auto& [key,directions] : edges) {
        if (directions.size()==1) {
            if (!boundary.emplace(directions[0].first,directions[0].second).second) return false;
        } else if (directions.size()==2 && directions[0].first==directions[1].second &&
                   directions[0].second==directions[1].first) { shared=true; }
        else return false;
    }
    if (!shared || boundary.size()!=4) return false;
    result=a; result.vertices.clear(); result.attributes.clear();
    auto v=boundary.begin()->first; const auto start=v;
    for (unsigned i=0;i<4;++i) {
        const auto at=boundary.find(v); if (at==boundary.end()) return false;
        result.vertices.push_back(v); result.attributes.push_back(attributes.at(v)); v=at->second;
    }
    if (v!=start) return false;
    (void)cage;
    constexpr Float2 domain[4]={{0,0},{1,0},{1,1},{0,1}};
    result.parameters.assign(domain,domain+4);
    auto roots=std::make_shared<std::vector<AttributeTriangle>>();
    for (const auto* original : {&a,&b}) {
        AttributeTriangle triangle;
        for (unsigned i=0;i<3;++i) {
            const auto at=std::find(result.vertices.begin(),result.vertices.end(),original->vertices[i]);
            triangle.domain[i]=domain[std::size_t(at-result.vertices.begin())];
            triangle.attributes[i]=original->attributes[i];
        }
        roots->push_back(std::move(triangle));
    }
    result.rootAttributes=std::move(roots);
    result.triangles.insert(result.triangles.end(),b.triangles.begin(),b.triangles.end());
    return true;
}
Cage parse_cage(const CookedPolygonAsset& mesh,const SubdivisionSettings* settings=nullptr,std::string* error=nullptr) {
    Cage cage;
    std::map<std::tuple<float,float,float>,std::uint32_t> weld;
    std::vector<std::uint32_t> indices(mesh.vertices.size());
    for (std::size_t i=0;i<mesh.vertices.size();++i) {
        const auto p=mesh.vertices[i].position;
        auto [at,inserted]=weld.emplace(std::make_tuple(p.x,p.y,p.z),static_cast<std::uint32_t>(cage.points.size()));
        if (inserted) cage.points.push_back(p);
        indices[i]=at->second;
    }
    std::vector<std::uint32_t> materials(mesh.indices.size()/3);
    for (const auto& part : mesh.submeshes)
        for (std::uint32_t i=part.firstIndex/3;i<(part.firstIndex+part.indexCount)/3;++i) materials[i]=part.materialIndex;
    std::vector<Face> triangles;
    for (std::uint32_t i=0;i<materials.size();++i) {
        Face face; face.material=materials[i]; face.triangle=i; face.triangles.push_back(i);
        for (unsigned j=0;j<3;++j) {
            const auto index=mesh.indices[i*3+j];
            face.vertices.push_back(indices[index]); face.attributes.push_back(mesh.vertices[index]);
        }
        face.parameters={{0,0},{1,0},{0,1}};
        AttributeTriangle root;
        for (unsigned j=0;j<3;++j) {root.domain[j]=face.parameters[j];root.attributes[j]=face.attributes[j];}
        face.rootAttributes=std::make_shared<const std::vector<AttributeTriangle>>(std::vector<AttributeTriangle>{root});
        triangles.push_back(std::move(face));
    }
    if (settings && !settings->controlFaces.empty()) {
        using Signature=std::array<std::uint32_t,3>;
        const auto signature=[](Signature a) {
            if (a[1]<a[0] && a[1]<a[2]) return Signature{a[1],a[2],a[0]};
            if (a[2]<a[0] && a[2]<a[1]) return Signature{a[2],a[0],a[1]};
            return a;
        };
        std::map<Signature,std::vector<std::uint32_t>> lookup;
        for (std::uint32_t i=0;i<triangles.size();++i)
            lookup[signature({mesh.indices[i*3],mesh.indices[i*3+1],mesh.indices[i*3+2]})].push_back(i);
        std::set<std::uint32_t> used;
        for (const auto& record : settings->controlFaces) {
            std::vector<std::uint32_t> ids;
            bool valid=record.size()==3 || record.size()==6;
            for (std::size_t j=0;valid && j<record.size();j+=3) {
                const auto at=lookup.find(signature({record[j],record[j+1],record[j+2]}));
                if (at==lookup.end()) {valid=false;break;}
                const auto id=std::find_if(at->second.begin(),at->second.end(),[&](auto v){return !used.contains(v);});
                if (id==at->second.end()) {valid=false;break;}
                ids.push_back(*id);used.insert(*id);
            }
            Face pair;
            if (valid && ids.size()==2) valid=quad_pair(cage,triangles[ids[0]],triangles[ids[1]],pair);
            if (!valid) {
                if (error) *error="Saved control face does not match this cage; repair topology metadata";
                cage.faces.clear();return cage;
            }
            cage.faces.push_back(ids.size()==2?std::move(pair):triangles[ids[0]]);
        }
        if (used.size()!=triangles.size()) {
            if (error) *error="Saved control faces do not cover the cage";
            cage.faces.clear();
        }
    } else for (std::size_t i=0;i<triangles.size();++i) {
        Face pair;
        if (i+1<triangles.size() && quad_pair(cage,triangles[i],triangles[i+1],pair)) {
            cage.faces.push_back(std::move(pair)); ++i;
        } else cage.faces.push_back(std::move(triangles[i]));
    }
    return cage;
}
struct EdgeData { std::vector<std::uint32_t> faces; bool sharp{}; float strength{}; std::uint32_t point{}; };
struct Topology {
    std::map<Edge,EdgeData> edges;
    std::vector<std::vector<Edge>> vertexEdges;
    std::vector<std::vector<std::uint32_t>> vertexFaces;
    std::vector<Float3> normals;
};
bool topology(const Cage& cage,Topology& result,const std::map<Edge,float>& creases,bool detectSharp,std::string& error) {
    result.vertexEdges.resize(cage.points.size()); result.vertexFaces.resize(cage.points.size());
    std::map<Edge,int> winding;
    for (std::uint32_t i=0;i<cage.faces.size();++i) {
        const auto& face=cage.faces[i];
        const auto n=face_normal(cage,face);
        if (length(normal(n))==0 || std::set<std::uint32_t>(face.vertices.begin(),face.vertices.end()).size()!=face.vertices.size()) {
            error="Degenerate control face; repair the cage first"; return false;
        }
        result.normals.push_back(n);
        for (std::size_t j=0;j<face.vertices.size();++j) {
            const auto v=face.vertices[j], next=face.vertices[(j+1)%face.vertices.size()];
            result.edges[edge(v,next)].faces.push_back(i);
            winding[edge(v,next)]+=v<next?1:-1;
            result.vertexFaces[v].push_back(i);
        }
    }
    for (auto& [key,data] : result.edges) {
        if (data.faces.size()>2) { error="Non-manifold cage edge; split or repair it first"; return false; }
        if (data.faces.size()==2 && winding[key]!=0) {error="Inconsistent cage winding; repair face orientation";return false;}
        data.strength=data.faces.size()==1 ? 100.0F : 0.0F;
        if (const auto at=creases.find(key);at!=creases.end()) data.strength=std::max(data.strength,at->second);
        if (detectSharp && data.faces.size()==2 &&
            dot(normal(result.normals[data.faces[0]]),normal(result.normals[data.faces[1]]))<0.5F) data.strength=100.0F;
        data.sharp=data.strength>=1.0F;
        result.vertexEdges[key.first].push_back(key); result.vertexEdges[key.second].push_back(key);
    }
    for (std::uint32_t v=0;v<cage.points.size();++v) {
        const auto& faces=result.vertexFaces[v]; if (faces.empty()) continue;
        if (faces.size()>64) { error="Control vertex valence exceeds 64; simplify this cage"; return false; }
        unsigned boundary=0;
        for (const auto key : result.vertexEdges[v]) if (result.edges.at(key).faces.size()==1) ++boundary;
        if (boundary!=0 && boundary!=2) { error="Non-manifold cage boundary vertex"; return false; }
        if (boundary==0 && faces.size()<3) {error="Degenerate closed cage vertex";return false;}
        std::set<std::uint32_t> seen{faces.front()};
        std::vector<std::uint32_t> pending{faces.front()};
        while (!pending.empty()) {
            const auto face=pending.back(); pending.pop_back();
            for (const auto key : result.vertexEdges[v]) {
                const auto& joined=result.edges.at(key).faces;
                if (std::find(joined.begin(),joined.end(),face)==joined.end()) continue;
                for (auto next : joined) if (seen.insert(next).second) pending.push_back(next);
            }
        }
        if (seen.size()!=faces.size()) { error="Touching shells share a control vertex; separate them first"; return false; }
    }
    return true;
}
PolygonVertex average_attributes(const std::vector<PolygonVertex>& attributes) {
    PolygonVertex result; result.texcoord={};result.texcoord1={};result.color={0,0,0,0};
    const float n=float(attributes.size());
    for (const auto& a : attributes) {
        result.texcoord.x+=a.texcoord.x/n;result.texcoord.y+=a.texcoord.y/n;
        result.texcoord1.x+=a.texcoord1.x/n;result.texcoord1.y+=a.texcoord1.y/n;
        result.color.x+=a.color.x/n;result.color.y+=a.color.y/n;result.color.z+=a.color.z/n;result.color.w+=a.color.w/n;
    }
    return result;
}

std::array<float,3> barycentric(const AttributeTriangle& triangle,Float2 p) {
    const auto a=triangle.domain[0],b=triangle.domain[1],c=triangle.domain[2];
    const float denominator=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);
    const float u=((b.y-c.y)*(p.x-c.x)+(c.x-b.x)*(p.y-c.y))/denominator;
    const float v=((c.y-a.y)*(p.x-c.x)+(a.x-c.x)*(p.y-c.y))/denominator;
    return {u,v,1-u-v};
}
PolygonVertex sample_attributes(const AttributeTriangle& triangle,Float2 p) {
    const auto weights=barycentric(triangle,p);PolygonVertex result;
    result.texcoord={};result.texcoord1={};result.color={0,0,0,0};
    for (unsigned i=0;i<3;++i) {
        const auto& a=triangle.attributes[i];const float w=weights[i];
        result.texcoord.x+=a.texcoord.x*w;result.texcoord.y+=a.texcoord.y*w;
        result.texcoord1.x+=a.texcoord1.x*w;result.texcoord1.y+=a.texcoord1.y*w;
        result.color.x+=a.color.x*w;result.color.y+=a.color.y*w;result.color.z+=a.color.z*w;result.color.w+=a.color.w*w;
    }
    return result;
}

bool refine(Cage& cage,Topology& graph,std::map<Edge,float>& creases,std::string& error) {
    std::size_t count=0; for (const auto& face : cage.faces) count+=face.vertices.size();
    if (count>kMaxSubdivisionFaces || cage.points.size()+graph.edges.size()+cage.faces.size()>500'000) {
        error="Subdivision exceeds 125000 faces / 500000 points; lower the level or simplify the cage"; return false;
    }
    Cage next; next.points=cage.points;
    std::vector<std::uint32_t> centres;
    for (const auto& face : cage.faces) {
        Float3 centre{}; for (auto v : face.vertices) centre=add(centre,cage.points[v]);
        centres.push_back(static_cast<std::uint32_t>(next.points.size()));
        next.points.push_back(multiply(centre,1/float(face.vertices.size())));
    }
    for (auto& [key,data] : graph.edges) {
        const auto midpoint=multiply(add(cage.points[key.first],cage.points[key.second]),0.5F);
        auto smooth=midpoint;
        if (data.faces.size()==2) smooth=multiply(add(add(cage.points[key.first],cage.points[key.second]),
            add(next.points[centres[data.faces[0]]],next.points[centres[data.faces[1]]])),0.25F);
        const float weight=std::clamp(data.strength,0.0F,1.0F);
        const auto p=add(multiply(smooth,1-weight),multiply(midpoint,weight));
        data.point=static_cast<std::uint32_t>(next.points.size()); next.points.push_back(p);
    }
    for (std::uint32_t v=0;v<cage.points.size();++v) {
        const auto& edges=graph.vertexEdges[v];const auto& faces=graph.vertexFaces[v];
        if (faces.empty()) continue;
        Float3 f{},r{};
        for (auto face : faces) f=add(f,next.points[centres[face]]);
        for (auto key : edges) r=add(r,multiply(add(cage.points[key.first],cage.points[key.second]),0.5F));
        const float n=float(faces.size());
        const auto smooth=multiply(add(add(multiply(f,1/n),multiply(r,2/float(edges.size()))),
            multiply(cage.points[v],n-3)),1/n);
        std::vector<std::pair<float,std::uint32_t>> sharp;
        for (auto key : edges) if (graph.edges.at(key).strength>0)
            sharp.push_back({graph.edges.at(key).strength,key.first==v?key.second:key.first});
        std::sort(sharp.begin(),sharp.end(),[](const auto& a,const auto& b){return a.first>b.first;});
        auto point=smooth;
        if (sharp.size()>=2) {
            const auto crease=add(multiply(cage.points[v],0.75F),
                multiply(add(cage.points[sharp[0].second],cage.points[sharp[1].second]),0.125F));
            const float w=std::clamp(sharp[1].first,0.0F,1.0F);
            point=add(multiply(smooth,1-w),multiply(crease,w));
            const float corner=sharp.size()>=3?std::clamp(sharp[2].first,0.0F,1.0F):
                faces.size()==1?std::clamp(sharp[1].first,0.0F,1.0F):0.0F;
            point=add(multiply(point,1-corner),multiply(cage.points[v],corner));
        }
        next.points[v]=point;
    }
    std::map<Edge,float> childCreases;
    for (const auto& [key,data] : graph.edges) if (data.strength>1.0F) {
        childCreases[edge(key.first,data.point)]=data.strength-1.0F;
        childCreases[edge(data.point,key.second)]=data.strength-1.0F;
    }
    for (std::uint32_t face=0;face<cage.faces.size();++face) {
        const auto& original=cage.faces[face];const auto centre=average_attributes(original.attributes);
        for (std::size_t i=0;i<original.vertices.size();++i) {
            const auto previous=(i+original.vertices.size()-1)%original.vertices.size(),following=(i+1)%original.vertices.size();
            Face quad;
            quad.material=original.material;quad.triangle=original.triangle;quad.triangles=original.triangles;
            quad.vertices={original.vertices[i],graph.edges.at(edge(original.vertices[i],original.vertices[following])).point,
                centres[face],graph.edges.at(edge(original.vertices[previous],original.vertices[i])).point};
            Float2 centreParameter{};
            for (auto p : original.parameters) {centreParameter.x+=p.x/float(original.parameters.size());centreParameter.y+=p.y/float(original.parameters.size());}
            const auto midpoint=[](Float2 a,Float2 b){return Float2{(a.x+b.x)*0.5F,(a.y+b.y)*0.5F};};
            quad.parameters={original.parameters[i],midpoint(original.parameters[i],original.parameters[following]),
                centreParameter,midpoint(original.parameters[previous],original.parameters[i])};
            quad.rootAttributes=original.rootAttributes;
            quad.attributes={original.attributes[i],average_attributes({original.attributes[i],original.attributes[following]}),
                centre,average_attributes({original.attributes[previous],original.attributes[i]})};
            next.faces.push_back(std::move(quad));
        }
    }
    cage=std::move(next);creases=std::move(childCreases);return true;
}
}

std::vector<std::vector<std::uint32_t>> subdivision_control_face_triangles(const CookedPolygonAsset& mesh,const SubdivisionSettings* settings) {
    if (!validate_polygon_asset(mesh) || mesh.indices.size()/3>kMaxSubdivisionControlTriangles) return {};
    const auto cage=parse_cage(mesh,settings);std::vector<std::vector<std::uint32_t>> result;
    for (const auto& face : cage.faces) result.push_back(face.triangles);
    return result;
}

std::set<std::uint64_t> subdivision_control_edges(const CookedPolygonAsset& mesh,const SubdivisionSettings* settings) {
    if (!validate_polygon_asset(mesh) || mesh.indices.size()/3>kMaxSubdivisionControlTriangles) return {};
    const auto cage=parse_cage(mesh,settings);
    std::map<std::tuple<float,float,float>,std::uint32_t> first;
    for (std::uint32_t i=0;i<mesh.vertices.size();++i) {
        const auto p=mesh.vertices[i].position;first.emplace(std::make_tuple(p.x,p.y,p.z),i);
    }
    std::set<std::uint64_t> result;
    for (const auto& face : cage.faces) for (std::size_t i=0;i<face.vertices.size();++i) {
        const auto a=cage.points[face.vertices[i]],b=cage.points[face.vertices[(i+1)%face.vertices.size()]];
        const auto key=edge(first.at(std::make_tuple(a.x,a.y,a.z)),first.at(std::make_tuple(b.x,b.y,b.z)));
        result.insert((std::uint64_t(key.first)<<32U)|key.second);
    }
    return result;
}

CageEdgeSelectionResult select_cage_edge_chain(const CookedPolygonAsset& mesh,
    const SubdivisionSettings& settings, const std::set<std::uint64_t>& seeds,
    CageEdgeTraversal traversal, std::size_t maximumEdges) {
    CageEdgeSelectionResult result;
    const auto fail=[&](std::string message) {
        result.edges.clear(); result.stops.clear(); result.error=std::move(message); return result;
    };
    if (traversal!=CageEdgeTraversal::Loop && traversal!=CageEdgeTraversal::Ring)
        return fail("Unknown cage edge traversal");
    if (seeds.empty()) return fail("Select at least one control-cage edge");
    if (maximumEdges==0 || mesh.indices.size()/3>kMaxSubdivisionControlTriangles ||
        mesh.vertices.size()>100'000 || settings.controlFaces.size()>kMaxSubdivisionControlTriangles)
        return fail("Edge selection exceeds its cage or selection budget");
    std::size_t recorded=0;
    if (settings.controlFaces.empty()) return fail("Edge loops/rings require saved control faces");
    for (const auto& face : settings.controlFaces) {
        if ((face.size()!=3 && face.size()!=6) || (recorded+=face.size())>mesh.indices.size())
            return fail("Invalid saved control-face metadata");
    }
    if (const auto valid=validate_polygon_asset(mesh); !valid) return fail(valid.message);
    auto cage=parse_cage(mesh,&settings,&result.error);
    if (!result.error.empty()) return fail(result.error);
    Topology graph;
    if (!topology(cage,graph,{},false,result.error)) return fail(result.error);

    // Match the existing component-selection key convention: first attribute vertex at
    // each geometric position. This read-only operation does not create a new ID scheme.
    std::map<std::tuple<float,float,float>,std::uint32_t> first;
    for (std::uint32_t i=0;i<mesh.vertices.size();++i) {
        const auto p=mesh.vertices[i].position; first.emplace(std::make_tuple(p.x,p.y,p.z),i);
    }
    std::vector<std::uint32_t> canonical;
    for (auto p : cage.points) canonical.push_back(first.at(std::make_tuple(p.x,p.y,p.z)));
    const auto sourceKey=[&](Edge key) {
        const auto indices=edge(canonical[key.first],canonical[key.second]);
        return (std::uint64_t(indices.first)<<32U)|indices.second;
    };
    std::map<std::uint64_t,Edge> lookup;
    for (const auto& [key,data] : graph.edges) { (void)data; lookup.emplace(sourceKey(key),key); }
    for (auto key : seeds) if (!lookup.contains(key))
        return fail("Select control-cage edges; hidden diagonals and stale keys are not valid seeds");
    maximumEdges=std::min(maximumEdges,kMaxSubdivisionControlTriangles*3);
    const auto include=[&](Edge key) {
        result.edges.insert(sourceKey(key)); return result.edges.size()<=maximumEdges;
    };
    if (traversal==CageEdgeTraversal::Ring) {
        std::vector<Edge> pending;
        std::set<Edge> seen;
        for (auto key : seeds) pending.push_back(lookup.at(key));
        while (!pending.empty()) {
            const auto key=pending.back(); pending.pop_back();
            if (!seen.insert(key).second) continue;
            if (!include(key)) return fail("Edge selection budget exceeded; selection unchanged");
            const auto& faces=graph.edges.at(key).faces;
            if (faces.size()==1) result.stops.insert(CageEdgeStop::Boundary);
            for (auto id : faces) {
                const auto& face=cage.faces[id];
                if (face.vertices.size()!=4) { result.stops.insert(CageEdgeStop::Triangle); continue; }
                for (std::size_t i=0;i<4;++i) if (edge(face.vertices[i],face.vertices[(i+1)%4])==key)
                    pending.push_back(edge(face.vertices[(i+2)%4],face.vertices[(i+3)%4]));
            }
        }
    } else {
        using Step=std::pair<Edge,std::uint32_t>;
        std::vector<Step> pending;
        std::set<Step> seen;
        for (auto key : seeds) {
            const auto e=lookup.at(key);
            if (!include(e)) return fail("Edge selection budget exceeded; selection unchanged");
            pending.push_back({e,e.first}); pending.push_back({e,e.second});
        }
        while (!pending.empty()) {
            const auto [incoming,v]=pending.back(); pending.pop_back();
            if (!seen.insert({incoming,v}).second) continue;
            const auto& faces=graph.vertexFaces[v];
            const auto& edges=graph.vertexEdges[v];
            if (std::any_of(faces.begin(),faces.end(),[&](auto id){return cage.faces[id].vertices.size()!=4;})) {
                result.stops.insert(CageEdgeStop::Triangle); continue;
            }
            std::vector<Edge> boundary;
            for (auto e : edges) if (graph.edges.at(e).faces.size()==1) boundary.push_back(e);
            std::optional<Edge> outgoing;
            if (!boundary.empty()) {
                // Only a boundary chain has an unambiguous continuation at a regular
                // valence-three boundary vertex. An interior chain ends at the boundary.
                if (edges.size()==3 && faces.size()==2 && graph.edges.at(incoming).faces.size()==1)
                    for (auto e : boundary) if (e!=incoming) outgoing=e;
                if (!outgoing) { result.stops.insert(CageEdgeStop::Boundary); continue; }
            } else {
                if (edges.size()!=4 || faces.size()!=4) { result.stops.insert(CageEdgeStop::Pole); continue; }
                for (auto e : edges) {
                    if (e==incoming) continue;
                    const auto& adjacent=graph.edges.at(e).faces;
                    const auto& previous=graph.edges.at(incoming).faces;
                    if (std::none_of(adjacent.begin(),adjacent.end(),[&](auto id){
                            return std::find(previous.begin(),previous.end(),id)!=previous.end();})) {
                        if (outgoing) return fail("Ambiguous cage loop continuation; repair topology");
                        outgoing=e;
                    }
                }
                if (!outgoing) return fail("Missing cage loop continuation; repair topology");
            }
            if (!include(*outgoing)) return fail("Edge selection budget exceeded; selection unchanged");
            pending.push_back({*outgoing,outgoing->first==v?outgoing->second:outgoing->first});
        }
    }
    return result;
}

CageLoopCutResult cut_cage_edge_ring(const CookedPolygonAsset& source,
    const SubdivisionSettings& settings,std::uint64_t seed,float fraction) {
    CageLoopCutResult result;
    const auto fail=[&](std::string message) { result.polygon.error=std::move(message); return result; };
    if (!std::isfinite(fraction) || fraction<.001F || fraction>.999F)
        return fail("Loop position must be finite and between 0.001 and 0.999");
    const auto ring=select_cage_edge_chain(source,settings,{seed},CageEdgeTraversal::Ring);
    if (!ring) return fail(ring.error);
    if (ring.stops.contains(CageEdgeStop::Triangle)) return fail("Loop cut requires a complete quad strip; it cannot split a neighboring triangle");
    auto initial=settings; initial.level=0;
    if (const auto evaluated=evaluate_subdivision(source,initial); !evaluated.mesh) return fail(evaluated.error);
    const auto cage=parse_cage(source,&settings);
    Topology graph; std::string error;
    if (!topology(cage,graph,{},false,error)) return fail(error);
    std::map<std::tuple<float,float,float>,std::uint32_t> first;
    for (std::uint32_t i=0;i<source.vertices.size();++i) {
        const auto p=source.vertices[i].position; first.emplace(std::make_tuple(p.x,p.y,p.z),i);
        result.lineage.vertexSurvivors.emplace(i,i);
    }
    std::vector<std::uint32_t> canonical;
    for (auto p : cage.points) canonical.push_back(first.at(std::make_tuple(p.x,p.y,p.z)));
    const auto keyOf=[&](Edge key) {
        const auto indices=edge(canonical[key.first],canonical[key.second]);
        return (std::uint64_t(indices.first)<<32U)|indices.second;
    };
    std::map<std::uint64_t,Edge> lookup;
    for (const auto& [key,data] : graph.edges) { (void)data; lookup.emplace(keyOf(key),key); }
    const auto start=lookup.at(seed);
    std::map<Edge,float> fractions{{start,canonical[start.first]<canonical[start.second]?fraction:1-fraction}};
    std::vector<Edge> pending{start};
    while (!pending.empty()) {
        const auto incoming=pending.back(); pending.pop_back();
        for (auto id : graph.edges.at(incoming).faces) {
            const auto& face=cage.faces[id];
            for (std::size_t i=0;i<4;++i) if (edge(face.vertices[i],face.vertices[(i+1)%4])==incoming) {
                const float t=face.vertices[i]==incoming.first?fractions.at(incoming):1-fractions.at(incoming);
                const auto a=face.vertices[(i+2)%4],b=face.vertices[(i+3)%4];
                const auto opposite=edge(a,b); const float next=a<b?1-t:t;
                const auto [at,inserted]=fractions.emplace(opposite,next);
                if (!inserted && std::abs(at->second-next)>1e-5F) return fail("Quad strip has conflicting slide orientation");
                if (inserted) pending.push_back(opposite);
            }
        }
    }
    std::map<Edge,Float3> cutPoints;
    std::map<std::tuple<float,float,float>,Edge> distinctPoints;
    for (const auto& [key,t] : fractions) {
        const auto p=add(multiply(cage.points[key.first],1-t),multiply(cage.points[key.second],t));
        const auto position=std::make_tuple(p.x,p.y,p.z);
        if (first.contains(position) || !distinctPoints.emplace(position,key).second)
            return fail("Loop cut would merge distinct control vertices; choose another position");
        cutPoints.emplace(key,p);
    }
    result.polygon.mesh=source; auto& mesh=result.polygon.mesh;
    mesh.indices.clear(); mesh.submeshes.clear();
    result.subdivision=settings; result.subdivision.controlFaces.clear(); result.subdivision.edgeCreases.clear();
    result.polygon.selection={source.objectId,PolygonSelectionMode::Edge,{}};
    std::map<Edge,std::uint32_t> midpoint;
    std::vector<std::pair<std::uint32_t,std::uint32_t>> selectedPairs;
    const auto sameAttributes=[](const PolygonVertex& a,const PolygonVertex& b) {
        return a.texcoord.x==b.texcoord.x && a.texcoord.y==b.texcoord.y &&
            a.texcoord1.x==b.texcoord1.x && a.texcoord1.y==b.texcoord1.y &&
            a.color.x==b.color.x && a.color.y==b.color.y && a.color.z==b.color.z && a.color.w==b.color.w &&
            a.normal.x==b.normal.x && a.normal.y==b.normal.y && a.normal.z==b.normal.z;
    };
    const auto emit=[&](std::array<std::uint32_t,3> indices,std::uint32_t material,
                        const Face& original,bool unchanged,std::uint32_t triangle) {
        const auto firstIndex=std::uint32_t(mesh.indices.size());
        if(unchanged)result.lineage.triangleDescendants[triangle].insert(firstIndex/3);
        else for(auto old:original.triangles)result.lineage.triangleDescendants[old].insert(firstIndex/3);
        for (unsigned j=0;j<3;++j) {
            mesh.indices.push_back(indices[j]);
            if (unchanged) result.lineage.cornerDescendants[triangle*3+j].insert(firstIndex+j);
            else for (auto oldTriangle : original.triangles) for (unsigned k=0;k<3;++k)
                if (source.indices[oldTriangle*3+k]==indices[j])
                    result.lineage.cornerDescendants[oldTriangle*3+k].insert(firstIndex+j);
        }
        if (!mesh.submeshes.empty() && mesh.submeshes.back().materialIndex==material) mesh.submeshes.back().indexCount+=3;
        else mesh.submeshes.push_back({"Control faces",firstIndex,3,material});
    };
    for (std::uint32_t faceId=0;faceId<cage.faces.size();++faceId) {
        const auto& face=cage.faces[faceId]; std::vector<std::size_t> cuts;
        for (std::size_t j=0;j<face.vertices.size();++j)
            if (fractions.contains(edge(face.vertices[j],face.vertices[(j+1)%face.vertices.size()]))) cuts.push_back(j);
        auto& descendants=result.lineage.controlFaceDescendants[faceId];
        if (cuts.empty()) {
            std::vector<std::uint32_t> record;
            for (auto triangle : face.triangles) {
                const std::array<std::uint32_t,3> ids={source.indices[triangle*3],source.indices[triangle*3+1],source.indices[triangle*3+2]};
                emit(ids,face.material,face,true,triangle); record.insert(record.end(),ids.begin(),ids.end());
            }
            descendants.push_back(std::uint32_t(result.subdivision.controlFaces.size()));
            result.subdivision.controlFaces.push_back(std::move(record)); continue;
        }
        if (face.vertices.size()!=4 || cuts.size()!=2 || (cuts[0]+2)%4!=cuts[1])
            return fail("The selected ring crosses itself or does not define a simple quad strip");
        if (mesh.vertices.size()+2>100'000 || result.subdivision.controlFaces.size()+2>kMaxSubdivisionControlTriangles)
            return fail("Loop cut exceeds the cage size budget");
        const auto n=normal(face_normal(cage,face)); const auto origin=cage.points[face.vertices[0]];
        float scale=0;
        for (auto v : face.vertices) scale=(std::max)(scale,length(subtract(cage.points[v],origin)));
        for (std::size_t j=0;j<4;++j) {
            const auto p=cage.points[face.vertices[j]],q=cage.points[face.vertices[(j+1)%4]],r=cage.points[face.vertices[(j+2)%4]];
            if (std::abs(dot(subtract(p,origin),n))>scale*1e-5F ||
                dot(cross(subtract(q,p),subtract(r,q)),n)<=scale*scale*1e-8F)
                return fail("Initial loop cut supports convex planar control quads; repair this strip first");
        }
        std::vector<std::uint32_t> attributes;
        for (auto geometric : face.vertices) {
            std::optional<std::uint32_t> chosen;
            for (auto triangle : face.triangles) for (unsigned j=0;j<3;++j) {
                const auto index=source.indices[triangle*3+j]; const auto p=source.vertices[index].position;
                if (first.at(std::make_tuple(p.x,p.y,p.z))!=canonical[geometric]) continue;
                if (chosen && !sameAttributes(source.vertices[*chosen],source.vertices[index]))
                    return fail("Control quad has an internal UV/color/normal split; triangulate it before loop cutting");
                if (chosen && *chosen!=index)
                    return fail("Control quad has distinct attribute corners on its diagonal; triangulate it before loop cutting");
                chosen=index;
            }
            if (!chosen) return fail("Control face is missing an attribute corner");
            attributes.push_back(*chosen);
        }
        const auto mid=[&](std::size_t j) {
            const auto a=face.vertices[j],b=face.vertices[(j+1)%4]; const auto key=edge(a,b);
            const float t=a==key.first?fractions.at(key):1-fractions.at(key);
            const auto& va=source.vertices[attributes[j]]; const auto& vb=source.vertices[attributes[(j+1)%4]];
            auto v=va; v.position=cutPoints.at(key); v.normal=normal(add(multiply(va.normal,1-t),multiply(vb.normal,t)));
            v.texcoord={va.texcoord.x*(1-t)+vb.texcoord.x*t,va.texcoord.y*(1-t)+vb.texcoord.y*t};
            v.texcoord1={va.texcoord1.x*(1-t)+vb.texcoord1.x*t,va.texcoord1.y*(1-t)+vb.texcoord1.y*t};
            v.color={va.color.x*(1-t)+vb.color.x*t,va.color.y*(1-t)+vb.color.y*t,va.color.z*(1-t)+vb.color.z*t,va.color.w*(1-t)+vb.color.w*t};
            const auto index=std::uint32_t(mesh.vertices.size()); mesh.vertices.push_back(v); midpoint.emplace(key,index); return index;
        };
        const auto i=cuts[0]; const auto m=mid(i),other=mid((i+2)%4);
        selectedPairs.push_back({m,other});
        const std::array<std::uint32_t,4> quads[]={{attributes[i],m,other,attributes[(i+3)%4]},
            {m,attributes[(i+1)%4],attributes[(i+2)%4],other}};
        for (const auto& quad : quads) {
            const std::array<std::uint32_t,3> a={quad[0],quad[1],quad[2]},b={quad[0],quad[2],quad[3]};
            emit(a,face.material,face,false,0); emit(b,face.material,face,false,0);
            descendants.push_back(std::uint32_t(result.subdivision.controlFaces.size()));
            result.subdivision.controlFaces.push_back({a[0],a[1],a[2],b[0],b[1],b[2]});
        }
    }
    if (mesh.indices.size()/3>kMaxSubdivisionControlTriangles) return fail("Loop cut exceeds 50000 control triangles");
    refresh_editor_polygon(mesh);
    // Canonical IDs are resolved after emission, but lineage is built from known endpoints,
    // never from nearest-position matching between the before and after meshes.
    std::map<std::tuple<float,float,float>,std::uint32_t> afterFirst;
    std::vector<std::uint32_t> afterCanonical;
    for (std::uint32_t i=0;i<mesh.vertices.size();++i) {
        const auto p=mesh.vertices[i].position; afterCanonical.push_back(afterFirst.emplace(std::make_tuple(p.x,p.y,p.z),i).first->second);
    }
    result.lineage.afterVertexGroups=afterCanonical;
    const auto afterKey=[&](std::uint32_t a,std::uint32_t b) {
        const auto key=edge(afterCanonical[a],afterCanonical[b]); return (std::uint64_t(key.first)<<32U)|key.second;
    };
    const auto allowed=polygon_all_components(mesh,PolygonSelectionMode::Edge);
    for (auto old : polygon_all_components(source,PolygonSelectionMode::Edge))
        if (allowed.contains(old)) result.lineage.edgeDescendants[old].insert(old);
    for (const auto& [key,index] : midpoint)
        result.lineage.edgeDescendants[keyOf(key)]={afterKey(canonical[key.first],index),afterKey(index,canonical[key.second])};
    for (const auto& [key,strength] : settings.edgeCreases) {
        const auto at=result.lineage.edgeDescendants.find(key);
        if (at==result.lineage.edgeDescendants.end() || at->second.empty()) return fail("Saved crease has no supported cut lineage");
        for (auto descendant : at->second) result.subdivision.edgeCreases[descendant]=strength;
    }
    for (auto [a,b] : selectedPairs) result.polygon.selection.components.insert(afterKey(a,b));
    if (const auto evaluated=evaluate_subdivision(mesh,result.subdivision); !evaluated.mesh) return fail(evaluated.error);
    return result;
}

CageDissolveResult dissolve_cage_edges(const CookedPolygonAsset& mesh,const SubdivisionSettings& settings,
    const PolygonSelection& selection,const std::set<std::uint64_t>& protectedEdges) {
    CageDissolveResult result;
    const auto fail=[&](std::string message){result.error=std::move(message);return result;};
    if (selection.object!=mesh.objectId || selection.mode!=PolygonSelectionMode::Edge || selection.components.empty())
        return fail("Select control-cage edges to dissolve");
    if (settings.controlFaces.empty()) return fail("Dissolve requires saved triangle/quad control faces");
    auto initial=settings;initial.level=0;
    if (const auto valid=evaluate_subdivision(mesh,initial);!valid.mesh) return fail(valid.error);
    const auto cage=parse_cage(mesh,&settings);Topology graph;std::string error;
    if (!topology(cage,graph,{},false,error)) return fail(error);
    std::map<std::tuple<float,float,float>,std::uint32_t> first;
    for (std::uint32_t i=0;i<mesh.vertices.size();++i) {
        const auto p=mesh.vertices[i].position;first.emplace(std::make_tuple(p.x,p.y,p.z),i);
        result.lineage.vertexSurvivors.emplace(i,i);
    }
    std::vector<std::uint32_t> canonical;
    for (auto p : cage.points) canonical.push_back(first.at(std::make_tuple(p.x,p.y,p.z)));
    const auto sourceKey=[&](Edge e){const auto p=edge(canonical[e.first],canonical[e.second]);return (std::uint64_t(p.first)<<32U)|p.second;};
    std::map<std::uint64_t,Edge> lookup;
    for (const auto& [key,data] : graph.edges) {(void)data;lookup.emplace(sourceKey(key),key);}
    std::map<std::uint32_t,std::uint32_t> partners;
    std::set<std::uint32_t> reserved;
    const auto same=[](const PolygonVertex& a,const PolygonVertex& b) {
        return a.texcoord.x==b.texcoord.x&&a.texcoord.y==b.texcoord.y&&
            a.texcoord1.x==b.texcoord1.x&&a.texcoord1.y==b.texcoord1.y&&
            a.color.x==b.color.x&&a.color.y==b.color.y&&a.color.z==b.color.z&&a.color.w==b.color.w&&
            a.normal.x==b.normal.x&&a.normal.y==b.normal.y&&a.normal.z==b.normal.z;
    };
    for (auto key : selection.components) {
        const auto at=lookup.find(key);
        if (at==lookup.end()) return fail("Dissolve selection includes a stale edge or hidden diagonal");
        if (protectedEdges.contains(key)) return fail("Dissolve protects saved UV seams; clear the seam deliberately first");
        if (settings.edgeCreases.contains(key)) return fail("Dissolve protects authored creases; clear the crease deliberately first");
        const auto& faces=graph.edges.at(at->second).faces;
        if (faces.size()!=2) return fail("Boundary edges cannot be dissolved without removing the surrounding surface");
        const auto a=(std::min)(faces[0],faces[1]),b=(std::max)(faces[0],faces[1]);
        if (cage.faces[a].vertices.size()!=3 || cage.faces[b].vertices.size()!=3)
            return fail("Initial dissolve joins two triangles; merging existing quads needs larger authored faces");
        if (!reserved.insert(a).second || !reserved.insert(b).second)
            return fail("Selected dissolves share a face; choose disjoint triangle pairs");
        Face pair;
        if (!quad_pair(cage,cage.faces[a],cage.faces[b],pair))
            return fail("Dissolve requires adjacent, consistently wound triangles with the same material");
        const auto n=normal(face_normal(cage,pair));const auto origin=cage.points[pair.vertices[0]];
        float scale=0;for(auto v:pair.vertices)scale=(std::max)(scale,length(subtract(cage.points[v],origin)));
        for (unsigned j=0;j<4;++j) {
            const auto p=cage.points[pair.vertices[j]],q=cage.points[pair.vertices[(j+1)%4]],r=cage.points[pair.vertices[(j+2)%4]];
            if (std::abs(dot(subtract(p,origin),n))>scale*1e-5F ||
                dot(cross(subtract(q,p),subtract(r,q)),n)<=scale*scale*1e-8F)
                return fail("Dissolve requires a convex planar quad to retain the surrounding shape");
        }
        for (auto endpoint : {at->second.first,at->second.second}) {
            const PolygonVertex* left=nullptr;const PolygonVertex* right=nullptr;
            for (unsigned j=0;j<3;++j) {
                if (cage.faces[a].vertices[j]==endpoint) left=&cage.faces[a].attributes[j];
                if (cage.faces[b].vertices[j]==endpoint) right=&cage.faces[b].attributes[j];
            }
            if (!left || !right || !same(*left,*right))
                return fail("Dissolve protects UV, color and normal discontinuities across the selected edge");
        }
        partners.emplace(a,b);
    }
    result.subdivision=settings;result.subdivision.controlFaces.clear();
    result.selection={mesh.objectId,PolygonSelectionMode::Face,{}};
    std::set<std::uint32_t> consumed;
    for (std::uint32_t i=0;i<cage.faces.size();++i) {
        if (consumed.contains(i)) continue;
        const auto next=std::uint32_t(result.subdivision.controlFaces.size());
        auto record=settings.controlFaces[i];result.lineage.controlFaceDescendants[i].push_back(next);
        if (const auto at=partners.find(i);at!=partners.end()) {
            const auto b=at->second;consumed.insert(b);
            record.insert(record.end(),settings.controlFaces[b].begin(),settings.controlFaces[b].end());
            result.lineage.controlFaceDescendants[b].push_back(next);
            for (auto triangle : cage.faces[i].triangles) result.selection.components.insert(triangle);
            for (auto triangle : cage.faces[b].triangles) result.selection.components.insert(triangle);
        }
        result.subdivision.controlFaces.push_back(std::move(record));
    }
    for (auto key : subdivision_control_edges(mesh,&settings))
        if (!selection.components.contains(key)) result.lineage.edgeDescendants[key].insert(key);
        else result.lineage.edgeDescendants[key]; // Explicit retirement of the authored edge.
    for (std::uint32_t i=0;i<mesh.indices.size();++i) result.lineage.cornerDescendants[i].insert(i);
    if (const auto evaluated=evaluate_subdivision(mesh,result.subdivision);!evaluated.mesh) return fail(evaluated.error);
    return result;
}

std::optional<SubdivisionSettings> remap_compacted_cage(const CookedPolygonAsset& before,
    const CookedPolygonAsset& after,const SubdivisionSettings& settings,const PolygonTopologyLineage& lineage,std::string* error) {
    const auto fail=[&](std::string message)->std::optional<SubdivisionSettings>{if(error)*error=std::move(message);return {};};
    if(error)error->clear();auto initial=settings;initial.level=0;
    if (const auto valid=evaluate_subdivision(before,initial);!valid.mesh) return fail(valid.error);
    auto result=settings;freeze_subdivision_topology(before,result);
    for (auto& face : result.controlFaces) for (auto& vertex : face) {
        const auto at=lineage.vertexSurvivors.find(vertex);
        if (at==lineage.vertexSurvivors.end()) return fail("Saved control-face corner has no compaction survivor");
        vertex=at->second;
    }
    result.edgeCreases.clear();
    for (const auto& [edge,strength] : settings.edgeCreases) {
        const auto at=lineage.edgeDescendants.find(edge);
        if (at==lineage.edgeDescendants.end() || at->second.size()!=1) return fail("Saved crease has no unique compaction descendant");
        result.edgeCreases[*at->second.begin()]=strength;
    }
    if (const auto valid=evaluate_subdivision(after,result);!valid.mesh) return fail(valid.error);
    return result;
}


std::optional<SubdivisionSettings> remap_cleanup_cage(const CookedPolygonAsset& before,const CookedPolygonAsset& after,
    const SubdivisionSettings& settings,PolygonTopologyLineage& lineage,std::string* error) {
    const auto fail=[&](std::string message)->std::optional<SubdivisionSettings>{if(error)*error=std::move(message);return {};};
    if(error)error->clear();auto initial=settings;initial.level=0;
    if(auto valid=evaluate_subdivision(before,initial);!valid.mesh)return fail(valid.error);
    const auto groups=subdivision_control_face_triangles(before,&settings);
    if(groups.empty())return fail("Cleanup requires resolvable saved control faces");
    auto result=settings;result.controlFaces.clear();result.edgeCreases.clear();
    std::vector<std::set<std::uint32_t>> mapped;
    for(const auto& group:groups) {
        std::set<std::uint32_t> descendants;
        for(auto triangle:group) {
            auto at=lineage.triangleDescendants.find(triangle);
            if(at==lineage.triangleDescendants.end()||at->second.empty())return fail("Control triangle has no cleanup lineage");
            descendants.insert(at->second.begin(),at->second.end());
        }
        mapped.push_back(std::move(descendants));
    }
    // Triangle pairs that dissolve to one triangle share the same new face record.
    std::set<std::uint32_t> assigned;
    std::map<std::set<std::uint32_t>,std::uint32_t> savedGroups;
    for(std::uint32_t old=0;old<mapped.size();++old) {
        if(auto existing=savedGroups.find(mapped[old]);existing!=savedGroups.end()) {
            lineage.controlFaceDescendants[old]={existing->second};continue;
        }
        std::vector<std::uint32_t> record;
        for(auto triangle:mapped[old]) {
            if(!assigned.insert(triangle).second)return fail("Cleanup would split overlapping saved control faces");
            for(unsigned j=0;j<3;++j)record.push_back(after.indices[triangle*3+j]);
        }
        lineage.controlFaceDescendants[old]={std::uint32_t(result.controlFaces.size())};
        savedGroups.emplace(mapped[old],std::uint32_t(result.controlFaces.size()));
        result.controlFaces.push_back(std::move(record));
    }
    const auto edges=subdivision_control_edges(after,&result);
    for(const auto& [edge,strength]:settings.edgeCreases) {
        auto at=lineage.edgeDescendants.find(edge);
        if(at==lineage.edgeDescendants.end()||at->second.size()!=1||!edges.contains(*at->second.begin()))return fail("Cleanup would retire an authored crease");
        const auto next=*at->second.begin();
        if(auto prior=result.edgeCreases.find(next);prior!=result.edgeCreases.end()&&prior->second!=strength)return fail("Merge would combine creases with different strengths");
        result.edgeCreases[next]=strength;
    }
    if(auto valid=evaluate_subdivision(after,result);!valid.mesh)return fail(valid.error);
    return result;
}

void freeze_subdivision_topology(const CookedPolygonAsset& mesh,SubdivisionSettings& settings) {
    if (!settings.controlFaces.empty()) return;
    for (const auto& face : subdivision_control_face_triangles(mesh)) {
        std::vector<std::uint32_t> record;
        for (auto triangle : face) for (unsigned j=0;j<3;++j) record.push_back(mesh.indices[triangle*3+j]);
        settings.controlFaces.push_back(std::move(record));
    }
}
SubdivisionSettings remap_subdivision_topology(const CookedPolygonAsset& before,
    const CookedPolygonAsset& after,const SubdivisionSettings& original) {
    if (!validate_polygon_asset(before) || !validate_polygon_asset(after) ||
        before.indices.size()/3>kMaxSubdivisionControlTriangles || after.indices.size()/3>kMaxSubdivisionControlTriangles)
        return original; // The committing command reports invalid geometry before mutating the object.
    auto source=original;freeze_subdivision_topology(before,source);
    const auto materialIds=[](const CookedPolygonAsset& mesh) {
        std::vector<std::uint32_t> ids(mesh.indices.size()/3);
        for (const auto& part : mesh.submeshes)
            for (auto t=part.firstIndex/3;t<(part.firstIndex+part.indexCount)/3;++t) ids[t]=part.materialIndex;
        return ids;
    };
    if (before.indices==after.indices && before.vertices.size()==after.vertices.size() &&
        materialIds(before)==materialIds(after)) return source;
    auto result=source;result.controlFaces.clear();result.edgeCreases.clear();
    std::map<std::tuple<float,float,float>,std::uint32_t> positions;
    for (std::uint32_t i=0;i<after.vertices.size();++i) {
        const auto p=after.vertices[i].position;positions.emplace(std::make_tuple(p.x,p.y,p.z),i);
    }
    const auto mapVertex=[&](std::uint32_t index)->std::optional<std::uint32_t> {
        if (index>=before.vertices.size()) return {};
        const auto p=before.vertices[index].position;
        const auto at=positions.find(std::make_tuple(p.x,p.y,p.z));
        return at==positions.end()?std::nullopt:std::optional<std::uint32_t>(at->second);
    };
    using Signature=std::array<std::uint32_t,3>;
    const auto signature=[](Signature v) {
        if (v[1]<v[0] && v[1]<v[2]) return Signature{v[1],v[2],v[0]};
        if (v[2]<v[0] && v[2]<v[1]) return Signature{v[2],v[0],v[1]};
        return v;
    };
    std::map<Signature,std::vector<std::uint32_t>> lookup;
    for (std::uint32_t i=0;i<after.indices.size()/3;++i) {
        Signature v{};
        for (unsigned j=0;j<3;++j) {const auto p=after.vertices[after.indices[i*3+j]].position;v[j]=positions.at(std::make_tuple(p.x,p.y,p.z));}
        lookup[signature(v)].push_back(i);
    }
    std::set<std::uint32_t> used;
    SubdivisionSettings triangleSettings;
    for (std::size_t i=0;i<after.indices.size();i+=3)
        triangleSettings.controlFaces.push_back({after.indices[i],after.indices[i+1],after.indices[i+2]});
    const auto triangleCage=parse_cage(after,&triangleSettings);
    for (const auto& group : subdivision_control_face_triangles(before,&source)) {
        std::vector<std::uint32_t> ids;bool valid=true;
        for (auto triangle : group) {
            Signature mapped{};
            for (unsigned j=0;j<3;++j) {
                const auto v=mapVertex(before.indices[triangle*3+j]);
                if (!v) {valid=false;break;}mapped[j]=*v;
            }
            if (!valid) break;
            const auto at=lookup.find(signature(mapped));
            if (at==lookup.end()) {valid=false;break;}
            const auto id=std::find_if(at->second.begin(),at->second.end(),[&](auto v){return !used.contains(v) && std::find(ids.begin(),ids.end(),v)==ids.end();});
            if (id==at->second.end()) {valid=false;break;}ids.push_back(*id);
        }
        if (!valid) continue;
        // A material split turns a former quad into two independent triangle faces.
        std::vector<std::uint32_t> record;
        for (auto id : ids) for (unsigned j=0;j<3;++j) record.push_back(after.indices[id*3+j]);
        Face pair;
        const bool keepQuad=ids.size()==2 && quad_pair(triangleCage,triangleCage.faces[ids[0]],triangleCage.faces[ids[1]],pair);
        if (ids.size()==1 || keepQuad) result.controlFaces.push_back(std::move(record));
        else for (auto id : ids) result.controlFaces.push_back({after.indices[id*3],after.indices[id*3+1],after.indices[id*3+2]});
        used.insert(ids.begin(),ids.end());
    }
    // Never pair a surviving old triangle with unrelated newly created geometry.
    for (const auto& group : subdivision_control_face_triangles(after)) {
        std::vector<std::uint32_t> remaining;
        for (auto id : group) if (!used.contains(id)) remaining.push_back(id);
        if (remaining.empty()) continue;
        std::vector<std::uint32_t> record;
        for (auto id : remaining) for (unsigned j=0;j<3;++j) record.push_back(after.indices[id*3+j]);
        result.controlFaces.push_back(std::move(record));used.insert(remaining.begin(),remaining.end());
    }
    const auto allowed=subdivision_control_edges(after,&result);
    for (const auto& [key,strength] : source.edgeCreases) {
        const auto a=mapVertex(std::uint32_t(key>>32U)),b=mapVertex(std::uint32_t(key));
        if (!a || !b || *a==*b) continue;
        const auto v=edge(*a,*b);const auto mapped=(std::uint64_t(v.first)<<32U)|v.second;
        if (allowed.contains(mapped)) result.edgeCreases[mapped]=strength;
    }
    return result;
}

namespace {
// Optional face-varying UV relaxation on the evaluated mesh only. Vertices that
// coincide in position AND UV are considered continuous; all chart boundary
// vertices are fixed. UV0 and UV1 are evaluated independently.
void relax_subdivision_uv_interior(CookedPolygonAsset& mesh,std::uint32_t channel) {
    using Key=std::tuple<float,float,float,float,float>;
    struct Node {
        Float2 uv{};
        std::vector<std::uint32_t> vertices;
        std::set<std::uint32_t> neighbors;
        bool boundary{};
    };
    std::map<Key,std::uint32_t> keyToId;
    std::vector<Node> nodes;
    std::vector<std::uint32_t> id(mesh.vertices.size());
    const auto channel_uv=[&](const PolygonVertex& v){return channel==0?v.texcoord:v.texcoord1;};
    for(std::uint32_t index=0;index<mesh.vertices.size();++index) {
        const auto& v=mesh.vertices[index];
        const Float2 uv=channel_uv(v);
        const Key key{v.position.x,v.position.y,v.position.z,uv.x,uv.y};
        auto [it,newNode]=keyToId.emplace(key,static_cast<std::uint32_t>(nodes.size()));
        if(newNode){Node node;node.uv=uv;nodes.push_back(std::move(node));}
        id[index]=it->second;
        nodes[it->second].vertices.push_back(index);
    }
    std::map<Edge,std::uint32_t> edgeUse;
    for(std::size_t i=0;i+2<mesh.indices.size();i+=3)for(unsigned k=0;k<3;++k){
        const std::uint32_t a=id[mesh.indices[i+k]],b=id[mesh.indices[i+(k+1)%3]];
        if(a==b)continue;
        const Edge e=edge(a,b);
        ++edgeUse[e];nodes[a].neighbors.insert(b);nodes[b].neighbors.insert(a);
    }
    for(const auto& [e,uses]:edgeUse)if(uses!=2){
        nodes[e.first].boundary=true;nodes[e.second].boundary=true;
    }
    // Conservative single-step relaxation; preserves chart boundaries and
    // does not alter geometry, normals, tangents or the other UV channel.
    std::vector<Float2> next(nodes.size());
    for(std::size_t i=0;i<nodes.size();++i) {
        next[i]=nodes[i].uv;
        if(nodes[i].boundary||nodes[i].neighbors.size()<3)continue;
        Float2 average{};
        for(auto j:nodes[i].neighbors){
            average.x+=nodes[j].uv.x;average.y+=nodes[j].uv.y;
        }
        const float weight=.25F/float(nodes[i].neighbors.size());
        next[i].x=nodes[i].uv.x*.75F+average.x*weight;
        next[i].y=nodes[i].uv.y*.75F+average.y*weight;
    }
    for(std::size_t i=0;i<nodes.size();++i)
        for(auto vertex:nodes[i].vertices)
            if(channel==0)mesh.vertices[vertex].texcoord=next[i];
            else mesh.vertices[vertex].texcoord1=next[i];
}
} // namespace

SubdivisionEvaluation evaluate_subdivision(const CookedPolygonAsset& control,const SubdivisionSettings& settings) {
    SubdivisionEvaluation result;
    if (settings.level>kMaxSubdivisionLevel) {result.error="Choose subdivision level 0..4";return result;}
    if (settings.uvMode!=SubdivisionUvMode::Linear &&
        settings.uvMode!=SubdivisionUvMode::SmoothInterior) {
        result.error="Unknown subdivision UV smoothing mode";return result;
    }
    if (const auto valid=validate_polygon_asset(control);!valid) {result.error=valid.message;return result;}
    if (control.indices.size()/3>kMaxSubdivisionControlTriangles || control.vertices.size()>100'000) {
        result.error="Control cage exceeds 50000 triangles / 100000 vertices";return result;
    }
    std::size_t imageBytes=0;
    for (const auto& image : control.images) {
        if (image.rgba8.size()>64U*1024U*1024U-imageBytes) {result.error="Subdivision texture data exceeds 64 MiB";return result;}
        imageBytes+=image.rgba8.size();
    }
    if (settings.controlFaces.size()>kMaxSubdivisionControlTriangles || settings.edgeCreases.size()>kMaxSubdivisionControlTriangles*3) {
        result.error="Subdivision metadata exceeds its budget";return result;
    }
    std::size_t recorded=0;
    for (const auto& face : settings.controlFaces) {
        if ((face.size()!=3 && face.size()!=6) || (recorded+=face.size())>control.indices.size()) {
            result.error="Invalid subdivision control-face metadata";return result;
        }
    }
    auto cage=parse_cage(control,&settings,&result.error);
    if (!result.error.empty()) return result;
    std::map<std::tuple<float,float,float>,std::uint32_t> geometric;
    for (std::uint32_t i=0;i<cage.points.size();++i) {
        const auto p=cage.points[i];geometric.emplace(std::make_tuple(p.x,p.y,p.z),i);
    }
    const auto allowed=subdivision_control_edges(control,&settings);
    std::map<Edge,float> creases;
    for (const auto& [key,strength] : settings.edgeCreases) {
        if (!std::isfinite(strength) || strength<=0 || strength>4 || !allowed.contains(key)) {
            result.error="Invalid saved subdivision edge crease";return result;
        }
        const auto a=control.vertices[std::size_t(key>>32U)].position,b=control.vertices[std::size_t(key&0xffffffffU)].position;
        creases[edge(geometric.at(std::make_tuple(a.x,a.y,a.z)),geometric.at(std::make_tuple(b.x,b.y,b.z)))]=strength;
    }
    if (settings.level==0) {
        result.mesh=control;for (std::uint32_t i=0;i<control.indices.size()/3;++i) result.controlTriangles.push_back(i);
        return result;
    }
    for (std::uint32_t level=0;level<settings.level;++level) {
        Topology graph;
        if (!topology(cage,graph,creases,level==0 && settings.preserveSharpEdges,result.error) ||
            !refine(cage,graph,creases,result.error)) return result;
    }
    Topology graph;
    if (!topology(cage,graph,creases,false,result.error)) return result;
    std::vector<std::vector<std::pair<std::uint32_t,Float3>>> normals(cage.points.size());
    for (std::uint32_t v=0;v<cage.points.size();++v) {
        std::set<std::uint32_t> remaining(graph.vertexFaces[v].begin(),graph.vertexFaces[v].end());
        while (!remaining.empty()) {
            std::vector<std::uint32_t> pending{*remaining.begin()},fan;
            remaining.erase(pending.back());Float3 sum{};
            while (!pending.empty()) {
                const auto face=pending.back();pending.pop_back();fan.push_back(face);sum=add(sum,graph.normals[face]);
                for (auto key : graph.vertexEdges[v]) {
                    const auto& data=graph.edges.at(key);
                    if (data.sharp || std::find(data.faces.begin(),data.faces.end(),face)==data.faces.end()) continue;
                    for (auto next : data.faces) if (remaining.erase(next)) pending.push_back(next);
                }
            }
            for (auto face : fan) normals[v].push_back({face,normal(sum)});
        }
    }
    CookedPolygonAsset mesh=control;mesh.vertices.clear();mesh.indices.clear();mesh.submeshes.clear();
    std::map<std::uint32_t,std::vector<std::pair<std::array<std::uint32_t,3>,std::uint32_t>>> groups;
    for (std::uint32_t f=0;f<cage.faces.size();++f) {
        const auto& face=cage.faces[f];
        for (std::uint32_t i=1;i+1<face.vertices.size();++i) {
            const std::array<std::uint32_t,3> corners{0,i,i+1};
            Float2 centre{};
            for (auto corner : corners) {centre.x+=face.parameters[corner].x/3;centre.y+=face.parameters[corner].y/3;}
            const AttributeTriangle* root=&face.rootAttributes->front();
            for (const auto& candidate : *face.rootAttributes) {
                const auto weights=barycentric(candidate,centre);
                if (*std::min_element(weights.begin(),weights.end())>=-1e-6F) {root=&candidate;break;}
            }
            const auto base=static_cast<std::uint32_t>(mesh.vertices.size());
            for (auto corner : corners) {
                auto vertex=sample_attributes(*root,face.parameters[corner]);const auto v=face.vertices[corner];
                vertex.position=cage.points[v];
                const auto at=std::find_if(normals[v].begin(),normals[v].end(),[&](const auto& item){return item.first==f;});
                vertex.normal=at==normals[v].end()?normal(graph.normals[f]):at->second;
                mesh.vertices.push_back(vertex);
            }
            groups[face.material].push_back({{base,base+1,base+2},face.triangle});
        }
    }
    for (const auto& [material,triangles] : groups) {
        mesh.submeshes.push_back({material<mesh.materials.size()?mesh.materials[material].name:"Subdivision",
            static_cast<std::uint32_t>(mesh.indices.size()),static_cast<std::uint32_t>(triangles.size()*3),material});
        for (const auto& [triangle,origin] : triangles) {
            mesh.indices.insert(mesh.indices.end(),triangle.begin(),triangle.end());result.controlTriangles.push_back(origin);
        }
    }
    if(settings.uvMode==SubdivisionUvMode::SmoothInterior) {
        relax_subdivision_uv_interior(mesh,0);
        relax_subdivision_uv_interior(mesh,1);
    }
    refresh_editor_polygon(mesh,false);
    if (const auto valid=validate_polygon_asset(mesh);!valid) {result.error=valid.message;return result;}
    result.mesh=std::move(mesh);return result;
}
const CookedPolygonAsset* editor_surface_mesh(const EditorObject& object) {
    if (!object.polygon) return nullptr;
    if (!object.subdivision || object.subdivision->level==0) return &*object.polygon;
    if (!object.subdivisionCache || object.subdivisionCacheHash!=object.polygon->contentHash ||
        object.subdivisionCacheSettings!=*object.subdivision) {
        object.subdivisionCache=evaluate_subdivision(*object.polygon,*object.subdivision);
        object.subdivisionCacheHash=object.polygon->contentHash;object.subdivisionCacheSettings=*object.subdivision;
    }
    return object.subdivisionCache->mesh ? &*object.subdivisionCache->mesh : &*object.polygon;
}
std::uint32_t subdivision_control_triangle(const EditorObject& object,std::uint32_t triangle) {
    if (!object.subdivision || object.subdivision->level==0) return triangle;
    (void)editor_surface_mesh(object);
    if (object.subdivisionCache && object.subdivisionCache->mesh &&
        triangle<object.subdivisionCache->controlTriangles.size()) return object.subdivisionCache->controlTriangles[triangle];
    return triangle;
}
SetSubdivisionSettingsCommand::SetSubdivisionSettingsCommand(EditorObjectId id,SubdivisionSettings before,SubdivisionSettings after,std::optional<ModelingTopology> authoredTopology):
    id_(id),before_(before),after_(after),afterTopology_(std::move(authoredTopology)) {}
CommandResult SetSubdivisionSettingsCommand::apply(EditorDocument& document,SubdivisionSettings expected,SubdivisionSettings value) {
    auto* object=document.find_object(id_);
    if (!object || !object->polygon || !object->subdivision) return CommandResult::fail("Subdivision source no longer exists");
    if (object->flags.locked || *object->subdivision!=expected) return CommandResult::fail("Subdivision source is locked or changed");
    auto result=evaluate_subdivision(*object->polygon,value);
    if (!result.mesh) return CommandResult::fail(result.error);
    object->subdivision=value;object->subdivisionCache=std::move(result);
    object->subdivisionCacheHash=object->polygon->contentHash;object->subdivisionCacheSettings=value;
    document.mark_dirty();return CommandResult::ok();
}
CommandResult SetSubdivisionSettingsCommand::execute(EditorDocument& document) {
    auto* object=document.find_object(id_);
    if (!captured_) {
        if (object && object->prefabLink) {
            const auto& overrides=object->prefabLink->overrides;
            if (const auto at=overrides.find("subdivision.level");at!=overrides.end()) levelOverride_=at->second;
            if (const auto at=overrides.find("subdivision.sharp_edges");at!=overrides.end()) sharpOverride_=at->second;
            if (const auto at=overrides.find("subdivision.uv_smooth");at!=overrides.end()) uvSmoothingOverride_=at->second;
            if (const auto at=overrides.find("polygon.geometry");at!=overrides.end()) geometryOverride_=at->second;
        }
        beforeTopology_=object?object->modelingTopology:std::nullopt;
        if(beforeTopology_&&object->prefabLink&&before_.controlFaces!=after_.controlFaces)return CommandResult::fail("Edit the prefab source or detach this instance before changing topology");
        if(beforeTopology_ && !afterTopology_) {
            const auto changed=change_modeling_faces(*object->polygon,&before_,&after_,*beforeTopology_,object->modelingNextId);
            if(!changed)return CommandResult::fail(changed.error);afterTopology_=changed.topology;
        }
        if(afterTopology_) {std::string error;if(!validate_modeling_topology(*object->polygon,&after_,*afterTopology_,&error))return CommandResult::fail(error);}
        captured_=true;
    }
    const auto result=apply(document,before_,after_);
    if(result.success && afterTopology_) {object->modelingTopology=afterTopology_;object->modelingNextId=(std::max)(object->modelingNextId,afterTopology_->nextId);}
    if (result.success && object->prefabLink) {
        if (before_.level!=after_.level) object->prefabLink->overrides["subdivision.level"]=std::int64_t(after_.level);
        if (before_.preserveSharpEdges!=after_.preserveSharpEdges)
            object->prefabLink->overrides["subdivision.sharp_edges"]=after_.preserveSharpEdges;
        if (before_.uvMode!=after_.uvMode)
            object->prefabLink->overrides["subdivision.uv_smooth"]=after_.uvMode==SubdivisionUvMode::SmoothInterior;
        if (before_.edgeCreases!=after_.edgeCreases || before_.controlFaces!=after_.controlFaces)
            object->prefabLink->overrides["polygon.geometry"]=std::int64_t{1};
    }
    return result;
}
CommandResult SetSubdivisionSettingsCommand::undo(EditorDocument& document) {
    const auto result=apply(document,after_,before_);
    if(result.success)document.find_object(id_)->modelingTopology=beforeTopology_;
    if (result.success) if (auto* object=document.find_object(id_);object && object->prefabLink) {
        auto& overrides=object->prefabLink->overrides;
        if (levelOverride_) overrides["subdivision.level"]=*levelOverride_;else overrides.erase("subdivision.level");
        if (sharpOverride_) overrides["subdivision.sharp_edges"]=*sharpOverride_;else overrides.erase("subdivision.sharp_edges");
        if (uvSmoothingOverride_) overrides["subdivision.uv_smooth"]=*uvSmoothingOverride_;else overrides.erase("subdivision.uv_smooth");
        if (geometryOverride_) overrides["polygon.geometry"]=*geometryOverride_;else overrides.erase("polygon.geometry");
    }
    return result;
}
}
