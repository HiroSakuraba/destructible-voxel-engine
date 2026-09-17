#include "dve/editor_command.hpp"

#include <algorithm>
#include <map>
#include <cmath>
#include <exception>
#include <set>

namespace dve::editor {
namespace {

bool valid_transform(const RigidTransform& transform) noexcept {
    const Quaternion q = transform.rotation;
    const float normSquared = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
    return std::isfinite(transform.position.x) && std::isfinite(transform.position.y) &&
           std::isfinite(transform.position.z) && std::isfinite(q.x) && std::isfinite(q.y) &&
           std::isfinite(q.z) && std::isfinite(q.w) && normSquared > 1.0e-12F;
}

} // namespace

CommandResult CommandResult::ok() { return {true, {}}; }
CommandResult CommandResult::fail(std::string message) { return {false, std::move(message)}; }

EditorCommandStack::EditorCommandStack(std::size_t maximumEntries)
    : maximumEntries_(std::max<std::size_t>(1, maximumEntries)) {}

CommandResult EditorCommandStack::execute(EditorDocument& document, std::unique_ptr<IEditorCommand> command) {
    if (!command) return CommandResult::fail("null editor command");
    if (cursor_ < history_.size()) history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
    CommandResult result = command->execute(document);
    if (!result.success) return result;
    if (cursor_ > 0 && history_[cursor_ - 1]->merge_with(*command)) return result;
    history_.push_back(std::move(command));
    cursor_ = history_.size();
    if (history_.size() > maximumEntries_) {
        history_.erase(history_.begin());
        --cursor_;
    }
    return result;
}

CommandResult EditorCommandStack::undo(EditorDocument& document) {
    if (!can_undo()) return CommandResult::fail("nothing to undo");
    CommandResult result = history_[cursor_ - 1]->undo(document);
    if (result.success) --cursor_;
    return result;
}
CommandResult EditorCommandStack::redo(EditorDocument& document) {
    if (!can_redo()) return CommandResult::fail("nothing to redo");
    CommandResult result = history_[cursor_]->redo(document);
    if (result.success) ++cursor_;
    return result;
}
void EditorCommandStack::clear() noexcept { history_.clear(); cursor_ = 0; }
bool EditorCommandStack::can_undo() const noexcept { return cursor_ > 0; }
bool EditorCommandStack::can_redo() const noexcept { return cursor_ < history_.size(); }
std::string_view EditorCommandStack::undo_label() const noexcept { return can_undo() ? history_[cursor_ - 1]->label() : std::string_view{}; }
std::string_view EditorCommandStack::redo_label() const noexcept { return can_redo() ? history_[cursor_]->label() : std::string_view{}; }

VoxelObjectState capture_voxel_object_state(const EditorObject& object) {
    VoxelObjectState state;
    if (!object.voxels) return state;
    state.voxels.reserve(static_cast<std::size_t>(object.voxels->occupied_voxel_count()));
    for (const auto& [key, brick] : object.voxels->bricks()) {
        brick.occupancy().for_each_set([&](std::uint16_t index) {
            state.voxels.push_back({global_from_local(key, local_from_index_unchecked(index)),
                                    brick.material(index)});
        });
    }
    std::sort(state.voxels.begin(), state.voxels.end(), [](const SparseVoxelStateEntry& a,
                                                           const SparseVoxelStateEntry& b) {
        return a.voxel < b.voxel;
    });
    state.anchors.assign(object.anchors.begin(), object.anchors.end());
    return state;
}

ReplaceVoxelObjectStateCommand::ReplaceVoxelObjectStateCommand(
    EditorObjectId id, VoxelObjectState before, VoxelObjectState after, std::string label)
    : id_(id), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}

