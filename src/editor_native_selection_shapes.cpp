// Lasso and paint selection (artist worklist ART-018). Edit > Selection Shape picks Box, Lasso or
// Paint for Select-tool drags; Shift+Q cycles. All three use the same set operations (Shift adds,
// Ctrl removes, Ctrl+Shift intersects; paint strokes add, or remove with Ctrl), respect the
// active domain, the selection masks and X-Ray Selection (visible only unless X-Ray).
#include "dve/editor_native.hpp"
#include "dve/voxel_metric.hpp"

#include <algorithm>
#include <cmath>

namespace dve::editor {
namespace {
using ScreenPath = std::vector<std::pair<int, int>>;

bool inside(const ScreenPath& path, float x, float y) {
    bool in = false;
    for (std::size_t i = 0, j = path.size() - 1; i < path.size(); j = i++) {
        const float xi = static_cast<float>(path[i].first), yi = static_cast<float>(path[i].second);
        const float xj = static_cast<float>(path[j].first), yj = static_cast<float>(path[j].second);
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

UiRect bounds_of(const ScreenPath& path) {
    int left = path.front().first, right = left, top = path.front().second, bottom = top;
    for (const auto& [x, y] : path) {
        left = std::min(left, x); right = std::max(right, x);
        top = std::min(top, y); bottom = std::max(bottom, y);
    }
    return {left, top, right - left + 1, bottom - top + 1};
}

Float3 component_centre(const CookedPolygonAsset& mesh, PolygonSelectionMode mode, std::uint64_t id) {
    if (mode == PolygonSelectionMode::Vertex) return mesh.vertices.at(std::size_t(id)).position;
    if (mode == PolygonSelectionMode::Edge)
        return multiply(add(mesh.vertices.at(std::size_t(id >> 32U)).position,
                            mesh.vertices.at(std::size_t(id & 0xffffffffU)).position), .5F);
    const auto i = std::size_t(id) * 3;
    return multiply(add(add(mesh.vertices[mesh.indices[i]].position, mesh.vertices[mesh.indices[i + 1]].position),
                        mesh.vertices[mesh.indices[i + 2]].position), 1.0F / 3.0F);
}

const char* shape_name(SelectionShape shape) {
    return shape == SelectionShape::Lasso ? "Lasso" : shape == SelectionShape::Paint ? "Paint" : "Box";
}
}  // namespace

bool NativeEditorController::dispatch_selection_shape_action(std::string_view action) {
    if (action == "select.paint_radius_increase" || action == "select.paint_radius_decrease") {
        const int step = std::max(2, selectionPaintRadius_ / 5);
        selectionPaintRadius_ = std::clamp(selectionPaintRadius_ + (action == "select.paint_radius_increase" ? step : -step), 4, 400);
        set_status("Paint selection radius: " + std::to_string(selectionPaintRadius_) + " px");
        return true;
    }
    SelectionShape shape = selectionShape_;
    if (action == "select.shape_box") shape = SelectionShape::Box;
    else if (action == "select.shape_lasso") shape = SelectionShape::Lasso;
    else if (action == "select.shape_paint") shape = SelectionShape::Paint;
    else if (action == "select.shape_cycle")
        shape = shape == SelectionShape::Box ? SelectionShape::Lasso : shape == SelectionShape::Lasso ? SelectionShape::Paint : SelectionShape::Box;
    else return false;
    selectionShape_ = shape;
    lassoPath_.clear();
    paintSelecting_ = false;
    // The voxel Selection Brush is the voxel domain's paint shape.
    voxelSelectionBrushEnabled_ = shape == SelectionShape::Paint && selectionDomain_ == SelectionDomain::Voxels;
    if (activeTool_ != EditorToolId::Select) set_active_tool(EditorToolId::Select);
    refresh_menu_state();
    set_status(std::string("Selection shape: ") + shape_name(shape) +
               (shape == SelectionShape::Lasso ? " (drag around things; Shift adds, Ctrl removes, Ctrl+Shift intersects)"
                : shape == SelectionShape::Paint ? " (drag over things to add, Ctrl-drag removes; Ctrl+[ / Ctrl+] size)"
                : " (left to right contains, right to left crosses)") + "; Shift+Q cycles");
    return true;
}

bool NativeEditorController::begin_shape_selection(int x, int y, std::uint32_t modifiers) {
    lassoPath_.clear();
    if (selectionShape_ == SelectionShape::Lasso) {
        lassoPath_.push_back({x, y});
        return false;  // the ordinary marquee starts too, so a click stays a click
    }
    if (selectionShape_ != SelectionShape::Paint) return false;
    if (selectionDomain_ == SelectionDomain::Voxels) {
        voxelSelectionBrushEnabled_ = true;  // handled by the voxel Selection Brush
        return false;
    }
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) return false;
    paintSelecting_ = true;
    paintOperation_ = (modifiers & 2U) != 0U ? SelectionOperation::Subtract : SelectionOperation::Add;
    lastPaintDab_.reset();
    paint_select_at(x, y);
    return true;
}

void NativeEditorController::extend_lasso(int x, int y) {
    if (lassoPath_.empty()) return;
    const auto [lx, ly] = lassoPath_.back();
    if ((x - lx) * (x - lx) + (y - ly) * (y - ly) < 9) return;  // keep the path light
    if (lassoPath_.size() < 4096U) lassoPath_.push_back({x, y});
}

bool NativeEditorController::finish_lasso_selection(int x, int y) {
    extend_lasso(x, y);
    ScreenPath path = lassoPath_;
    lassoPath_.clear();
    // A tiny gesture is a click: the ordinary click handling runs. The whole path is measured, so
    // a loop that ends where it started is still a lasso.
    if (path.size() < 3U) return false;
    if (const UiRect extent = bounds_of(path); extent.width <= 6 && extent.height <= 6) return false;
    const SelectionOperation operation = marquee_.operation;
    const UiRect box = bounds_of(path);
    const bool through = viewportSettings_.xraySelection;
    // Ray samples inside the lasso: a grid of at most about 2500, never closer than 3 px.
    std::vector<std::pair<float, float>> samples;
    {
        const double area = static_cast<double>(box.width) * static_cast<double>(box.height);
        const int step = std::max(3, static_cast<int>(std::ceil(std::sqrt(area / 2500.0))));
        for (int sy = box.y + step / 2; sy < box.y + box.height; sy += step)
            for (int sx = box.x + step / 2; sx < box.x + box.width; sx += step)
                if (inside(path, static_cast<float>(sx) + 0.5F, static_cast<float>(sy) + 0.5F))
                    samples.emplace_back(static_cast<float>(sx) + 0.5F, static_cast<float>(sy) + 0.5F);
    }
    const EditorDocument& document = workspace_.document();
    if (selectionDomain_ == SelectionDomain::Objects) {
        std::set<EditorObjectId> hits;
        const auto take = [&](float sx, float sy) {
            const ViewportRay ray = make_viewport_ray(camera_, layout_.viewport, sx, sy);
            const auto rayHits = pick_editor_document_all(document, ray, 10000.0F, &viewportSettings_.isolatedObjects);
            for (const EditorPickResult& hit : rayHits) {
                const EditorObjectId target = viewport_selection_target(hit.objectId);
                if (object_selectable(target)) hits.insert(target);
                if (!through) break;  // the nearest surface hides the rest
            }
        };
        for (const auto& [sx, sy] : samples) take(sx, sy);
        // Small objects between samples: a ray through each object's centre when it lies inside.
        for (const auto& [id, object] : document.objects()) {
            if (!object.flags.visible || is_isolated_out(id) || !has_own_geometry(object)) continue;
            const EditorObjectBounds bounds = object_world_bounds(object);
            if (!bounds.valid) continue;
            const ScreenPoint centre = project_world_to_screen(camera_, layout_.viewport, multiply(add(bounds.minimum, bounds.maximum), 0.5F));
            if (!centre.visible || !inside(path, centre.x, centre.y)) continue;
            if (through) { const EditorObjectId target = viewport_selection_target(id); if (object_selectable(target)) hits.insert(target); }
            else take(centre.x, centre.y);
        }
        workspace_.apply_selection(hits, operation);
        refresh_menu_state();
        set_status("Lasso: " + std::to_string(workspace_.selection_count()) + " objects selected" +
                   (through ? " (X-Ray: through)" : " (visible only)"));
        return true;
    }
    if (selectionDomain_ == SelectionDomain::Voxels) {
        EditorObjectId target = voxelSelectionObject_;
        if (!document.find_object(target)) { const auto active = workspace_.selected_object(); target = active ? *active : 0; }
        const EditorObject* object = document.find_object(target);
        if (!object || !object->voxels || object->polygon || object->text3d || object->gaborVolume) {
            set_status("Lasso voxels of an object: click one of its voxels first", true);
            return true;
        }
        std::string error;
        auto cells = voxels_in_screen_rect(*object, camera_, layout_.viewport, box, through, true, &document,
                                           &viewportSettings_.isolatedObjects, &error);
        if (!error.empty()) { set_status(error, true, 6.0F); return true; }
        std::erase_if(cells, [&](const Int3& cell) {
            const float size = object->voxelSizeMeters;
            const Float3 local{(static_cast<float>(cell.x) + 0.5F) * size, (static_cast<float>(cell.y) + 0.5F) * size,
                               (static_cast<float>(cell.z) + 0.5F) * size};
            const ScreenPoint p = project_world_to_screen(camera_, layout_.viewport, voxel_to_world(object->transform, object->voxelScale, local));
            return !p.visible || !inside(path, p.x, p.y);
        });
        select_voxels(object->id, cells, operation);
        set_status("Lasso: " + std::to_string(voxelSelection_.size()) + " cells selected" + (through ? " (X-Ray: through)" : " (visible only)"));
        return true;
    }
    // Mesh components.
    prune_polygon_selection();
    const EditorObject* object = document.find_object(polygonSelection_.object);
    if (!object || !object->polygon || !object->flags.visible || object->flags.locked) return true;
    const bool componentsThrough = through || (object->subdivision && subdivisionCageXray_);
    auto hits = polygon_components_in_rect(*object, camera_, layout_.viewport, box, polygonSelection_.mode, componentsThrough, true,
                                           &document, &viewportSettings_.isolatedObjects);
    std::erase_if(hits, [&](std::uint64_t id) {
        const ScreenPoint p = project_world_to_screen(camera_, layout_.viewport,
            voxel_to_world(object->transform, object->voxelScale, component_centre(*object->polygon, polygonSelection_.mode, id)));
        return !p.visible || !inside(path, p.x, p.y);
    });
    select_polygon_components(hits, operation);
    set_status("Lasso: " + std::to_string(polygonSelection_.components.size()) + " components selected");
    return true;
}

void NativeEditorController::paint_select_at(int x, int y) {
    // Dabs every third of a radius along the drag so fast strokes leave no gaps.
    if (lastPaintDab_) {
        const float dx = static_cast<float>(x - lastPaintDab_->first), dy = static_cast<float>(y - lastPaintDab_->second);
        const float distance = std::sqrt(dx * dx + dy * dy);
        const float spacing = std::max(2.0F, static_cast<float>(selectionPaintRadius_) / 3.0F);
        if (distance < spacing) return;
        const int steps = std::min(64, static_cast<int>(distance / spacing));
        for (int i = 1; i < steps; ++i)
            paint_select_dab(lastPaintDab_->first + static_cast<int>(dx * static_cast<float>(i) / static_cast<float>(steps)),
                             lastPaintDab_->second + static_cast<int>(dy * static_cast<float>(i) / static_cast<float>(steps)));
    }
    paint_select_dab(x, y);
    lastPaintDab_ = std::pair{x, y};
}

void NativeEditorController::paint_select_dab(int x, int y) {
    const float r = static_cast<float>(selectionPaintRadius_);
    const bool through = viewportSettings_.xraySelection;
    const EditorDocument& document = workspace_.document();
    if (selectionDomain_ == SelectionDomain::Objects) {
        // The centre plus two rings of rays across the brush.
        std::set<EditorObjectId> hits;
        for (int ring = 0; ring <= 2; ++ring) {
            const int count = ring == 0 ? 1 : 8 * ring;
            for (int i = 0; i < count; ++i) {
                const float angle = 6.2831853F * static_cast<float>(i) / static_cast<float>(count);
                const float distance = r * static_cast<float>(ring) / 2.0F;
                const ViewportRay ray = make_viewport_ray(camera_, layout_.viewport, static_cast<float>(x) + 0.5F + distance * std::cos(angle),
                                                          static_cast<float>(y) + 0.5F + distance * std::sin(angle));
                for (const EditorPickResult& hit : pick_editor_document_all(document, ray, 10000.0F, &viewportSettings_.isolatedObjects)) {
                    const EditorObjectId target = viewport_selection_target(hit.objectId);
                    if (object_selectable(target)) hits.insert(target);
                    if (!through) break;
                }
            }
        }
        if (hits.empty()) return;
        workspace_.apply_selection(hits, paintOperation_);
        refresh_menu_state();
        set_status("Paint selection: " + std::to_string(workspace_.selection_count()) + " objects" + (paintOperation_ == SelectionOperation::Subtract ? " (removing)" : ""));
        return;
    }
    if (selectionDomain_ != SelectionDomain::Polygons) return;
    prune_polygon_selection();
    const EditorObject* object = document.find_object(polygonSelection_.object);
    if (!object || !object->polygon || !object->flags.visible || object->flags.locked) return;
    const int ri = selectionPaintRadius_;
    const UiRect rect{x - ri, y - ri, 2 * ri + 1, 2 * ri + 1};
    const bool componentsThrough = through || (object->subdivision && subdivisionCageXray_);
    auto hits = polygon_components_in_rect(*object, camera_, layout_.viewport, rect, polygonSelection_.mode, componentsThrough, true,
                                           &document, &viewportSettings_.isolatedObjects);
    std::erase_if(hits, [&](std::uint64_t id) {
        const ScreenPoint p = project_world_to_screen(camera_, layout_.viewport,
            voxel_to_world(object->transform, object->voxelScale, component_centre(*object->polygon, polygonSelection_.mode, id)));
        const float ddx = p.x - static_cast<float>(x), ddy = p.y - static_cast<float>(y);
        return !p.visible || ddx * ddx + ddy * ddy > r * r;
    });
    if (hits.empty()) return;
    select_polygon_components(hits, paintOperation_);
    set_status("Paint selection: " + std::to_string(polygonSelection_.components.size()) + " components" +
               (paintOperation_ == SelectionOperation::Subtract ? " (removing)" : ""));
}

}  // namespace dve::editor
