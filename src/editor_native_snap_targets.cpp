// Point snapping (artist worklist ART-025, ART-026).
//
// Hold V during a Move drag and the selection's pivot jumps to the nearest target under the
// cursor: a mesh vertex, an edge midpoint, a face centre, a voxel or collision-box corner, an
// object's bounds corner, or the surface itself (with the surface offset, and turned to the normal
// when Align Up to Surface Normal is on). The viewport draws the source pivot, the target and a
// line between them. The same search backs the Snap to Mesh Vertex / Edge / Face placement modes
// and View > Measure. CAD objects whose exact shape has been read (brepkernel describe) snap to
// their exact corners, edge midpoints and circle centres instead of their triangles.
#include "dve/editor_native.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "dve/collision_proxy.hpp"
#include "dve/editor_exact_cad.hpp"
#include "dve/voxel_metric.hpp"

namespace dve::editor {
namespace {
constexpr float kSnapRadiusPixels = 14.0F;

int rank(SnapTargetKind kind) {
    switch (kind) {
        case SnapTargetKind::Vertex: return 0;
        case SnapTargetKind::VoxelCorner: return 1;
        case SnapTargetKind::CollisionVertex: return 1;
        case SnapTargetKind::EdgeMidpoint: return 2;
        case SnapTargetKind::FaceCenter: return 3;
        case SnapTargetKind::BoundsVertex: return 4;
        case SnapTargetKind::Surface: return 5;
        case SnapTargetKind::CircleCentre: return 1;
    }
    return 5;
}

// Turn `rotation` so its local up (+Y) follows `normal`.
Quaternion align_up_to(Quaternion rotation, Float3 normal) {
    const Float3 n = normalize(normal);
    const Float3 up = rotate(rotation, {0.0F, 1.0F, 0.0F});
    const float d = dot(up, n);
    const Float3 axis{up.y * n.z - up.z * n.y, up.z * n.x - up.x * n.z, up.x * n.y - up.y * n.x};
    if (d < -0.999F) {
        const Float3 fallback = std::abs(up.x) < 0.9F ? Float3{1.0F, 0.0F, 0.0F} : Float3{0.0F, 0.0F, 1.0F};
        const Float3 opposite{up.y * fallback.z - up.z * fallback.y, up.z * fallback.x - up.x * fallback.z,
                              up.x * fallback.y - up.y * fallback.x};
        return normalize(multiply(quaternion_from_axis_angle(normalize(opposite), 3.14159265F), rotation));
    }
    return normalize(multiply(normalize(Quaternion{axis.x, axis.y, axis.z, 1.0F + d}), rotation));
}
}  // namespace

const char* snap_target_name(SnapTargetKind kind) noexcept {
    switch (kind) {
        case SnapTargetKind::Vertex: return "vertex";
        case SnapTargetKind::EdgeMidpoint: return "edge midpoint";
        case SnapTargetKind::FaceCenter: return "face centre";
        case SnapTargetKind::VoxelCorner: return "voxel corner";
        case SnapTargetKind::CollisionVertex: return "collision corner";
        case SnapTargetKind::BoundsVertex: return "bounds corner";
        case SnapTargetKind::Surface: return "surface";
        case SnapTargetKind::CircleCentre: return "circle centre";
    }
    return "surface";
}

std::set<EditorObjectId> NativeEditorController::snap_excluded_objects() const {
    const EditorDocument& document = workspace_.document();
    // What moves is never a target: the selection, the pieces of selected (closed) groups and
    // children attached to a selected object; when moving mesh components, the mesh being edited.
    std::set<EditorObjectId> moving;
    if (selectionDomain_ == SelectionDomain::Polygons) moving.insert(polygonSelection_.object);
    for (const ObjectTransformChange& change : selection_transform_snapshot()) moving.insert(change.id);
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& [id, object] : document.objects())
            if (object.attachment && object.parent && moving.contains(*object.parent) && moving.insert(id).second) grew = true;
    }
    return moving;
}