CommandResult ReplaceVoxelObjectStateCommand::apply(EditorDocument& document,
                                                    const VoxelObjectState& state) {
    EditorObject* object = document.find_object(id_);
    if (!object) return CommandResult::fail("voxel object target no longer exists");
    if (object->flags.locked) return CommandResult::fail("voxel object target is locked");
    auto replacement = std::make_unique<VoxelObject>(id_);
    replacement->reserve_bricks((state.voxels.size() + kBrickVoxelCount - 1) / kBrickVoxelCount);
    for (const SparseVoxelStateEntry& entry : state.voxels) {
        if (entry.material == kAirMaterial) return CommandResult::fail("voxel object state contains air entries");
        replacement->set_voxel(entry.voxel, entry.material);
    }
    if (!replacement->validate()) return CommandResult::fail("voxel object state failed validation");
    std::set<Int3> anchors;
    for (const Int3 anchor : state.anchors) {
        if (replacement->occupied_at(anchor)) anchors.insert(anchor);
    }
    object->voxels = std::move(replacement);
    object->anchors = std::move(anchors);
    document.mark_dirty();
    return CommandResult::ok();
}
CommandResult ReplaceVoxelObjectStateCommand::execute(EditorDocument& document) { return apply(document, after_); }
CommandResult ReplaceVoxelObjectStateCommand::undo(EditorDocument& document) { return apply(document, before_); }

VoxelEditCommand::VoxelEditCommand(EditorObjectId objectId, std::vector<VoxelChange> changes,
                                   std::string label, std::uint64_t mergeKey)
    : objectId_(objectId), changes_(std::move(changes)), label_(std::move(label)), mergeKey_(mergeKey) {
    std::sort(changes_.begin(), changes_.end(), [](const VoxelChange& a, const VoxelChange& b) { return a.voxel < b.voxel; });
    changes_.erase(std::unique(changes_.begin(), changes_.end(), [](const VoxelChange& a, const VoxelChange& b) {
        return a.voxel == b.voxel;
    }), changes_.end());
}

CommandResult VoxelEditCommand::apply(EditorDocument& document, bool forward) {
    EditorObject* object = document.find_object(objectId_);
    if (!object) return CommandResult::fail("voxel edit target no longer exists");
    if (object->flags.locked) return CommandResult::fail("voxel edit target is locked");
    for (const VoxelChange& change : changes_) {
        const MaterialId expected = forward ? change.before : change.after;
        if (object->voxels->material_at(change.voxel) != expected) {
            return CommandResult::fail("voxel edit precondition no longer holds");
        }
    }
    for (const VoxelChange& change : changes_) {
        object->voxels->set_voxel(change.voxel, forward ? change.after : change.before);
        if ((forward ? change.after : change.before) == kAirMaterial) object->anchors.erase(change.voxel);
    }
    document.mark_dirty();
    return CommandResult::ok();
}
CommandResult VoxelEditCommand::execute(EditorDocument& document) { return apply(document, true); }
CommandResult VoxelEditCommand::undo(EditorDocument& document) { return apply(document, false); }

bool VoxelEditCommand::merge_with(const IEditorCommand& otherBase) {
    const auto* other = dynamic_cast<const VoxelEditCommand*>(&otherBase);
    if (!other || mergeKey_ == 0 || mergeKey_ != other->mergeKey_ || objectId_ != other->objectId_) return false;
    std::map<Int3, VoxelChange> combined;
    for (const VoxelChange& change : changes_) combined[change.voxel] = change;
    for (const VoxelChange& change : other->changes_) {
        auto [it, inserted] = combined.emplace(change.voxel, change);
        if (!inserted) it->second.after = change.after;
    }
    changes_.clear();
    for (const auto& [voxel, change] : combined) {
        (void)voxel;
        if (change.before != change.after) changes_.push_back(change);
    }
    return true;
}

TransformObjectsCommand::TransformObjectsCommand(std::vector<ObjectTransformChange> changes,
                                                 std::string label)
    : changes_(std::move(changes)), label_(std::move(label)) {}

