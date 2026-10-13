#include "dve/editor_subdivision.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>
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
Cage parse_cage(const CookedPolygonAsset& mesh) {
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
    for (std::size_t i=0;i<triangles.size();++i) {
        Face pair;
        if (i+1<triangles.size() && quad_pair(cage,triangles[i],triangles[i+1],pair)) {
            cage.faces.push_back(std::move(pair)); ++i;
        } else cage.faces.push_back(std::move(triangles[i]));
    }
    return cage;
}
struct EdgeData { std::vector<std::uint32_t> faces; bool sharp{}; std::uint32_t point{}; };
struct Topology {
    std::map<Edge,EdgeData> edges;
    std::vector<std::vector<Edge>> vertexEdges;
    std::vector<std::vector<std::uint32_t>> vertexFaces;
    std::vector<Float3> normals;
};
bool topology(const Cage& cage,Topology& result,const std::set<Edge>& creases,bool detectSharp,std::string& error) {
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
        data.sharp=data.faces.size()==1 || creases.contains(key);
        if (detectSharp && data.faces.size()==2)
            data.sharp=data.sharp || dot(normal(result.normals[data.faces[0]]),normal(result.normals[data.faces[1]]))<0.5F;
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

bool refine(Cage& cage,Topology& graph,std::set<Edge>& creases,std::string& error) {
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
        auto p=add(cage.points[key.first],cage.points[key.second]);
        if (!data.sharp) for (auto face : data.faces) p=add(p,next.points[centres[face]]);
        p=multiply(p,data.sharp ? 0.5F : 0.25F);
        data.point=static_cast<std::uint32_t>(next.points.size()); next.points.push_back(p);
    }
    for (std::uint32_t v=0;v<cage.points.size();++v) {
        const auto& edges=graph.vertexEdges[v];const auto& faces=graph.vertexFaces[v];
        if (faces.empty()) continue;
        std::vector<std::uint32_t> sharp;
        for (auto key : edges) if (graph.edges.at(key).sharp) sharp.push_back(key.first==v ? key.second : key.first);
        if (sharp.size()>=3 || (sharp.size()==2 && faces.size()==1)) continue; // Boundary corners interpolate.
        if (sharp.size()==2) {
            next.points[v]=add(multiply(cage.points[v],0.75F),multiply(add(cage.points[sharp[0]],cage.points[sharp[1]]),0.125F));
        } else {
            Float3 f{},r{};
            for (auto face : faces) f=add(f,next.points[centres[face]]);
            for (auto key : edges) r=add(r,multiply(add(cage.points[key.first],cage.points[key.second]),0.5F));
            const float n=float(faces.size());
            next.points[v]=multiply(add(add(multiply(f,1/n),multiply(r,2/float(edges.size()))),
                                             multiply(cage.points[v],n-3)),1/n);
        }
    }
    std::set<Edge> childCreases;
    for (const auto& [key,data] : graph.edges) if (data.sharp) {
        childCreases.insert(edge(key.first,data.point));childCreases.insert(edge(data.point,key.second));
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

std::vector<std::vector<std::uint32_t>> subdivision_control_face_triangles(const CookedPolygonAsset& mesh) {
    if (!validate_polygon_asset(mesh) || mesh.indices.size()/3>kMaxSubdivisionControlTriangles) return {};
    const auto cage=parse_cage(mesh);std::vector<std::vector<std::uint32_t>> result;
    for (const auto& face : cage.faces) result.push_back(face.triangles);
    return result;
}

std::set<std::uint64_t> subdivision_control_edges(const CookedPolygonAsset& mesh) {
    if (!validate_polygon_asset(mesh) || mesh.indices.size()/3>kMaxSubdivisionControlTriangles) return {};
    const auto cage=parse_cage(mesh);
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

SubdivisionEvaluation evaluate_subdivision(const CookedPolygonAsset& control,SubdivisionSettings settings) {
    SubdivisionEvaluation result;
    if (settings.level>kMaxSubdivisionLevel) {result.error="Choose subdivision level 0..4";return result;}
    if (const auto valid=validate_polygon_asset(control);!valid) {result.error=valid.message;return result;}
    if (control.indices.size()/3>kMaxSubdivisionControlTriangles || control.vertices.size()>100'000) {
        result.error="Control cage exceeds 50000 triangles / 100000 vertices";return result;
    }
    std::size_t imageBytes=0;
    for (const auto& image : control.images) {
        if (image.rgba8.size()>64U*1024U*1024U-imageBytes) {result.error="Subdivision texture data exceeds 64 MiB";return result;}
        imageBytes+=image.rgba8.size();
    }
    if (settings.level==0) {
        result.mesh=control;for (std::uint32_t i=0;i<control.indices.size()/3;++i) result.controlTriangles.push_back(i);
        return result;
    }
    auto cage=parse_cage(control);std::set<Edge> creases;
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
SetSubdivisionSettingsCommand::SetSubdivisionSettingsCommand(EditorObjectId id,SubdivisionSettings before,SubdivisionSettings after):
    id_(id),before_(before),after_(after) {}
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
        }
        captured_=true;
    }
    const auto result=apply(document,before_,after_);
    if (result.success && object->prefabLink) {
        object->prefabLink->overrides["subdivision.level"]=std::int64_t(after_.level);
        object->prefabLink->overrides["subdivision.sharp_edges"]=after_.preserveSharpEdges;
    }
    return result;
}
CommandResult SetSubdivisionSettingsCommand::undo(EditorDocument& document) {
    const auto result=apply(document,after_,before_);
    if (result.success) if (auto* object=document.find_object(id_);object && object->prefabLink) {
        auto& overrides=object->prefabLink->overrides;
        if (levelOverride_) overrides["subdivision.level"]=*levelOverride_;else overrides.erase("subdivision.level");
        if (sharpOverride_) overrides["subdivision.sharp_edges"]=*sharpOverride_;else overrides.erase("subdivision.sharp_edges");
    }
    return result;
}
}
