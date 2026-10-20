#include "dve/editor_command.hpp"
#include "dve/editor_nurbs.hpp"
#include "dve/editor_subdivision.hpp"
#include "dve/voxel_metric.hpp"

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
    CommandResult result = command->execute(document);
    if (!result.success) return result;
    if (cursor_ < history_.size()) history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
    if (cursor_ > 0 && history_[cursor_ - 1].command->merge_with(*command)) return result;
    history_.push_back({std::move(command), std::nullopt});
    cursor_ = history_.size();
    if (history_.size() > maximumEntries_) {
        history_.erase(history_.begin());
        --cursor_;
    }
    return result;
}

CommandResult EditorCommandStack::undo(EditorDocument& document) {
    if (!can_undo()) return CommandResult::fail("nothing to undo");
    CommandResult result = history_[cursor_ - 1].command->undo(document);
    if (result.success) --cursor_;
    return result;
}
CommandResult EditorCommandStack::redo(EditorDocument& document) {
    if (!can_redo()) return CommandResult::fail("nothing to redo");
    CommandResult result = history_[cursor_].command->redo(document);
    if (result.success) ++cursor_;
    return result;
}
void EditorCommandStack::set_last_selection_transition(EditorSelectionSnapshot before, EditorSelectionSnapshot after) {
    if (cursor_ == 0 || cursor_ != history_.size()) return;
    history_[cursor_ - 1].selection.emplace(std::move(before), std::move(after));
}
const EditorSelectionSnapshot* EditorCommandStack::undo_selection() const noexcept {
    if (!can_undo() || !history_[cursor_ - 1].selection) return nullptr;
    return &history_[cursor_ - 1].selection->first;
}
const EditorSelectionSnapshot* EditorCommandStack::redo_selection() const noexcept {
    if (!can_redo() || !history_[cursor_].selection) return nullptr;
    return &history_[cursor_].selection->second;
}

namespace {
// Steps that were applied one by one and are now undone and redone as one.
class JoinedCommands final : public IEditorCommand {
public:
    JoinedCommands(std::string label, std::vector<std::unique_ptr<IEditorCommand>> commands)
        : label_(std::move(label)), commands_(std::move(commands)) {}
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument& document) override { return redo(document); }
    CommandResult undo(EditorDocument& document) override {
        for (std::size_t i = commands_.size(); i > 0; --i) {
            const CommandResult result = commands_[i - 1]->undo(document);
            if (!result.success) {
                for (std::size_t restored = i; restored < commands_.size(); ++restored) (void)commands_[restored]->redo(document);
                return result;
            }
        }
        return CommandResult::ok();
    }
    CommandResult redo(EditorDocument& document) override {
        for (std::size_t i = 0; i < commands_.size(); ++i) {
            const CommandResult result = commands_[i]->redo(document);
            if (!result.success) {
                while (i > 0) { --i; (void)commands_[i]->undo(document); }
                return result;
            }
        }
        return CommandResult::ok();
    }
private:
    std::string label_;
    std::vector<std::unique_ptr<IEditorCommand>> commands_;
};
}  // namespace

bool EditorCommandStack::join_last(std::size_t count, std::string label) {
    if (count < 2 || cursor_ != history_.size() || count > history_.size()) return count == 1 && cursor_ == history_.size();
    const std::size_t first = history_.size() - count;
    std::vector<std::unique_ptr<IEditorCommand>> commands;
    std::optional<EditorSelectionSnapshot> before, after;
    for (std::size_t i = first; i < history_.size(); ++i) {
        if (history_[i].selection) {
            if (!before) before = history_[i].selection->first;
            after = history_[i].selection->second;
        }
        commands.push_back(std::move(history_[i].command));
    }
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(first), history_.end());
    history_.push_back({std::make_unique<JoinedCommands>(std::move(label), std::move(commands)), std::nullopt});
    if (before && after) history_.back().selection.emplace(std::move(*before), std::move(*after));
    cursor_ = history_.size();
    return true;
}