CommandResult TransformObjectsCommand::apply(EditorDocument& document, bool forward) {
    if (changes_.empty()) return CommandResult::fail("transform command has no objects");
    std::set<EditorObjectId> seen;
    for (const ObjectTransformChange& change : changes_) {
        if (change.id == 0 || !seen.insert(change.id).second)
            return CommandResult::fail("transform command contains duplicate or invalid object identifiers");
        EditorObject* object = document.find_object(change.id);
        if (!object) return CommandResult::fail("transform target no longer exists");
        if (object->flags.locked) return CommandResult::fail("transform target is locked");
        const RigidTransform& target = forward ? change.after : change.before;
        if (!valid_transform(target)) return CommandResult::fail("transform is not finite or has an invalid rotation");
    }
    for (const ObjectTransformChange& change : changes_) {
        const RigidTransform& target = forward ? change.after : change.before;
        if (!document.set_world_transform(change.id, target))
            return CommandResult::fail("failed to apply object transform");
    }
    document.mark_dirty();
    return CommandResult::ok();
}
CommandResult TransformObjectsCommand::execute(EditorDocument& document) { return apply(document, true); }
CommandResult TransformObjectsCommand::undo(EditorDocument& document) { return apply(document, false); }

TransformObjectCommand::TransformObjectCommand(EditorObjectId id, RigidTransform before, RigidTransform after)
    : id_(id), before_(before), after_(after) {}
CommandResult TransformObjectCommand::apply(EditorDocument& document, const RigidTransform& transform) {
    EditorObject* object = document.find_object(id_);
    if (!object) return CommandResult::fail("transform target no longer exists");
    if (object->flags.locked) return CommandResult::fail("transform target is locked");
    if (!document.set_world_transform(id_, transform))
        return CommandResult::fail("failed to apply object transform");
    return CommandResult::ok();
}
CommandResult TransformObjectCommand::execute(EditorDocument& document) { return apply(document, after_); }
CommandResult TransformObjectCommand::undo(EditorDocument& document) { return apply(document, before_); }

ReplaceText3DObjectCommand::ReplaceText3DObjectCommand(
    EditorObjectId id, Text3DObjectState before, Text3DObjectState after, std::string label)
    : id_(id), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}

CommandResult ReplaceText3DObjectCommand::apply(EditorDocument& document,
                                                 const Text3DObjectState& state) {
    EditorObject* object = document.find_object(id_);
    if (!object) return CommandResult::fail("3D text target no longer exists");
    if (object->flags.locked) return CommandResult::fail("3D text target is locked");
    if (state.asset) {
        CookedText3DAsset replacement = *state.asset;
        replacement.objectId = id_;
        replacement.sideMesh.objectId = id_;
        replacement.sideMesh.contentHash = polygon_asset_content_hash(replacement.sideMesh);
        replacement.contentHash = text3d_content_hash(replacement);
        std::string validation;
        if (!validate_text3d_asset(replacement, &validation))
            return CommandResult::fail("invalid replacement 3D text asset: " + validation);
        object->text3d = std::move(replacement);
        object->voxels = std::make_unique<VoxelObject>(id_);
    } else {
        object->text3d.reset();
    }
    object->textFontAsset = state.fontAsset;
    object->sourceAsset = state.sourceAsset;
    document.mark_dirty();
    return CommandResult::ok();
}
CommandResult ReplaceText3DObjectCommand::execute(EditorDocument& document) { return apply(document, after_); }
CommandResult ReplaceText3DObjectCommand::undo(EditorDocument& document) { return apply(document, before_); }

ReplaceGaborVolumeObjectCommand::ReplaceGaborVolumeObjectCommand(
    EditorObjectId id, GaborVolumeObjectState before, GaborVolumeObjectState after,
    std::string label)
    : id_(id), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}

