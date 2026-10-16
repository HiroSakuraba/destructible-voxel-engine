// Selection masks, front-only object boxes and domain-wide grow / shrink / connected / invert
// (artist worklist ART-015, ART-016, ART-017).
#include "dve/editor_native.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace dve::editor {

bool NativeEditorController::object_selectable(EditorObjectId id) const {
    const EditorDocument& document = workspace_.document();
    const EditorObject* object = document.find_object(id);
    if (!object) return false;
    if (!selectionMask_.locked && object->flags.locked) return false;
    if (!selectionMask_.prefabs && object->prefabLink) return false;
    if (!selectionMask_.helpers && !has_own_geometry(*object) && document.children_of(id).empty()) return false;
    return true;
}

std::vector<EditorPickResult> NativeEditorController::selectable_targets_at(int x, int y) const {
    const auto rawHits = pick_editor_document_all(workspace_.document(),
        make_viewport_ray(camera_, layout_.viewport, static_cast<float>(x), static_cast<float>(y)),
        10000.0F, &viewportSettings_.isolatedObjects);
    std::vector<EditorPickResult> targets;
    std::set<EditorObjectId> seen;
    for (EditorPickResult hit : rawHits) {
        hit.objectId = viewport_selection_target(hit.objectId);
        if (!seen.insert(hit.objectId).second || !object_selectable(hit.objectId)) continue;
        targets.push_back(hit);
    }
    if (selectionMask_.helpers) {
        constexpr float kMarkerRadius = 8.0F;
        bool added = false;
        for (const HelperMarker& marker : helper_markers()) {
            const float dx = marker.x - static_cast<float>(x), dy = marker.y - static_cast<float>(y);
            if (dx * dx + dy * dy > kMarkerRadius * kMarkerRadius || !object_selectable(marker.id) || !seen.insert(marker.id).second) continue;
            EditorPickResult hit;
            hit.objectId = marker.id;
            hit.worldPosition = marker.world;
            hit.worldDistance = marker.distance;
            targets.push_back(hit);
            added = true;
        }
        if (added)
            std::stable_sort(targets.begin(), targets.end(),
                             [](const EditorPickResult& a, const EditorPickResult& b) { return a.worldDistance < b.worldDistance; });
    }
    return targets;
}

std::vector<NativeEditorController::HelperMarker> NativeEditorController::helper_markers() const {
    std::vector<HelperMarker> markers;
    if (workspace_.mode() != EditorMode::Edit || playSession_.active() || selectionDomain_ != SelectionDomain::Objects) return markers;
    const EditorDocument& document = workspace_.document();
    for (const auto& [id, object] : document.objects()) {
        if (has_own_geometry(object) || !document.children_of(id).empty() || !object.flags.visible || is_isolated_out(id) ||
            viewport_selection_target(id) != id)
            continue;
        const auto world = document.world_transform(id);
        if (!world) continue;
        const ScreenPoint point = project_world_to_screen(camera_, layout_.viewport, world->position);
        if (!point.visible || !layout_.viewport.contains(static_cast<int>(point.x), static_cast<int>(point.y))) continue;
        const float dx = world->position.x - camera_.position.x, dy = world->position.y - camera_.position.y,
                    dz = world->position.z - camera_.position.z;
        markers.push_back({id, point.x, point.y, std::sqrt(dx * dx + dy * dy + dz * dz), world->position});
    }
    return markers;
}

