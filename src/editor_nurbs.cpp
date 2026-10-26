#include "dve/editor_nurbs.hpp"
#include "dve/editor_nurbs_seam.hpp"
#include "dve/editor_nurbs_surface.hpp"
#include <algorithm>
namespace dve::editor {
bool has_nurbs_source(const EditorObject& o) noexcept {return find_component_by_type(o.components,kNurbsComponent)!=nullptr;}
std::optional<NurbsSource> editor_nurbs_source(const EditorObject& o,std::string* e){
    const auto* c=find_component_by_type(o.components,kNurbsComponent);
    if(!c||!c->enabled){if(e)*e="No enabled NURBS source";return {};}
    auto it=c->properties.find("source");
    if(it==c->properties.end()||!std::holds_alternative<std::string>(it->second)){if(e)*e="Missing NURBS source";return {};}
    return decode_nurbs(std::get<std::string>(it->second),e);
}
bool validate_editor_nurbs(const EditorObject& o,std::string* e,std::stop_token stop){
    if(!has_nurbs_source(o))return true;
    auto s=editor_nurbs_source(o,e);
    if(!s||!o.polygon||o.subdivision||o.modelingTopology||find_component_by_type(o.components,"dve.exact_brep")){
        if(e&&e->empty())*e="NURBS object has conflicting source geometry";
        return false;
    }
    auto m=tessellate_editor_nurbs_display(o,*s,e,stop);
    if(!m)return false;
    // Cache geometry must match source. Material edits are separate; geometric
    // bake mutations cannot be accepted as changes to a NURBS source.
    if(m->indices!=o.polygon->indices||m->vertices.size()!=o.polygon->vertices.size()){
        if(e)*e="NURBS display cache no longer matches source";
        return false;
    }
    for(std::size_t i=0;i<m->vertices.size();++i) {
        const auto& expected=m->vertices[i];const auto& cached=o.polygon->vertices[i];
        const auto same2=[](Float2 a,Float2 b){return a.x==b.x&&a.y==b.y;};
        const auto same3=[](Float3 a,Float3 b){return a.x==b.x&&a.y==b.y&&a.z==b.z;};
        const auto same4=[](Float4 a,Float4 b){return a.x==b.x&&a.y==b.y&&a.z==b.z&&a.w==b.w;};
        if(!same2(expected.texcoord,cached.texcoord)||!same2(expected.texcoord1,cached.texcoord1)||
           ((s->creaseNormals||s->adaptiveDisplay||has_nurbs_seam(o))&&(!same3(expected.normal,cached.normal)||!same4(expected.tangent,cached.tangent)))){
            if(e)*e="NURBS display normals or UVs diverged from source";
            return false;
        }
        auto a=expected.position,b=cached.position;
        if(a.x!=b.x||a.y!=b.y||a.z!=b.z){if(e)*e="NURBS display geometry diverged; bake a polygon before mesh editing";return false;}
    }
    return true;
}
std::optional<EditorObject> make_editor_nurbs(EditorObjectId id,const NurbsSource& s,std::string* e){
    auto m=tessellate_nurbs(s,id,e);if(!m)return {};
    EditorObject o(id,s.is_curve()?"NURBS Curve":"NURBS Surface");
    o.polygon=std::move(*m);o.voxelSizeMeters=1;o.flags.collisionEnabled=false;o.flags.anchored=true;
    Component c;c.id=1;c.type=std::string(kNurbsComponent);c.properties["source"]=encode_nurbs(s);
    o.components.push_back(std::move(c));return o;
}
bool replace_editor_nurbs_source(EditorObject& o,const NurbsSource& value,std::string* error){
    if(has_nurbs_seam(o)){if(error)*error="Use coordinated linked-seam edits or unlink first";return false;}
    if(!o.polygon||!has_nurbs_source(o)){if(error)*error="Missing native NURBS source";return false;}
    auto mesh=tessellate_editor_nurbs_display(o,value,error);if(!mesh)return false;
    // Keep object material and texture resources. The initial source has one
    // patch/material domain; tessellation output always references material 0.
    mesh->materials=o.polygon->materials;mesh->materialBindings=o.polygon->materialBindings;
    mesh->images=o.polygon->images;mesh->samplers=o.polygon->samplers;mesh->textures=o.polygon->textures;
    mesh->contentHash=polygon_asset_content_hash(*mesh);
    auto valid=validate_polygon_asset(*mesh);if(!valid){if(error)*error=valid.message;return false;};
    find_component_by_type(std::span<Component>(o.components),kNurbsComponent)->properties["source"]=encode_nurbs(value);
    o.polygon=std::move(*mesh);return true;
}
bool nurbs_display_geometry_matches(const CookedPolygonAsset& a,const CookedPolygonAsset& b){
    const auto same2=[](Float2 x,Float2 y){return x.x==y.x&&x.y==y.y;};
    const auto same3=[](Float3 x,Float3 y){return x.x==y.x&&x.y==y.y&&x.z==y.z;};
    const auto same4=[](Float4 x,Float4 y){return x.x==y.x&&x.y==y.y&&x.z==y.z&&x.w==y.w;};
    if(a.objectId!=b.objectId||a.indices!=b.indices||a.vertices.size()!=b.vertices.size()||a.submeshes.size()!=b.submeshes.size()||
       !same3(a.bounds.minimum,b.bounds.minimum)||!same3(a.bounds.maximum,b.bounds.maximum))return false;
    for(std::size_t i=0;i<a.vertices.size();++i){const auto& x=a.vertices[i];const auto& y=b.vertices[i];
        if(!same3(x.position,y.position)||!same3(x.normal,y.normal)||!same4(x.tangent,y.tangent)||
           !same2(x.texcoord,y.texcoord)||!same2(x.texcoord1,y.texcoord1)||!same4(x.color,y.color))return false;
    }
    for(std::size_t i=0;i<a.submeshes.size();++i){const auto& x=a.submeshes[i];const auto& y=b.submeshes[i];
        if(x.name!=y.name||x.firstIndex!=y.firstIndex||x.indexCount!=y.indexCount||x.materialIndex!=y.materialIndex)return false;
    }
    return true;
}
std::uint64_t nurbs_source_stamp(const NurbsSource& source){
    std::uint64_t hash=14695981039346656037ULL;
    for(unsigned char c:encode_nurbs(source)){hash^=c;hash*=1099511628211ULL;}
    return hash;
}
EditNurbsCommand::EditNurbsCommand(EditorObjectId id,NurbsSource b,NurbsSource a):id_(id),before_(std::move(b)),after_(std::move(a)){}
CommandResult EditNurbsCommand::apply(EditorDocument& d,const NurbsSource& expected,const NurbsSource& value){
    auto* o=d.find_object(id_);std::string error;
    if(!o||!o->polygon||o->flags.locked||!o->flags.visible||o->prefabLink)
        return CommandResult::fail("Select a visible unlocked NURBS source; detach prefab instances before editing");
    auto current=editor_nurbs_source(*o,&error);
    if(!current||*current!=expected)return CommandResult::fail("NURBS source changed; reopen the edit");
    if(o->subdivision||o->modelingTopology||find_component_by_type(std::span<const Component>(o->components),"dve.exact_brep"))return CommandResult::fail("Conflicting NURBS source geometry");
    if(beforeMesh_&&afterMesh_){
        const auto& expectedMesh=expected==before_?*beforeMesh_:*afterMesh_;
        // Materials may have been edited separately; geometry and source must still match.
        if(!nurbs_display_geometry_matches(expectedMesh,*o->polygon))
            return CommandResult::fail("NURBS display cache changed; reopen the edit");
        auto mesh=value==after_?*afterMesh_:*beforeMesh_;
        mesh.materials=o->polygon->materials;mesh.materialBindings=o->polygon->materialBindings;
        mesh.images=o->polygon->images;mesh.samplers=o->polygon->samplers;mesh.textures=o->polygon->textures;
        mesh.contentHash=polygon_asset_content_hash(mesh);
        auto valid=validate_polygon_asset(mesh);if(!valid)return CommandResult::fail(valid.message);
        find_component_by_type(std::span<Component>(o->components),kNurbsComponent)->properties["source"]=encode_nurbs(value);
        o->polygon=std::move(mesh);
    }else{
        if(!validate_editor_nurbs(*o,&error))return CommandResult::fail(error);
        if(!replace_editor_nurbs_source(*o,value,&error))return CommandResult::fail(error);
    }
    d.mark_dirty();return CommandResult::ok();
}
std::unique_ptr<EditNurbsCommand> EditNurbsCommand::prepare(const EditorObject& object,const NurbsSource& after,std::string* error,std::stop_token stop){
    if(stop.stop_requested())return {};
    if(has_nurbs_seam(object)){if(error)*error="Linked seams require a coordinated edit";return {};}
    auto before=editor_nurbs_source(object,error);
    if(!before||!object.polygon)return {};
    auto valid=validate_polygon_asset(*object.polygon);
    if(!valid){if(error)*error=valid.message;return {};}
    if(!validate_editor_nurbs(object,error,stop))return {};
    auto mesh=tessellate_editor_nurbs_display(object,after,error,stop);if(!mesh||stop.stop_requested())return {};
    auto command=std::make_unique<EditNurbsCommand>(object.id,*before,after);
    // History needs display geometry, not a second copy of embedded texture resources.
    CookedPolygonAsset beforeMesh;beforeMesh.objectId=object.polygon->objectId;
    beforeMesh.vertices=object.polygon->vertices;beforeMesh.indices=object.polygon->indices;
    beforeMesh.submeshes=object.polygon->submeshes;beforeMesh.bounds=object.polygon->bounds;
    command->beforeMesh_=std::move(beforeMesh);command->afterMesh_=std::move(mesh);return command;
}
bool EditNurbsCommand::matches_before(const EditorObject& o) const{
    if(o.id!=id_||!o.polygon||!beforeMesh_||o.subdivision||o.modelingTopology||find_component_by_type(o.components,"dve.exact_brep"))return false;
    const auto source=editor_nurbs_source(o);if(!source||*source!=before_)return false;
    return nurbs_display_geometry_matches(*beforeMesh_,*o.polygon);
}
CommandResult EditNurbsCommand::execute(EditorDocument& d){
    auto* object=d.find_object(id_);
    if(!seamEdit_&&object&&has_nurbs_seam(*object))seamEdit_=edit_nurbs_seam_command(id_,before_,after_);
    return seamEdit_?seamEdit_->execute(d):apply(d,before_,after_);
}
CommandResult EditNurbsCommand::undo(EditorDocument& d){return seamEdit_?seamEdit_->undo(d):apply(d,after_,before_);}
}