CommandResult ReplaceGaborVolumeObjectCommand::apply(
    EditorDocument& document, const GaborVolumeObjectState& state) {
    EditorObject* object = document.find_object(id_);
    if (!object) return CommandResult::fail("Gabor volume target no longer exists");
    if (object->flags.locked) return CommandResult::fail("Gabor volume target is locked");
    if (state.asset) {
        GaborVolumeAsset replacement = *state.asset;
        replacement.recompute_bounds_and_hash();
        std::string validation;
        if (!replacement.validate(&validation))
            return CommandResult::fail("invalid replacement Gabor volume: " + validation);
        object->gaborVolume = std::move(replacement);
        object->text3d.reset();
        object->textFontAsset.clear();
        object->voxels = std::make_unique<VoxelObject>(id_);
    } else {
        object->gaborVolume.reset();
    }
    object->sourceAsset = state.sourceAsset;
    object->importRecipe = state.importRecipe;
    document.mark_dirty();
    return CommandResult::ok();
}
CommandResult ReplaceGaborVolumeObjectCommand::execute(EditorDocument& document) { return apply(document, after_); }
CommandResult ReplaceGaborVolumeObjectCommand::undo(EditorDocument& document) { return apply(document, before_); }

SetObjectFlagsBatchCommand::SetObjectFlagsBatchCommand(std::vector<ObjectFlagsChange> changes)
    : changes_(std::move(changes)) {}

CommandResult SetObjectFlagsBatchCommand::apply(EditorDocument& document, bool forward) {
    if (changes_.empty()) return CommandResult::fail("object policy command has no objects");
    std::set<EditorObjectId> seen;
    for (const ObjectFlagsChange& change : changes_) {
        if (change.id == 0 || !seen.insert(change.id).second)
            return CommandResult::fail("object policy command contains duplicate or invalid identifiers");
        EditorObject* object = document.find_object(change.id);
        if (!object) return CommandResult::fail("object policy target no longer exists");
    }
    for (const ObjectFlagsChange& change : changes_) {
        document.find_object(change.id)->flags = forward ? change.after : change.before;
    }
    document.mark_dirty();
    return CommandResult::ok();
}
CommandResult SetObjectFlagsBatchCommand::execute(EditorDocument& document) { return apply(document, true); }
CommandResult SetObjectFlagsBatchCommand::undo(EditorDocument& document) { return apply(document, false); }

SetObjectFlagsCommand::SetObjectFlagsCommand(EditorObjectId id, EditorObjectFlags before, EditorObjectFlags after)
    : id_(id), before_(before), after_(after) {}
CommandResult SetObjectFlagsCommand::apply(EditorDocument& document, const EditorObjectFlags& flags) {
    EditorObject* object = document.find_object(id_);
    if (!object) return CommandResult::fail("policy target no longer exists");
    object->flags = flags;
    document.mark_dirty();
    return CommandResult::ok();
}
CommandResult SetObjectFlagsCommand::execute(EditorDocument& document) { return apply(document, after_); }
CommandResult SetObjectFlagsCommand::undo(EditorDocument& document) { return apply(document, before_); }

SetAnchorsCommand::SetAnchorsCommand(EditorObjectId id, std::vector<Int3> add, std::vector<Int3> remove)
    : id_(id), add_(std::move(add)), remove_(std::move(remove)) {}
CommandResult SetAnchorsCommand::apply(EditorDocument& document, bool forward) {
    EditorObject* object = document.find_object(id_);
    if (!object) return CommandResult::fail("anchor target no longer exists");
    if (object->flags.locked) return CommandResult::fail("anchor target is locked");
    const auto& adding = forward ? add_ : remove_;
    const auto& removing = forward ? remove_ : add_;
    for (const Int3 voxel : adding) {
        if (!object->voxels->occupied_at(voxel)) return CommandResult::fail("cannot anchor an empty voxel");
    }
    for (const Int3 voxel : removing) object->anchors.erase(voxel);
    for (const Int3 voxel : adding) object->anchors.insert(voxel);
    document.mark_dirty();
    return CommandResult::ok();
}
CommandResult SetAnchorsCommand::execute(EditorDocument& document) { return apply(document, true); }
CommandResult SetAnchorsCommand::undo(EditorDocument& document) { return apply(document, false); }

std::vector<EditorObjectId> collect_editor_object_subtree_ids(
    const EditorDocument& document,
    std::span<const EditorObjectId> roots) {
    std::vector<EditorObjectId> result;
    std::set<EditorObjectId> seen;
    const auto append = [&](const auto& self, EditorObjectId id) -> void {
        if (!document.find_object(id) || !seen.insert(id).second) return;
        result.push_back(id);
        for (const EditorObjectId child : document.children_of(id)) self(self, child);
    };
    for (const EditorObjectId root : roots) append(append, root);
    return result;
}