std::set<EditorObjectId> NativeEditorController::targets_seen_in_rect(UiRect rect, const std::set<EditorObjectId>& candidates) const {
    std::set<EditorObjectId> seen;
    if (candidates.empty() || rect.width <= 0 || rect.height <= 0) return seen;
    const EditorDocument& document = workspace_.document();
    // Whatever is nearest along the ray shows there, selectable or not (a locked wall still hides
    // what is behind it).
    const auto sample = [&](float sx, float sy) {
        const auto hit = pick_editor_document(document, make_viewport_ray(camera_, layout_.viewport, sx, sy), 10000.0F,
                                              &viewportSettings_.isolatedObjects);
        if (!hit) return;
        const EditorObjectId target = viewport_selection_target(hit->objectId);
        if (candidates.contains(target)) seen.insert(target);
    };
    // Each candidate's own centre first, so small objects between grid samples are not missed.
    for (const auto& [id, object] : document.objects()) {
        if (seen.size() == candidates.size()) return seen;
        const EditorObjectId target = viewport_selection_target(id);
        if (!candidates.contains(target) || seen.contains(target) || !has_own_geometry(object)) continue;
        const EditorObjectBounds bounds = object_world_bounds(object);
        if (!bounds.valid) continue;
        const ScreenPoint centre = project_world_to_screen(camera_, layout_.viewport,
            multiply(add(bounds.minimum, bounds.maximum), 0.5F));
        if (centre.visible && rect.contains(static_cast<int>(centre.x), static_cast<int>(centre.y))) sample(centre.x, centre.y);
    }
    // Then a grid over the box: at most about 2500 rays, never closer than 3 px.
    const double area = static_cast<double>(rect.width) * static_cast<double>(rect.height);
    const int step = std::max(3, static_cast<int>(std::ceil(std::sqrt(area / 2500.0))));
    for (int y = rect.y + step / 2; y < rect.y + rect.height; y += step)
        for (int x = rect.x + step / 2; x < rect.x + rect.width; x += step) {
            if (seen.size() == candidates.size()) return seen;
            sample(static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F);
        }
    return seen;
}

std::string NativeEditorController::marquee_label() const {
    if (!marquee_.active) return {};
    std::string label = !lassoPath_.empty() ? "Lasso" : marquee_.currentX >= marquee_.startX ? "Window" : "Crossing";
    label += viewportSettings_.xraySelection ? " \xC2\xB7 X-Ray (through)" : " \xC2\xB7 visible only";
    if (selectionDomain_ == SelectionDomain::Objects && !selectionMask_.everything()) {
        std::string off;
        const auto note = [&](bool on, const char* name) { if (!on) off += (off.empty() ? "" : ", ") + std::string(name); };
        note(selectionMask_.locked, "locked");
        note(selectionMask_.helpers, "helpers");
        note(selectionMask_.prefabs, "prefabs");
        label += " \xC2\xB7 skipping " + off;
    }
    return label;
}

