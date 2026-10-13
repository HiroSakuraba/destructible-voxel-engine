#include "dve/editor_polygon.hpp"
#include "dve/editor_subdivision.hpp"
#include "dve/voxel_metric.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <tuple>

namespace dve::editor {
namespace {
Float3 cross3(Float3 a,Float3 b) {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
bool finite3(Float3 v) {
    return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);
}
Float3 normal3(const CookedPolygonAsset& mesh,std::uint32_t face) {
    const auto i=std::size_t(face)*3;
    return normalize(cross3(subtract(mesh.vertices[mesh.indices[i+1]].position,mesh.vertices[mesh.indices[i]].position),
        subtract(mesh.vertices[mesh.indices[i+2]].position,mesh.vertices[mesh.indices[i]].position)));
}
std::vector<std::uint32_t> canonical(const CookedPolygonAsset& mesh) {
    std::map<std::tuple<float,float,float>,std::uint32_t> map;
    std::vector<std::uint32_t> result;
    for(std::uint32_t i=0;i<mesh.vertices.size();++i) {
        const auto p=mesh.vertices[i].position;
        const auto [it,inserted]=map.emplace(std::tuple{p.x,p.y,p.z},i);
        (void)inserted; result.push_back(it->second);
    }
    return result;
}
PolygonEdge edge(std::uint32_t a,std::uint32_t b) { return {std::min(a,b),std::max(a,b)}; }
std::vector<std::uint32_t> face_materials(const CookedPolygonAsset& mesh) {
    std::vector<std::uint32_t> result(mesh.indices.size()/3);
    for(const auto& sub:mesh.submeshes)
        for(std::size_t i=sub.firstIndex/3;i<(sub.firstIndex+sub.indexCount)/3&&i<result.size();++i)
            result[i]=sub.materialIndex;
    return result;
}
void submeshes(CookedPolygonAsset& mesh,const std::vector<std::uint32_t>& materials) {
    mesh.submeshes.clear();
    for(std::size_t i=0;i<materials.size();++i) {
        if(mesh.submeshes.empty()||mesh.submeshes.back().materialIndex!=materials[i])
            mesh.submeshes.push_back({"Surface",std::uint32_t(i*3),3,materials[i]});
        else mesh.submeshes.back().indexCount+=3;
    }
}
void append_face(CookedPolygonAsset& mesh,std::vector<std::uint32_t>& materials,
                 std::uint32_t a,std::uint32_t b,std::uint32_t c,std::uint32_t material) {
    mesh.indices.insert(mesh.indices.end(),{a,b,c}); materials.push_back(material);
}
CookedPolygonAsset subset(const CookedPolygonAsset& source,const std::set<std::uint32_t>& faces) {
    auto mesh=source; mesh.indices.clear(); mesh.vertices.clear();
    std::map<std::uint32_t,std::uint32_t> remap;
    std::vector<std::uint32_t> materials;
    const auto original=face_materials(source);
    for(const auto face:faces) for(int j=0;j<3;++j) {
        const auto old=source.indices[std::size_t(face)*3+std::size_t(j)];
        auto [it,added]=remap.emplace(old,std::uint32_t(mesh.vertices.size()));
        if(added) mesh.vertices.push_back(source.vertices[old]);
        mesh.indices.push_back(it->second);
        if(j==2) materials.push_back(original[face]);
    }
    submeshes(mesh,materials); refresh_editor_polygon(mesh); return mesh;
}
PolygonVertex interpolate(const PolygonVertex& a,const PolygonVertex& b,float t) {
    const auto mix=[t](float x,float y) { return x+(y-x)*t; };
    PolygonVertex v;
    v.position=add(a.position,multiply(subtract(b.position,a.position),t));
    v.normal=normalize(add(a.normal,multiply(subtract(b.normal,a.normal),t)));
    v.texcoord={mix(a.texcoord.x,b.texcoord.x),mix(a.texcoord.y,b.texcoord.y)};
    v.texcoord1={mix(a.texcoord1.x,b.texcoord1.x),mix(a.texcoord1.y,b.texcoord1.y)};
    v.color={mix(a.color.x,b.color.x),mix(a.color.y,b.color.y),mix(a.color.z,b.color.z),mix(a.color.w,b.color.w)};
    v.tangent={mix(a.tangent.x,b.tangent.x),mix(a.tangent.y,b.tangent.y),mix(a.tangent.z,b.tangent.z),a.tangent.w};
    return v;
}
} // namespace

void refresh_editor_polygon(CookedPolygonAsset& mesh,bool normals) {
    mesh.bounds.minimum={INFINITY,INFINITY,INFINITY};
    mesh.bounds.maximum={-INFINITY,-INFINITY,-INFINITY};
    if(normals) for(auto& vertex:mesh.vertices) vertex.normal={};
    for(const auto& vertex:mesh.vertices) {
        const auto p=vertex.position;
        mesh.bounds.minimum={std::min(mesh.bounds.minimum.x,p.x),std::min(mesh.bounds.minimum.y,p.y),std::min(mesh.bounds.minimum.z,p.z)};
        mesh.bounds.maximum={std::max(mesh.bounds.maximum.x,p.x),std::max(mesh.bounds.maximum.y,p.y),std::max(mesh.bounds.maximum.z,p.z)};
    }
    std::vector<Float3> tangents(mesh.vertices.size()),bitangents(mesh.vertices.size());
    for(std::size_t i=0;i+2<mesh.indices.size();i+=3) {
        const auto a=mesh.indices[i],b=mesh.indices[i+1],c=mesh.indices[i+2];
        const auto e1=subtract(mesh.vertices[b].position,mesh.vertices[a].position);
        const auto e2=subtract(mesh.vertices[c].position,mesh.vertices[a].position);
        if(normals) {
            const auto n=cross3(e1,e2);
            for(auto v:{a,b,c}) mesh.vertices[v].normal=add(mesh.vertices[v].normal,n);
        }
        const auto ta=mesh.vertices[a].texcoord,tb=mesh.vertices[b].texcoord,tc=mesh.vertices[c].texcoord;
        const float ux=tb.x-ta.x,uy=tb.y-ta.y,vx=tc.x-ta.x,vy=tc.y-ta.y;
        const float denominator=ux*vy-vx*uy;
        if(std::abs(denominator)>1e-12F) {
            const auto tangent=multiply(subtract(multiply(e1,vy),multiply(e2,uy)),1/denominator);
            const auto bitangent=multiply(subtract(multiply(e2,ux),multiply(e1,vx)),1/denominator);
            for(auto v:{a,b,c}) {
                tangents[v]=add(tangents[v],tangent); bitangents[v]=add(bitangents[v],bitangent);
            }
        }
    }
    for(std::size_t i=0;i<mesh.vertices.size();++i) {
        auto& vertex=mesh.vertices[i]; vertex.normal=normalize(vertex.normal);
        if(length_squared(vertex.normal)<0.01F) vertex.normal={0,1,0};
        auto tangent=normalize(subtract(tangents[i],multiply(vertex.normal,dot(vertex.normal,tangents[i]))));
        if(length_squared(tangent)<0.01F) {
            const auto helper=std::abs(vertex.normal.y)<0.9F?Float3{0,1,0}:Float3{1,0,0};
            tangent=normalize(cross3(helper,vertex.normal));
        }
        const float sign=dot(cross3(vertex.normal,tangent),bitangents[i])<0?-1.0F:1.0F;
        vertex.tangent={tangent.x,tangent.y,tangent.z,sign};
    }
    if(mesh.vertices.empty()) mesh.bounds={};
    mesh.contentHash=polygon_asset_content_hash(mesh);
}
CookedPolygonAsset make_editor_polygon_cube(EditorObjectId id,float size) {
    CookedPolygonAsset mesh; mesh.objectId=id;
    mesh.materials.resize(1); mesh.materials[0].name="Mesh"; mesh.materialBindings.resize(1);
    const Float3 corners[]={{0,0,0},{size,0,0},{size,size,0},{0,size,0},{0,0,size},{size,0,size},{size,size,size},{0,size,size}};
    const int faces[][4]={{0,3,2,1},{4,5,6,7},{0,4,7,3},{1,2,6,5},{0,1,5,4},{3,7,6,2}};
    for(const auto& face:faces) {
        const auto base=std::uint32_t(mesh.vertices.size());
        for(int j=0;j<4;++j) {
            PolygonVertex vertex; vertex.position=corners[face[j]];
            vertex.texcoord={j==1||j==2?1.0F:0.0F,j>=2?1.0F:0.0F}; mesh.vertices.push_back(vertex);
        }
        mesh.indices.insert(mesh.indices.end(),{base,base+1,base+2,base,base+2,base+3});
    }
    mesh.submeshes={{"Cube",0,36,0}}; refresh_editor_polygon(mesh,true); return mesh;
}
CookedPolygonAsset make_editor_polygon_plane(EditorObjectId id,float size) {
    auto mesh=make_editor_polygon_cube(id,size); mesh.vertices.resize(4); mesh.indices={0,1,2,0,2,3};
    mesh.vertices[0].position={-size/2,0,-size/2}; mesh.vertices[1].position={-size/2,0,size/2};
    mesh.vertices[2].position={size/2,0,size/2}; mesh.vertices[3].position={size/2,0,-size/2};
    mesh.submeshes={{"Plane",0,6,0}}; mesh.materialBindings[0].doubleSided=true;
    refresh_editor_polygon(mesh,true); return mesh;
}
CookedPolygonAsset bake_editor_polygon(const EditorObject& object,bool controlCage) {
    auto mesh=controlCage ? *object.polygon : *editor_surface_mesh(object); mesh.objectId=object.id;
    for(auto& vertex:mesh.vertices) {
        vertex.position=multiply_components(vertex.position,object.voxelScale);
        vertex.normal=normalize(divide_components(vertex.normal,object.voxelScale));
    }
    refresh_editor_polygon(mesh); return mesh;
}
std::vector<PolygonEdge> polygon_edges(const CookedPolygonAsset& mesh) {
    const auto canon=canonical(mesh); std::map<std::uint64_t,PolygonEdge> edges;
    for(std::size_t i=0;i+2<mesh.indices.size();i+=3) for(int j=0;j<3;++j) {
        const auto e=edge(canon[mesh.indices[i+std::size_t(j)]],canon[mesh.indices[i+std::size_t((j+1)%3)]]);
        if(e.a!=e.b) edges.emplace(e.key(),e);
    }
    std::vector<PolygonEdge> result;
    for(const auto& [key,e]:edges) { (void)key; result.push_back(e); }
    return result;
}
std::set<std::uint64_t> polygon_all_components(const CookedPolygonAsset& mesh,PolygonSelectionMode mode) {
    std::set<std::uint64_t> result;
    if(mode==PolygonSelectionMode::Edge) for(const auto e:polygon_edges(mesh)) result.insert(e.key());
    else if(mode==PolygonSelectionMode::Vertex) {
        const auto canon=canonical(mesh); for(const auto i:mesh.indices) result.insert(canon[i]);
    } else for(std::uint32_t face=0;face<mesh.indices.size()/3;++face) result.insert(face);
    return result;
}
bool polygon_selection_valid(const CookedPolygonAsset& mesh,const PolygonSelection& selection) {
    if(selection.object!=mesh.objectId) return false;
    const auto all=polygon_all_components(mesh,selection.mode);
    return std::includes(all.begin(),all.end(),selection.components.begin(),selection.components.end());
}
std::set<std::uint32_t> polygon_selected_vertices(const CookedPolygonAsset& mesh,const PolygonSelection& selection) {
    const auto canon=canonical(mesh); std::set<std::uint32_t> picked;
    if(selection.mode==PolygonSelectionMode::Face) for(const auto face:selection.components) {
        if(face>=mesh.indices.size()/3) continue;
        for(int j=0;j<3;++j) picked.insert(canon[mesh.indices[face*3+std::size_t(j)]]);
    } else if(selection.mode==PolygonSelectionMode::Edge) for(const auto key:selection.components) {
        picked.insert(std::uint32_t(key>>32U)); picked.insert(std::uint32_t(key));
    } else for(const auto key:selection.components) if(key<mesh.vertices.size()) picked.insert(canon[std::size_t(key)]);
    std::set<std::uint32_t> result;
    for(std::uint32_t i=0;i<canon.size();++i) if(picked.contains(canon[i])) result.insert(i);
    return result;
}
std::set<std::uint32_t> polygon_selected_faces(const CookedPolygonAsset& mesh,const PolygonSelection& selection) {
    std::set<std::uint32_t> result;
    if(selection.mode==PolygonSelectionMode::Face) {
        for(const auto face:selection.components) if(face<mesh.indices.size()/3) result.insert(std::uint32_t(face));
        return result;
    }
    const auto vertices=polygon_selected_vertices(mesh,selection); const auto canon=canonical(mesh);
    for(std::uint32_t face=0;face<mesh.indices.size()/3;++face) {
        const auto i=std::size_t(face)*3;
        if(selection.mode==PolygonSelectionMode::Vertex) {
            if(vertices.contains(mesh.indices[i])||vertices.contains(mesh.indices[i+1])||vertices.contains(mesh.indices[i+2])) result.insert(face);
        } else for(int j=0;j<3;++j)
            if(selection.components.contains(edge(canon[mesh.indices[i+std::size_t(j)]],canon[mesh.indices[i+std::size_t((j+1)%3)]]).key()))
                result.insert(face);
    }
    return result;
}

PolygonSelection convert_polygon_selection(const CookedPolygonAsset& mesh,
    const PolygonSelection& source, PolygonSelectionMode mode) {
    PolygonSelection result{mesh.objectId,mode,{}};
    if (!validate_polygon_asset(mesh) || !polygon_selection_valid(mesh,source)) return result;
    if (source.mode == mode) { result.components = source.components; return result; }
    const auto canon = canonical(mesh);
    if (mode == PolygonSelectionMode::Vertex) {
        for (const auto index : polygon_selected_vertices(mesh,source))
            result.components.insert(canon[index]);
    } else if (mode == PolygonSelectionMode::Face) {
        const auto faces = polygon_selected_faces(mesh,source);
        result.components.insert(faces.begin(),faces.end());
    } else if (source.mode == PolygonSelectionMode::Face) {
        for (const auto face : source.components)
            for (unsigned j = 0; j < 3; ++j)
                result.components.insert(edge(canon[mesh.indices[std::size_t(face)*3+j]],
                    canon[mesh.indices[std::size_t(face)*3+(j+1)%3]]).key());
    } else {
        std::set<std::uint32_t> points;
        for (const auto index : polygon_selected_vertices(mesh,source)) points.insert(canon[index]);
        for (const auto item : polygon_edges(mesh))
            if (points.contains(item.a) || points.contains(item.b)) result.components.insert(item.key());
    }
    const auto valid = polygon_all_components(mesh,mode);
    std::erase_if(result.components,[&](auto key) { return !valid.contains(key); });
    return result;
}

std::set<std::uint64_t> edit_polygon_selection(const CookedPolygonAsset& mesh,
    const PolygonSelection& selection, PolygonSelectionEdit operation) {
    if (!validate_polygon_asset(mesh) || !polygon_selection_valid(mesh, selection)) return {};
    const auto& selected = selection.components;
    if (selected.empty()) return {};
    const auto canon = canonical(mesh);
    // Incidence buckets avoid constructing dense adjacency cliques at high-valence vertices.
    std::map<std::uint64_t, std::set<std::uint64_t>> buckets;
    std::map<std::uint64_t, std::set<std::uint64_t>> membership;
    const auto connect = [&](std::uint64_t component, std::uint64_t bucket) {
        buckets[bucket].insert(component);
        membership[component].insert(bucket);
    };
    for (std::uint32_t f = 0; f < mesh.indices.size()/3; ++f) {
        for (unsigned j = 0; j < 3; ++j) {
            const auto a = canon[mesh.indices[std::size_t(f)*3+j]];
            const auto b = canon[mesh.indices[std::size_t(f)*3+(j+1)%3]];
            if (a == b) continue;
            const auto key = edge(a,b).key();
            if (selection.mode == PolygonSelectionMode::Face) connect(f,key);
            else if (selection.mode == PolygonSelectionMode::Edge) {
                connect(key,a);
                connect(key,b);
            } else {
                connect(a,key);
                connect(b,key);
            }
        }
    }
    auto result = selected;
    if (operation == PolygonSelectionEdit::Shrink) {
        for (const auto component : selected) {
            bool boundary = false;
            for (const auto bucket : membership[component]) {
                for (const auto neighbor : buckets[bucket]) {
                    if (!selected.contains(neighbor)) { boundary = true; break; }
                }
                if (boundary) break;
            }
            if (boundary) result.erase(component);
        }
        return result;
    }
    std::queue<std::uint64_t> pending;
    for (const auto component : selected) pending.push(component);
    std::set<std::uint64_t> visitedBuckets;
    while (!pending.empty()) {
        const auto component = pending.front();
        pending.pop();
        for (const auto bucket : membership[component]) {
            if (!visitedBuckets.insert(bucket).second) continue;
            for (const auto neighbor : buckets[bucket]) {
                if (result.insert(neighbor).second && operation == PolygonSelectionEdit::Connected)
                    pending.push(neighbor);
            }
        }
    }
    return result;
}

PolygonEditResult edit_polygon(const CookedPolygonAsset& source,const PolygonSelection& selection,
                               PolygonEdit operation,const PolygonEditSettings& settings) {
    PolygonEditResult result{source,selection,{}}; auto& mesh=result.mesh;
    if(!validate_polygon_asset(source)||!polygon_selection_valid(source,selection)) {
        result.error="Mesh or component selection is invalid"; return result;
    }
    if(!finite3(settings.vector)||!finite3(settings.pivot)||!std::isfinite(settings.amount)||!std::isfinite(settings.inset)) {
        result.error="Tool values must be finite"; return result;
    }
    if(selection.components.empty()&&operation!=PolygonEdit::RecalculateNormals) {
        result.error="Select mesh components first"; return result;
    }
    const auto selected=polygon_selected_vertices(source,selection);
    const auto faces=polygon_selected_faces(source,selection);
    std::set<std::uint32_t> direct;
    for(const auto face:faces) for(int j=0;j<3;++j) direct.insert(source.indices[std::size_t(face)*3+std::size_t(j)]);
    auto materials=face_materials(mesh); bool normals=false;
    if(operation==PolygonEdit::Translate||operation==PolygonEdit::Rotate||operation==PolygonEdit::Scale) {
        if(operation==PolygonEdit::Scale&&!valid_voxel_metric(settings.vector)) {
            result.error="Scale factors must be positive"; return result;
        }
        for(const auto i:selected) {
            auto& p=mesh.vertices[i].position;
            if(operation==PolygonEdit::Translate) p=add(p,settings.vector);
            else if(operation==PolygonEdit::Rotate) p=add(settings.pivot,rotate(settings.rotation,subtract(p,settings.pivot)));
            else p=add(settings.pivot,multiply_components(subtract(p,settings.pivot),settings.vector));
        }
        normals=true;
    } else if(operation==PolygonEdit::Delete) {
        std::set<std::uint32_t> keep;
        for(std::uint32_t face=0;face<mesh.indices.size()/3;++face) if(!faces.contains(face)) keep.insert(face);
        mesh=subset(source,keep); result.selection.components.clear();
    } else if(operation==PolygonEdit::Duplicate) {
        const auto copied=subset(source,faces); const auto base=std::uint32_t(mesh.vertices.size());
        const auto first=std::uint32_t(mesh.indices.size()/3);
        for(auto vertex:copied.vertices) { vertex.position=add(vertex.position,settings.vector); mesh.vertices.push_back(vertex); }
        for(auto index:copied.indices) mesh.indices.push_back(index+base);
        const auto more=face_materials(copied); materials.insert(materials.end(),more.begin(),more.end());
        submeshes(mesh,materials); result.selection.mode=PolygonSelectionMode::Face; result.selection.components.clear();
        for(std::uint32_t face=first;face<mesh.indices.size()/3;++face) result.selection.components.insert(face);
    } else if(operation==PolygonEdit::Extrude||operation==PolygonEdit::Inset||operation==PolygonEdit::Bevel) {
        if(selection.mode!=PolygonSelectionMode::Face) { result.error="Face Extrude, Inset and Bevel require face selection"; return result; }
        const float inset=operation==PolygonEdit::Extrude?0:settings.inset;
        if(!(inset>=0&&inset<1)) { result.error="Inset fraction must be in [0,1)"; return result; }
        const auto canon=canonical(source);
        std::map<std::uint64_t,std::vector<std::pair<std::uint32_t,std::uint32_t>>> boundary;
        std::map<std::uint32_t,Float3> normalsAt; Float3 center{};
        std::map<std::uint64_t,std::vector<std::uint32_t>> atEdge;
        std::set<std::uint32_t> geometric;
        for(const auto vertex:direct) geometric.insert(canon[vertex]);
        for(const auto vertex:geometric) center=add(center,source.vertices[vertex].position);
        center=multiply(center,1.0F/float(geometric.size()));
        for(const auto face:faces) {
            const auto n=normal3(source,face);
            for(int j=0;j<3;++j) {
                const auto a=source.indices[std::size_t(face)*3+std::size_t(j)];
                const auto b=source.indices[std::size_t(face)*3+std::size_t((j+1)%3)];
                boundary[edge(canon[a],canon[b]).key()].push_back({a,b});
                atEdge[edge(canon[a],canon[b]).key()].push_back(face);
                normalsAt[canon[a]]=add(normalsAt[canon[a]],n);
            }
        }
        for(const auto& [key,edges]:boundary) { (void)key; if(edges.size()>2) { result.error="Selected region has nonmanifold edges"; return result; } }
        const auto boundaryCount=std::count_if(boundary.begin(),boundary.end(),[](const auto& item){return item.second.size()==1;});
        if(boundaryCount==0) {result.error="Selected face patch has no boundary; use Scale for the whole closed mesh";return result;}
        if(operation!=PolygonEdit::Extrude) {
            const auto normal=normal3(source,*faces.begin());
            std::set<std::uint32_t> reached{*faces.begin()};std::vector<std::uint32_t> queue{*faces.begin()};
            for(std::size_t i=0;i<queue.size();++i) {
                const auto face=queue[i];
                if(dot(normal,normal3(source,face))<.9999F) {
                    result.error="Inset and face Bevel need a coplanar face patch";return result;
                }
                for(int j=0;j<3;++j) {
                    const auto a=canon[source.indices[std::size_t(face)*3+std::size_t(j)]];
                    const auto b=canon[source.indices[std::size_t(face)*3+std::size_t((j+1)%3)]];
                    for(const auto next:atEdge[edge(a,b).key()]) if(reached.insert(next).second) queue.push_back(next);
                }
            }
            if(reached.size()!=faces.size()) {
                result.error="Inset and face Bevel need one connected patch";return result;
            }
            for(const auto& [key,edges]:boundary) {
                (void)key;if(edges.size()!=1)continue;
                const auto [a,b]=edges[0];
                const auto inward=cross3(normal,subtract(source.vertices[b].position,source.vertices[a].position));
                for(const auto vertex:geometric) if(dot(inward,subtract(source.vertices[vertex].position,source.vertices[a].position))<-1e-5F) {
                    result.error="Inset and face Bevel currently support convex patches";return result;
                }
            }
        }
        if(mesh.vertices.size()+direct.size()>settings.maximumVertices||mesh.indices.size()+boundary.size()*6>settings.maximumIndices) {
            result.error="Polygon edit exceeds the geometry budget"; return result;
        }
        std::map<std::uint32_t,std::uint32_t> remap;
        for(const auto vertex:direct) {
            auto next=source.vertices[vertex]; next.position=add(center,multiply(subtract(next.position,center),1-inset));
            if(operation!=PolygonEdit::Inset) next.position=add(next.position,multiply(normalize(normalsAt[canon[vertex]]),settings.amount));
            remap[vertex]=std::uint32_t(mesh.vertices.size()); mesh.vertices.push_back(next);
        }
        for(const auto face:faces) for(int j=0;j<3;++j)
            mesh.indices[std::size_t(face)*3+std::size_t(j)]=remap.at(source.indices[std::size_t(face)*3+std::size_t(j)]);
        for(const auto& [key,edges]:boundary) {
            (void)key; if(edges.size()!=1) continue; const auto [a,b]=edges[0]; const auto mat=materials[*faces.begin()];
            append_face(mesh,materials,a,b,remap.at(b),mat); append_face(mesh,materials,a,remap.at(b),remap.at(a),mat);
        }
        submeshes(mesh,materials); normals=true;
    } else if(operation==PolygonEdit::RecalculateNormals) normals=true;
    else if(operation==PolygonEdit::FlipNormals) {
        for(const auto face:faces) std::swap(mesh.indices[std::size_t(face)*3+1],mesh.indices[std::size_t(face)*3+2]);
        normals=true;
    } else if(operation==PolygonEdit::ProjectUV) {
        // Detach shared attributes before changing face UVs; geometric seam identity still joins positions.
        std::set<std::uint32_t> uvVertices=selected;
        if(selection.mode==PolygonSelectionMode::Face) {
            std::set<std::uint32_t> elsewhere;
            for(std::uint32_t face=0;face<source.indices.size()/3;++face) if(!faces.contains(face))
                for(int j=0;j<3;++j) elsewhere.insert(source.indices[std::size_t(face)*3+std::size_t(j)]);
            std::map<std::uint32_t,std::uint32_t> remap;
            uvVertices.clear();
            for(const auto vertex:direct) {
                auto current=vertex;
                if(elsewhere.contains(vertex)) { current=std::uint32_t(mesh.vertices.size()); mesh.vertices.push_back(source.vertices[vertex]); }
                remap[vertex]=current; uvVertices.insert(current);
            }
            for(const auto face:faces) for(int j=0;j<3;++j)
                mesh.indices[std::size_t(face)*3+std::size_t(j)]=remap.at(source.indices[std::size_t(face)*3+std::size_t(j)]);
        }
        for(const auto vertex:uvVertices) {
            const auto p=mesh.vertices[vertex].position;
            const auto uv=settings.uvAxis==0?Float2{p.z,p.y}:settings.uvAxis==1?Float2{p.x,p.z}:Float2{p.x,p.y};
            mesh.vertices[vertex].texcoord={uv.x*settings.amount,uv.y*settings.amount};
        }
    } else if(operation==PolygonEdit::AssignMaterial) {
        if(settings.material>=mesh.materials.size()) { result.error="Material is outside the mesh palette"; return result; }
        for(const auto face:faces) materials[face]=settings.material;
        submeshes(mesh,materials);
    } else if(operation==PolygonEdit::Weld) {
        if(!(settings.amount>0)) { result.error="Weld distance must be positive"; return result; }
        if(selected.size()>10000) { result.error="Weld exceeds the 10,000 selected-vertex budget"; return result; }
        std::map<std::uint32_t,std::uint32_t> remap; std::vector<std::uint32_t> kept;
        for(const auto vertex:selected) {
            const auto it=std::find_if(kept.begin(),kept.end(),[&](auto other) {
                return length_squared(subtract(mesh.vertices[vertex].position,mesh.vertices[other].position))<=settings.amount*settings.amount;
            });
            if(it==kept.end()) { kept.push_back(vertex); remap[vertex]=vertex; } else remap[vertex]=*it;
        }
        for(auto& index:mesh.indices) if(remap.contains(index)) index=remap[index];
        std::vector<std::uint32_t> indices,newMaterials;
        for(std::size_t face=0;face<mesh.indices.size()/3;++face) {
            const auto a=mesh.indices[face*3],b=mesh.indices[face*3+1],c=mesh.indices[face*3+2];
            if(a==b||a==c||b==c) continue;
            indices.insert(indices.end(),{a,b,c}); newMaterials.push_back(materials[face]);
        }
        mesh.indices=std::move(indices); submeshes(mesh,newMaterials); result.selection.components.clear(); normals=true;
    }
    if(mesh.vertices.size()>settings.maximumVertices||mesh.indices.size()>settings.maximumIndices) {
        result.error="Polygon edit exceeds the geometry budget"; return result;
    }
    refresh_editor_polygon(mesh,normals);
    if(!mesh.indices.empty()) { const auto valid=validate_polygon_asset(mesh); if(!valid) result.error=valid.message; }
    return result;
}

PolygonEditResult slice_polygon(const CookedPolygonAsset& source,Float3 point,Float3 normal,bool front,bool cap) {
    PolygonEditResult result{source,{source.objectId,PolygonSelectionMode::Face,{}},{}};
    auto& mesh=result.mesh;
    if(!validate_polygon_asset(source)||!finite3(point)||!finite3(normal)||length_squared(normal)<1e-12F) {
        result.error="Slice needs a valid mesh and plane"; return result;
    }
    normal=normalize(normal); if(front) normal=multiply(normal,-1);
    mesh.vertices.clear(); mesh.indices.clear(); mesh.submeshes.clear();
    const auto sourceMaterials=face_materials(source); std::vector<std::uint32_t> materials;
    std::vector<Float3> cutPoints; std::set<std::pair<std::uint32_t,std::uint32_t>> segments;
    constexpr float epsilon=1e-5F;
    const auto cut_id=[&](Float3 p) {
        for(std::uint32_t i=0;i<cutPoints.size();++i)
            if(length_squared(subtract(p,cutPoints[i]))<epsilon*epsilon) return i;
        cutPoints.push_back(p); return std::uint32_t(cutPoints.size()-1);
    };
    for(std::uint32_t face=0;face<source.indices.size()/3;++face) {
        std::vector<PolygonVertex> output; std::vector<Float3> crossings;
        for(int j=0;j<3;++j) {
            const auto& a=source.vertices[source.indices[std::size_t(face)*3+std::size_t(j)]];
            const auto& b=source.vertices[source.indices[std::size_t(face)*3+std::size_t((j+1)%3)]];
            const float da=dot(subtract(a.position,point),normal),db=dot(subtract(b.position,point),normal);
            const bool insideA=da<=epsilon,insideB=db<=epsilon;
            if(insideA) output.push_back(a);
            if(insideA!=insideB) {
                const auto vertex=interpolate(a,b,std::clamp(da/(da-db),0.0F,1.0F));
                output.push_back(vertex); crossings.push_back(vertex.position);
            }
        }
        if(crossings.size()==2&&length_squared(subtract(crossings[0],crossings[1]))>epsilon*epsilon) {
            const auto a=cut_id(crossings[0]),b=cut_id(crossings[1]); segments.insert(std::minmax(a,b));
        }
        if(cap&&cutPoints.size()>4096) { result.error="Slice cap exceeds the 4096-point contour budget"; return result; }
        if(output.size()<3) continue;
        const auto base=std::uint32_t(mesh.vertices.size()); mesh.vertices.insert(mesh.vertices.end(),output.begin(),output.end());
        for(std::uint32_t j=1;j+1<output.size();++j) {
            const auto area=cross3(subtract(output[j].position,output[0].position),subtract(output[j+1].position,output[0].position));
            if(length_squared(area)>1e-16F) append_face(mesh,materials,base,base+j,base+j+1,sourceMaterials[face]);
        }
    }
    if(cap&&!segments.empty()) {
        std::map<std::uint32_t,std::set<std::uint32_t>> graph;
        for(const auto& [a,b]:segments) { graph[a].insert(b); graph[b].insert(a); }
        for(const auto& [vertex,links]:graph) {
            (void)vertex; if(links.size()!=2) {
                result.error="Slice cap needs closed simple contours; disable caps for open meshes"; return result;
            }
        }
        std::vector<std::vector<std::uint32_t>> loops; auto unused=segments;
        while(!unused.empty()) {
            const auto first=unused.begin()->first; std::vector<std::uint32_t> loop{first};
            auto previous=first,current=unused.begin()->second; unused.erase(unused.begin());
            while(current!=first) {
                loop.push_back(current); const auto& links=graph[current];
                const auto next=*links.begin()==previous?*links.rbegin():*links.begin();
                if(!unused.erase(std::minmax(current,next))) { result.error="Slice contour is self-intersecting"; return result; }
                previous=current; current=next;
                if(loop.size()>cutPoints.size()) { result.error="Slice contour could not close"; return result; }
            }
            loops.push_back(std::move(loop));
        }
        const auto helper=std::abs(normal.y)<0.9F?Float3{0,1,0}:Float3{1,0,0};
        const auto u=normalize(cross3(helper,normal)),v=cross3(normal,u);
        const auto project=[&](std::uint32_t i) { return Float2{dot(cutPoints[i],u),dot(cutPoints[i],v)}; };
        const auto orient=[](Float2 a,Float2 b,Float2 c) { return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x); };
        const auto contains=[&](const std::vector<std::uint32_t>& loop,Float2 p) {
            bool inside=false;
            for(std::size_t i=0,j=loop.size()-1;i<loop.size();j=i++) {
                const auto a=project(loop[i]),b=project(loop[j]);
                if((a.y>p.y)!=(b.y>p.y)&&p.x<(b.x-a.x)*(p.y-a.y)/(b.y-a.y)+a.x) inside=!inside;
            }
            return inside;
        };
        for(std::size_t i=0;i<loops.size();++i) for(std::size_t j=0;j<loops.size();++j)
            if(i!=j&&contains(loops[i],project(loops[j][0]))) {
                result.error="Nested cap contours require hole triangulation; disable caps"; return result;
            }
        for(auto loop:loops) {
            float area=0;
            for(std::size_t i=0;i<loop.size();++i) {
                const auto a=project(loop[i]),b=project(loop[(i+1)%loop.size()]); area+=a.x*b.y-b.x*a.y;
            }
            if(area<0) std::reverse(loop.begin(),loop.end());
            std::map<std::uint32_t,std::uint32_t> mapped;
            for(const auto i:loop) {
                PolygonVertex vertex; vertex.position=cutPoints[i]; vertex.normal=normal; vertex.texcoord=project(i);
                mapped[i]=std::uint32_t(mesh.vertices.size()); mesh.vertices.push_back(vertex);
            }
            while(loop.size()>2) {
                bool found=false;
                for(std::size_t i=0;i<loop.size();++i) {
                    const auto a=loop[(i+loop.size()-1)%loop.size()],b=loop[i],c=loop[(i+1)%loop.size()];
                    const auto pa=project(a),pb=project(b),pc=project(c);
                    if(orient(pa,pb,pc)<=epsilon*epsilon) continue;
                    bool covered=false;
                    for(const auto other:loop) {
                        if(other==a||other==b||other==c) continue;
                        const auto p=project(other);
                        // Closed test: a loop vertex on the new diagonal (a,c) would become a T-junction
                        // and later leave a zero-area remainder, so it blocks this ear as well.
                        if(orient(pa,pb,p)>=-epsilon*epsilon&&orient(pb,pc,p)>=-epsilon*epsilon&&orient(pc,pa,p)>=-epsilon*epsilon) {
                            covered=true; break;
                        }
                    }
                    if(covered) continue;
                    append_face(mesh,materials,mapped[a],mapped[b],mapped[c],sourceMaterials.empty()?0:sourceMaterials[0]);
                    loop.erase(loop.begin()+std::ptrdiff_t(i)); found=true; break;
                }
                if(!found) { result.error="Slice cap could not be triangulated without dropping boundary vertices"; return result; }
            }
        }
    }
    submeshes(mesh,materials); refresh_editor_polygon(mesh);
    result.selection.components=polygon_all_components(mesh,PolygonSelectionMode::Face);
    if(!mesh.indices.empty()) { const auto valid=validate_polygon_asset(mesh); if(!valid) result.error=valid.message; }
    return result;
}

