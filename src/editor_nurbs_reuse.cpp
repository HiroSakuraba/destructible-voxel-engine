#include "dve/editor_nurbs_seam.hpp"
#include <algorithm>
namespace dve::editor {
namespace {bool fail(std::string* error,const std::string& text){if(error)*error=text;return false;}}
bool nurbs_network_selection_complete(const EditorDocument& document,const std::set<EditorObjectId>& selected,std::string* error){
    std::set<EditorObjectId> checked;
    for(auto id:selected){auto* object=document.find_object(id);if(!object)return fail(error,"Selected object no longer exists");if(!has_nurbs_seam(*object)||checked.contains(id))continue;
        auto ids=nurbs_seam_network_ids(document,id,error);if(ids.empty())return false;
        for(auto next:ids){checked.insert(next);if(!selected.contains(next))return fail(error,"Select the complete linked NURBS network, or its containing group, for placement/copy/prefab operations");}
    }
    return true;
}
bool validate_nurbs_network_placement(const EditorDocument& document,const std::map<EditorObjectId,RigidTransform>& poses,const std::map<EditorObjectId,Float3>& scales,const std::map<EditorObjectId,std::optional<EditorObjectId>>& parents,std::string* error){
    std::set<EditorObjectId> selected,checked;for(const auto& [id,v]:poses)selected.insert(id);for(const auto& [id,v]:scales)selected.insert(id);for(const auto& [id,v]:parents)selected.insert(id);
    if(!nurbs_network_selection_complete(document,selected,error))return false;
    if(!validate_editor_nurbs_seams(document,error))return false;
    for(auto id:selected){const auto* current=document.find_object(id);if(!has_nurbs_seam(*current)||checked.contains(id))continue;auto ids=nurbs_seam_network_ids(document,id,error);if(ids.empty())return false;
        std::optional<EditorObject> first;
        for(auto next:ids){checked.insert(next);const auto* object=document.find_object(next);if(object->flags.locked||!object->flags.visible)return fail(error,"Show and unlock every linked patch before changing placement");
            EditorObject proposed(next);proposed.transform=poses.contains(next)?poses.at(next):object->transform;proposed.voxelScale=scales.contains(next)?scales.at(next):object->voxelScale;proposed.parent=parents.contains(next)?parents.at(next):object->parent;proposed.attachment=object->attachment;
            if(!first)first=std::move(proposed);else if(!nurbs_seam_same_frame(*first,proposed))return fail(error,"Linked patches must retain a common parent, pose and axis scale; transform/reparent the complete network together");
        }
    }
    return true;
}
namespace {
class AddNetworkObjects final:public IEditorCommand {
public:
    AddNetworkObjects(std::vector<EditorObject> objects,std::string label):objects_(std::move(objects)),label_(std::move(label)){}
    std::string_view label() const noexcept override{return label_;}
    CommandResult execute(EditorDocument& document) override{
        if(objects_.empty())return CommandResult::fail("No objects to add");EditorDocument staged=clone_editor_document(document);std::set<EditorObjectId> ids;
        for(const auto& object:objects_){if(staged.find_object(object.id)||!ids.insert(object.id).second)return CommandResult::fail("Copied object identifier already exists");staged.add_object(clone_editor_object(object));}
        std::string error;if(!canonicalize_nurbs_network_corners(staged,ids,&error))return CommandResult::fail(error);
        for(auto id:ids)if(!validate_editor_nurbs(*staged.find_object(id),&error))return CommandResult::fail(error);
        if(!staged.validate(&error))return CommandResult::fail(error);
        // Cache the canonical, resource-preserving copies for stable redo.
        for(auto& object:objects_)object=clone_editor_object(*staged.find_object(object.id));staged.mark_dirty();document=std::move(staged);return CommandResult::ok();
    }
    CommandResult undo(EditorDocument& document) override{
        std::set<EditorObjectId> ids;for(const auto& object:objects_)ids.insert(object.id);std::string error;
        if(!nurbs_network_selection_complete(document,ids,&error))return CommandResult::fail(error);
        for(auto id:ids){auto* object=document.find_object(id);if(!object||object->flags.locked)return CommandResult::fail("Copied object missing or locked");for(auto child:document.children_of(id))if(!ids.contains(child))return CommandResult::fail("Copied object acquired an external child");}
        for(auto& object:objects_)object=clone_editor_object(*document.find_object(object.id));
        EditorDocument staged=clone_editor_document(document);for(auto id:ids)if(staged.find_object(id))(void)staged.remove_object(id);
        if(!staged.validate(&error))return CommandResult::fail(error);staged.mark_dirty();document=std::move(staged);return CommandResult::ok();
    }
private:std::vector<EditorObject> objects_;std::string label_;
};
}
std::unique_ptr<IEditorCommand> add_nurbs_network_objects_command(std::vector<EditorObject> objects,std::string label){return std::make_unique<AddNetworkObjects>(std::move(objects),std::move(label));}
}
