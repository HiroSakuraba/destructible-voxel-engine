// Named selection sets (artist worklist ART-021): Edit > Selection Sets.
//   Save Selection as Set       objects, or the vertices / edges / faces of the edited mesh
//   Save Filter as Smart Set    the hierarchy filter (name or query), re-evaluated on recall
//   Recall / Add Selection To / Rename / Delete Set...   pick a set from a list
// Every change is one undo step and the sets are saved with the scene. Component sets keep
// persistent element IDs and follow topology edits through the document's selection lineage.
#include "dve/editor_native.hpp"
#include "dve/editor_selection_sets.hpp"

#include <algorithm>
#include <charconv>

namespace dve::editor {
namespace {
std::optional<std::size_t> parse_index(std::string_view text) {
    std::size_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

std::string unique_name(const std::vector<EditorSelectionSet>& sets, const std::string& base) {
    const auto taken = [&](const std::string& name) {
        return std::any_of(sets.begin(), sets.end(), [&](const EditorSelectionSet& set) { return set.name == name; });
    };
    for (std::size_t n = 1;; ++n) {
        std::string name = base + " " + std::to_string(n);
        if (!taken(name)) return name;
    }
}
}  // namespace

void NativeEditorController::note_selection_lineage(EditorObjectId mesh, const std::map<ModelingId, std::set<ModelingId>>& descendants) {
    const auto& sets = workspace_.document().selection_sets();
    const bool referenced = std::any_of(sets.begin(), sets.end(), [&](const EditorSelectionSet& set) {
        return set.mesh == mesh && set.kind != SelectionSetKind::Objects && set.kind != SelectionSetKind::Query;
    });
    if (referenced) workspace_.document().record_selection_lineage(mesh, descendants);
}

bool NativeEditorController::change_selection_sets(std::vector<EditorSelectionSet> after, std::string label) {
    std::vector<EditorSelectionSet> before = workspace_.document().selection_sets();
    const CommandResult result = workspace_.commands().execute(
        workspace_.document(), std::make_unique<ReplaceSelectionSetsCommand>(std::move(before), std::move(after), std::move(label)));
    if (!result.success) { set_status(result.message, true); return false; }
    refresh_menu_state();
    return true;
}

void NativeEditorController::open_selection_set_list(std::string_view verb) {
    const auto& sets = workspace_.document().selection_sets();
    if (sets.empty()) { set_status("No selection sets yet: Edit > Selection Sets > Save Selection as Set", true); return; }
    contextMenu_ = {};
    contextMenu_.open = true;
    const int itemHeight = 24;  // as the pick list (kLogicalLayoutScale is 1)
    const int popupWidth = 420;
    contextMenu_.width = popupWidth;
    const std::size_t count = std::min<std::size_t>(sets.size(), 20U);
    const UiRect viewport = layout_.viewport;
    contextMenu_.x = std::clamp(viewport.x + viewport.width / 2 - popupWidth / 2, 0, std::max(0, width_ - popupWidth));
    contextMenu_.y = std::clamp(viewport.y + 40, layout_.menuBar.height,
                                std::max(layout_.menuBar.height, height_ - itemHeight * static_cast<int>(count)));
    for (std::size_t index = 0; index < count; ++index) {
        contextMenu_.items.push_back({"selection.set." + std::string(verb) + "." + std::to_string(index),
                                      describe_selection_set(workspace_.document(), sets[index])});
        contextMenu_.itemRects.push_back({contextMenu_.x, contextMenu_.y + static_cast<int>(index) * itemHeight, popupWidth, itemHeight});
    }
    set_status(verb == "recall" ? "Recall which set?" : verb == "add" ? "Add the selection to which set?"
               : verb == "rename" ? "Rename which set?" : "Delete which set?");
}

std::optional<EditorSelectionSet> NativeEditorController::selection_as_set() {
    EditorSelectionSet set;
    if (selectionDomain_ == SelectionDomain::Polygons) {
        prune_polygon_selection();
        EditorObject* object = workspace_.document().find_object(polygonSelection_.object);
        if (!object || !object->polygon || polygonSelection_.components.empty()) {
            set_status("Select some vertices, edges or faces first", true);
            return std::nullopt;
        }
        std::string error;
        if (!ensure_modeling_topology(*object, &error)) {
            set_status("This mesh cannot keep element identities: " + error, true);
            return std::nullopt;
        }
        set.kind = polygonSelection_.mode == PolygonSelectionMode::Vertex ? SelectionSetKind::Vertices
                 : polygonSelection_.mode == PolygonSelectionMode::Edge ? SelectionSetKind::Edges : SelectionSetKind::Faces;
        set.mesh = object->id;
        set.elements = modeling_selection_ids(*object->polygon, *object->modelingTopology, polygonSelection_);
        if (set.elements.empty()) { set_status("These components have no persistent identities", true); return std::nullopt; }
        set.name = object->name + " " + selection_set_kind_name(set.kind);
        return set;
    }
    if (selectionDomain_ == SelectionDomain::Voxels) {
        set_status("Voxel cells cannot be saved as a set yet; save objects or mesh components", true);
        return std::nullopt;
    }
    if (workspace_.selected_objects().empty()) { set_status("Select something first", true); return std::nullopt; }
    set.kind = SelectionSetKind::Objects;
    set.objects = workspace_.selected_objects();
    set.name = "Objects";
    return set;
}

bool NativeEditorController::recall_selection_set(std::size_t index) {
    const auto& sets = workspace_.document().selection_sets();
    if (index >= sets.size()) return false;
    const EditorSelectionSet set = sets[index];
    const SelectionSetResolution resolved = resolve_selection_set(workspace_.document(), set);
    if (!resolved.error.empty()) { set_status("\"" + set.name + "\": " + resolved.error, true); return false; }
    std::string note;
    if (resolved.missing) note += ", " + std::to_string(resolved.missing) + " missing";
    if (resolved.retired) note += ", " + std::to_string(resolved.retired) + " retired";
    if (set.kind == SelectionSetKind::Objects || set.kind == SelectionSetKind::Query) {
        if (selectionDomain_ != SelectionDomain::Objects) set_selection_domain(SelectionDomain::Objects);
        workspace_.clear_selection();
        workspace_.apply_selection(resolved.objects, SelectionOperation::Replace);
        refresh_menu_state();
        set_status("Recalled \"" + set.name + "\": " + std::to_string(workspace_.selection_count()) + " objects" + note);
        return true;
    }
    if (resolved.components.empty()) { set_status("\"" + set.name + "\" has no members left" + note, true); return false; }
    workspace_.select_object(set.mesh);
    const PolygonSelectionMode mode = set.kind == SelectionSetKind::Vertices ? PolygonSelectionMode::Vertex
                                    : set.kind == SelectionSetKind::Edges ? PolygonSelectionMode::Edge : PolygonSelectionMode::Face;
    if (!set_polygon_mode(mode)) return false;
    select_polygon_components(resolved.components, SelectionOperation::Replace);
    set_status("Recalled \"" + set.name + "\": " + std::to_string(resolved.modelingIds.size()) + " " + selection_set_kind_name(set.kind) + note);
    return true;
}

bool NativeEditorController::dispatch_selection_set_action(std::string_view action) {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) { set_status("Stop Play before using selection sets", true); return false; }
    std::vector<EditorSelectionSet> sets = workspace_.document().selection_sets();
    if (action == "select.set_save") {
        auto set = selection_as_set();
        if (!set) return false;
        set->name = unique_name(sets, set->name);
        const std::string name = set->name;
        sets.push_back(std::move(*set));
        if (!change_selection_sets(std::move(sets), "Save selection set")) return false;
        // Offer a name straight away; Enter keeps the typed one, Esc keeps the default.
        begin_text_edit(TextEditKind::SelectionSetName, static_cast<EditorObjectId>(workspace_.document().selection_sets().size() - 1U), name);
        set_status("Saved \"" + name + "\"; type a name and press Enter (Esc keeps it)");
        return true;
    }
    if (action == "select.set_save_query") {
        if (hierarchyFilter_.empty()) { set_status("Type a filter in the hierarchy first, e.g. type:mesh tag:rock", true); return false; }
        std::string error;
        (void)objects_matching_filter(hierarchyFilter_, &error);
        if (!error.empty()) { set_status(error, true); return false; }
        EditorSelectionSet set;
        set.kind = SelectionSetKind::Query;
        set.query = hierarchyFilter_;
        set.name = unique_name(sets, "Query");
        const std::string name = set.name;
        sets.push_back(std::move(set));
        if (!change_selection_sets(std::move(sets), "Save smart selection set")) return false;
        begin_text_edit(TextEditKind::SelectionSetName, static_cast<EditorObjectId>(workspace_.document().selection_sets().size() - 1U), name);
        set_status("Saved \"" + name + "\" (" + hierarchyFilter_ + "); type a name and press Enter");
        return true;
    }
    if (action == "select.set_recall") { open_selection_set_list("recall"); return !sets.empty(); }
    if (action == "select.set_add") { open_selection_set_list("add"); return !sets.empty(); }
    if (action == "select.set_rename") { open_selection_set_list("rename"); return !sets.empty(); }
    if (action == "select.set_delete") { open_selection_set_list("delete"); return !sets.empty(); }
    // List picks: selection.set.<verb>.<index>
    const std::string_view prefix = "selection.set.";
    if (!action.starts_with(prefix)) return false;
    const std::string_view rest = action.substr(prefix.size());
    const std::size_t dot = rest.find('.');
    if (dot == std::string_view::npos) return false;
    const std::string_view verb = rest.substr(0, dot);
    const auto index = parse_index(rest.substr(dot + 1));
    if (!index || *index >= sets.size()) { set_status("That selection set no longer exists", true); return false; }
    if (verb == "recall") return recall_selection_set(*index);
    if (verb == "delete") {
        const std::string name = sets[*index].name;
        sets.erase(sets.begin() + static_cast<std::ptrdiff_t>(*index));
        if (!change_selection_sets(std::move(sets), "Delete selection set")) return false;
        set_status("Deleted \"" + name + "\" (Undo brings it back)");
        return true;
    }
    if (verb == "rename") {
        begin_text_edit(TextEditKind::SelectionSetName, static_cast<EditorObjectId>(*index), sets[*index].name);
        set_status("Rename \"" + sets[*index].name + "\": type a name and press Enter");
        return true;
    }
    if (verb == "add") {
        EditorSelectionSet& target = sets[*index];
        if (target.kind == SelectionSetKind::Query) { set_status("A smart set follows its query; save a new filter instead", true); return false; }
        auto current = selection_as_set();
        if (!current) return false;
        if (current->kind != target.kind || current->mesh != target.mesh) {
            set_status(std::string("\"") + target.name + "\" holds " + selection_set_kind_name(target.kind) +
                       (target.kind == SelectionSetKind::Objects ? "" : " of another mesh or mode") + "; select the same kind first", true);
            return false;
        }
        const std::size_t before = target.objects.size() + target.elements.size();
        target.objects.insert(current->objects.begin(), current->objects.end());
        target.elements.insert(current->elements.begin(), current->elements.end());
        const std::size_t added = target.objects.size() + target.elements.size() - before;
        const std::string name = target.name;
        if (added == 0) { set_status("\"" + name + "\" already holds the whole selection"); return true; }
        if (!change_selection_sets(std::move(sets), "Add to selection set")) return false;
        set_status("Added " + std::to_string(added) + " to \"" + name + "\"");
        return true;
    }
    return false;
}

bool NativeEditorController::commit_selection_set_name() {
    const std::size_t index = static_cast<std::size_t>(textEdit_.objectId);
    std::string name = textEdit_.buffer;
    textEdit_ = {};
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    std::vector<EditorSelectionSet> sets = workspace_.document().selection_sets();
    if (index >= sets.size() || name.empty() || sets[index].name == name) return false;
    if (std::any_of(sets.begin(), sets.end(), [&](const EditorSelectionSet& set) { return set.name == name; })) {
        set_status("A set called \"" + name + "\" already exists", true);
        return false;
    }
    const std::string old = sets[index].name;
    sets[index].name = name;
    if (!change_selection_sets(std::move(sets), "Rename selection set")) return false;
    set_status("Renamed \"" + old + "\" to \"" + name + "\"");
    return true;
}

}  // namespace dve::editor