CommandResult EditorCommandStack::discard_last(EditorDocument& document) {
    if (cursor_ == 0 || cursor_ != history_.size()) return CommandResult::fail("nothing to discard");
    CommandResult result = history_.back().command->undo(document);
    if (!result.success) return result;
    history_.pop_back();
    cursor_ = history_.size();
    return result;
}

void EditorCommandStack::clear() noexcept { history_.clear(); cursor_ = 0; }
bool EditorCommandStack::can_undo() const noexcept { return cursor_ > 0; }
bool EditorCommandStack::can_redo() const noexcept { return cursor_ < history_.size(); }
std::string_view EditorCommandStack::undo_label() const noexcept { return can_undo() ? history_[cursor_ - 1].command->label() : std::string_view{}; }
std::string_view EditorCommandStack::redo_label() const noexcept { return can_redo() ? history_[cursor_].command->label() : std::string_view{}; }

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

ScaleVoxelSizeCommand::ScaleVoxelSizeCommand(std::vector<ObjectVoxelSizeChange> changes, std::string label)
    : changes_(std::move(changes)), label_(std::move(label)) {}

CommandResult ScaleVoxelSizeCommand::apply(EditorDocument& document, bool forward) {
    if (changes_.empty()) return CommandResult::fail("voxel size command has no objects");
    std::set<EditorObjectId> seen;
    for (const ObjectVoxelSizeChange& change : changes_) {
        if (change.id == 0 || !seen.insert(change.id).second)
            return CommandResult::fail("voxel size command contains duplicate or invalid object identifiers");
        const EditorObject* object = document.find_object(change.id);
        if (!object) return CommandResult::fail("scale target no longer exists");
        if (object->flags.locked) return CommandResult::fail("scale target is locked");
        const float size = forward ? change.afterVoxelSize : change.beforeVoxelSize;
        if (!std::isfinite(size) || !(size > 0.0F)) return CommandResult::fail("scaled voxel size is invalid");
        if (change.beforeAxisScale.has_value()!=change.afterAxisScale.has_value())
            return CommandResult::fail("axis scale history is incomplete");
        const auto axes = forward ? change.afterAxisScale : change.beforeAxisScale;
        if (axes && !valid_voxel_metric(*axes)) return CommandResult::fail("axis scale is invalid");
        if (!valid_transform(forward ? change.after : change.before))
            return CommandResult::fail("transform is not finite or has an invalid rotation");
    }
    for (const ObjectVoxelSizeChange& change : changes_) {
        EditorObject* object = document.find_object(change.id);
        object->voxelSizeMeters = forward ? change.afterVoxelSize : change.beforeVoxelSize;
        const auto axes = forward ? change.afterAxisScale : change.beforeAxisScale;
        if (axes) object->voxelScale = *axes;
        if (!document.set_world_transform(change.id, forward ? change.after : change.before))
            return CommandResult::fail("failed to apply object transform");
    }
    document.mark_dirty();
    return CommandResult::ok();
}
CommandResult ScaleVoxelSizeCommand::execute(EditorDocument& document) { return apply(document, true); }
CommandResult ScaleVoxelSizeCommand::undo(EditorDocument& document) { return apply(document, false); }
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
        if (!result.success) {
            // Put successfully undone children back in forward order. The history
            // cursor remains unchanged when undo fails.
            for (std::size_t restored = i; restored < commands_.size(); ++restored) {
                const CommandResult rollback = commands_[restored]->redo(document);
                if (!rollback.success)
                    return CommandResult::fail(result.message + "; undo rollback failed: " + rollback.message);
            }
            return result;
        }
    }
    return CommandResult::ok();
}

AddObjectCommand::AddObjectCommand(EditorObject object, std::string label)
    : pending_(std::move(object)), id_(pending_->id), label_(std::move(label)) {}