CompoundCommand::CompoundCommand(std::string label) : label_(std::move(label)) {}
void CompoundCommand::add(std::unique_ptr<IEditorCommand> command) { if (command) commands_.push_back(std::move(command)); }
CommandResult CompoundCommand::execute(EditorDocument& document) {
    std::size_t completed = 0;
    for (auto& command : commands_) {
        CommandResult result = command->execute(document);
        if (!result.success) {
            while (completed > 0) { --completed; commands_[completed]->undo(document); }
            return result;
        }
        ++completed;
    }
    return CommandResult::ok();
}
CommandResult CompoundCommand::undo(EditorDocument& document) {
    for (std::size_t i = commands_.size(); i > 0; --i) {
        CommandResult result = commands_[i - 1]->undo(document);
        if (!result.success) return result;
    }
    return CommandResult::ok();
}

AddObjectCommand::AddObjectCommand(EditorObject object, std::string label)
    : pending_(std::move(object)), id_(pending_->id), label_(std::move(label)) {}

CommandResult AddObjectCommand::execute(EditorDocument& document) {
    if (!pending_) return CommandResult::fail("object was already added");
    if (document.find_object(id_)) return CommandResult::fail("an object with this identifier already exists");
    document.add_object(std::move(*pending_));
    pending_.reset();
    document.mark_dirty();
    return CommandResult::ok();
}
CommandResult AddObjectCommand::undo(EditorDocument& document) {
    const EditorObject* object = document.find_object(id_);
    if (!object) return CommandResult::fail("object no longer exists to remove");
    pending_ = clone_editor_object(*object);
    if (!document.remove_object(id_)) return CommandResult::fail("failed to remove object");
    document.mark_dirty();
    return CommandResult::ok();
}

RemoveObjectsCommand::RemoveObjectsCommand(std::vector<EditorObjectId> ids, std::string label)
    : requestedIds_(std::move(ids)), label_(std::move(label)) {}

CommandResult RemoveObjectsCommand::execute(EditorDocument& document) {
    if (requestedIds_.empty()) return CommandResult::fail("delete command has no objects");

    std::set<EditorObjectId> requestedSeen;
    for (const EditorObjectId id : requestedIds_) {
        if (id == 0 || !requestedSeen.insert(id).second)
            return CommandResult::fail("delete command contains duplicate or invalid identifiers");
        if (!document.find_object(id)) return CommandResult::fail("delete target no longer exists");
    }

    resolvedIds_ = collect_editor_object_subtree_ids(document, requestedIds_);
    if (resolvedIds_.empty()) return CommandResult::fail("delete command resolved to no objects");

    const std::set<EditorObjectId> resolvedSet(resolvedIds_.begin(), resolvedIds_.end());
    removalRoots_.clear();
    for (const EditorObjectId id : resolvedIds_) {
        const EditorObject* object = document.find_object(id);
        if (!object) return CommandResult::fail("delete target no longer exists");
        if (object->flags.locked)
            return CommandResult::fail("cannot delete a locked object or a parent containing one");
        if (!object->parent || !resolvedSet.contains(*object->parent)) removalRoots_.push_back(id);
    }

    // Capture the entire closure before mutating anything. EditorDocument::remove_object()
    // deletes a whole subtree, so cloning one selected root and then deleting it would make
    // later selected descendants disappear before they could be captured.
    removed_.clear();
    removed_.resize(resolvedIds_.size());
    for (std::size_t i = 0; i < resolvedIds_.size(); ++i)
        removed_[i] = clone_editor_object(*document.find_object(resolvedIds_[i]));

    for (const EditorObjectId root : removalRoots_) {
        if (!document.remove_object(root)) {
            // This should be unreachable after prevalidation, but preserve the command's
            // all-or-nothing contract if the document implementation changes.
            for (std::size_t i = 0; i < resolvedIds_.size(); ++i) {
                if (removed_[i] && !document.find_object(resolvedIds_[i]))
                    document.add_object(clone_editor_object(*removed_[i]));
            }
            return CommandResult::fail("failed to remove object hierarchy atomically");
        }
    }
    document.mark_dirty();
    return CommandResult::ok();
}