PolygonEditResult join_editor_polygons(const EditorDocument& document,EditorObjectId primary,std::span<const EditorObjectId> ids,bool evaluated) {
    PolygonEditResult result; const auto* target=document.find_object(primary);
    if(!target||!target->polygon) { result.error="Join needs a primary polygon object"; return result; }
    auto& mesh=result.mesh; mesh.objectId=primary; result.selection={primary,PolygonSelectionMode::Face,{}};
    std::vector<std::uint32_t> materials; std::set<EditorObjectId> seen;
    for(const auto id:ids) {
        if(!seen.insert(id).second) continue;
        const auto* object=document.find_object(id);
        if(!object||!object->polygon||object->flags.locked||!validate_polygon_asset(*object->polygon)) {
            result.error="Join needs valid unlocked polygon objects"; return result;
        }
        const auto& source=evaluated ? *editor_surface_mesh(*object) : *object->polygon;
        const auto vertexOffset=std::uint32_t(mesh.vertices.size()),materialOffset=std::uint32_t(mesh.materials.size());
        const auto textureOffset=std::uint32_t(mesh.textures.size()),imageOffset=std::uint32_t(mesh.images.size());
        const auto samplerOffset=std::uint32_t(mesh.samplers.size());
        mesh.images.insert(mesh.images.end(),source.images.begin(),source.images.end());
        mesh.samplers.insert(mesh.samplers.end(),source.samplers.begin(),source.samplers.end());
        for(auto texture:source.textures) {
            texture.imageIndex+=imageOffset; if(texture.samplerIndex) *texture.samplerIndex+=samplerOffset;
            mesh.textures.push_back(std::move(texture));
        }
        for(auto material:source.materials) {
            for(auto& layer:material.layers) layer.sourceMaterial=static_cast<MaterialId>(layer.sourceMaterial+materialOffset);
            mesh.materials.push_back(std::move(material));
        }
        for(std::size_t i=0;i<source.materials.size();++i) {
            auto binding=i<source.materialBindings.size()?source.materialBindings[i]:PolygonMaterialBinding{};
            const auto shift=[&](PolygonTextureBinding& b) { if(b.texture) *b.texture+=textureOffset; };
            for(auto* b:{&binding.baseColor,&binding.metallicRoughness,&binding.normal,&binding.emissive,&binding.opacity,
                         &binding.height,&binding.detailBaseColor,&binding.detailNormal,&binding.detailRoughness}) shift(*b);
            for(auto& layer:binding.layers) { shift(layer.mask); shift(layer.height); }
            mesh.materialBindings.push_back(std::move(binding));
        }
        const auto pose=document.world_transform(id).value_or(object->transform);
        const auto targetPose=document.world_transform(primary).value_or(target->transform);
        for(auto vertex:source.vertices) {
            vertex.position=world_to_voxel(targetPose,target->voxelScale,voxel_to_world(pose,object->voxelScale,vertex.position));
            const auto worldNormal=voxel_normal_to_world(pose,object->voxelScale,vertex.normal);
            vertex.normal=normalize(multiply_components(inverse_transform_vector(targetPose,worldNormal),target->voxelScale));
            mesh.vertices.push_back(vertex);
        }
        for(const auto index:source.indices) mesh.indices.push_back(index+vertexOffset);
        for(const auto material:face_materials(source)) materials.push_back(material+materialOffset);
    }
    if(mesh.vertices.size()>1000000||mesh.indices.size()>6000000) {
        result.error="Join exceeds the geometry budget"; return result;
    }
    submeshes(mesh,materials); refresh_editor_polygon(mesh);
    const auto valid=validate_polygon_asset(mesh); if(!valid) result.error=valid.message;
    result.selection.components=polygon_all_components(mesh,PolygonSelectionMode::Face); return result;
}
CookedPolygonAsset extract_polygon_faces(const CookedPolygonAsset& mesh,const PolygonSelection& selection) {
    return subset(mesh,polygon_selected_faces(mesh,selection));
}
std::vector<CookedPolygonAsset> separate_polygon_components(const CookedPolygonAsset& mesh) {
    const auto canon=canonical(mesh); std::map<std::uint32_t,std::vector<std::uint32_t>> atVertex;
    for(std::uint32_t face=0;face<mesh.indices.size()/3;++face) for(int j=0;j<3;++j)
        atVertex[canon[mesh.indices[std::size_t(face)*3+std::size_t(j)]]].push_back(face);
    std::set<std::uint32_t> remaining;
    for(std::uint32_t face=0;face<mesh.indices.size()/3;++face) remaining.insert(face);
    std::vector<CookedPolygonAsset> result;
    while(!remaining.empty()) {
        std::queue<std::uint32_t> queue; std::set<std::uint32_t> faces;
        queue.push(*remaining.begin()); remaining.erase(remaining.begin());
        while(!queue.empty()) {
            const auto face=queue.front(); queue.pop(); faces.insert(face);
            for(int j=0;j<3;++j) for(const auto next:atVertex[canon[mesh.indices[std::size_t(face)*3+std::size_t(j)]]])
                if(remaining.erase(next)) queue.push(next);
        }
        result.push_back(subset(mesh,faces));
    }
    return result;
}

