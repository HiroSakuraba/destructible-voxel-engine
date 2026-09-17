#include "dve/editor_viewport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace dve::editor {
namespace {

constexpr float kPi = 3.14159265358979323846F;

Float3 cross(Float3 a, Float3 b) noexcept {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

Float3 safe_normalize(Float3 value, Float3 fallback) noexcept {
    const float squared = length_squared(value);
    if (!(squared > 1.0e-12F) || !std::isfinite(squared)) return fallback;
    return multiply(value, 1.0F / std::sqrt(squared));
}

float clamp_pitch(float pitch) noexcept {
    return std::clamp(pitch, -1.553343F, 1.553343F);
}

Float3 voxel_center_world(const EditorObject& object, Int3 voxel) noexcept {
    const float size = object.voxelSizeMeters;
    const Float3 local{
        (static_cast<float>(voxel.x) + 0.5F) * size,
        (static_cast<float>(voxel.y) + 0.5F) * size,
        (static_cast<float>(voxel.z) + 0.5F) * size,
    };
    return transform_point(object.transform, local);
}


struct LocalVoxelBounds {
    Float3 minimum{};
    Float3 maximum{};
    bool valid{};
};

LocalVoxelBounds local_voxel_bounds(const EditorObject& object) noexcept {
    LocalVoxelBounds result;
    Int3 minimum = kInt3Max;
    Int3 maximum = kInt3Min;
    for (const auto& entry : object.voxels->bricks()) {
        const Int3 origin = brick_origin(entry.first);
        minimum = min_components(minimum, origin);
        maximum = max_components(maximum, {origin.x + kBrickDim, origin.y + kBrickDim, origin.z + kBrickDim});
        result.valid = true;
    }
    if (result.valid) {
        result.minimum = {static_cast<float>(minimum.x), static_cast<float>(minimum.y), static_cast<float>(minimum.z)};
        result.maximum = {static_cast<float>(maximum.x), static_cast<float>(maximum.y), static_cast<float>(maximum.z)};
    }
    return result;
}

bool ray_aabb_interval(Float3 origin, Float3 direction, Float3 minimum, Float3 maximum,
                       float& enter, float& exit) noexcept {
    enter = 0.0F;
    exit = std::numeric_limits<float>::infinity();
    const auto test = [&](float o, float d, float minValue, float maxValue, float& ioEnter, float& ioExit) {
        if (std::abs(d) < 1.0e-12F) return o >= minValue && o <= maxValue;
        float a = (minValue - o) / d;
        float b = (maxValue - o) / d;
        if (a > b) std::swap(a, b);
        ioEnter = std::max(ioEnter, a);
        ioExit = std::min(ioExit, b);
        return ioEnter <= ioExit;
    };
    return test(origin.x, direction.x, minimum.x, maximum.x, enter, exit) &&
           test(origin.y, direction.y, minimum.y, maximum.y, enter, exit) &&
           test(origin.z, direction.z, minimum.z, maximum.z, enter, exit) && exit >= 0.0F;
}

std::array<Float3, 8> bounds_corners(Float3 minimum, Float3 maximum) noexcept {
    return {{
        {minimum.x, minimum.y, minimum.z}, {maximum.x, minimum.y, minimum.z},
        {minimum.x, maximum.y, minimum.z}, {maximum.x, maximum.y, minimum.z},
        {minimum.x, minimum.y, maximum.z}, {maximum.x, minimum.y, maximum.z},
        {minimum.x, maximum.y, maximum.z}, {maximum.x, maximum.y, maximum.z},
    }};
}

Float3 box_surface_normal(Float3 point, Float3 minimum, Float3 maximum) noexcept {
    const std::array<float, 6> distances{
        std::abs(point.x - minimum.x), std::abs(maximum.x - point.x),
        std::abs(point.y - minimum.y), std::abs(maximum.y - point.y),
        std::abs(point.z - minimum.z), std::abs(maximum.z - point.z),
    };
    const auto iterator = std::min_element(distances.begin(), distances.end());
    switch (static_cast<std::size_t>(std::distance(distances.begin(), iterator))) {
        case 0: return {-1.0F, 0.0F, 0.0F};
        case 1: return {1.0F, 0.0F, 0.0F};
        case 2: return {0.0F, -1.0F, 0.0F};
        case 3: return {0.0F, 1.0F, 0.0F};
        case 4: return {0.0F, 0.0F, -1.0F};
        default: return {0.0F, 0.0F, 1.0F};
    }
}

} // namespace


float editor_camera_vertical_fov(const EditorCamera& camera, float aspectRatio) noexcept {
    if (camera.physicalLens.enabled)
        return camera.physicalLens.vertical_field_of_view_radians(aspectRatio);
    return camera.verticalFovRadians;
}

camera::CameraPose to_camera_pose(const EditorCamera& editorCamera, float aspectRatio) noexcept {
    camera::CameraPose pose;
    pose.position = editorCamera.position;
    pose.target = editorCamera.target;
    pose.worldUp = editorCamera.worldUp;
    pose.lens.projection = editorCamera.projection == EditorProjection::Perspective
        ? camera::CameraProjection::Perspective : camera::CameraProjection::Orthographic;
    pose.lens.verticalFieldOfViewRadians = editorCamera.verticalFovRadians;
    pose.lens.orthographicHeightMeters = editorCamera.orthographicHeight;
    pose.lens.nearPlaneMeters = editorCamera.nearPlane;
    pose.lens.farPlaneMeters = editorCamera.farPlane;
    pose.lens.aspectRatio = std::max(0.01F, aspectRatio);
    pose.lens.physical = editorCamera.physicalLens;
    return pose;
}

void apply_camera_pose(EditorCamera& editorCamera, const camera::CameraPose& pose) noexcept {
    editorCamera.position = pose.position;
    editorCamera.target = pose.target;
    editorCamera.worldUp = pose.worldUp;
    editorCamera.projection = pose.lens.projection == camera::CameraProjection::Perspective
        ? EditorProjection::Perspective : EditorProjection::Orthographic;
    editorCamera.verticalFovRadians = pose.lens.verticalFieldOfViewRadians;
    editorCamera.orthographicHeight = pose.lens.orthographicHeightMeters;
    editorCamera.nearPlane = pose.lens.nearPlaneMeters;
    editorCamera.farPlane = pose.lens.farPlaneMeters;
    editorCamera.physicalLens = pose.lens.physical;
}

CameraBasis camera_basis(const EditorCamera& camera) noexcept {
    const Float3 forward = safe_normalize(subtract(camera.target, camera.position), {0.0F, 0.0F, -1.0F});
    Float3 right = safe_normalize(cross(forward, camera.worldUp), {1.0F, 0.0F, 0.0F});
    Float3 up = safe_normalize(cross(right, forward), {0.0F, 1.0F, 0.0F});
    right = safe_normalize(cross(forward, up), right);
    return {forward, right, up};
}

ViewportRay make_viewport_ray(const EditorCamera& camera, UiRect viewport, float screenX, float screenY) noexcept {
    const CameraBasis basis = camera_basis(camera);
    const float width = static_cast<float>(std::max(1, viewport.width));
    const float height = static_cast<float>(std::max(1, viewport.height));
    const float normalizedX = ((screenX - static_cast<float>(viewport.x)) / width) * 2.0F - 1.0F;
    const float normalizedY = 1.0F - ((screenY - static_cast<float>(viewport.y)) / height) * 2.0F;
    const float aspect = width / height;
    if (camera.projection == EditorProjection::Orthographic) {
        const float halfHeight = std::max(0.001F, camera.orthographicHeight * 0.5F);
        const Float3 offset = add(multiply(basis.right, normalizedX * halfHeight * aspect),
                                  multiply(basis.up, normalizedY * halfHeight));
        return {add(camera.position, offset), basis.forward};
    }
    const float tangent = std::tan(std::clamp(editor_camera_vertical_fov(camera, aspect), 0.1F, kPi - 0.1F) * 0.5F);
    const float shiftedX = normalizedX - (camera.physicalLens.enabled ? camera.physicalLens.lensShiftX * 2.0F : 0.0F);
    const float shiftedY = normalizedY - (camera.physicalLens.enabled ? camera.physicalLens.lensShiftY * 2.0F : 0.0F);
    const Float3 direction = add(
        basis.forward,
        add(multiply(basis.right, shiftedX * tangent * aspect),
            multiply(basis.up, shiftedY * tangent)));
    return {camera.position, safe_normalize(direction, basis.forward)};
}

ScreenPoint project_world_to_screen(const EditorCamera& camera, UiRect viewport, Float3 worldPoint) noexcept {
    const CameraBasis basis = camera_basis(camera);
    const Float3 relative = subtract(worldPoint, camera.position);
    const float depth = dot(relative, basis.forward);
    if (!(depth > camera.nearPlane) || depth > camera.farPlane || viewport.width <= 0 || viewport.height <= 0)
        return {0.0F, 0.0F, depth, false};
    const float x = dot(relative, basis.right);
    const float y = dot(relative, basis.up);
    const float width = static_cast<float>(viewport.width);
    const float height = static_cast<float>(viewport.height);
    const float aspect = width / height;
    float normalizedX{};
    float normalizedY{};
    if (camera.projection == EditorProjection::Orthographic) {
        const float halfHeight = std::max(0.001F, camera.orthographicHeight * 0.5F);
        normalizedX = x / (halfHeight * aspect);
        normalizedY = y / halfHeight;
    } else {
        const float tangent = std::tan(std::clamp(editor_camera_vertical_fov(camera, aspect), 0.1F, kPi - 0.1F) * 0.5F);
        normalizedX = x / (depth * tangent * aspect) + (camera.physicalLens.enabled ? camera.physicalLens.lensShiftX * 2.0F : 0.0F);
        normalizedY = y / (depth * tangent) + (camera.physicalLens.enabled ? camera.physicalLens.lensShiftY * 2.0F : 0.0F);
    }
    const bool visible = normalizedX >= -1.2F && normalizedX <= 1.2F && normalizedY >= -1.2F && normalizedY <= 1.2F;
    return {
        static_cast<float>(viewport.x) + (normalizedX + 1.0F) * 0.5F * width,
        static_cast<float>(viewport.y) + (1.0F - normalizedY) * 0.5F * height,
        depth,
        visible,
    };
}

void orbit_camera(EditorCamera& camera, float deltaX, float deltaY, float sensitivity) noexcept {
    Float3 offset = subtract(camera.position, camera.target);
    float radius = std::max(0.05F, length(offset));
    float yaw = std::atan2(offset.x, offset.z);
    float pitch = std::asin(std::clamp(offset.y / radius, -1.0F, 1.0F));
    yaw -= deltaX * sensitivity;
    pitch = clamp_pitch(pitch + deltaY * sensitivity);
    const float horizontal = radius * std::cos(pitch);
    offset = {horizontal * std::sin(yaw), radius * std::sin(pitch), horizontal * std::cos(yaw)};
    camera.position = add(camera.target, offset);
}

void look_camera(EditorCamera& camera, float deltaX, float deltaY, float sensitivity) noexcept {
    Float3 direction = subtract(camera.target, camera.position);
    const float distance = std::max(0.05F, length(direction));
    direction = safe_normalize(direction, {0.0F, 0.0F, -1.0F});
    float yaw = std::atan2(direction.x, direction.z);
    float pitch = std::asin(std::clamp(direction.y, -1.0F, 1.0F));
    yaw -= deltaX * sensitivity;
    pitch = clamp_pitch(pitch + deltaY * sensitivity);
    const float horizontal = std::cos(pitch);
    direction = {horizontal * std::sin(yaw), std::sin(pitch), horizontal * std::cos(yaw)};
    camera.target = add(camera.position, multiply(direction, distance));
}

void pan_camera(EditorCamera& camera, float deltaX, float deltaY, UiRect viewport) noexcept {
    const CameraBasis basis = camera_basis(camera);
    const float distance = std::max(0.1F, length(subtract(camera.target, camera.position)));
    const float pixelScale = camera.projection == EditorProjection::Perspective
        ? 2.0F * distance * std::tan(camera.verticalFovRadians * 0.5F) / static_cast<float>(std::max(1, viewport.height))
        : camera.orthographicHeight / static_cast<float>(std::max(1, viewport.height));
    const Float3 movement = add(multiply(basis.right, -deltaX * pixelScale), multiply(basis.up, deltaY * pixelScale));
    camera.position = add(camera.position, movement);
    camera.target = add(camera.target, movement);
}

void zoom_camera(EditorCamera& camera, float wheelSteps) noexcept {
    if (camera.projection == EditorProjection::Orthographic) {
        camera.orthographicHeight = std::clamp(camera.orthographicHeight * std::pow(0.86F, wheelSteps), 0.05F, 100000.0F);
        return;
    }
    const Float3 offset = subtract(camera.position, camera.target);
    const float distance = std::clamp(length(offset) * std::pow(0.86F, wheelSteps), 0.05F, 100000.0F);
    camera.position = add(camera.target, multiply(safe_normalize(offset, {0,0,1}), distance));
}

void fly_camera(EditorCamera& camera, Float3 localMotion, float elapsedSeconds, float speed) noexcept {
    const CameraBasis basis = camera_basis(camera);
    const Float3 movement = multiply(
        add(add(multiply(basis.right, localMotion.x), multiply(basis.up, localMotion.y)), multiply(basis.forward, localMotion.z)),
        std::max(0.0F, elapsedSeconds) * std::max(0.0F, speed));
    camera.position = add(camera.position, movement);
    camera.target = add(camera.target, movement);
}

void frame_camera_on_bounds(EditorCamera& camera, const EditorObjectBounds& bounds) noexcept {
    if (!bounds.valid) return;
    camera.target = multiply(add(bounds.minimum, bounds.maximum), 0.5F);
    const Float3 extent = subtract(bounds.maximum, bounds.minimum);
    const float radius = std::max(0.1F, 0.5F * length(extent));
    const Float3 currentDirection = safe_normalize(subtract(camera.position, camera.target), {0.6F, 0.4F, 0.7F});
    const float distance = camera.projection == EditorProjection::Perspective
        ? radius / std::max(0.05F, std::sin(camera.verticalFovRadians * 0.38F))
        : radius * 2.5F;
    if (camera.projection == EditorProjection::Orthographic) camera.orthographicHeight = radius * 2.5F;
    camera.position = add(camera.target, multiply(currentDirection, distance));
}

EditorObjectBounds object_world_bounds(const EditorObject& object) noexcept {
    EditorObjectBounds result;
    if (object.text3d || object.gaborVolume) {
        const Float3 localMinimum = object.text3d ? object.text3d->bounds.minimum : object.gaborVolume->boundsMinimum;
        const Float3 localMaximum = object.text3d ? object.text3d->bounds.maximum : object.gaborVolume->boundsMaximum;
        const auto corners = bounds_corners(localMinimum, localMaximum);
        result.minimum = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::infinity()};
        result.maximum = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()};
        for (Float3 corner : corners) {
            const Float3 world = transform_point(object.transform, corner);
            result.minimum = {std::min(result.minimum.x, world.x), std::min(result.minimum.y, world.y),
                              std::min(result.minimum.z, world.z)};
            result.maximum = {std::max(result.maximum.x, world.x), std::max(result.maximum.y, world.y),
                              std::max(result.maximum.z, world.z)};
        }
        result.valid = true;
        return result;
    }
    Int3 minimum = kInt3Max;
    Int3 maximum = kInt3Min;
    bool any = false;
    for (const auto& entry : object.voxels->bricks()) {
        const BrickKey key = entry.first;
        entry.second.occupancy().for_each_set([&](std::uint16_t index) {
            const Int3 voxel = global_from_local(key, local_from_index_unchecked(index));
            minimum = min_components(minimum, voxel);
            maximum = max_components(maximum, voxel);
            any = true;
        });
    }
    if (!any) return result;
    const float size = object.voxelSizeMeters;
    const Float3 localMinimum{static_cast<float>(minimum.x) * size, static_cast<float>(minimum.y) * size, static_cast<float>(minimum.z) * size};
    const Float3 localMaximum{static_cast<float>(maximum.x + 1) * size, static_cast<float>(maximum.y + 1) * size, static_cast<float>(maximum.z + 1) * size};
    const auto corners = bounds_corners(localMinimum, localMaximum);
    result.minimum = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
    result.maximum = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
    for (Float3 corner : corners) {
        const Float3 world = transform_point(object.transform, corner);
        result.minimum = {std::min(result.minimum.x, world.x), std::min(result.minimum.y, world.y), std::min(result.minimum.z, world.z)};
        result.maximum = {std::max(result.maximum.x, world.x), std::max(result.maximum.y, world.y), std::max(result.maximum.z, world.z)};
    }
    result.valid = true;
    return result;
}

