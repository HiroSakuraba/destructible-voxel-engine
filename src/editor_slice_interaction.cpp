#include "dve/editor_native.hpp"

#include <algorithm>
#include <cmath>

namespace dve::editor {
namespace {
constexpr float pi = 3.141592653589793F;
Float3 axis_vector(int axis) {
    return axis == 1 ? Float3{1, 0, 0} : axis == 2 ? Float3{0, 1, 0} : Float3{0, 0, 1};
}
void ring_basis(Float3 axis, Float3& u, Float3& v) {
    const Float3 helper = std::abs(axis.y) < 0.9F ? Float3{0, 1, 0} : Float3{1, 0, 0};
    const auto cross = [](Float3 a, Float3 b) {
        return Float3{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
    };
    u = normalize(cross(axis, helper));
    v = cross(axis, u);
}
bool ring_angle(const EditorCamera& camera, UiRect viewport, Float3 pivot, int axis,
                int x, int y, float& angle) {
    const auto ray = make_viewport_ray(camera, viewport, static_cast<float>(x), static_cast<float>(y));
    const auto normal = axis_vector(axis);
    const float denominator = dot(ray.direction, normal);
    if (std::abs(denominator) < 0.0001F) return false;
    const float t = dot(subtract(pivot, ray.origin), normal) / denominator;
    if (t <= 0) return false;
    const auto relative = subtract(add(ray.origin, multiply(ray.direction, t)), pivot);
    if (dot(relative, relative) < 0.00000001F) return false;
    Float3 u{}, v{};
    ring_basis(normal, u, v);
    angle = std::atan2(dot(relative, v), dot(relative, u));
    return true;
}
bool axis_parameter(const EditorCamera& camera, UiRect viewport, Float3 pivot, Float3 axis,
                    int x, int y, float& parameter) {
    const auto ray = make_viewport_ray(camera, viewport, static_cast<float>(x), static_cast<float>(y));
    const float parallel = dot(ray.direction, axis);
    const float denominator = 1 - parallel * parallel;
    if (denominator < 0.0025F) return false;
    const auto relative = subtract(ray.origin, pivot);
    parameter = (dot(relative, axis) - parallel * dot(relative, ray.direction)) / denominator;
    return std::isfinite(parameter);
}
float segment_distance(int x, int y, ScreenPoint a, ScreenPoint b) {
    const float dx = b.x-a.x, dy = b.y-a.y;
    const float denominator = dx*dx+dy*dy;
    const float t = denominator > 0 ? std::clamp(((x-a.x)*dx+(y-a.y)*dy)/denominator, 0.0F, 1.0F) : 0;
    return std::hypot(x-a.x-t*dx, y-a.y-t*dy);
}
float handle_length(const EditorCamera& camera, Float3 point) {
    return std::max(0.2F, camera.projection == EditorProjection::Orthographic
        ? camera.orthographicHeight * 0.13F : length(subtract(camera.position, point)) * 0.12F);
}
} // namespace

NativeSlicePanelLayout NativeEditorController::voxel_slice_panel_layout() const {
    NativeSlicePanelLayout result;
    if (!voxelSlice_.active()) return result;
    const int width = std::max(1, std::min(480, layout_.viewport.width-16));
    const int x = layout_.viewport.x+8, y = layout_.viewport.y+28;
    result.panel = {x, y, width, 218};
    result.point = {x+6, y+45, std::max(1, width-12), 24};
    result.normal = {x+6, y+71, std::max(1, width-12), 24};
    const int third = std::max(1, (width-16)/3);
    for (int i=0; i<3; ++i) {
        result.axes[i] = {x+6+i*(third+2), y+97, third, 24};
        result.outputs[i] = {x+6+i*(third+2), y+123, third, 24};
    }
    result.snap = {x+6, y+149, third, 24};
    result.apply = {x+8+third, y+149, third, 24};
    result.cancel = {x+10+2*third, y+149, third, 24};
    return result;
}

GizmoScreenAxis NativeEditorController::voxel_slice_move_handle() const {
    if (!voxelSlice_.active()) return {};
    const auto point = voxelSlice_.point();
    return {-1, project_world_to_screen(camera_, layout_.viewport, point),
        project_world_to_screen(camera_, layout_.viewport,
            add(point, multiply(voxelSlice_.normal(), handle_length(camera_, point))))};
}

std::vector<GizmoScreenRing> NativeEditorController::voxel_slice_rotate_handles() const {
    std::vector<GizmoScreenRing> rings;
    if (!voxelSlice_.active()) return rings;
    const auto pivot = voxelSlice_.point();
    const float radius = handle_length(camera_, pivot)*0.8F;
    for (int axis=1; axis<=3; ++axis) {
        // Rotating about the normal would not change the plane.
        if (std::abs(dot(axis_vector(axis), voxelSlice_.normal())) > 0.999F) continue;
        Float3 u{}, v{};
        ring_basis(axis_vector(axis), u, v);
        GizmoScreenRing ring;
        ring.axis = axis;
        for (int i=0; i<64; ++i) {
            const float angle = i*2*pi/64;
            ring.points.push_back(project_world_to_screen(camera_, layout_.viewport,
                add(pivot, add(multiply(u, radius*std::cos(angle)), multiply(v, radius*std::sin(angle))))));
        }
        rings.push_back(std::move(ring));
    }
    return rings;
}

bool NativeEditorController::begin_slice_pointer(int x, int y, std::uint32_t modifiers) {
    if (!voxelSlice_.active() || !layout_.viewport.contains(x,y) || (modifiers & 4U)) return false;
    const auto panel = voxel_slice_panel_layout();
    if (panel.panel.contains(x,y)) {
        if (panel.cancel.contains(x,y)) { cancel_voxel_slice(); return true; }
        if (textEdit_.kind == TextEditKind::SlicePoint || textEdit_.kind == TextEditKind::SliceNormal) {
            const auto field = textEdit_.kind == TextEditKind::SlicePoint ? panel.point : panel.normal;
            if (field.contains(x,y)) return true;
            commit_text_edit();
            if (textEdit_.kind != TextEditKind::Inactive) return true;
        }
        if (panel.point.contains(x,y) || panel.normal.contains(x,y)) {
            const bool point = panel.point.contains(x,y);
            const auto value = point ? voxelSlice_.point() : voxelSlice_.normal();
            begin_text_edit(point ? TextEditKind::SlicePoint : TextEditKind::SliceNormal, voxelSlice_.target(),
                std::to_string(value.x)+" "+std::to_string(value.y)+" "+std::to_string(value.z));
        } else if (panel.snap.contains(x,y)) sliceSnap_ = !sliceSnap_;
        else if (panel.apply.contains(x,y)) (void)commit_voxel_slice();
        else for (int i=0; i<3; ++i) {
            if (panel.axes[i].contains(x,y))
                (void)configure_voxel_slice(voxelSlice_.point(), axis_vector(i+1), voxelSlice_.output());
            if (panel.outputs[i].contains(x,y))
                (void)configure_voxel_slice(voxelSlice_.point(), voxelSlice_.normal(), static_cast<VoxelSliceOutput>(i));
        }
        return true;
    }
    if (textEdit_.kind == TextEditKind::SlicePoint || textEdit_.kind == TextEditKind::SliceNormal) {
        commit_text_edit();
        if (textEdit_.kind != TextEditKind::Inactive) return true;
    }
    const auto move = voxel_slice_move_handle();
    int hit = 0;
    float best = 8;
    if (move.start.visible && move.end.visible && segment_distance(x,y,move.start,move.end) < best) hit = -1;
    if (hit == 0) for (const auto& ring : voxel_slice_rotate_handles()) {
        float angle{};
        if (!ring_angle(camera_,layout_.viewport,voxelSlice_.point(),ring.axis,x,y,angle)) continue;
        for (std::size_t i=0; i<ring.points.size(); ++i) {
            const auto a=ring.points[i], b=ring.points[(i+1)%ring.points.size()];
            if (!a.visible || !b.visible) continue;
            const float distance=segment_distance(x,y,a,b);
            if (distance < best) { best=distance; hit=ring.axis; }
        }
    }
    if (hit != 0) {
        sliceDrag_=hit;
        sliceStartX_=x; sliceStartY_=y;
        sliceStartPoint_=voxelSlice_.point(); sliceStartNormal_=voxelSlice_.normal();
        sliceAngle_=0;
        if (hit == -1) sliceAxisFallback_=!axis_parameter(camera_,layout_.viewport,sliceStartPoint_,sliceStartNormal_,x,y,sliceStartParameter_);
        else (void)ring_angle(camera_,layout_.viewport,sliceStartPoint_,hit,x,y,sliceLastAngle_);
        dragButton_=PointerButton::Primary;
        set_status("Slice plane: drag previews; release keeps preview; Esc restores this drag",false,5);
    }
    // The preview owns ordinary viewport clicks, so they cannot edit the object.
    return true;
}

void NativeEditorController::update_slice_pointer(int x, int y, std::uint32_t modifiers) {
    if (!sliceDrag_) return;
    if (!voxelSlice_.target_unchanged(workspace_.document())) { cancel_voxel_slice(); return; }
    const bool snap = sliceSnap_ || (modifiers & 2U);
    auto point=sliceStartPoint_, normal=sliceStartNormal_;
    if (sliceDrag_ == -1) {
        float distance{};
        if (sliceAxisFallback_) {
            // When looking along the normal, the shaft collapses to a point.
            distance = static_cast<float>(sliceStartY_-y)*handle_length(camera_,sliceStartPoint_)/80;
        } else {
            float parameter{};
            if (!axis_parameter(camera_,layout_.viewport,sliceStartPoint_,sliceStartNormal_,x,y,parameter)) return;
            distance=parameter-sliceStartParameter_;
        }
        if (modifiers & 1U) distance *= 0.1F;
        const auto* object=workspace_.document().find_object(voxelSlice_.target());
        if (snap && object) {
            const float step=object->voxelSizeMeters*((modifiers & 1U) ? 0.1F : 1.0F);
            distance=std::round(distance/step)*step;
        }
        point=add(point,multiply(normal,distance));
    } else {
        float angle{};
        if (!ring_angle(camera_,layout_.viewport,sliceStartPoint_,sliceDrag_,x,y,angle)) return;
        sliceAngle_ += std::remainder(angle-sliceLastAngle_,2*pi);
        sliceLastAngle_=angle;
        float delta=sliceAngle_*((modifiers & 1U) ? 0.1F : 1.0F);
        if (snap) {
            const float step=std::max(0.1F,workspace_.preferences().rotateSnapDegrees)*pi/180;
            delta=std::round(delta/step)*step;
        }
        normal=rotate(quaternion_from_axis_angle(axis_vector(sliceDrag_),delta),normal);
    }
    (void)configure_voxel_slice(point,normal,voxelSlice_.output());
}

void NativeEditorController::finish_slice_pointer(bool cancel) {
    if (!sliceDrag_) return;
    sliceDrag_=0;
    dragButton_=PointerButton::NoButton;
    if (cancel && voxelSlice_.active())
        (void)configure_voxel_slice(sliceStartPoint_,sliceStartNormal_,voxelSlice_.output());
}
} // namespace dve::editor