std::optional<SnapTarget> NativeEditorController::find_snap_target(int x, int y, std::optional<SnapTargetKind> only,
                                                                   bool includeSelection) const {
    const EditorDocument& document = workspace_.document();
    // Pivot to Point / Edit Pivot snap onto the object itself, so nothing is excluded there.
    const std::set<EditorObjectId> moving = includeSelection ? std::set<EditorObjectId>{} : snap_excluded_objects();
    const auto excluded = [&](EditorObjectId id) { return moving.contains(id); };
    const float cx = static_cast<float>(x), cy = static_cast<float>(y);
    const float radius = kSnapRadiusPixels;
    // Probe rays: the cursor, plus a ring inside the snap radius so corners on an object's
    // silhouette are found even when the cursor is just off it.
    const auto probe = [&](float px, float py) -> std::optional<EditorPickResult> {
        const auto hits = pick_editor_document_all(document, make_viewport_ray(camera_, layout_.viewport, px, py), 10000.0F,
                                                   &viewportSettings_.isolatedObjects);
        for (const EditorPickResult& h : hits) if (!excluded(h.objectId)) return h;
        return std::nullopt;
    };
    const std::optional<EditorPickResult> centreHit = probe(cx + 0.5F, cy + 0.5F);
    std::vector<EditorPickResult> probes;
    if (centreHit) probes.push_back(*centreHit);
    for (int i = 0; i < 8; ++i) {
        const float angle = 0.785398163F * static_cast<float>(i);
        if (auto h = probe(cx + 0.5F + radius * 0.6F * std::cos(angle), cy + 0.5F + radius * 0.6F * std::sin(angle))) probes.push_back(*h);
    }
    if (probes.empty()) return std::nullopt;

    std::optional<SnapTarget> best;
    float bestDistance = 0.0F;
    const auto consider = [&](Float3 point, Float3 normal, SnapTargetKind kind, EditorObjectId object, float reach) {
        if (only && kind != *only) return;
        const ScreenPoint p = project_world_to_screen(camera_, layout_.viewport, point);
        if (!p.visible) return;
        const float distance = std::hypot(p.x - cx, p.y - cy);
        if (distance > reach) return;
        if (best && (rank(best->kind) < rank(kind) || (rank(best->kind) == rank(kind) && bestDistance <= distance))) return;
        // Only what can be seen: a corner hidden behind the geometry is not a target.
        const ViewportRay ray = make_viewport_ray(camera_, layout_.viewport, p.x, p.y);
        const auto front = pick_editor_document_all(document, ray, 10000.0F, &viewportSettings_.isolatedObjects);
        const auto blocker = std::find_if(front.begin(), front.end(), [&](const EditorPickResult& h) { return !excluded(h.objectId); });
        const float depth = length(subtract(point, camera_.position));
        if (blocker != front.end() && blocker->worldDistance < depth - std::max(0.02F, depth * 0.005F)) return;
        best = SnapTarget{point, normal, kind, object};
        bestDistance = distance;
    };
    std::set<EditorObjectId> meshesDone, boundsDone;
    std::set<std::pair<EditorObjectId, Int3>> cellsDone;
    for (const EditorPickResult& hit : probes) {
        const EditorObject* target = document.find_object(hit.objectId);
        if (!target) continue;
        const bool atCursor = centreHit && &hit == &probes.front();
        const auto exact = target->polygon ? exact_cad_description(target->id) : nullptr;
        if (exact) {
            // A CAD object with its exact shape read: snap to the true corners, edge midpoints and
            // circle centres, not to the triangles (whose extra points lie along curved edges).
            const auto world = [&](const ExactPoint& p) {
                return voxel_to_world(target->transform, target->voxelScale, Float3{float(p[0]), float(p[1]), float(p[2])});
            };
            if (meshesDone.insert(target->id).second) {
                for (const ExactPoint& v : exact->vertices) consider(world(v), hit.worldNormal, SnapTargetKind::Vertex, target->id, radius);
                for (const ExactCadEdge& e : exact->edges) consider(world(e.mid), hit.worldNormal, SnapTargetKind::EdgeMidpoint, target->id, radius);
                for (const ExactCadCircle& c : exact->circles)
                    consider(world(c.centre), hit.worldNormal, SnapTargetKind::CircleCentre, target->id, radius * 2.0F);
            }
        } else if (target->polygon) {
            const CookedPolygonAsset& mesh = *target->polygon;
            const auto world = [&](std::uint32_t index) { return voxel_to_world(target->transform, target->voxelScale, mesh.vertices[index].position); };
            if (meshesDone.insert(target->id).second) {
                for (std::uint32_t i = 0; i < mesh.vertices.size(); ++i) consider(world(i), hit.worldNormal, SnapTargetKind::Vertex, target->id, radius);
                for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
                    for (int e = 0; e < 3; ++e) {
                        const std::uint32_t a = mesh.indices[t + std::size_t(e)], b = mesh.indices[t + std::size_t((e + 1) % 3)];
                        consider(multiply(add(world(a), world(b)), 0.5F), hit.worldNormal, SnapTargetKind::EdgeMidpoint, target->id, radius);
                    }
            }
            if (atCursor && hit.polygonFace && std::size_t(*hit.polygonFace) * 3 + 2 < mesh.indices.size()) {
                const std::size_t t = std::size_t(*hit.polygonFace) * 3;
                const Float3 centre = multiply(add(add(world(mesh.indices[t]), world(mesh.indices[t + 1])), world(mesh.indices[t + 2])), 1.0F / 3.0F);
                consider(centre, hit.worldNormal, SnapTargetKind::FaceCenter, target->id, radius * 3.0F);
            }
        } else if (target->voxels && target->voxels->brick_count() > 0 && target->voxelSizeMeters > 0.0F &&
                   cellsDone.insert({target->id, hit.voxel}).second) {
            const float size = target->voxelSizeMeters;
            const Int3 cell = hit.voxel;
            std::array<Float3, 8> corners{};
            for (int corner = 0; corner < 8; ++corner)
                corners[std::size_t(corner)] = voxel_to_world(target->transform, target->voxelScale, Float3{
                    (static_cast<float>(cell.x) + ((corner & 1) ? 1.0F : 0.0F)) * size,
                    (static_cast<float>(cell.y) + ((corner & 2) ? 1.0F : 0.0F)) * size,
                    (static_cast<float>(cell.z) + ((corner & 4) ? 1.0F : 0.0F)) * size});
            for (const Float3 corner : corners) consider(corner, hit.worldNormal, SnapTargetKind::VoxelCorner, target->id, radius);
            if (atCursor && (hit.normal.x != 0 || hit.normal.y != 0 || hit.normal.z != 0)) {
                // The centre of the cell face that was hit.
                const int axis = hit.normal.x != 0 ? 0 : hit.normal.y != 0 ? 1 : 2;
                const int side = (axis == 0 ? hit.normal.x : axis == 1 ? hit.normal.y : hit.normal.z) > 0 ? 1 : 0;
                Float3 centre{};
                for (int corner = 0; corner < 8; ++corner)
                    if (((corner >> axis) & 1) == side) centre = add(centre, corners[std::size_t(corner)]);
                consider(multiply(centre, 0.25F), hit.worldNormal, SnapTargetKind::FaceCenter, target->id, radius * 3.0F);
            }
            if (target->flags.collisionEnabled)
                for (const VoxelBox& box : build_brick_box_proxy(*target->voxels, brick_key_from_voxel(cell))) {
                    if (cell.x < box.min.x || cell.y < box.min.y || cell.z < box.min.z || cell.x >= box.maxExclusive.x ||
                        cell.y >= box.maxExclusive.y || cell.z >= box.maxExclusive.z) continue;
                    for (int corner = 0; corner < 8; ++corner)
                        consider(voxel_to_world(target->transform, multiply(target->voxelScale, size), Float3{
                            static_cast<float>((corner & 1) ? box.maxExclusive.x : box.min.x),
                            static_cast<float>((corner & 2) ? box.maxExclusive.y : box.min.y),
                            static_cast<float>((corner & 4) ? box.maxExclusive.z : box.min.z)}), hit.worldNormal,
                            SnapTargetKind::CollisionVertex, target->id, radius);
                }
        }
        if (boundsDone.insert(target->id).second)
            if (const EditorObjectBounds bounds = object_world_bounds(*target); bounds.valid)
                for (int corner = 0; corner < 8; ++corner)
                    consider({(corner & 1) ? bounds.maximum.x : bounds.minimum.x, (corner & 2) ? bounds.maximum.y : bounds.minimum.y,
                              (corner & 4) ? bounds.maximum.z : bounds.minimum.z}, hit.worldNormal, SnapTargetKind::BoundsVertex, target->id, radius);
    }
    if (!best && centreHit && (!only || *only == SnapTargetKind::Surface))
        best = SnapTarget{add(centreHit->worldPosition, multiply(centreHit->worldNormal, placementOffsetMeters_)), centreHit->worldNormal,
                          SnapTargetKind::Surface, centreHit->objectId};
    return best;
}

