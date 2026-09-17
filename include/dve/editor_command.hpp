#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/editor_document.hpp"

namespace dve::editor {

struct CommandResult {
    bool success{};
    std::string message;
    static CommandResult ok();
    static CommandResult fail(std::string message);
};

class IEditorCommand {
public:
    virtual ~IEditorCommand() = default;
    [[nodiscard]] virtual std::string_view label() const noexcept = 0;
    virtual CommandResult execute(EditorDocument& document) = 0;
    virtual CommandResult undo(EditorDocument& document) = 0;
    virtual CommandResult redo(EditorDocument& document) { return execute(document); }
    [[nodiscard]] virtual bool merge_with(const IEditorCommand&) { return false; }
};

class EditorCommandStack {
public:
    explicit EditorCommandStack(std::size_t maximumEntries = 256);
    CommandResult execute(EditorDocument& document, std::unique_ptr<IEditorCommand> command);
    CommandResult undo(EditorDocument& document);
    CommandResult redo(EditorDocument& document);
    void clear() noexcept;
    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;
    [[nodiscard]] std::string_view undo_label() const noexcept;
    [[nodiscard]] std::string_view redo_label() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return history_.size(); }

private:
    std::vector<std::unique_ptr<IEditorCommand>> history_;
    std::size_t cursor_{};
    std::size_t maximumEntries_{};
};


struct SparseVoxelStateEntry {
    Int3 voxel{};
    MaterialId material{kAirMaterial};
};

struct VoxelObjectState {
    std::vector<SparseVoxelStateEntry> voxels;
    std::vector<Int3> anchors;
};

[[nodiscard]] VoxelObjectState capture_voxel_object_state(const EditorObject& object);

class ReplaceVoxelObjectStateCommand final : public IEditorCommand {
public:
    ReplaceVoxelObjectStateCommand(EditorObjectId id, VoxelObjectState before,
                                   VoxelObjectState after, std::string label);
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&, const VoxelObjectState& state);
    EditorObjectId id_{};
    VoxelObjectState before_;
    VoxelObjectState after_;
    std::string label_;
};

struct VoxelChange {
    Int3 voxel{};
    MaterialId before{kAirMaterial};
    MaterialId after{kAirMaterial};
};

class VoxelEditCommand final : public IEditorCommand {
public:
    VoxelEditCommand(EditorObjectId objectId, std::vector<VoxelChange> changes,
                     std::string label = "Edit voxels", std::uint64_t mergeKey = 0);
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
    [[nodiscard]] bool merge_with(const IEditorCommand&) override;
    [[nodiscard]] std::size_t change_count() const noexcept { return changes_.size(); }

private:
    CommandResult apply(EditorDocument&, bool forward);
    EditorObjectId objectId_{};
    std::vector<VoxelChange> changes_;
    std::string label_;
    std::uint64_t mergeKey_{};
};


struct ObjectTransformChange {
    EditorObjectId id{};
    RigidTransform before{};
    RigidTransform after{};
};

class TransformObjectsCommand final : public IEditorCommand {
public:
    explicit TransformObjectsCommand(std::vector<ObjectTransformChange> changes,
                                     std::string label = "Transform objects");
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
    [[nodiscard]] std::size_t object_count() const noexcept { return changes_.size(); }
private:
    CommandResult apply(EditorDocument&, bool forward);
    std::vector<ObjectTransformChange> changes_;
    std::string label_;
};

class TransformObjectCommand final : public IEditorCommand {
public:
    TransformObjectCommand(EditorObjectId id, RigidTransform before, RigidTransform after);
    [[nodiscard]] std::string_view label() const noexcept override { return "Transform object"; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&, const RigidTransform&);
    EditorObjectId id_{};
    RigidTransform before_{};
    RigidTransform after_{};
};


struct Text3DObjectState {
    std::optional<CookedText3DAsset> asset;
    std::filesystem::path fontAsset;
    std::filesystem::path sourceAsset;
};