std::optional<PolygonBvhHit> pick_editor_polygon(const EditorObject& object,ViewportRay ray,float maximum,bool controlCage) {
    if(!object.polygon||!valid_voxel_metric(object.voxelScale)) return std::nullopt;
    struct Cached { std::uint64_t hash{}; Float3 scale{}; std::shared_ptr<PolygonBvh> bvh; };
    static std::mutex mutex; static std::map<std::pair<EditorObjectId,bool>,Cached> cache;
    std::shared_ptr<PolygonBvh> bvh;
    {
        const auto* surface=controlCage ? &*object.polygon : editor_surface_mesh(object);
        std::lock_guard lock(mutex); auto& entry=cache[{object.id,controlCage}]; const auto scale=object.voxelScale;
        if(entry.hash!=surface->contentHash||entry.scale.x!=scale.x||entry.scale.y!=scale.y||
           entry.scale.z!=scale.z||!entry.bvh) {
            auto next=std::make_shared<PolygonBvh>();
            if(!next->build(bake_editor_polygon(object,controlCage))) return std::nullopt;
            entry={surface->contentHash,scale,next};
        }
        bvh=entry.bvh;
        if(cache.size()>128) { auto retained=entry; cache.clear(); cache.emplace(std::make_pair(object.id,controlCage),std::move(retained)); }
    }
    auto hit=bvh->raycast(inverse_transform_point(object.transform,ray.origin),
        normalize(inverse_transform_vector(object.transform,ray.direction)),maximum);
    if(hit) {
        hit->position=transform_point(object.transform,hit->position);
        hit->normal=normalize(transform_vector(object.transform,hit->normal));
    }
    return hit;
}
namespace {
struct ComponentPoint { std::uint64_t key{}; Float3 point{}; };
std::vector<ComponentPoint> component_points(const CookedPolygonAsset& mesh,PolygonSelectionMode mode) {
    std::vector<ComponentPoint> result;
    if(mode==PolygonSelectionMode::Edge)
        for(const auto e:polygon_edges(mesh)) result.push_back({e.key(),multiply(add(mesh.vertices[e.a].position,mesh.vertices[e.b].position),0.5F)});
    else if(mode==PolygonSelectionMode::Vertex)
        for(const auto vertex:polygon_all_components(mesh,mode)) result.push_back({vertex,mesh.vertices[std::size_t(vertex)].position});
    else for(std::uint32_t face=0;face<mesh.indices.size()/3;++face) {
        Float3 p{};
        for(int j=0;j<3;++j) p=add(p,mesh.vertices[mesh.indices[std::size_t(face)*3+std::size_t(j)]].position);
        result.push_back({face,multiply(p,1.0F/3)});
    }
    return result;
}
bool visible_point(const EditorObject& object,const EditorCamera& camera,UiRect viewport,Float3 world) {
    const auto projected=project_world_to_screen(camera,viewport,world);
    if(!projected.visible) return false;
    const auto ray=make_viewport_ray(camera,viewport,projected.x,projected.y);
    const float distance=length(subtract(world,ray.origin));
    const auto hit=pick_editor_polygon(object,ray,distance+0.001F,true);
    return !hit||hit->distance>=distance-0.002F;
}
}
std::optional<std::uint64_t> pick_polygon_component(const EditorObject& object,const EditorCamera& camera,
    UiRect viewport,int x,int y,PolygonSelectionMode mode,bool through) {
    if(!object.polygon) return std::nullopt;
    if(mode==PolygonSelectionMode::Face) {
        const auto hit=pick_editor_polygon(object,make_viewport_ray(camera,viewport,float(x),float(y)),10000,true);
        return hit?std::optional<std::uint64_t>(hit->triangleIndex):std::nullopt;
    }
    float best=100; std::optional<std::uint64_t> result; const auto& mesh=*object.polygon;
    const auto controlEdges=object.subdivision && mode==PolygonSelectionMode::Edge ? subdivision_control_edges(mesh) : std::set<std::uint64_t>{};
    if(mode==PolygonSelectionMode::Vertex) for(const auto item:component_points(mesh,mode)) {
        const auto world=voxel_to_world(object.transform,object.voxelScale,item.point);
        const auto p=project_world_to_screen(camera,viewport,world);
        if(!p.visible||(!through&&!visible_point(object,camera,viewport,world))) continue;
        const float distance=(p.x-float(x))*(p.x-float(x))+(p.y-float(y))*(p.y-float(y));
        if(distance<best) { best=distance; result=item.key; }
    } else for(const auto e:polygon_edges(mesh)) {
        if (object.subdivision && !controlEdges.contains(e.key())) continue;
        const auto a=project_world_to_screen(camera,viewport,voxel_to_world(object.transform,object.voxelScale,mesh.vertices[e.a].position));
        const auto b=project_world_to_screen(camera,viewport,voxel_to_world(object.transform,object.voxelScale,mesh.vertices[e.b].position));
        if(!a.visible||!b.visible) continue;
        const float dx=b.x-a.x,dy=b.y-a.y;
        const float t=std::clamp(((float(x)-a.x)*dx+(float(y)-a.y)*dy)/std::max(0.001F,dx*dx+dy*dy),0.0F,1.0F);
        const float ex=float(x)-a.x-t*dx,ey=float(y)-a.y-t*dy;
        const float distance=ex*ex+ey*ey;
        const auto world=voxel_to_world(object.transform,object.voxelScale,
            add(mesh.vertices[e.a].position,multiply(subtract(mesh.vertices[e.b].position,mesh.vertices[e.a].position),t)));
        if(distance<best&&(through||visible_point(object,camera,viewport,world))) { best=distance; result=e.key(); }
    }
    return result;
}
std::set<std::uint64_t> polygon_components_in_rect(const EditorObject& object,const EditorCamera& camera,
    UiRect viewport,UiRect rect,PolygonSelectionMode mode,bool through,bool crossing,
    const EditorDocument* scene,const std::set<EditorObjectId>* onlyObjects) {
    std::set<std::uint64_t> result;
    if (!object.polygon || rect.width <= 0 || rect.height <= 0 ||
        viewport.width <= 0 || viewport.height <= 0) return result;
    const auto& mesh = *object.polygon;
    const auto basis = camera_basis(camera);
    const double near = std::max(1e-6,double(camera.nearPlane)), far = camera.farPlane;
    if (!(far > near) || !std::isfinite(far)) return result;
    const double aspect = double(viewport.width)/viewport.height;
    const bool orthographic = camera.projection == EditorProjection::Orthographic;
    const double halfHeight = std::max(.001,double(camera.orthographicHeight)*.5);
    const double tangent = std::tan(double(std::clamp(
        editor_camera_vertical_fov(camera,float(aspect)),.1F,3.14159265358979323846F-.1F))*.5);
    const double shiftX = camera.physicalLens.enabled ? double(camera.physicalLens.lensShiftX)*2 : 0;
    const double shiftY = camera.physicalLens.enabled ? double(camera.physicalLens.lensShiftY)*2 : 0;
    const double left = std::max(double(rect.x),double(viewport.x));
    const double top = std::max(double(rect.y),double(viewport.y));
    const double right = std::min(double(rect.x)+rect.width-1,double(viewport.x)+viewport.width-1);
    const double bottom = std::min(double(rect.y)+rect.height-1,double(viewport.y)+viewport.height-1);
    if (right < left || bottom < top) return result;

    struct Point { Float3 world; double vx{},vy{},depth{},x{},y{}; };
    const auto viewPoint = [&](Float3 world) {
        const double x = double(world.x)-camera.position.x;
        const double y = double(world.y)-camera.position.y;
        const double z = double(world.z)-camera.position.z;
        return Point{world,x*basis.right.x+y*basis.right.y+z*basis.right.z,
            x*basis.up.x+y*basis.up.y+z*basis.up.z,
            x*basis.forward.x+y*basis.forward.y+z*basis.forward.z,0,0};
    };
    const auto project = [&](Point point) {
        // Use depth validity separately from viewport bounds. Off-screen endpoints are
        // necessary to select long edges and large triangles crossing the marquee.
        const double nx = orthographic ? point.vx/(halfHeight*aspect)
            : point.vx/(point.depth*tangent*aspect)+shiftX;
        const double ny = orthographic ? point.vy/halfHeight
            : point.vy/(point.depth*tangent)+shiftY;
        point.x = viewport.x+(nx+1)*.5*viewport.width;
        point.y = viewport.y+(1-ny)*.5*viewport.height;
        return point;
    };
    const auto interpolate = [](const Point& a,const Point& b,double t) {
        return Point{{float(double(a.world.x)+(double(b.world.x)-a.world.x)*t),
                      float(double(a.world.y)+(double(b.world.y)-a.world.y)*t),
                      float(double(a.world.z)+(double(b.world.z)-a.world.z)*t)},
            a.vx+(b.vx-a.vx)*t,a.vy+(b.vy-a.vy)*t,a.depth+(b.depth-a.depth)*t,
            a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t};
    };
    const auto screenInterpolate = [&](const Point& a,const Point& b,double t) {
        // Perspective-correct world position at a screen-space edge intersection.
        const double worldT = orthographic ? t : (t/b.depth)/((1-t)/a.depth+t/b.depth);
        auto point = interpolate(a,b,worldT);
        point.x = a.x+(b.x-a.x)*t;
        point.y = a.y+(b.y-a.y)*t;
        return point;
    };
    const auto clip = [&](std::vector<Point> input,unsigned axis,double bound,bool greater,bool screen) {
        std::vector<Point> output;
        if (input.empty()) return output;
        const auto value = [&](const Point& point) { return axis==2 ? point.depth : axis==0 ? point.x : point.y; };
        const auto inside = [&](const Point& point) { return greater ? value(point)>=bound : value(point)<=bound; };
        const auto intersection = [&](const Point& a,const Point& b) {
            const double t = std::clamp((bound-value(a))/(value(b)-value(a)),0.0,1.0);
            auto point = screen ? screenInterpolate(a,b,t) : interpolate(a,b,t);
            if (axis==2) point.depth=bound;
            else if (axis==0) point.x=bound;
            else point.y=bound;
            return point;
        };
        if (input.size()==1) {
            if (inside(input[0])) output.push_back(input[0]);
        } else if (input.size()==2) {
            const bool aInside=inside(input[0]), bInside=inside(input[1]);
            if (aInside && bInside) return input;
            if (aInside) output={input[0],intersection(input[0],input[1])};
            else if (bInside) output={intersection(input[0],input[1]),input[1]};
        } else {
            auto previous=input.back();
            bool previousInside=inside(previous);
            for (const auto& current : input) {
                const bool currentInside=inside(current);
                if (currentInside != previousInside) output.push_back(intersection(previous,current));
                if (currentInside) output.push_back(current);
                previous=current;
                previousInside=currentInside;
            }
        }
        return output;
    };
    const auto visible = [&](const Point& point) {
        auto ray = make_viewport_ray(camera,viewport,float(point.x),float(point.y));
        const float cosine = dot(ray.direction,basis.forward);
        if (!(cosine > 0)) return false;
        // Geometry behind the near plane cannot occlude a visible clipped component.
        ray.origin = add(ray.origin,multiply(ray.direction,float(near)/cosine));
        const float distance = dot(subtract(point.world,ray.origin),ray.direction);
        if (!std::isfinite(distance)) return false;
        if (!(distance > .002F)) return true;
        if (scene) {
            const auto hit = pick_editor_document(*scene,ray,distance+.002F,onlyObjects);
            if (hit && hit->objectId==object.id && object.subdivision) {
                const auto cageHit=pick_editor_polygon(object,ray,distance+.002F,true);
                return !cageHit || cageHit->distance>=distance-.002F;
            }
            return !hit || hit->worldDistance >= distance-.002F;
        }
        const auto hit = pick_editor_polygon(object,ray,distance+.002F,true);
        return !hit || hit->distance >= distance-.002F;
    };
    for (const auto item : component_points(mesh,mode)) {
        std::vector<Point> patch;
        if (mode==PolygonSelectionMode::Edge) {
            patch.push_back(viewPoint(voxel_to_world(object.transform,object.voxelScale,
                mesh.vertices[std::size_t(item.key>>32U)].position)));
            patch.push_back(viewPoint(voxel_to_world(object.transform,object.voxelScale,
                mesh.vertices[std::size_t(item.key&0xffffffffU)].position)));
        } else if (mode==PolygonSelectionMode::Face) {
            for (unsigned j=0;j<3;++j) patch.push_back(viewPoint(voxel_to_world(object.transform,object.voxelScale,
                mesh.vertices[mesh.indices[std::size_t(item.key)*3+j]].position)));
        } else patch.push_back(viewPoint(voxel_to_world(object.transform,object.voxelScale,item.point)));
        if (crossing) {
            patch=clip(std::move(patch),2,near,true,false);
            patch=clip(std::move(patch),2,far,false,false);
        } else if (std::any_of(patch.begin(),patch.end(),[&](const auto& point) {
            return point.depth < near || point.depth > far;
        })) continue;
        if (patch.empty()) continue;
        for (auto& point : patch) point=project(point);
        if (std::any_of(patch.begin(),patch.end(),[](const auto& point) {
            return !std::isfinite(point.x) || !std::isfinite(point.y);
        })) continue;
        if (!crossing && std::any_of(patch.begin(),patch.end(),[&](const auto& point) {
            return point.x < left || point.x > right || point.y < top || point.y > bottom;
        })) continue;
        if (crossing) {
            patch=clip(std::move(patch),0,left,true,true);
            patch=clip(std::move(patch),0,right,false,true);
            patch=clip(std::move(patch),1,top,true,true);
            patch=clip(std::move(patch),1,bottom,false,true);
        }
        if (patch.empty()) continue;
        if (through) { result.insert(item.key); continue; }
        auto center=patch.front();
        for (std::size_t i=1;i<patch.size();++i) center=interpolate(center,patch[i],1.0/double(i+1));
        center=project(center);
        bool seen=visible(center);
        for (std::size_t i=0;!seen && i<patch.size();++i) {
            seen=visible(patch[i]);
            if (!seen && patch.size()>1)
                seen=visible(screenInterpolate(patch[i],patch[(i+1)%patch.size()],.5));
        }
        if (seen) result.insert(item.key);
    }
    return result;
}
ReplacePolygonCommand::ReplacePolygonCommand(EditorObjectId id,CookedPolygonAsset before,CookedPolygonAsset after,
    std::string label,std::uint64_t stroke):
    id_(id),stroke_(stroke),before_(std::move(before)),after_(std::move(after)),label_(std::move(label)) {}
