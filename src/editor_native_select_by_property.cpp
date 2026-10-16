// Select by property (artist worklist ART-019): Edit > Select Similar (same type, material, tag,
// layer or prefab as the selection) and Edit > Select Matching Filter (everything the hierarchy
// filter matches, plain names or key:value queries).
#include "dve/editor_native.hpp"

#include <algorithm>

namespace dve::editor {
namespace {
std::string lower_text(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
}  // namespace

std::vector<EditorObjectId> NativeEditorController::objects_matching_filter(std::string_view text, std::string* error) const {
    std::vector<EditorObjectId> result;
    const EditorDocument& document = workspace_.document();
    if (looks_like_selection_query(text)) {
        const auto query = parse_selection_query(text, error);
        if (!query) return result;
        for (const auto& [id, object] : document.objects())
            if (selection_query_matches(document, object, *query)) result.push_back(id);
        return result;
    }
    const std::string needle = lower_text(text);
    for (const auto& [id, object] : document.objects())
        if (lower_text(object.name).find(needle) != std::string::npos) result.push_back(id);
    return result;
}

void NativeEditorController::set_hierarchy_filter(std::string text) {
    hierarchyFilter_ = std::move(text);
    hierarchyScroll_ = 0;
    recompute_layout();
}

bool NativeEditorController::dispatch_select_by_property_action(std::string_view action) {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) { set_status("Stop Play before changing the selection", true); return false; }
    const EditorDocument& document = workspace_.document();
    if (action == "select.matching") {
        if (hierarchyFilter_.empty()) {
            set_status("Type a filter in the hierarchy first, e.g. type:mesh tag:rock or a name", true);
            return false;
        }
        std::string error;
        const auto matches = objects_matching_filter(hierarchyFilter_, &error);
        if (!error.empty()) { set_status(error, true); return false; }
        if (selectionDomain_ != SelectionDomain::Objects) set_selection_domain(SelectionDomain::Objects);
        workspace_.clear_selection();
        workspace_.apply_selection(std::set<EditorObjectId>(matches.begin(), matches.end()), SelectionOperation::Replace);
        refresh_menu_state();
        set_status("Selected " + std::to_string(workspace_.selection_count()) + " objects matching \"" + hierarchyFilter_ + "\"");
        return true;
    }
    // Select Similar: compare against every selected object; candidates are what viewport
    // selection could take (visible, not isolated out, allowed by the masks).
    const auto& selected = workspace_.selected_objects();
    if (selected.empty()) { set_status("Select an object first; Select Similar finds others like it", true); return false; }
    std::set<std::string> kinds, materials, tags, prefabs;
    std::set<std::uint32_t> layers;
    for (const EditorObjectId id : selected) {
        const EditorObject* object = document.find_object(id);
        if (!object) continue;
        kinds.insert(selection_object_kind(document, *object));
        if (action == "select.similar_material") {
            const auto used = selection_object_materials(*object);
            materials.insert(used.begin(), used.end());
        }
        for (const std::string& tag : object->tags) tags.insert(lower_text(tag));
        layers.insert(object->layer);
        if (object->prefabLink) prefabs.insert(object->prefabLink->prefabAsset.generic_string());
    }
    const char* what = action == "select.similar_type" ? "type" : action == "select.similar_material" ? "material"
                     : action == "select.similar_tag" ? "tag" : action == "select.similar_layer" ? "layer"
                     : action == "select.similar_prefab" ? "prefab" : nullptr;
    if (!what) return false;
    if (action == "select.similar_material" && materials.empty()) { set_status("The selection has no materials to compare", true); return false; }
    if (action == "select.similar_tag" && tags.empty()) { set_status("The selection has no tags", true); return false; }
    if (action == "select.similar_prefab" && prefabs.empty()) { set_status("The selection holds no prefab instances", true); return false; }
    std::set<EditorObjectId> matches;
    std::size_t insideClosedGroups = 0;
    for (const auto& [id, object] : document.objects()) {
        if (!object.flags.visible || is_isolated_out(id) || !object_selectable(id)) continue;
        // Pieces inside a closed group are not reachable from the viewport; open the group first.
        bool hit = false;
        if (action == "select.similar_type") hit = kinds.contains(selection_object_kind(document, object));
        else if (action == "select.similar_material") {
            const auto used = selection_object_materials(object);
            hit = std::any_of(used.begin(), used.end(), [&](const std::string& m) { return materials.contains(m); });
        } else if (action == "select.similar_tag")
            hit = std::any_of(object.tags.begin(), object.tags.end(), [&](const std::string& tag) { return tags.contains(lower_text(tag)); });
        else if (action == "select.similar_layer") hit = layers.contains(object.layer);
        else hit = object.prefabLink && prefabs.contains(object.prefabLink->prefabAsset.generic_string());
        if (!hit) continue;
        if (viewport_selection_target(id) != id) { if (!selected.contains(id)) ++insideClosedGroups; }
        else matches.insert(id);
    }
    matches.insert(selected.begin(), selected.end());
    workspace_.apply_selection(matches, SelectionOperation::Replace);
    refresh_menu_state();
    set_status("Selected " + std::to_string(workspace_.selection_count()) + " objects with the same " + what +
               (insideClosedGroups == 0 ? std::string()
                                        : " (" + std::to_string(insideClosedGroups) + " inside closed groups skipped; Open Group to include them)"));
    return true;
}

}  // namespace dve::editor