bool NativeEditorController::apply_drag_snap(int x, int y) {
    if (!dragSnapHeld_ || activeTool_ != EditorToolId::Translate) { dragSnapTarget_.reset(); return false; }
    // Editing a pivot may snap it to the object's own vertices and corners.
    const auto target = find_snap_target(x, y, std::nullopt, origin_drag());
    if (!target) {
        dragSnapTarget_.reset();
        set_status("Snap (V): move over something to snap to", false, 1.0F);
        return false;
    }
    dragSnapTarget_ = target;
    const Float3 delta = subtract(target->point, gizmoPivot_);
    // Name what the artist sees: a piece of a closed group reads as the group.
    const EditorObject* targetObject = workspace_.document().find_object(viewport_selection_target(target->object));
    const std::string where = targetObject ? " of " + targetObject->name : std::string();
    if(nurbsDragBefore_) {
        (void)preview_nurbs_transform(delta,Quaternion{},{1,1,1},gizmoPivot_);
        set_status("NURBS controls snapped to target",false,1);return true;
    }
    if (polygonDragBefore_) {
        auto* object = workspace_.document().find_object(polygonSelection_.object);
        if (!object || !object->polygon) return false;
        *object->polygon = *polygonDragBefore_;
        (void)transform_polygon_components(delta, Quaternion{}, {1.0F, 1.0F, 1.0F}, gizmoPivot_);
        set_status(std::string("Snapped to ") + snap_target_name(target->kind) + where + " (release V for axis moves)", false, 1.0F);
        return true;
    }
    const bool align = target->kind == SnapTargetKind::Surface && alignToSurfaceNormal_;
    for (ObjectTransformChange& change : gizmoChanges_) {
        change.after = change.before;
        change.after.position = add(change.before.position, delta);
        if (align) {
            // Turn about the pivot so the object's up follows the surface normal.
            const Quaternion turned = align_up_to(change.before.rotation, target->normal);
            const Quaternion spin = normalize(multiply(turned, conjugate(change.before.rotation)));
            change.after.position = add(target->point, rotate(spin, subtract(change.before.position, gizmoPivot_)));
            change.after.rotation = turned;
        }
    }
    if (origin_drag()) originDragPoint_ = gizmoChanges_.front().after.position;  // Edit Pivot: the marker only
    else apply_transform_preview(gizmoChanges_, true);
    set_status(std::string("Snapped to ") + snap_target_name(target->kind) + where +
               (align ? " (aligned to the normal)" : "") + " (release V for axis moves)", false, 1.0F);
    return true;
}

}  // namespace dve::editor