std::optional<EditorPickResult> pick_editor_document(const EditorDocument& document, ViewportRay ray, float maximumWorldDistance) {
    std::optional<EditorPickResult> best;
    const Float3 worldDirection = safe_normalize(ray.direction, {0,0,-1});
    for (const auto& [id, object] : document.objects()) {
        if (!object.flags.visible) continue;
        if (object.text3d || object.gaborVolume) {
            const Float3 localMinimum = object.text3d ? object.text3d->bounds.minimum : object.gaborVolume->boundsMinimum;
            const Float3 localMaximum = object.text3d ? object.text3d->bounds.maximum : object.gaborVolume->boundsMaximum;
            const Float3 localOrigin = inverse_transform_point(object.transform, ray.origin);
            const Float3 localDirection = safe_normalize(
                inverse_transform_vector(object.transform, worldDirection), {0.0F, 0.0F, -1.0F});
            float enter = 0.0F;
            float exit = maximumWorldDistance;
            if (ray_aabb_interval(localOrigin, localDirection, localMinimum,
                                  localMaximum, enter, exit)) {
                const float worldDistance = std::clamp(enter, 0.0F, maximumWorldDistance);
                if ((!best || worldDistance < best->worldDistance) && worldDistance <= maximumWorldDistance) {
                    const Float3 localHit = add(localOrigin, multiply(localDirection, worldDistance));
                    const Float3 localNormal = box_surface_normal(localHit, localMinimum, localMaximum);
                    const auto material = object.text3d
                        ? static_cast<MaterialId>(std::min<std::uint32_t>(
                            object.text3d->style.faceMaterialId, std::numeric_limits<MaterialId>::max()))
                        : kAirMaterial;
                    best = EditorPickResult{
                        id, {}, {}, material, transform_point(object.transform, localHit),
                        safe_normalize(transform_vector(object.transform, localNormal), {0.0F, 1.0F, 0.0F}),
                        worldDistance};
                }
            }
            continue;
        }
        if (object.voxelSizeMeters <= 0.0F || object.voxels->brick_count() == 0) continue;
        const float inverseScale = 1.0F / object.voxelSizeMeters;
        const Float3 localOriginMeters = inverse_transform_point(object.transform, ray.origin);
        const Float3 localDirection = inverse_transform_vector(object.transform, worldDirection);
        const Float3 localOriginVoxels = multiply(localOriginMeters, inverseScale);
        const Float3 localDirectionVoxels = multiply(localDirection, inverseScale);
        const LocalVoxelBounds broad = local_voxel_bounds(object);
        float enterWorld = 0.0F;
        float exitWorld = maximumWorldDistance;
        if (!broad.valid || !ray_aabb_interval(localOriginVoxels, localDirectionVoxels,
                                               broad.minimum, broad.maximum, enterWorld, exitWorld)) continue;
        enterWorld = std::clamp(enterWorld, 0.0F, maximumWorldDistance);
        exitWorld = std::clamp(exitWorld, 0.0F, maximumWorldDistance);
        if (exitWorld < enterWorld) continue;
        const float epsilonWorld = std::min(0.0001F, object.voxelSizeMeters * 0.0001F);
        const float startWorld = std::max(0.0F, enterWorld - epsilonWorld);
        const Float3 clippedOriginVoxels = add(localOriginVoxels, multiply(localDirectionVoxels, startWorld));
        const float segmentWorld = exitWorld - startWorld + epsilonWorld;
        const auto hit = raycast_voxels(*object.voxels, clippedOriginVoxels, localDirectionVoxels,
                                        segmentWorld * inverseScale);
        if (!hit) continue;
        const float worldDistance = startWorld + hit->distance * object.voxelSizeMeters;
        if (best && worldDistance >= best->worldDistance) continue;
        const Float3 normalizedLocalDirection = safe_normalize(localDirectionVoxels, {0,0,-1});
        const Float3 localHitVoxels = add(clippedOriginVoxels, multiply(normalizedLocalDirection, hit->distance));
        const Float3 localHitMeters = multiply(localHitVoxels, object.voxelSizeMeters);
        const Float3 localNormal{static_cast<float>(hit->normal.x), static_cast<float>(hit->normal.y), static_cast<float>(hit->normal.z)};
        best = EditorPickResult{
            id,
            hit->voxel,
            hit->normal,
            hit->material,
            transform_point(object.transform, localHitMeters),
            safe_normalize(transform_vector(object.transform, localNormal), {0,1,0}),
            worldDistance,
        };
    }
    return best;
}

