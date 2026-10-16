#include "dve/editor_selection_sets.hpp"

#include <algorithm>
#include <cctype>

#include "dve/editor_polygon.hpp"
#include "dve/editor_selection_query.hpp"

namespace dve::editor {
namespace {
std::set<ModelingId> live_ids(const ModelingTopology& topology, SelectionSetKind kind) {
    std::set<ModelingId> ids;
    if (kind == SelectionSetKind::Vertices) ids.insert(topology.vertices.begin(), topology.vertices.end());
    else if (kind == SelectionSetKind::Edges) for (const auto& [edge, id] : topology.edges) { (void)edge; ids.insert(id); }
    else ids.insert(topology.faces.begin(), topology.faces.end());
    return ids;
}
std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
}  // namespace

const char* selection_set_kind_name(SelectionSetKind kind) noexcept {
    switch (kind) {
        case SelectionSetKind::Objects: return "objects";
        case SelectionSetKind::Vertices: return "vertices";
        case SelectionSetKind::Edges: return "edges";
        case SelectionSetKind::Faces: return "faces";
        case SelectionSetKind::Query: return "query";
    }
    return "objects";
}

SelectionSetResolution resolve_selection_set(const EditorDocument& document, const EditorSelectionSet& set) {
    SelectionSetResolution result;
    if (set.kind == SelectionSetKind::Objects) {
        result.members = set.objects.size();
        for (const EditorObjectId id : set.objects) {
            if (document.find_object(id)) result.objects.insert(id);
            else ++result.missing;
        }
        return result;
    }
    if (set.kind == SelectionSetKind::Query) {
        if (looks_like_selection_query(set.query)) {
            const auto query = parse_selection_query(set.query, &result.error);
            if (!query) return result;
            for (const auto& [id, object] : document.objects())
                if (selection_query_matches(document, object, *query)) result.objects.insert(id);
        } else {
            const std::string needle = lower(set.query);
            for (const auto& [id, object] : document.objects())
                if (lower(object.name).find(needle) != std::string::npos) result.objects.insert(id);
        }
        result.members = result.objects.size();
        return result;
    }
    result.members = set.elements.size();
    const EditorObject* object = document.find_object(set.mesh);
    if (!object || !object->polygon) {
        result.error = "The set's mesh no longer exists";
        result.missing = set.elements.size();
        return result;
    }
    if (!object->modelingTopology) {
        result.error = "The set's mesh has lost its element identities";
        result.missing = set.elements.size();
        return result;
    }
    const std::set<ModelingId> live = live_ids(*object->modelingTopology, set.kind);
    const auto lineageAt = document.selection_lineage().find(set.mesh);
    const std::map<ModelingId, std::set<ModelingId>>* lineage =
        lineageAt == document.selection_lineage().end() ? nullptr : &lineageAt->second;
    for (const ModelingId member : set.elements) {
        // Follow the lineage until live IDs are reached ("pieces stay in the set").
        std::set<ModelingId> reached, visited;
        bool retiredOnTheWay = false;
        std::vector<ModelingId> stack{member};
        while (!stack.empty()) {
            const ModelingId id = stack.back();
            stack.pop_back();
            if (!visited.insert(id).second) continue;
            if (live.contains(id)) { reached.insert(id); continue; }
            if (!lineage) continue;
            const auto next = lineage->find(id);
            if (next == lineage->end()) continue;
            if (next->second.empty()) retiredOnTheWay = true;
            for (const ModelingId n : next->second) stack.push_back(n);
        }
        if (reached.empty()) { retiredOnTheWay ? ++result.retired : ++result.missing; continue; }
        result.modelingIds.insert(reached.begin(), reached.end());
    }
    const auto mode = set.kind == SelectionSetKind::Vertices ? PolygonSelectionMode::Vertex
                    : set.kind == SelectionSetKind::Edges ? PolygonSelectionMode::Edge : PolygonSelectionMode::Face;
    result.components = resolve_modeling_selection(*object->polygon, *object->modelingTopology, std::uint8_t(mode), result.modelingIds);
    return result;
}

std::string describe_selection_set(const EditorDocument& document, const EditorSelectionSet& set) {
    const SelectionSetResolution resolved = resolve_selection_set(document, set);
    std::string text = set.name + "  (" + selection_set_kind_name(set.kind) + ", ";
    if (set.kind == SelectionSetKind::Query) text += set.query + " = " + std::to_string(resolved.objects.size());
    else if (set.kind == SelectionSetKind::Objects) text += std::to_string(resolved.objects.size());
    else {
        const EditorObject* mesh = document.find_object(set.mesh);
        text += std::to_string(resolved.modelingIds.size()) + " on " + (mesh ? mesh->name : std::string("missing mesh"));
    }
    text += ")";
    if (resolved.missing) text += "  \xC2\xB7 " + std::to_string(resolved.missing) + " missing";
    if (resolved.retired) text += "  \xC2\xB7 " + std::to_string(resolved.retired) + " retired";
    if (!resolved.error.empty() && set.kind == SelectionSetKind::Query) text += "  \xC2\xB7 " + resolved.error;
    return text;
}

ReplaceSelectionSetsCommand::ReplaceSelectionSetsCommand(std::vector<EditorSelectionSet> before, std::vector<EditorSelectionSet> after,
                                                         std::string label)
    : before_(std::move(before)), after_(std::move(after)), label_(std::move(label)), namingPending_(label_.starts_with("Save")) {}

bool ReplaceSelectionSetsCommand::merge_with(const IEditorCommand& other) {
    const auto* next = dynamic_cast<const ReplaceSelectionSetsCommand*>(&other);
    if (!next || !namingPending_ || next->label_ != "Rename selection set" || next->before_ != after_) return false;
    after_ = next->after_;
    namingPending_ = false;
    return true;
}

CommandResult ReplaceSelectionSetsCommand::execute(EditorDocument& document) {
    document.set_selection_sets(after_);
    return CommandResult::ok();
}

CommandResult ReplaceSelectionSetsCommand::undo(EditorDocument& document) {
    document.set_selection_sets(before_);
    return CommandResult::ok();
}

}  // namespace dve::editor