CommandResult ReplacePolygonCommand::apply(EditorDocument& document,const CookedPolygonAsset& mesh) {
    auto* object=document.find_object(id_);
    if(!object||!object->polygon) return CommandResult::fail("Polygon target no longer exists");
    if(object->flags.locked) return CommandResult::fail("Polygon target is locked");
    const auto valid=validate_polygon_asset(mesh); if(!valid) return CommandResult::fail(valid.message);
    if (object->subdivision) {
        const auto evaluated=evaluate_subdivision(mesh,*object->subdivision);
        if (!evaluated.mesh) return CommandResult::fail(evaluated.error);
    }
    object->subdivisionCache.reset();
    object->polygon=mesh; object->polygon->objectId=id_; refresh_editor_polygon(*object->polygon);
    document.mark_dirty(); return CommandResult::ok();
}
CommandResult ReplacePolygonCommand::execute(EditorDocument& document) {
    auto* object=document.find_object(id_);
    if(!capturedOverride_) {
        if(object&&object->prefabLink) {
            const auto it=object->prefabLink->overrides.find("polygon.geometry");
            if(it!=object->prefabLink->overrides.end()) previousOverride_=it->second;
        }
        capturedOverride_=true;
    }
    const auto result=apply(document,after_);
    if(result.success&&object->prefabLink) object->prefabLink->overrides["polygon.geometry"]=std::int64_t{1};
    return result;
}
CommandResult ReplacePolygonCommand::undo(EditorDocument& document) {
    const auto result=apply(document,before_);
    if(result.success) if(auto* object=document.find_object(id_);object->prefabLink) {
        if(previousOverride_) object->prefabLink->overrides["polygon.geometry"]=*previousOverride_;
        else object->prefabLink->overrides.erase("polygon.geometry");
    }
    return result;
}
bool ReplacePolygonCommand::merge_with(const IEditorCommand& other) {
    const auto* next=dynamic_cast<const ReplacePolygonCommand*>(&other);
    if(!next||stroke_==0||stroke_!=next->stroke_||id_!=next->id_||label_!=next->label_) return false;
    after_=next->after_; return true;
}