std::vector<EditorVoxelDrawItem> build_voxel_draw_list(
    const EditorDocument& document,
    const EditorMaterialLibrary& materials,
    const EditorCamera& camera,
    UiRect viewport,
    const EditorViewportSettings& settings,
    const std::set<EditorObjectId>& selectedObjects) {
    std::vector<EditorVoxelDrawItem> result;
    result.reserve(std::min<std::size_t>(settings.maximumDrawVoxels, 16384));
    const float viewportHeight = static_cast<float>(std::max(1, viewport.height));
    const float tangent = std::tan(camera.verticalFovRadians * 0.5F);
    for (const auto& [id, object] : document.objects()) {
        if (!object.flags.visible) continue;
        for (const auto& entry : object.voxels->bricks()) {
            const BrickKey key = entry.first;
            const Brick& brick = entry.second;
            brick.occupancy().for_each_set([&](std::uint16_t index) {
                if (result.size() >= settings.maximumDrawVoxels) return;
                const Int3 voxel = global_from_local(key, local_from_index_unchecked(index));
                const Float3 world = voxel_center_world(object, voxel);
                const ScreenPoint screen = project_world_to_screen(camera, viewport, world);
                if (!screen.visible) return;
                float radius = 2.0F;
                if (camera.projection == EditorProjection::Perspective) {
                    radius = (object.voxelSizeMeters * viewportHeight) /
                             std::max(0.001F, 2.0F * screen.depth * tangent);
                } else {
                    radius = object.voxelSizeMeters * viewportHeight / std::max(0.001F, camera.orthographicHeight);
                }
                (void)materials;
                result.push_back({id, voxel, brick.material(index), screen.x, screen.y, screen.depth,
                                  std::clamp(radius * 0.72F, 1.0F, 12.0F),
                                  selectedObjects.contains(id), object.anchors.contains(voxel)});
            });
            if (result.size() >= settings.maximumDrawVoxels) break;
        }
        if (result.size() >= settings.maximumDrawVoxels) break;
    }
    std::stable_sort(result.begin(), result.end(), [](const EditorVoxelDrawItem& a, const EditorVoxelDrawItem& b) {
        if (a.depth != b.depth) return a.depth > b.depth;
        if (a.objectId != b.objectId) return a.objectId < b.objectId;
        return a.voxel < b.voxel;
    });
    return result;
}

