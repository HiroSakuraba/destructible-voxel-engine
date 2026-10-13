#include "dve/editor_mesh_authoring.hpp"
#include "dve/editor_subdivision.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <tuple>
#ifdef DVE_HAVE_MESH_AUTHORING
#include <manifold/manifold.h>
#endif
namespace dve::editor {
bool mesh_boolean_available() noexcept {
#ifdef DVE_HAVE_MESH_AUTHORING
    return true;
#else
    return false;
#endif
}
#ifdef DVE_HAVE_MESH_AUTHORING
namespace {
manifold::MeshGL64 manifold_mesh(const CookedPolygonAsset& source,std::size_t first,std::size_t count,std::uint32_t original) {
    manifold::MeshGL64 mesh; mesh.numProp=18;
    for (const auto& v:source.vertices) {
        for (float x:{v.position.x,v.position.y,v.position.z,v.normal.x,v.normal.y,v.normal.z,
            v.texcoord.x,v.texcoord.y,v.texcoord1.x,v.texcoord1.y,v.color.x,v.color.y,v.color.z,v.color.w,
            v.tangent.x,v.tangent.y,v.tangent.z,v.tangent.w}) mesh.vertProperties.push_back(x);
    }
    mesh.triVerts.assign(source.indices.begin()+std::ptrdiff_t(first),source.indices.begin()+std::ptrdiff_t(first+count));
    std::map<std::tuple<float,float,float>,std::uint64_t> canonical;
    std::set<std::uint64_t> used(mesh.triVerts.begin(),mesh.triVerts.end());
    for (auto i:used) {
        const auto p=source.vertices[std::size_t(i)].position;
        const auto [it,added]=canonical.emplace(std::tuple{p.x,p.y,p.z},i);
        if (!added) { mesh.mergeFromVert.push_back(i); mesh.mergeToVert.push_back(it->second); }
    }
    mesh.runIndex={0,std::uint64_t(count)}; mesh.runOriginalID={original};
    for (std::size_t i=0;i<count/3;++i) mesh.faceID.push_back(first/3+i);
    return mesh;
}
void smooth_bevel_normals(CookedPolygonAsset& mesh,float angle) {
    const auto cross=[](Float3 a,Float3 b){return Float3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
    std::map<std::tuple<float,float,float>,std::uint32_t> points;
    std::vector<std::uint32_t> canonical(mesh.vertices.size());
    for (std::uint32_t i=0;i<mesh.vertices.size();++i) {
        const auto p=mesh.vertices[i].position;auto [at,inserted]=points.emplace(std::tuple{p.x,p.y,p.z},i);
        (void)inserted;canonical[i]=at->second;
    }
    const auto edgeKey=[](std::uint32_t a,std::uint32_t b){return (std::uint64_t(std::min(a,b))<<32U)|std::max(a,b);};
    const std::size_t count=mesh.indices.size()/3;
    std::vector<Float3> normals(count);std::map<std::uint64_t,std::vector<std::uint32_t>> edges;
    std::map<std::uint32_t,std::vector<std::uint32_t>> faces;
    for (std::uint32_t f=0;f<count;++f) {
        const auto a=mesh.vertices[mesh.indices[f*3]].position,b=mesh.vertices[mesh.indices[f*3+1]].position,c=mesh.vertices[mesh.indices[f*3+2]].position;
        normals[f]=cross(subtract(b,a),subtract(c,a));
        for (unsigned j=0;j<3;++j) {
            const auto v=canonical[mesh.indices[f*3+j]],next=canonical[mesh.indices[f*3+(j+1)%3]];
            faces[v].push_back(f);edges[edgeKey(v,next)].push_back(f);
        }
    }
    const float threshold=std::cos(angle*3.14159265358979323846F/180);
    std::map<std::pair<std::uint32_t,std::uint32_t>,Float3> fanNormals;
    for (const auto& [v,incident] : faces) {
        std::set<std::uint32_t> remaining(incident.begin(),incident.end());
        while (!remaining.empty()) {
            std::vector<std::uint32_t> pending{*remaining.begin()},fan;remaining.erase(pending.back());Float3 sum{};
            while (!pending.empty()) {
                const auto f=pending.back();pending.pop_back();fan.push_back(f);sum=add(sum,normals[f]);
                if (angle==0) continue;
                for (unsigned j=0;j<3;++j) {
                    const auto a=canonical[mesh.indices[f*3+j]],b=canonical[mesh.indices[f*3+(j+1)%3]];
                    if (a!=v && b!=v) continue;
                    for (auto next : edges.at(edgeKey(a,b))) if (remaining.contains(next) &&
                        dot(normalize(normals[f]),normalize(normals[next]))>=threshold-1e-6F) {
                        remaining.erase(next);pending.push_back(next);
                    }
                }
            }
            for (auto f : fan) fanNormals[{v,f}]=normalize(sum);
        }
    }
    // Each triangle corner can belong to a different normal fan; keep UV/color domains intact.
    auto old=std::move(mesh.vertices);mesh.vertices.clear();mesh.vertices.reserve(mesh.indices.size());
    for (std::uint32_t f=0;f<count;++f) for (unsigned j=0;j<3;++j) {
        const auto original=mesh.indices[f*3+j];auto vertex=old[original];vertex.normal=fanNormals.at({canonical[original],f});
        mesh.indices[f*3+j]=std::uint32_t(mesh.vertices.size());mesh.vertices.push_back(vertex);
    }
}
PolygonEditResult fail(std::string message) { PolygonEditResult r; r.error=std::move(message); return r; }
}
#endif
PolygonEditResult boolean_editor_polygons(const EditorDocument& document,EditorObjectId a,EditorObjectId b,MeshBoolean operation) {
#ifndef DVE_HAVE_MESH_AUTHORING
    (void)document;(void)a;(void)b;(void)operation;
    PolygonEditResult r; r.error="Mesh Booleans require the Manifold authoring backend in this build";return r;
#else
    if (a==b) return fail("Choose two different polygon operands");
    const auto* first=document.find_object(a); const auto* second=document.find_object(b);
    if (!first||!second||!first->polygon||!second->polygon) return fail("Select two owned polygon meshes");
    for (const auto* object:{first,second}) {
        if (!object->flags.visible||object->flags.locked) return fail("Boolean operands must be visible and unlocked");
        const auto d=diagnose_polygon_topology(*editor_surface_mesh(*object));
        if (d.boundaries||d.nonmanifold||d.degenerate||d.inconsistentWinding)
            return fail(object->name+": Boolean input must be closed, consistently wound and manifold; run Topology Diagnostics");
    }
    try {
        const std::array<EditorObjectId,2> ids{a,b}; auto result=join_editor_polygons(document,a,ids,true);
        if (!result) return result;
        const auto count=editor_surface_mesh(*first)->indices.size(); const auto original=manifold::Manifold::ReserveIDs(2);
        const manifold::Manifold A(manifold_mesh(result.mesh,0,count,original));
        const manifold::Manifold B(manifold_mesh(result.mesh,count,editor_surface_mesh(*second)->indices.size(),original+1));
        if (A.Status()!=manifold::Manifold::Error::NoError||B.Status()!=manifold::Manifold::Error::NoError)
            return fail("Manifold rejected the input topology");
        if (!(A.Volume()>0)||!(B.Volume()>0)) return fail("Boolean operands need outward winding and positive enclosed volume");
        const auto solid=operation==MeshBoolean::Union?A+B:operation==MeshBoolean::Difference?A-B:
            operation==MeshBoolean::ReverseDifference?B-A:A^B;
        if (solid.Status()!=manifold::Manifold::Error::NoError) return fail("Manifold could not produce a valid Boolean solid");
        if (solid.IsEmpty()) return fail("Boolean result is empty; no geometry was changed");
        const auto output=solid.GetMeshGL64(3);
        if (output.NumVert()>1000000||output.triVerts.size()>6000000) return fail("Boolean result exceeds the editor geometry budget");
        std::vector<std::uint32_t> materials(result.mesh.indices.size()/3);
        for (const auto& s:result.mesh.submeshes) for (std::size_t f=s.firstIndex/3;f<(s.firstIndex+s.indexCount)/3;++f) materials[f]=s.materialIndex;
        result.mesh.vertices.clear(); result.mesh.indices.clear(); result.mesh.submeshes.clear();
        for (std::uint64_t i=0;i<output.NumVert();++i) {
            const auto k=std::size_t(i*output.numProp); const auto& p=output.vertProperties;
            PolygonVertex v;v.position={float(p[k]),float(p[k+1]),float(p[k+2])};v.normal={float(p[k+3]),float(p[k+4]),float(p[k+5])};
            v.texcoord={float(p[k+6]),float(p[k+7])};v.texcoord1={float(p[k+8]),float(p[k+9])};
            v.color={float(p[k+10]),float(p[k+11]),float(p[k+12]),float(p[k+13])};
            result.mesh.vertices.push_back(v);
        }
        for (auto i:output.triVerts) result.mesh.indices.push_back(std::uint32_t(i));
        if (output.faceID.size()!=output.NumTri()) return fail("Boolean result lost face provenance");
        for (std::size_t f=0;f<output.NumTri();++f) {
            if (output.faceID[f]>=materials.size()) return fail("Boolean result has unknown material provenance");
            const auto material=materials[std::size_t(output.faceID[f])];
            if (result.mesh.submeshes.empty()||result.mesh.submeshes.back().materialIndex!=material)
                result.mesh.submeshes.push_back({"Boolean surface",std::uint32_t(f*3),3,material});
            else result.mesh.submeshes.back().indexCount+=3;
        }
        refresh_editor_polygon(result.mesh);
        const auto valid=validate_polygon_asset(result.mesh);if (!valid) return fail(valid.message);
        result.selection={a,PolygonSelectionMode::Face,polygon_all_components(result.mesh,PolygonSelectionMode::Face)};
        return result;
    } catch (const std::exception& e) { return fail(std::string("Boolean failed: ")+e.what()); }
#endif
}
PolygonEditResult bevel_convex_editor_edges(const CookedPolygonAsset& source,const PolygonSelection& selection,const EdgeBevelSettings& settings,Float3 objectScale){
#ifndef DVE_HAVE_MESH_AUTHORING
    (void)source;(void)selection;(void)settings;(void)objectScale;PolygonEditResult r;r.error="Edge bevel requires Manifold";return r;
#else
    if (!validate_polygon_asset(source) || selection.mode!=PolygonSelectionMode::Edge || selection.components.empty() ||
        !polygon_selection_valid(source,selection)) return fail("Select valid convex crease edges");
    if (!std::isfinite(settings.width) || !(settings.width>0) ||
        (settings.offsetType==BevelOffsetType::Fractional && settings.width>1) ||
        settings.segments<1 || settings.segments>16 || !std::isfinite(settings.depth) || settings.depth<0 || settings.depth>1 ||
        !std::isfinite(settings.smoothingAngle) || settings.smoothingAngle<0 || settings.smoothingAngle>180 ||
        (settings.offsetType!=BevelOffsetType::Fractional && settings.offsetType!=BevelOffsetType::Absolute))
        return fail("Width must be positive (fraction <=1), Segments 1..16, Depth 0..1 and Smoothing Angle 0..180");
    if (source.indices.size()/3>100000 || source.vertices.size()>200000 ||
        selection.components.size()>256/settings.segments) return fail("Split the selection: bevel supports 256 profile planes and 100000 source triangles");
    const bool world=settings.offsetType==BevelOffsetType::Absolute && settings.worldSpace;
    if (world && (!std::isfinite(objectScale.x) || !std::isfinite(objectScale.y) || !std::isfinite(objectScale.z) ||
        objectScale.x<=1e-8F || objectScale.y<=1e-8F || objectScale.z<=1e-8F)) return fail("World offset requires a finite positive object scale");
    auto working=source;
    if (world) for (auto& vertex : working.vertices) {
        vertex.position=multiply_components(vertex.position,objectScale);
        if (!std::isfinite(vertex.position.x) || !std::isfinite(vertex.position.y) || !std::isfinite(vertex.position.z))
            return fail("World-space bevel coordinates overflow; reduce object scale");
    }
    const auto& input=working;
    auto diagnostics=diagnose_polygon_topology(source);if(diagnostics.boundaries||diagnostics.nonmanifold||diagnostics.degenerate||diagnostics.inconsistentWinding)return fail("Edge bevel needs a closed manifold solid");
    try{
        const auto original=manifold::Manifold::ReserveIDs(1);manifold::Manifold solid(manifold_mesh(input,0,input.indices.size(),original));
        const auto sourceVolume=solid.Volume();if(solid.Status()!=manifold::Manifold::Error::NoError||!std::isfinite(sourceVolume)||!(sourceVolume>0))return fail("Invalid bevel solid");
        if(std::abs(solid.Hull().Volume()-sourceVolume)>std::max(1e-8,sourceVolume*1e-6))return fail("This bevel tool supports convex solids; concave edge reconstruction is separate work");
        auto cross=[](Float3 a,Float3 b){return Float3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
        std::map<std::tuple<float,float,float>,std::uint32_t> points;std::vector<std::uint32_t> canonical;for(std::uint32_t i=0;i<input.vertices.size();++i){auto p=input.vertices[i].position;auto [it,added]=points.emplace(std::tuple{p.x,p.y,p.z},i);(void)added;canonical.push_back(it->second);}
        std::map<std::uint64_t,std::vector<Float3>> edgeNormals;
        for(std::size_t f=0;f<input.indices.size()/3;++f){auto a=input.vertices[input.indices[f*3]].position,b=input.vertices[input.indices[f*3+1]].position,c=input.vertices[input.indices[f*3+2]].position;auto normal=normalize(cross(subtract(b,a),subtract(c,a)));
            for(unsigned j=0;j<3;++j){auto x=canonical[source.indices[f*3+j]],y=canonical[source.indices[f*3+(j+1)%3]];auto k=(std::uint64_t(std::min(x,y))<<32U)|std::max(x,y);edgeNormals[k].push_back(normal);}}
        float shortest=std::numeric_limits<float>::infinity(),minSine=1;
        for (const auto& [key,normals] : edgeNormals) if (normals.size()==2 && dot(normals[0],normals[1])<.99999F) {
            const auto a=input.vertices[std::size_t(key>>32U)].position,b=input.vertices[std::size_t(key&0xffffffffU)].position;
            shortest=std::min(shortest,length(subtract(a,b)));
        }
        for (auto key : selection.components) {
            const auto& normals=edgeNormals.at(key);
            if (normals.size()!=2 || dot(normals[0],normals[1])>.99999F) return fail("Select a crease edge, not a coplanar triangle diagonal");
            const float cosine=std::clamp(dot(normals[0],normals[1]),-1.0F,1.0F);
            if (cosine<-.9999F) return fail("Edge normals are opposed");
            minSine=std::min(minSine,std::sqrt((1-cosine)*.5F));
        }
        // Conservative common width protects short features; this is not Maya's maximum-fit solver.
        const float safeWidth=shortest*.49F*minSine;
        if (!std::isfinite(safeWidth) || !(safeWidth>0)) return fail("Cannot determine a safe bevel width");
        float width=settings.offsetType==BevelOffsetType::Fractional ? settings.width*safeWidth : settings.width;
        if (settings.autoFit) width=std::min(width,safeWidth);
        for (auto key : selection.components) {
            const auto& normals=edgeNormals.at(key);
            const auto n1=normals[0],n2=normals[1];
            const float cosine=std::clamp(dot(n1,n2),-1.0F,1.0F),angle=std::acos(cosine);
            const float c=std::cos(angle*.5F),sinHalf=std::sin(angle*.5F);
            const auto bisector=normalize(add(n1,n2));
            const auto a=input.vertices[std::size_t(key>>32U)].position,b=input.vertices[std::size_t(key&0xffffffffU)].position;
            const auto origin=multiply(add(a,b),.5F),direction=normalize(subtract(b,a));
            const float radius=width*c/(sinHalf*sinHalf);
            const auto centre=subtract(origin,multiply(bisector,radius/c));
            const auto first=add(centre,multiply(n1,radius)),last=add(centre,multiply(n2,radius));
            const auto point=[&](float t) {
                const auto arcNormal=multiply(add(multiply(n1,std::sin((1-t)*angle)),multiply(n2,std::sin(t*angle))),1/std::sin(angle));
                const auto arc=add(centre,multiply(arcNormal,radius));
                const auto flat=add(first,multiply(subtract(last,first),t));
                return add(flat,multiply(subtract(arc,flat),settings.depth));
            };
            const auto segments=settings.depth==0?1U:settings.segments;
            auto previous=point(0);
            for (std::uint32_t i=1;i<=segments;++i) {
                const auto next=point(float(i)/float(segments));
                auto normal=normalize(cross(direction,subtract(next,previous)));
                if (dot(normal,bisector)<0) normal=multiply(normal,-1);
                if (length_squared(normal)<.5F) return fail("Bevel profile collapsed; reduce width or segments");
                const double distance=dot(normal,multiply(add(previous,next),.5F));
                solid=solid.TrimByPlane({-normal.x,-normal.y,-normal.z},-distance);
                if (solid.IsEmpty() || solid.Status()!=manifold::Manifold::Error::NoError) return fail("Bevel removes the solid; reduce Width or enable Auto Fit");
                previous=next;
            }
        }
        if(solid.IsEmpty()||solid.Status()!=manifold::Manifold::Error::NoError)return fail("Bevel removes the whole solid; reduce the offset");
        const auto output=solid.GetMeshGL64(3);if(output.NumVert()>1000000||output.triVerts.size()>1000000||output.numProp<18)return fail("Bevel output exceeds the one-million-corner budget or lost attributes");
        if (output.faceID.size()!=output.NumTri() || output.runOriginalID.empty() ||
            output.runIndex.size()!=output.runOriginalID.size()+1) return fail("Bevel output lost surface provenance");
        PolygonEditResult result{source,selection,{}};auto& mesh=result.mesh;std::vector<std::uint32_t> materials(source.indices.size()/3);for(const auto& s:source.submeshes)for(std::size_t f=s.firstIndex/3;f<(s.firstIndex+s.indexCount)/3;++f)materials[f]=s.materialIndex;
        auto capMaterial=std::uint32_t(mesh.materials.size());mesh.materials.push_back(source.materials[0]);mesh.materialBindings.resize(source.materials.size());mesh.materialBindings.push_back(source.materialBindings.empty()?PolygonMaterialBinding{}:source.materialBindings[0]);mesh.vertices.clear();mesh.indices.clear();mesh.submeshes.clear();
        for(std::size_t i=0;i<output.NumVert();++i){auto k=i*std::size_t(output.numProp);auto& p=output.vertProperties;PolygonVertex v;v.position={float(p[k]),float(p[k+1]),float(p[k+2])};v.normal={float(p[k+3]),float(p[k+4]),float(p[k+5])};v.texcoord={float(p[k+6]),float(p[k+7])};v.texcoord1={float(p[k+8]),float(p[k+9])};v.color={float(p[k+10]),float(p[k+11]),float(p[k+12]),float(p[k+13])};mesh.vertices.push_back(v);}
        result.selection={source.objectId,PolygonSelectionMode::Face,{}};std::size_t run=0;
        for(std::size_t f=0;f<output.NumTri();++f){while(run+1<output.runOriginalID.size()&&f*3>=output.runIndex[run+1])++run;bool cap=output.runOriginalID[run]!=original;auto material=cap?capMaterial:output.faceID[f]<materials.size()?materials[std::size_t(output.faceID[f])]:0;
            std::array<std::uint32_t,3> ids;for(unsigned j=0;j<3;++j)ids[j]=std::uint32_t(output.triVerts[f*3+j]);
            if(cap){auto a=mesh.vertices[ids[0]].position,b=mesh.vertices[ids[1]].position,c=mesh.vertices[ids[2]].position;auto normal=normalize(cross(subtract(b,a),subtract(c,a)));for(auto& id:ids){auto v=mesh.vertices[id];v.normal=normal;v.color={1,1,1,1};v.texcoord=std::abs(normal.x)>std::abs(normal.y)&&std::abs(normal.x)>std::abs(normal.z)?Float2{v.position.z,v.position.y}:std::abs(normal.y)>std::abs(normal.z)?Float2{v.position.x,v.position.z}:Float2{v.position.x,v.position.y};v.texcoord1=v.texcoord;id=std::uint32_t(mesh.vertices.size());mesh.vertices.push_back(v);}result.selection.components.insert(f);}
            mesh.indices.insert(mesh.indices.end(),ids.begin(),ids.end());if(mesh.submeshes.empty()||mesh.submeshes.back().materialIndex!=material)mesh.submeshes.push_back({"Bevel surface",std::uint32_t(f*3),3,material});else mesh.submeshes.back().indexCount+=3;}
        if (world) for (auto& vertex : mesh.vertices)
            vertex.position={vertex.position.x/objectScale.x,vertex.position.y/objectScale.y,vertex.position.z/objectScale.z};
        smooth_bevel_normals(mesh,settings.smoothingAngle);
        refresh_editor_polygon(mesh);auto valid=validate_polygon_asset(mesh);if(!valid)return fail(valid.message);
        const auto topology=diagnose_polygon_topology(mesh);
        if (topology.boundaries || topology.nonmanifold || topology.degenerate || topology.inconsistentWinding)
            return fail("Bevel output has invalid topology; reduce width or segments");
        return result;
    }catch(const std::exception& e){return fail(e.what());}
#endif
}
PolygonEditResult bevel_convex_editor_edges(const CookedPolygonAsset& source,const PolygonSelection& selection,float width) {
    EdgeBevelSettings settings;settings.width=width;settings.offsetType=BevelOffsetType::Absolute;
    settings.worldSpace=false;settings.depth=0;settings.autoFit=false;settings.smoothingAngle=0;
    return bevel_convex_editor_edges(source,selection,settings);
}
PolygonEditResult convex_editor_hull(EditorObjectId id,std::span<const Float3> points) {
    PolygonEditResult r;
#ifndef DVE_HAVE_MESH_AUTHORING
    (void)id;(void)points;r.error="Convex hull construction requires the Manifold authoring backend";
#else
    if (points.size()<4||points.size()>1000000) {r.error="Hull fit needs 4 to 1,000,000 finite source points";return r;}
    std::vector<manifold::vec3> p;for (auto v:points) {
        if (!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z)) {r.error="Hull point must be finite";return r;}
        p.emplace_back(v.x,v.y,v.z);
    }
    try {
        auto hull=manifold::Manifold::Hull(p);
        if (hull.Status()!=manifold::Manifold::Error::NoError||hull.IsEmpty()||!(hull.Volume()>1e-12)) {r.error="Hull points must span a three-dimensional volume";return r;}
        const auto output=hull.GetMeshGL64();
        if(output.NumVert()>256){r.error="Hull exceeds 256 surface vertices; simplify the source first";return r;}
        r.mesh=make_editor_polygon_cube(id);r.mesh.vertices.clear();r.mesh.indices.clear();
        for (std::size_t i=0;i<output.NumVert();++i) {PolygonVertex v;v.position={float(output.vertProperties[i*3]),float(output.vertProperties[i*3+1]),float(output.vertProperties[i*3+2])};r.mesh.vertices.push_back(v);}
        for (auto i:output.triVerts) r.mesh.indices.push_back(std::uint32_t(i));
        r.mesh.submeshes={{"Collision hull",0,std::uint32_t(r.mesh.indices.size()),0}};refresh_editor_polygon(r.mesh,true);
    } catch(const std::exception& e) {r.error=e.what();}
#endif
    return r;
}
} // namespace dve::editor