class ReplaceText3DObjectCommand final : public IEditorCommand {
public:
    ReplaceText3DObjectCommand(EditorObjectId id, Text3DObjectState before,
                               Text3DObjectState after,
                               std::string label = "Edit 3D text");
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&, const Text3DObjectState&);
    EditorObjectId id_{};
    Text3DObjectState before_;
    Text3DObjectState after_;
    std::string label_;
};

struct GaborVolumeObjectState {
    std::optional<GaborVolumeAsset> asset;
    std::filesystem::path sourceAsset;
    std::filesystem::path importRecipe;
};

class ReplaceGaborVolumeObjectCommand final : public IEditorCommand {
public:
    ReplaceGaborVolumeObjectCommand(EditorObjectId id, GaborVolumeObjectState before,
                                    GaborVolumeObjectState after,
                                    std::string label = "Edit Gabor volume");
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&, const GaborVolumeObjectState&);
    EditorObjectId id_{};
    GaborVolumeObjectState before_;
    GaborVolumeObjectState after_;
    std::string label_;
};


struct ObjectFlagsChange {
    EditorObjectId id{};
    EditorObjectFlags before{};
    EditorObjectFlags after{};
};

class SetObjectFlagsBatchCommand final : public IEditorCommand {
public:
    explicit SetObjectFlagsBatchCommand(std::vector<ObjectFlagsChange> changes);
    [[nodiscard]] std::string_view label() const noexcept override { return "Change object policies"; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&, bool forward);
    std::vector<ObjectFlagsChange> changes_;
};

class SetObjectFlagsCommand final : public IEditorCommand {
public:
    SetObjectFlagsCommand(EditorObjectId id, EditorObjectFlags before, EditorObjectFlags after);
    [[nodiscard]] std::string_view label() const noexcept override { return "Change object policy"; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&, const EditorObjectFlags&);
    EditorObjectId id_{};
    EditorObjectFlags before_{};
    EditorObjectFlags after_{};
};

class SetAnchorsCommand final : public IEditorCommand {
public:
    SetAnchorsCommand(EditorObjectId id, std::vector<Int3> add, std::vector<Int3> remove);
    [[nodiscard]] std::string_view label() const noexcept override { return "Paint anchors"; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&, bool forward);
    EditorObjectId id_{};
    std::vector<Int3> add_;
    std::vector<Int3> remove_;
};

// Returns a deterministic parent-before-child closure for the requested roots. Missing roots
// are omitted; commands that require strict existence validation perform that check before
// using the closure. Duplicate roots and nested roots are de-duplicated.
[[nodiscard]] std::vector<EditorObjectId> collect_editor_object_subtree_ids(
    const EditorDocument& document,
    std::span<const EditorObjectId> roots);

class CompoundCommand final : public IEditorCommand {
public:
    explicit CompoundCommand(std::string label);
    void add(std::unique_ptr<IEditorCommand> command);
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    std::string label_;
    std::vector<std::unique_ptr<IEditorCommand>> commands_;
};

// Inserts a single new object (an empty object, a freshly created voxel object, or a pasted
// clone) at a caller-chosen id. The object is taken by value at construction and moved into
// the document on first execute(); undo() clones it back out of the document before removing
// it, so redo() (default: re-runs execute()) always has a valid object to reinsert even after
// a later command modified it and was itself undone first, since command-stack undo/redo is
// strictly LIFO.
class AddObjectCommand final : public IEditorCommand {
public:
    explicit AddObjectCommand(EditorObject object, std::string label = "Add object");
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    [[nodiscard]] EditorObjectId object_id() const noexcept { return id_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    std::optional<EditorObject> pending_;
    EditorObjectId id_{};
    std::string label_;
};

// Removes one or more objects atomically: if any requested id is missing or locked, nothing
// is removed and the command fails outright (matching the existing batch-transform
// convention that a locked object invalidates the whole batch, not just its own entry).
class RemoveObjectsCommand final : public IEditorCommand {
public:
    explicit RemoveObjectsCommand(std::vector<EditorObjectId> ids, std::string label = "Delete objects");
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    [[nodiscard]] std::size_t removed_object_count() const noexcept { return resolvedIds_.size(); }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    std::vector<EditorObjectId> requestedIds_;
    std::vector<EditorObjectId> resolvedIds_;
    std::vector<EditorObjectId> removalRoots_;
    std::vector<std::optional<EditorObject>> removed_;
    std::string label_;
};

class RenameObjectCommand final : public IEditorCommand {
public:
    RenameObjectCommand(EditorObjectId id, std::string before, std::string after);
    [[nodiscard]] std::string_view label() const noexcept override { return "Rename object"; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&, const std::string& name);
    EditorObjectId id_{};
    std::string before_;
    std::string after_;
};



struct ObjectAttachmentState {
    std::optional<EditorObjectId> parent;
    std::optional<EditorAttachment> attachment;
    RigidTransform worldTransform{};
};

[[nodiscard]] std::optional<ObjectAttachmentState> capture_object_attachment_state(
    const EditorDocument& document, EditorObjectId id) noexcept;

class SetObjectAttachmentCommand final : public IEditorCommand {
public:
    SetObjectAttachmentCommand(EditorObjectId id, ObjectAttachmentState before,
                               ObjectAttachmentState after,
                               std::string label = "Change attachment");
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&, const ObjectAttachmentState&);
    EditorObjectId id_{};
    ObjectAttachmentState before_;
    ObjectAttachmentState after_;
    std::string label_;
};