CommandResult RemoveObjectsCommand::undo(EditorDocument& document) {
    if (resolvedIds_.empty() || removed_.size() != resolvedIds_.size())
        return CommandResult::fail("nothing to restore for this delete command");
    for (std::size_t i = 0; i < resolvedIds_.size(); ++i) {
        if (!removed_[i]) return CommandResult::fail("nothing to restore for this delete command");
        if (document.find_object(resolvedIds_[i]))
            return CommandResult::fail("an object with this identifier already exists");
    }

    try {
        // resolvedIds_ is parent-before-child, so the document remains valid throughout the
        // restoration rather than relying on add_object() accepting a temporarily missing
        // parent. Restore from clones rather than consuming the snapshots: an exceptional
        // add must leave both the document and the command retryable.
        for (std::size_t i = 0; i < resolvedIds_.size(); ++i)
            document.add_object(clone_editor_object(*removed_[i]));
    } catch (const std::exception& exception) {
        for (auto it = removalRoots_.rbegin(); it != removalRoots_.rend(); ++it)
            if (document.find_object(*it)) (void)document.remove_object(*it);
        return CommandResult::fail(std::string("failed to restore deleted hierarchy: ") + exception.what());
    }
    document.mark_dirty();
    return CommandResult::ok();
}

RenameObjectCommand::RenameObjectCommand(EditorObjectId id, std::string before, std::string after)
    : id_(id), before_(std::move(before)), after_(std::move(after)) {}
CommandResult RenameObjectCommand::apply(EditorDocument& document, const std::string& name) {
    EditorObject* object = document.find_object(id_);
    if (!object) return CommandResult::fail("rename target no longer exists");
    if (object->flags.locked) return CommandResult::fail("rename target is locked");
    object->name = name;
    document.mark_dirty();
    return CommandResult::ok();
}
CommandResult RenameObjectCommand::execute(EditorDocument& document) { return apply(document, after_); }
CommandResult RenameObjectCommand::undo(EditorDocument& document) { return apply(document, before_); }


std::optional<ObjectAttachmentState> capture_object_attachment_state(
    const EditorDocument& document, EditorObjectId id) noexcept {
    const EditorObject* object = document.find_object(id);
    const auto world = document.world_transform(id);
    if (!object || !world) return std::nullopt;
    return ObjectAttachmentState{object->parent, object->attachment, *world};
}

SetObjectAttachmentCommand::SetObjectAttachmentCommand(
    EditorObjectId id, ObjectAttachmentState before, ObjectAttachmentState after,
    std::string label)
    : id_(id), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}

CommandResult SetObjectAttachmentCommand::apply(
    EditorDocument& document, const ObjectAttachmentState& state) {
    EditorObject* object = document.find_object(id_);
    if (!object) return CommandResult::fail("attachment target no longer exists");
    if (object->flags.locked) return CommandResult::fail("attachment target is locked");
    if (state.parent.has_value() != state.attachment.has_value())
        return CommandResult::fail("attachment state must contain both parent and attachment");
    if (state.attachment) {
        std::string validation;
        if (!state.attachment->validate(&validation))
            return CommandResult::fail("invalid attachment state: " + validation);
        std::string error;
        if (!document.attach_object(id_, *state.parent, true, state.attachment->socket,
                                    state.attachment->inheritPosition,
                                    state.attachment->inheritRotation, &error))
            return CommandResult::fail(error.empty() ? "failed to attach object" : error);
        object = document.find_object(id_);
        object->attachment = state.attachment;
        document.refresh_attachment_world_transforms();
    } else {
        std::string error;
        if (!document.detach_object(id_, true, &error))
            return CommandResult::fail(error.empty() ? "failed to detach object" : error);
        if (!document.set_world_transform(id_, state.worldTransform))
            return CommandResult::fail("failed to restore detached world transform");
    }
    document.mark_dirty();
    return CommandResult::ok();
}
CommandResult SetObjectAttachmentCommand::execute(EditorDocument& document) { return apply(document, after_); }
CommandResult SetObjectAttachmentCommand::undo(EditorDocument& document) { return apply(document, before_); }