std::vector<EditorPolygonDrawTriangle> build_editor_polygon_draw_list(
    const EditorDocument& document,const EditorCamera& camera,UiRect viewport,
    const std::set<EditorObjectId>& selected,const std::set<EditorObjectId>& isolated,std::size_t maximum,
    const std::map<EditorObjectId,RigidTransform>* poses) {
    std::vector<EditorPolygonDrawTriangle> result;
    for(const auto& [id,object]:document.objects()) {
        if(!object.polygon||!object.flags.visible||(!isolated.empty()&&!isolated.contains(id))) continue;
        const auto& mesh=*editor_surface_mesh(object);const auto materials=face_materials(mesh);
        auto pose=object.transform;
        if(poses) if(const auto it=poses->find(id);it!=poses->end()) pose=it->second;
        for(std::uint32_t face=0;face<mesh.indices.size()/3&&result.size()<maximum;++face) {
            std::array<Float3,3> world{};std::array<float,3> depth{};bool finite=true;
            for(int j=0;j<3;++j) {
                world[std::size_t(j)]=voxel_to_world(pose,object.voxelScale,
                    mesh.vertices[mesh.indices[std::size_t(face)*3+std::size_t(j)]].position);
                depth[std::size_t(j)]=project_world_to_screen(camera,viewport,world[std::size_t(j)]).depth;
                finite=finite&&finite3(world[std::size_t(j)])&&std::isfinite(depth[std::size_t(j)]);
            }
            if(!finite) continue;
            // Clip against the near plane (view depth is affine in world position), so a large
            // surface that passes behind the camera keeps its visible part. Off-screen points in
            // front of the camera are kept; the canvas clips them.
            const float nearDepth=camera.nearPlane+std::max(1e-4F,camera.nearPlane*1e-3F);
            std::array<Float3,4> clipped{};std::size_t count=0;
            for(std::size_t j=0;j<3;++j) {
                const std::size_t k=(j+1)%3;
                const bool inA=depth[j]>=nearDepth,inB=depth[k]>=nearDepth;
                if(inA) clipped[count++]=world[j];
                if(inA!=inB) {
                    const float t=(nearDepth-depth[j])/(depth[k]-depth[j]);
                    clipped[count++]=add(world[j],multiply(subtract(world[k],world[j]),t));
                }
            }
            if(count<3) continue;
            std::array<ScreenPoint,4> screen{};bool usable=true;float depthSum=0;
            for(std::size_t j=0;j<count;++j) {
                screen[j]=project_world_to_screen(camera,viewport,clipped[j]);
                usable=usable&&screen[j].depth>camera.nearPlane&&screen[j].depth<=camera.farPlane&&
                    std::isfinite(screen[j].x)&&std::isfinite(screen[j].y);
                depthSum+=screen[j].depth;
            }
            if(!usable) continue;
            const auto normal=normalize(cross3(subtract(world[1],world[0]),subtract(world[2],world[0])));
            const auto centre=multiply(add(add(world[0],world[1]),world[2]),1.0F/3.0F);
            const float facing=std::abs(dot(normal,normalize(subtract(camera.position,centre))));
            for(std::size_t j=1;j+1<count&&result.size()<maximum;++j) {
                EditorPolygonDrawTriangle triangle;triangle.object=id;triangle.face=subdivision_control_triangle(object,face);
                triangle.points={screen[0],screen[j],screen[j+1]};
                triangle.depth=depthSum/float(count);
                triangle.facing=std::isfinite(facing)?std::clamp(facing,0.0F,1.0F):1.0F;
                triangle.selected=selected.contains(id);triangle.color=mesh.materials[materials[face]].baseColor;
                result.push_back(triangle);
            }
        }
    }
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.depth>b.depth;});
    return result;
}
} // namespace dve::editor