std::vector<EditorVoxelDrawItem> build_voxel_draw_list(
    const EditorDocument& document,
    const EditorMaterialLibrary& materials,
    const EditorCamera& camera,
    UiRect viewport,
    const EditorViewportSettings& settings,
    std::optional<EditorObjectId> selectedObject) {
    std::set<EditorObjectId> selection;
    if (selectedObject) selection.insert(*selectedObject);
    return build_voxel_draw_list(document, materials, camera, viewport, settings, selection);
}

std::vector<EditorText3DDrawItem> build_text3d_draw_list(
    const EditorDocument& document,
    const EditorCamera& camera,
    UiRect viewport,
    const std::set<EditorObjectId>& selectedObjects) {
    std::vector<EditorText3DDrawItem> result;
    for (const auto& [id, object] : document.objects()) {
        if (!object.flags.visible || !object.text3d) continue;
        const EditorObjectBounds worldBounds = object_world_bounds(object);
        if (!worldBounds.valid) continue;
        float minimumX = std::numeric_limits<float>::infinity();
        float minimumY = std::numeric_limits<float>::infinity();
        float maximumX = -std::numeric_limits<float>::infinity();
        float maximumY = -std::numeric_limits<float>::infinity();
        float depth = std::numeric_limits<float>::infinity();
        bool anyVisible = false;
        for (Float3 corner : bounds_corners(worldBounds.minimum, worldBounds.maximum)) {
            const ScreenPoint screen = project_world_to_screen(camera, viewport, corner);
            if (!(screen.depth > camera.nearPlane) || screen.depth > camera.farPlane) continue;
            minimumX = std::min(minimumX, screen.x);
            minimumY = std::min(minimumY, screen.y);
            maximumX = std::max(maximumX, screen.x);
            maximumY = std::max(maximumY, screen.y);
            depth = std::min(depth, screen.depth);
            anyVisible = true;
        }
        if (!anyVisible || maximumX < static_cast<float>(viewport.x) ||
            minimumX > static_cast<float>(viewport.x + viewport.width) ||
            maximumY < static_cast<float>(viewport.y) ||
            minimumY > static_cast<float>(viewport.y + viewport.height)) continue;
        const int left = std::clamp(static_cast<int>(std::floor(minimumX)), viewport.x, viewport.x + viewport.width);
        const int top = std::clamp(static_cast<int>(std::floor(minimumY)), viewport.y, viewport.y + viewport.height);
        const int right = std::clamp(static_cast<int>(std::ceil(maximumX)), viewport.x, viewport.x + viewport.width);
        const int bottom = std::clamp(static_cast<int>(std::ceil(maximumY)), viewport.y, viewport.y + viewport.height);
        if (right <= left || bottom <= top) continue;
        result.push_back({id, object.text3d->textUtf8, {left, top, right - left, bottom - top}, depth,
                          object.text3d->style.faceMaterialId, object.text3d->style.sideMaterialId,
                          selectedObjects.contains(id)});
    }
    std::stable_sort(result.begin(), result.end(), [](const EditorText3DDrawItem& a, const EditorText3DDrawItem& b) {
        if (a.depth != b.depth) return a.depth > b.depth;
        return a.objectId < b.objectId;
    });
    return result;
}