AddComponentCommand::AddComponentCommand(
    EditorObjectId objectId, Component component, std::string label)
    : objectId_(objectId), component_(std::move(component)), label_(std::move(label)) {}
CommandResult AddComponentCommand::execute(EditorDocument& document) {
    EditorObject* object = document.find_object(objectId_);
    if (!object) return CommandResult::fail("component owner no longer exists");
    if (object->flags.locked) return CommandResult::fail("component owner is locked");
    if (component_.id == kInvalidComponentId) component_.id = document.allocate_component_id(objectId_);
    std::string error;
    if (!document.add_component(objectId_, component_, &error))
        return CommandResult::fail(error.empty() ? "failed to add component" : error);
    return CommandResult::ok();
}
CommandResult AddComponentCommand::undo(EditorDocument& document) {
    EditorObject* object = document.find_object(objectId_);
    if (!object) return CommandResult::fail("component owner no longer exists");
    if (object->flags.locked) return CommandResult::fail("component owner is locked");
    if (!document.remove_component(objectId_, component_.id))
        return CommandResult::fail("component no longer exists to remove");
    return CommandResult::ok();
}

RemoveComponentCommand::RemoveComponentCommand(
    EditorObjectId objectId, ComponentId componentId, std::string label)
    : objectId_(objectId), componentId_(componentId), label_(std::move(label)) {}
CommandResult RemoveComponentCommand::execute(EditorDocument& document) {
    EditorObject* object = document.find_object(objectId_);
    if (!object) return CommandResult::fail("component owner no longer exists");
    if (object->flags.locked) return CommandResult::fail("component owner is locked");
    const Component* component = document.find_component(objectId_, componentId_);
    if (!component) return CommandResult::fail("component no longer exists");
    removed_ = *component;
    if (!document.remove_component(objectId_, componentId_))
        return CommandResult::fail("failed to remove component");
    return CommandResult::ok();
}
CommandResult RemoveComponentCommand::undo(EditorDocument& document) {
    EditorObject* object = document.find_object(objectId_);
    if (!object) return CommandResult::fail("component owner no longer exists");
    if (object->flags.locked) return CommandResult::fail("component owner is locked");
    if (!removed_) return CommandResult::fail("component removal has no captured state");
    std::string error;
    if (!document.add_component(objectId_, *removed_, &error))
        return CommandResult::fail(error.empty() ? "failed to restore component" : error);
    return CommandResult::ok();
}

SetComponentPropertyCommand::SetComponentPropertyCommand(
    EditorObjectId objectId, ComponentId componentId, std::string property,
    std::optional<ComponentValue> before, std::optional<ComponentValue> after,
    std::string label)
    : objectId_(objectId), componentId_(componentId), property_(std::move(property)),
      before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}
CommandResult SetComponentPropertyCommand::apply(
    EditorDocument& document, const std::optional<ComponentValue>& value) {
    EditorObject* object = document.find_object(objectId_);
    if (!object) return CommandResult::fail("component owner no longer exists");
    if (object->flags.locked) return CommandResult::fail("component owner is locked");
    Component* component = document.find_component(objectId_, componentId_);
    if (!component) return CommandResult::fail("component no longer exists");
    if (!component_property_name_is_valid(property_))
        return CommandResult::fail("invalid component property name");
    if (value) {
        std::string error;
        if (!document.set_component_property(objectId_, componentId_, property_, *value, &error))
            return CommandResult::fail(error.empty() ? "failed to set component property" : error);
    } else {
        component->properties.erase(property_);
        document.mark_dirty();
    }
    return CommandResult::ok();
}
CommandResult SetComponentPropertyCommand::execute(EditorDocument& document) { return apply(document, after_); }
CommandResult SetComponentPropertyCommand::undo(EditorDocument& document) { return apply(document, before_); }