class AddComponentCommand final : public IEditorCommand {
public:
    AddComponentCommand(EditorObjectId objectId, Component component,
                        std::string label = "Add component");
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    [[nodiscard]] ComponentId component_id() const noexcept { return component_.id; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    EditorObjectId objectId_{};
    Component component_;
    std::string label_;
};

class RemoveComponentCommand final : public IEditorCommand {
public:
    RemoveComponentCommand(EditorObjectId objectId, ComponentId componentId,
                           std::string label = "Remove component");
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    EditorObjectId objectId_{};
    ComponentId componentId_{};
    std::optional<Component> removed_;
    std::string label_;
};

class SetComponentEnabledCommand final : public IEditorCommand {
public:
    SetComponentEnabledCommand(EditorObjectId objectId, ComponentId componentId,
                               bool before, bool after,
                               std::string label = "Enable component");
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&, bool enabled);
    EditorObjectId objectId_{};
    ComponentId componentId_{};
    bool before_{};
    bool after_{};
    std::string label_;
};

class ReorderComponentCommand final : public IEditorCommand {
public:
    ReorderComponentCommand(EditorObjectId objectId, ComponentId componentId,
                            std::size_t beforeIndex, std::size_t afterIndex,
                            std::string label = "Reorder component");
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&, std::size_t index);
    EditorObjectId objectId_{};
    ComponentId componentId_{};
    std::size_t beforeIndex_{};
    std::size_t afterIndex_{};
    std::string label_;
};

class SetComponentPropertyCommand final : public IEditorCommand {
public:
    SetComponentPropertyCommand(EditorObjectId objectId, ComponentId componentId,
                                std::string property,
                                std::optional<ComponentValue> before,
                                std::optional<ComponentValue> after,
                                std::string label = "Edit component property");
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&, const std::optional<ComponentValue>&);
    EditorObjectId objectId_{};
    ComponentId componentId_{};
    std::string property_;
    std::optional<ComponentValue> before_;
    std::optional<ComponentValue> after_;
    std::string label_;
};

struct ObjectReparentChange {
    EditorObjectId id{};
    std::optional<EditorObjectId> before;
    std::optional<EditorObjectId> after;
};

// Batch reparent, used by both Group/Ungroup and hierarchy drag-and-drop. All-or-nothing like
// the other batch commands; EditorDocument::reparent() already rejects cycles and missing
// parents, so a partial failure here would leave some objects reparented and others not for a
// single logical drag or group action, which is worse than just refusing the whole thing.
class ReparentObjectsCommand final : public IEditorCommand {
public:
    explicit ReparentObjectsCommand(std::vector<ObjectReparentChange> changes, std::string label = "Reparent objects");
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument&) override;
    CommandResult undo(EditorDocument&) override;
private:
    CommandResult apply(EditorDocument&, bool forward);
    std::vector<ObjectReparentChange> changes_;
    std::string label_;
};

} // namespace dve::editor
