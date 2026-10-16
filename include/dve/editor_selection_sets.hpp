#pragma once

// Named selection sets (artist worklist ART-021). The sets themselves live in the document
// (EditorDocument::selection_sets(), saved with the scene); this header resolves them against the
// current scene and provides the undoable command that changes them.
//
// Component sets store persistent modeling IDs. When a topology edit replaces an element, the
// document's selection lineage records which IDs replaced it, so the pieces of a split face stay
// in the set. Lineage only grows, so a set resolves the same way after Undo and Redo.

#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "dve/editor_command.hpp"
#include "dve/editor_document.hpp"

namespace dve::editor {

struct SelectionSetResolution {
    std::set<EditorObjectId> objects;      // object and query sets
    std::set<std::uint64_t> components;    // component sets: polygon selection component IDs
    std::set<ModelingId> modelingIds;      // component sets: the live modeling IDs reached
    std::size_t members{};                 // stored members (objects or elements)
    std::size_t missing{};                 // members that resolve to nothing (deleted)
    std::size_t retired{};                 // members a topology edit retired without successors
    std::string error;                     // set cannot be used now (mesh gone, bad query)
};

[[nodiscard]] SelectionSetResolution resolve_selection_set(const EditorDocument& document, const EditorSelectionSet& set);
[[nodiscard]] const char* selection_set_kind_name(SelectionSetKind kind) noexcept;
// "Rocks  (objects, 12)" plus "  · 2 missing" / "  · 1 retired" when that applies.
[[nodiscard]] std::string describe_selection_set(const EditorDocument& document, const EditorSelectionSet& set);

// Replaces the document's selection sets as one undo step (create, update, rename, delete).
class ReplaceSelectionSetsCommand final : public IEditorCommand {
public:
    ReplaceSelectionSetsCommand(std::vector<EditorSelectionSet> before, std::vector<EditorSelectionSet> after, std::string label);
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    CommandResult execute(EditorDocument& document) override;
    CommandResult undo(EditorDocument& document) override;
    // Naming a just-saved set folds into the save: one undo step for Save + name.
    [[nodiscard]] bool merge_with(const IEditorCommand& other) override;

private:
    std::vector<EditorSelectionSet> before_;
    std::vector<EditorSelectionSet> after_;
    std::string label_;
    bool namingPending_{};
};

}  // namespace dve::editor