ReparentObjectsCommand::ReparentObjectsCommand(std::vector<ObjectReparentChange> changes, std::string label)
    : changes_(std::move(changes)), label_(std::move(label)) {}
CommandResult ReparentObjectsCommand::apply(EditorDocument& document, bool forward) {
    if (changes_.empty()) return CommandResult::fail("reparent command has no objects");
    std::set<EditorObjectId> seen;
    for (const ObjectReparentChange& change : changes_) {
        if (change.id == 0 || !seen.insert(change.id).second)
            return CommandResult::fail("reparent command contains duplicate or invalid identifiers");
        const EditorObject* object = document.find_object(change.id);
        if (!object) return CommandResult::fail("reparent target no longer exists");
        if (object->flags.locked) return CommandResult::fail("reparent target is locked");
    }
    std::size_t applied = 0;
    for (const ObjectReparentChange& change : changes_) {
        const auto target = forward ? change.after : change.before;
        if (!document.reparent(change.id, target)) {
            // Roll back whatever this call already applied: reparent's cycle check depends on
            // the current parent chain, so a batch is not naturally atomic the way a flags or
            // transform batch is, and a partial application here would silently corrupt the
            // hierarchy instead of just refusing the whole command.
            for (std::size_t i = applied; i > 0; --i) {
                const ObjectReparentChange& rollback = changes_[i - 1];
                (void)document.reparent(rollback.id, forward ? rollback.before : rollback.after);
            }
            return CommandResult::fail("reparent would create a cycle or an invalid parent");
        }
        ++applied;
    }
    document.mark_dirty();
    return CommandResult::ok();
}
CommandResult ReparentObjectsCommand::execute(EditorDocument& document) { return apply(document, true); }
CommandResult ReparentObjectsCommand::undo(EditorDocument& document) { return apply(document, false); }

} // namespace dve::editor

namespace dve::editor {

SetComponentEnabledCommand::SetComponentEnabledCommand(
    EditorObjectId objectId, ComponentId componentId, bool before, bool after, std::string label)
    : objectId_(objectId), componentId_(componentId), before_(before), after_(after), label_(std::move(label)) {}

CommandResult SetComponentEnabledCommand::apply(EditorDocument& document, bool enabled) {
    EditorObject* object = document.find_object(objectId_);
    if (!object) return CommandResult::fail("component owner no longer exists");
    if (object->flags.locked) return CommandResult::fail("component owner is locked");
    if (!document.set_component_enabled(objectId_, componentId_, enabled))
        return CommandResult::fail("component no longer exists");
    return CommandResult::ok();
}
CommandResult SetComponentEnabledCommand::execute(EditorDocument& document) { return apply(document, after_); }
CommandResult SetComponentEnabledCommand::undo(EditorDocument& document) { return apply(document, before_); }

ReorderComponentCommand::ReorderComponentCommand(
    EditorObjectId objectId, ComponentId componentId, std::size_t beforeIndex,
    std::size_t afterIndex, std::string label)
    : objectId_(objectId), componentId_(componentId), beforeIndex_(beforeIndex),
      afterIndex_(afterIndex), label_(std::move(label)) {}

CommandResult ReorderComponentCommand::apply(EditorDocument& document, std::size_t index) {
    EditorObject* object = document.find_object(objectId_);
    if (!object) return CommandResult::fail("component owner no longer exists");
    if (object->flags.locked) return CommandResult::fail("component owner is locked");
    if (!document.reorder_component(objectId_, componentId_, index))
        return CommandResult::fail("component reorder target is invalid");
    return CommandResult::ok();
}
CommandResult ReorderComponentCommand::execute(EditorDocument& document) { return apply(document, afterIndex_); }
CommandResult ReorderComponentCommand::undo(EditorDocument& document) { return apply(document, beforeIndex_); }

} // namespace dve::editor