bool NativeEditorController::dispatch_selection_filter_action(std::string_view action) {
    if (action.starts_with("select.mask_")) {
        if (action == "select.mask_locked") selectionMask_.locked = !selectionMask_.locked;
        else if (action == "select.mask_helpers") selectionMask_.helpers = !selectionMask_.helpers;
        else if (action == "select.mask_prefabs") selectionMask_.prefabs = !selectionMask_.prefabs;
        else if (action == "select.mask_all") selectionMask_ = {};
        else return false;
        refresh_menu_state();
        if (selectionMask_.everything()) set_status("Everything is selectable in the viewport");
        else {
            std::string off;
            const auto note = [&](bool on, const char* name) { if (!on) off += (off.empty() ? "" : ", ") + std::string(name); };
            note(selectionMask_.locked, "locked objects");
            note(selectionMask_.helpers, "helpers");
            note(selectionMask_.prefabs, "prefab instances");
            set_status("Viewport selection skips " + off + " (the hierarchy still selects them)");
        }
        return true;
    }
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) { set_status("Stop Play before changing the selection", true); return false; }
    // Voxel and mesh domains use their own commands; this only routes to them.
    if (selectionDomain_ == SelectionDomain::Voxels) {
        if (action == "edit.select_invert") return dispatch_action("voxel.select_invert");
        if (action == "edit.select_grow") return dispatch_action("voxel.select_grow");
        if (action == "edit.select_shrink") return dispatch_action("voxel.select_shrink");
        // Connected: every face-connected piece touched by the selection.
        const EditorObject* object = workspace_.document().find_object(voxelSelectionObject_);
        if (!object || !object->voxels || voxelSelection_.empty()) { set_status("Select some voxel cells first", true); return false; }
        std::set<Int3> pieces;
        for (const Int3 cell : voxelSelection_) {
            if (pieces.contains(cell)) continue;
            const auto piece = voxel_connected_piece(*object->voxels, cell);
            pieces.insert(piece.begin(), piece.end());
        }
        if (pieces.size() == voxelSelection_.size()) { set_status("The selection already covers whole pieces"); return true; }
        select_voxels(voxelSelectionObject_, pieces, SelectionOperation::Replace);
        set_status("Selected the connected pieces (" + std::to_string(pieces.size()) + " cells)");
        return true;
    }
    if (selectionDomain_ == SelectionDomain::Polygons) {
        if (action == "edit.select_invert") return dispatch_action("polygon.select_invert");
        if (action == "edit.select_grow") return dispatch_action("polygon.select_grow");
        if (action == "edit.select_shrink") return dispatch_action("polygon.select_shrink");
        return dispatch_action("polygon.select_shell");
    }
    // Objects. Candidates are what viewport selection could take: visible, not isolated out,
    // allowed by the masks, and closed groups as one target.
    const EditorDocument& document = workspace_.document();
    const auto candidate = [&](EditorObjectId id) {
        const EditorObject* object = document.find_object(id);
        return object && object->flags.visible && !is_isolated_out(id) && object_selectable(id);
    };
    const std::set<EditorObjectId> before = workspace_.selected_objects();
    std::set<EditorObjectId> after = before;
    if (action == "edit.select_invert") {
        after.clear();
        for (const auto& [id, object] : document.objects()) {
            (void)object;
            const EditorObjectId target = viewport_selection_target(id);
            if (target == id && candidate(id) && !before.contains(id)) after.insert(id);
        }
        // Closed groups whose pieces are drawn count as one target.
        for (const auto& [id, object] : document.objects()) {
            (void)object;
            const EditorObjectId target = viewport_selection_target(id);
            if (target != id && candidate(target) && !before.contains(target)) after.insert(target);
        }
    } else if (action == "edit.select_grow") {
        for (const EditorObjectId id : before)
            for (const EditorObjectId child : document.children_of(id))
                if (candidate(child)) after.insert(child);
    } else if (action == "edit.select_shrink") {
        for (const EditorObjectId id : before) {
            const EditorObject* object = document.find_object(id);
            if (object && object->parent && before.contains(*object->parent)) after.erase(id);
        }
    } else if (action == "edit.select_connected") {
        for (const EditorObjectId id : before) {
            EditorObjectId root = id;
            for (const EditorObject* object = document.find_object(root); object && object->parent;) {
                root = *object->parent;
                object = document.find_object(root);
            }
            std::vector<EditorObjectId> stack{root};
            while (!stack.empty()) {
                const EditorObjectId current = stack.back();
                stack.pop_back();
                if (candidate(current)) after.insert(current);
                for (const EditorObjectId child : document.children_of(current)) stack.push_back(child);
            }
        }
    } else return false;
    if (after == before) {
        set_status(action == "edit.select_grow" ? "Nothing more to grow into (no unselected children)"
                   : action == "edit.select_shrink" ? "Nothing to shrink (no selected object has a selected parent)"
                   : before.empty() && action == "edit.select_connected" ? "Select an object first"
                   : "The selection did not change", action != "edit.select_invert");
        return action == "edit.select_invert";
    }
    workspace_.apply_selection(after, SelectionOperation::Replace);
    if (after.empty()) workspace_.clear_selection();
    refresh_menu_state();
    set_status(std::string(action == "edit.select_invert" ? "Inverted the selection"
                           : action == "edit.select_grow" ? "Added the children"
                           : action == "edit.select_shrink" ? "Dropped children of selected parents"
                           : "Selected the whole hierarchy") + " (" + std::to_string(workspace_.selection_count()) + " objects)");
    return true;
}

}  // namespace dve::editor
