#include "dve/editor_nurbs.hpp"
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
bool validate_editor_nurbs(const EditorObject& o,std::string* e){
    if(!has_nurbs_source(o))return true;
    auto s=editor_nurbs_source(o,e);
    if(!s||!o.polygon||o.subdivision||o.modelingTopology||find_component_by_type(o.components,"dve.exact_brep")){
        if(e&&e->empty())*e="NURBS object has conflicting source geometry";
        return false;
    }
    auto m=tessellate_nurbs(*s,o.id,e);
    if(!m)return false;
    // Cache geometry must match source. Material edits are separate; geometric
    // bake mutations cannot be accepted as changes to a NURBS source.
    if(m->indices!=o.polygon->indices||m->vertices.size()!=o.polygon->vertices.size()){
        if(e)*e="NURBS display cache no longer matches source";
        return false;
    }
    for(std::size_t i=0;i<m->vertices.size();++i) {
        auto a=m->vertices[i].position,b=o.polygon->vertices[i].position;
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
EditNurbsCommand::EditNurbsCommand(EditorObjectId id,NurbsSource b,NurbsSource a):id_(id),before_(std::move(b)),after_(std::move(a)){}
CommandResult EditNurbsCommand::apply(EditorDocument& d,const NurbsSource& expected,const NurbsSource& value){
    auto* o=d.find_object(id_);std::string error;
    if(!o||!o->polygon||o->flags.locked||!o->flags.visible||o->prefabLink)
        return CommandResult::fail("Select a visible unlocked NURBS source; detach prefab instances before editing");
    auto current=editor_nurbs_source(*o,&error);
    if(!current||*current!=expected)return CommandResult::fail("NURBS source changed; reopen the edit");
    if(!validate_editor_nurbs(*o,&error))return CommandResult::fail(error);
    auto mesh=tessellate_nurbs(value,id_,&error);if(!mesh)return CommandResult::fail(error);
    // Keep object material and texture resources. The initial source has one
    // patch/material domain; tessellation output always references material 0.
    mesh->materials=o->polygon->materials;mesh->materialBindings=o->polygon->materialBindings;
    mesh->images=o->polygon->images;mesh->samplers=o->polygon->samplers;mesh->textures=o->polygon->textures;
    mesh->contentHash=polygon_asset_content_hash(*mesh);
    auto valid=validate_polygon_asset(*mesh);if(!valid)return CommandResult::fail(valid.message);
    find_component_by_type(std::span<Component>(o->components),kNurbsComponent)->properties["source"]=encode_nurbs(value);
    o->polygon=std::move(*mesh);d.mark_dirty();return CommandResult::ok();
}
CommandResult EditNurbsCommand::execute(EditorDocument& d){return apply(d,before_,after_);}
CommandResult EditNurbsCommand::undo(EditorDocument& d){return apply(d,after_,before_);}
}