std::vector<EditorText3DDrawItem> build_text3d_draw_list(
    const EditorDocument& document,
    const EditorCamera& camera,
    UiRect viewport,
    std::optional<EditorObjectId> selectedObject) {
    std::set<EditorObjectId> selection;
    if (selectedObject) selection.insert(*selectedObject);
    return build_text3d_draw_list(document, camera, viewport, selection);
}

std::vector<EditorGaborVolumeDrawItem> build_gabor_volume_draw_list(
    const EditorDocument& document,
    const EditorCamera& camera,
    UiRect viewport,
    const std::set<EditorObjectId>& selectedObjects) {
    std::vector<EditorGaborVolumeDrawItem> result;
    for (const auto& [id, object] : document.objects()) {
        if (!object.flags.visible || !object.gaborVolume) continue;
        const EditorObjectBounds worldBounds = object_world_bounds(object);
        if (!worldBounds.valid) continue;
        float minimumX = std::numeric_limits<float>::infinity();
        float minimumY = std::numeric_limits<float>::infinity();
        float maximumX = -std::numeric_limits<float>::infinity();
        float maximumY = -std::numeric_limits<float>::infinity();
        float depth = std::numeric_limits<float>::infinity();
        bool anyVisible = false;
        for (Float3 corner : bounds_corners(worldBounds.minimum, worldBounds.maximum)) {
            const ScreenPoint screen = project_world_to_screen(camera, viewport, corner);
            if (!(screen.depth > camera.nearPlane) || screen.depth > camera.farPlane) continue;
            minimumX = std::min(minimumX, screen.x);
            minimumY = std::min(minimumY, screen.y);
            maximumX = std::max(maximumX, screen.x);
            maximumY = std::max(maximumY, screen.y);
            depth = std::min(depth, screen.depth);
            anyVisible = true;
        }
        if (!anyVisible || maximumX < static_cast<float>(viewport.x) ||
            minimumX > static_cast<float>(viewport.x + viewport.width) ||
            maximumY < static_cast<float>(viewport.y) ||
            minimumY > static_cast<float>(viewport.y + viewport.height)) continue;
        const int left = std::clamp(static_cast<int>(std::floor(minimumX)), viewport.x, viewport.x + viewport.width);
        const int top = std::clamp(static_cast<int>(std::floor(minimumY)), viewport.y, viewport.y + viewport.height);
        const int right = std::clamp(static_cast<int>(std::ceil(maximumX)), viewport.x, viewport.x + viewport.width);
        const int bottom = std::clamp(static_cast<int>(std::ceil(maximumY)), viewport.y, viewport.y + viewport.height);
        if (right <= left || bottom <= top) continue;
        std::uint16_t maximumLod{};
        for (const auto& primitive : object.gaborVolume->primitives)
            maximumLod = std::max(maximumLod, primitive.lodLevel);
        result.push_back({id, {left, top, right-left, bottom-top}, depth,
                          object.gaborVolume->primitives.size(), maximumLod,
                          object.gaborVolume->material.albedoTint,
                          object.gaborVolume->material.densityMultiplier,
                          selectedObjects.contains(id)});
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.depth != b.depth) return a.depth > b.depth;
        return a.objectId < b.objectId;
    });
    return result;
}

std::vector<EditorGaborVolumeDrawItem> build_gabor_volume_draw_list(
    const EditorDocument& document,
    const EditorCamera& camera,
    UiRect viewport,
    std::optional<EditorObjectId> selectedObject) {
    std::set<EditorObjectId> selection;
    if (selectedObject) selection.insert(*selectedObject);
    return build_gabor_volume_draw_list(document, camera, viewport, selection);
}

} // namespace dve::editor