CommandResult AddObjectCommand::execute(EditorDocument& document) {
    if (!pending_) return CommandResult::fail("object was already added");
    std::string nurbsError;if(!validate_editor_nurbs(*pending_,&nurbsError))return CommandResult::fail(nurbsError);
    if (document.find_object(id_)) return CommandResult::fail("an object with this identifier already exists");
    if (pending_->subdivision) {
        if (!pending_->polygon) return CommandResult::fail("Subdivision object has no cage");
        freeze_subdivision_topology(*pending_->polygon,*pending_->subdivision);
        const auto evaluated=evaluate_subdivision(*pending_->polygon,*pending_->subdivision);
        if (!evaluated.mesh) return CommandResult::fail(evaluated.error);
    }
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
    // A NURBS source must be authored with its geometry cache (NURBS menu); a generic empty source
    // would turn the object into an invalid, unsaveable NURBS object.
    if (component_.type == "dve.nurbs") return CommandResult::fail("Create NURBS objects from the NURBS menu");
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
    if(component->type=="dve.nurbs")return CommandResult::fail("Use NURBS source commands");
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
    std::vector<HierarchyState> original;
    original.reserve(changes_.size());
    for (const ObjectReparentChange& change : changes_) {
        if (change.id == 0 || !seen.insert(change.id).second)
            return CommandResult::fail("reparent command contains duplicate or invalid identifiers");
        const EditorObject* object = document.find_object(change.id);
        if (!object) return CommandResult::fail("reparent target no longer exists");
        if (object->flags.locked) return CommandResult::fail("reparent target is locked");
        original.push_back({object->parent,object->attachment,object->transform});
    }
    if (capturedStates_) {
        const auto& target = forward ? afterStates_ : beforeStates_;
        std::map<EditorObjectId,std::optional<EditorObjectId>> parents;
        for (std::size_t i = 0; i < changes_.size(); ++i) {
            if (target[i].parent && !document.find_object(*target[i].parent))
                return CommandResult::fail("reparent parent no longer exists");
            parents.emplace(changes_[i].id,target[i].parent);
        }
        // Validate the complete restored hierarchy before changing any object.
        for (const auto& change : changes_) {
            std::set<EditorObjectId> ancestors{change.id};
            auto cursor = parents.at(change.id);
            while (cursor) {
                if (!ancestors.insert(*cursor).second)
                    return CommandResult::fail("reparent would create a cycle");
                const auto planned = parents.find(*cursor);
                if (planned != parents.end()) cursor = planned->second;
                else {
                    const auto* parent = document.find_object(*cursor);
                    if (!parent) return CommandResult::fail("reparent parent no longer exists");
                    cursor = parent->parent;
                }
            }
        }
        for (std::size_t i = 0; i < changes_.size(); ++i) {
            auto* object = document.find_object(changes_[i].id);
            object->parent = target[i].parent;
            object->attachment = target[i].attachment;
            object->transform = target[i].transform;
        }
    } else {
        if (!forward) return CommandResult::fail("reparent command has not executed");
        for (const auto& change : changes_) {
            if (!document.reparent(change.id,change.after)) {
                // Restore attachment records too: detaching to a root clears them.
                for (std::size_t i = 0; i < changes_.size(); ++i) {
                    auto* object = document.find_object(changes_[i].id);
                    object->parent = original[i].parent;
                    object->attachment = original[i].attachment;
                    object->transform = original[i].transform;
                }
                document.refresh_attachment_world_transforms();
                return CommandResult::fail("reparent would create a cycle or an invalid parent");
            }
        }
        beforeStates_ = std::move(original);
        for (const auto& change : changes_) {
            const auto* object = document.find_object(change.id);
            afterStates_.push_back({object->parent,object->attachment,object->transform});
        }
        capturedStates_ = true;
    }
    document.refresh_attachment_world_transforms();
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
