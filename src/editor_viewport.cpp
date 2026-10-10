#include "dve/editor_viewport.hpp"

#include "dve/job_system.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <mutex>

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

Float3 voxel_center_world(const EditorObject& object, Int3 voxel, const RigidTransform* pose = nullptr) noexcept {
    const float size = object.voxelSizeMeters;
    const Float3 local{
        (static_cast<float>(voxel.x) + 0.5F) * size,
        (static_cast<float>(voxel.y) + 0.5F) * size,
        (static_cast<float>(voxel.z) + 0.5F) * size,
    };
    return transform_point(pose ? *pose : object.transform, local);
}


struct LocalVoxelBounds {
    Float3 minimum{};
    Float3 maximum{};
    bool valid{};
};

LocalVoxelBounds local_voxel_bounds(const EditorObject& object) noexcept {
    LocalVoxelBounds result;
    const VoxelObject::DerivedBounds extent = object.voxels->brick_extent_bounds();
    if (extent.valid) {
        result.minimum = {static_cast<float>(extent.minimum.x), static_cast<float>(extent.minimum.y),
                          static_cast<float>(extent.minimum.z)};
        result.maximum = {static_cast<float>(extent.maximum.x), static_cast<float>(extent.maximum.y),
                          static_cast<float>(extent.maximum.z)};
        result.valid = true;
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

namespace {

// Everything project_world_to_screen derives from the camera and viewport alone,
// computed once per view instead of once per voxel (the basis takes four
// normalisations and the field of view a tan, both previously per point).
struct ScreenProjector {
    CameraBasis basis{};
    Float3 position{};
    float nearPlane{};
    float farPlane{};
    bool hasArea{};
    bool orthographic{};
    float viewportX{};
    float viewportY{};
    float width{};
    float height{};
    float aspect{};
    float halfHeight{};
    float tangent{};
    float lensShiftX{};
    float lensShiftY{};
};

ScreenProjector make_screen_projector(const EditorCamera& camera, UiRect viewport) noexcept {
    ScreenProjector projector;
    projector.basis = camera_basis(camera);
    projector.position = camera.position;
    projector.nearPlane = camera.nearPlane;
    projector.farPlane = camera.farPlane;
    projector.hasArea = viewport.width > 0 && viewport.height > 0;
    if (!projector.hasArea) return projector;
    projector.orthographic = camera.projection == EditorProjection::Orthographic;
    projector.viewportX = static_cast<float>(viewport.x);
    projector.viewportY = static_cast<float>(viewport.y);
    projector.width = static_cast<float>(viewport.width);
    projector.height = static_cast<float>(viewport.height);
    projector.aspect = projector.width / projector.height;
    if (projector.orthographic) {
        projector.halfHeight = std::max(0.001F, camera.orthographicHeight * 0.5F);
    } else {
        projector.tangent = std::tan(std::clamp(editor_camera_vertical_fov(camera, projector.aspect), 0.1F, kPi - 0.1F) * 0.5F);
        projector.lensShiftX = camera.physicalLens.enabled ? camera.physicalLens.lensShiftX * 2.0F : 0.0F;
        projector.lensShiftY = camera.physicalLens.enabled ? camera.physicalLens.lensShiftY * 2.0F : 0.0F;
    }
    return projector;
}

ScreenPoint project_with(const ScreenProjector& projector, Float3 worldPoint) noexcept {
    const Float3 relative = subtract(worldPoint, projector.position);
    const float depth = dot(relative, projector.basis.forward);
    if (!(depth > projector.nearPlane) || depth > projector.farPlane || !projector.hasArea)
        return {0.0F, 0.0F, depth, false};
    const float x = dot(relative, projector.basis.right);
    const float y = dot(relative, projector.basis.up);
    float normalizedX{};
    float normalizedY{};
    if (projector.orthographic) {
        normalizedX = x / (projector.halfHeight * projector.aspect);
        normalizedY = y / projector.halfHeight;
    } else {
        normalizedX = x / (depth * projector.tangent * projector.aspect) + projector.lensShiftX;
        normalizedY = y / (depth * projector.tangent) + projector.lensShiftY;
    }
    const bool visible = normalizedX >= -1.2F && normalizedX <= 1.2F && normalizedY >= -1.2F && normalizedY <= 1.2F;
    return {
        projector.viewportX + (normalizedX + 1.0F) * 0.5F * projector.width,
        projector.viewportY + (1.0F - normalizedY) * 0.5F * projector.height,
        depth,
        visible,
    };
}

} // namespace

ScreenPoint project_world_to_screen(const EditorCamera& camera, UiRect viewport, Float3 worldPoint) noexcept {
    return project_with(make_screen_projector(camera, viewport), worldPoint);
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
    return object_world_bounds(object, object.transform);
}

EditorObjectBounds object_world_bounds(const EditorObject& object, const RigidTransform& pose) noexcept {
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
            const Float3 world = transform_point(pose, corner);
            result.minimum = {std::min(result.minimum.x, world.x), std::min(result.minimum.y, world.y),
                              std::min(result.minimum.z, world.z)};
            result.maximum = {std::max(result.maximum.x, world.x), std::max(result.maximum.y, world.y),
                              std::max(result.maximum.z, world.z)};
        }
        result.valid = true;
        return result;
    }
    const VoxelObject::DerivedBounds occupied = object.voxels->occupied_bounds();
    if (!occupied.valid) return result;
    const float size = object.voxelSizeMeters;
    const Float3 localMinimum{static_cast<float>(occupied.minimum.x) * size, static_cast<float>(occupied.minimum.y) * size, static_cast<float>(occupied.minimum.z) * size};
    const Float3 localMaximum{static_cast<float>(occupied.maximum.x + 1) * size, static_cast<float>(occupied.maximum.y + 1) * size, static_cast<float>(occupied.maximum.z + 1) * size};
    const auto corners = bounds_corners(localMinimum, localMaximum);
    result.minimum = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
    result.maximum = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
    for (Float3 corner : corners) {
        const Float3 world = transform_point(pose, corner);
        result.minimum = {std::min(result.minimum.x, world.x), std::min(result.minimum.y, world.y), std::min(result.minimum.z, world.z)};
        result.maximum = {std::max(result.maximum.x, world.x), std::max(result.maximum.y, world.y), std::max(result.maximum.z, world.z)};
    }
    result.valid = true;
    return result;
}

std::optional<EditorPickResult> pick_editor_document(const EditorDocument& document, ViewportRay ray,
                                                     float maximumWorldDistance,
                                                     const std::set<EditorObjectId>* onlyObjects) {
    std::optional<EditorPickResult> best;
    const Float3 worldDirection = safe_normalize(ray.direction, {0,0,-1});
    for (const auto& [id, object] : document.objects()) {
        if (!object.flags.visible) continue;
        if (onlyObjects && !onlyObjects->empty() && !onlyObjects->contains(id)) continue;
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

namespace {

// Occupied voxels of `brick` that are within two voxels of empty space: everything
// except voxels whose whole 5x5x5 neighbourhood is solid. One solid layer (a 3x3x3
// neighbourhood) is enough almost everywhere, but at grazing silhouettes a buried
// voxel's splat can still leak a stray pixel past the surface splats; keeping two
// layers made culled and unculled frames pixel-identical in every test pose.
//
// Bitset512 stores one z plane per word with bit (x + 8y). The 5x5x5 erosion is
// separable (x, then y, then z), and a two-voxel reach never crosses more than one
// brick, so each direction is a shift plus the facing columns/rows/planes of the
// adjacent brick: 26 brick lookups per brick instead of 124 voxel lookups per voxel.
Bitset512 exposed_voxel_mask(const VoxelObject& voxels, BrickKey key, const Brick& brick) {
    static_assert(kBrickDim == 8, "exposed_voxel_mask assumes 8x8x8 bricks");
    constexpr std::uint64_t kColumnX0 = 0x0101010101010101ULL;
    // Columns x < k / x >= 8 - k, and rows y < k / y >= 8 - k, for k = 1, 2.
    constexpr std::array<std::uint64_t, 3> kLowColumns{0, kColumnX0, kColumnX0 * 3U};
    constexpr std::array<std::uint64_t, 3> kHighColumns{0, kColumnX0 << 7U, (kColumnX0 * 3U) << 6U};
    constexpr std::array<std::uint64_t, 3> kLowRows{0, 0xFFULL, 0xFFFFULL};
    constexpr std::array<std::uint64_t, 3> kHighRows{0, 0xFFULL << 56U, 0xFFFFULL << 48U};
    const auto slot = [](int dx, int dy, int dz) { return static_cast<std::size_t>((dx + 1) * 9 + (dy + 1) * 3 + (dz + 1)); };

    const Bitset512 occupied = brick.occupancy();
    std::array<Bitset512, 27> block{};
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz) {
                if (dx == 0 && dy == 0 && dz == 0) { block[slot(0, 0, 0)] = occupied; continue; }
                if (const Brick* adjacent = voxels.find_brick({key.x + dx, key.y + dy, key.z + dz}))
                    block[slot(dx, dy, dz)] = adjacent->occupancy();
            }

    const auto erodeX = [&](const Bitset512& b, const Bitset512& plus, const Bitset512& minus) {
        Bitset512 out;
        for (std::size_t z = 0; z < 8U; ++z) {
            const std::uint64_t p = b.words[z];
            std::uint64_t kept = p;
            for (unsigned k = 1; k <= 2U; ++k) {
                kept &= ((p >> k) & ~kHighColumns[k]) | ((plus.words[z] & kLowColumns[k]) << (8U - k));
                kept &= ((p << k) & ~kLowColumns[k]) | ((minus.words[z] & kHighColumns[k]) >> (8U - k));
            }
            out.words[z] = kept;
        }
        return out;
    };
    const auto erodeY = [&](const Bitset512& b, const Bitset512& plus, const Bitset512& minus) {
        Bitset512 out;
        for (std::size_t z = 0; z < 8U; ++z) {
            const std::uint64_t p = b.words[z];
            std::uint64_t kept = p;
            for (unsigned k = 1; k <= 2U; ++k) {
                kept &= (p >> (8U * k)) | ((plus.words[z] & kLowRows[k]) << (64U - 8U * k));
                kept &= (p << (8U * k)) | ((minus.words[z] & kHighRows[k]) >> (64U - 8U * k));
            }
            out.words[z] = kept;
        }
        return out;
    };

    // x-erosion of the nine bricks in this brick's (y, z) neighbourhood, then
    // y-erosion of the three bricks in its z column, then z-erosion of this brick.
    std::array<Bitset512, 9> erodedX{};
    for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz)
            erodedX[static_cast<std::size_t>((dy + 1) * 3 + (dz + 1))] =
                erodeX(block[slot(0, dy, dz)], block[slot(1, dy, dz)], block[slot(-1, dy, dz)]);
    std::array<Bitset512, 3> erodedXY{};
    for (int dz = -1; dz <= 1; ++dz)
        erodedXY[static_cast<std::size_t>(dz + 1)] = erodeY(erodedX[static_cast<std::size_t>(3 + dz + 1)],
                                                            erodedX[static_cast<std::size_t>(6 + dz + 1)],
                                                            erodedX[static_cast<std::size_t>(dz + 1)]);
    const auto plane = [&](int z) -> std::uint64_t {
        if (z < 0) return erodedXY[0].words[static_cast<std::size_t>(z + 8)];
        if (z > 7) return erodedXY[2].words[static_cast<std::size_t>(z - 8)];
        return erodedXY[1].words[static_cast<std::size_t>(z)];
    };
    Bitset512 exposed;
    for (int z = 0; z < 8; ++z) {
        const std::uint64_t buried = plane(z - 2) & plane(z - 1) & plane(z) & plane(z + 1) & plane(z + 2);
        exposed.words[static_cast<std::size_t>(z)] = occupied.words[static_cast<std::size_t>(z)] & ~buried;
    }
    return exposed;
}

// Whether culling enclosed voxels of `object` is invisible for this view. Splats are
// filled squares of side 2r+1 px with r = max(1, int(clamp(0.72 * edge, 1, 12))),
// where edge is the projected voxel size; those squares tile the projected lattice
// without gaps while edge <= 24 px. Further requirements:
//  - the whole object is beyond the near plane (a clipped surface would expose the
//    interior as a cross-section);
//  - the projection is no narrower than the field of view the splat size is
//    computed from (a zoomed physical lens makes splats smaller than the spacing);
//  - the off-screen margin (draw items are kept out to 1.2x the viewport) is wide
//    enough that the voxels covering a near-edge interior voxel are kept too.
// Checked at the object's nearest depth, where voxels project largest. When any of
// this fails, every voxel of the object is drawn as before.
bool surface_splats_cover_object(const EditorObject& object, const EditorCamera& camera, UiRect viewport, const RigidTransform* pose = nullptr) noexcept {
    if (viewport.width <= 0 || viewport.height <= 0 || !(object.voxelSizeMeters > 0.0F)) return false;
    const float width = static_cast<float>(viewport.width);
    const float height = static_cast<float>(viewport.height);
    const float marginPixels = 0.1F * std::min(width, height);
    const float maximumEdge = std::min(24.0F, (marginPixels - 1.0F) * 0.5F);
    if (!(maximumEdge >= 1.0F)) return false;

    const EditorObjectBounds bounds = pose ? object_world_bounds(object, *pose) : object_world_bounds(object);
    if (!bounds.valid) return false;
    const Float3 forward = camera_basis(camera).forward;
    float nearest = std::numeric_limits<float>::infinity();
    for (Float3 corner : bounds_corners(bounds.minimum, bounds.maximum))
        nearest = std::min(nearest, dot(subtract(corner, camera.position), forward));
    // Bounds are voxel-centre bounds; keep a full voxel of clearance from the near plane.
    if (!(nearest - object.voxelSizeMeters > camera.nearPlane)) return false;

    if (camera.projection == EditorProjection::Orthographic) {
        const float edge = object.voxelSizeMeters * height / std::max(0.001F, camera.orthographicHeight);
        return edge <= maximumEdge;
    }
    const float aspect = width / height;
    const float projectedTangent =
        std::tan(std::clamp(editor_camera_vertical_fov(camera, aspect), 0.1F, kPi - 0.1F) * 0.5F);
    const float splatTangent = std::tan(camera.verticalFovRadians * 0.5F);
    if (!(projectedTangent >= splatTangent * 0.999F)) return false;
    const float edge = object.voxelSizeMeters * height / (2.0F * (nearest - object.voxelSizeMeters) * projectedTangent);
    return edge <= maximumEdge;
}

} // namespace

namespace {

// Order-preserving map from a finite float to a uint32: ascending keys order
// like the floats. -0.0 is canonicalized to +0.0 first so both zeroes share a
// key and tie exactly as float equality ties them. (Depths reaching the draw
// list are always finite: project_with rejects NaN and out-of-range depths.)
std::uint32_t depth_sort_key(float depth) noexcept {
    if (depth == 0.0F) depth = 0.0F;
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(depth);
    return (bits & 0x8000'0000U) != 0 ? ~bits : bits | 0x8000'0000U;
}

// The draw order: depth descending, then object id, then voxel. No two
// distinct items compare equal (each voxel is emitted once per object), so
// this is a total order and any correct sort yields the same sequence.
bool voxel_draw_item_less(const EditorVoxelDrawItem& left, const EditorVoxelDrawItem& right) noexcept {
    if (left.depth != right.depth) return left.depth > right.depth;
    if (left.objectId != right.objectId) return left.objectId < right.objectId;
    return left.voxel < right.voxel;
}

// Far-to-near sort of the draw list. A comparison sort spends most of its
// time on the float depth key, so for large lists this radix-sorts 8-byte
// {depth key, original index} proxies by depth alone (4 stable byte passes),
// applies the resulting permutation to the items exactly once, and then
// repairs each maximal run of equal depths with the full comparator; small
// lists keep the plain comparison sort. Sorting the 48-byte items by radix
// directly was measured slower than the comparison sort at 120k items (the
// records scatter outside the cache), while the proxies stay cache-resident.
// The result is item-for-item identical to sorting everything with
// voxel_draw_item_less: equal depths share a radix key, different keys are
// depth-ordered, and the tie-break keys decide within a run exactly as the
// comparator would.
void sort_voxel_draw_items(std::vector<EditorVoxelDrawItem>& items) {
    // The radix path is used only inside the size window where it was
    // measured faster than the comparison sort on the benchmark scenes:
    // at 46k items it roughly halves the sort, but at 120k items the
    // permutation gather ranges over ~12 MB of item arrays and falls off a
    // cache/TLB cliff, ending up slower than the comparison sort.
    constexpr std::size_t kRadixSortMinimum = 8192;
    constexpr std::size_t kRadixSortMaximum = 65536;
    const std::size_t count = items.size();
    if (count < kRadixSortMinimum || count > kRadixSortMaximum) {
        std::stable_sort(items.begin(), items.end(), voxel_draw_item_less);
        return;
    }
    struct SortProxy {
        std::uint32_t key;
        std::uint32_t index;
    };
    // Descending depth == ascending bitwise complement of the ascending key.
    std::vector<SortProxy> proxies(count);
    for (std::size_t i = 0; i < count; ++i)
        proxies[i] = {~depth_sort_key(items[i].depth), static_cast<std::uint32_t>(i)};
    std::vector<SortProxy> scratchProxies(count);
    std::vector<SortProxy>* source = &proxies;
    std::vector<SortProxy>* dest = &scratchProxies;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        std::array<std::size_t, 256> offsets{};
        for (const SortProxy& proxy : *source) ++offsets[(proxy.key >> shift) & 0xFFU];
        std::size_t sum = 0;
        for (auto& offset : offsets) {
            const std::size_t bucket = offset;
            offset = sum;
            sum += bucket;
        }
        for (const SortProxy& proxy : *source)
            (*dest)[offsets[(proxy.key >> shift) & 0xFFU]++] = proxy;
        std::swap(source, dest);
    }
    // Four passes is even, so the sorted proxies are back in `proxies`.
    std::vector<EditorVoxelDrawItem> sorted(count);
    for (std::size_t i = 0; i < count; ++i) sorted[i] = items[proxies[i].index];
    items.swap(sorted);
    std::size_t runBegin = 0;
    for (std::size_t i = 1; i <= count; ++i) {
        if (i == count || items[i].depth != items[runBegin].depth) {
            if (i - runBegin > 1)
                std::sort(items.begin() + static_cast<std::ptrdiff_t>(runBegin),
                          items.begin() + static_cast<std::ptrdiff_t>(i), voxel_draw_item_less);
            runBegin = i;
        }
    }
}

// The sort is most of a large rebuild (about 17 ms of 20 ms for a capped 120,000-item list
// here), so with a worker pool the list is cut into one run per thread, the runs are sorted
// at the same time, and pairs of runs are merged level by level (also in parallel). The
// order is a total order, so the result is item-for-item the single-threaded sort's.
constexpr std::size_t kParallelSortMinimum = 32768;

void sort_voxel_draw_items(std::vector<EditorVoxelDrawItem>& items, JobSystem* jobs) {
    const std::size_t count = items.size();
    const std::size_t threads = jobs != nullptr ? jobs->worker_count() + 1U : 1U;
    if (threads < 2U || count < kParallelSortMinimum) {
        sort_voxel_draw_items(items);
        return;
    }
    const std::size_t runs = std::min(threads, count / (kParallelSortMinimum / 4U));
    std::vector<std::size_t> bounds(runs + 1U);
    for (std::size_t run = 0; run <= runs; ++run) bounds[run] = count * run / runs;
    jobs->parallel_for(runs, [&](std::size_t run) {
        std::vector<EditorVoxelDrawItem> part(items.begin() + static_cast<std::ptrdiff_t>(bounds[run]),
                                              items.begin() + static_cast<std::ptrdiff_t>(bounds[run + 1U]));
        sort_voxel_draw_items(part);
        std::copy(part.begin(), part.end(), items.begin() + static_cast<std::ptrdiff_t>(bounds[run]));
    });
    std::vector<EditorVoxelDrawItem> scratch(count);
    std::vector<EditorVoxelDrawItem>* source = &items;
    std::vector<EditorVoxelDrawItem>* target = &scratch;
    for (std::size_t width = 1; width < runs; width *= 2U) {
        const std::size_t pairs = (runs + 2U * width - 1U) / (2U * width);
        jobs->parallel_for(pairs, [&](std::size_t pair) {
            const std::size_t first = bounds[pair * 2U * width];
            const std::size_t middle = bounds[std::min(runs, pair * 2U * width + width)];
            const std::size_t last = bounds[std::min(runs, pair * 2U * width + 2U * width)];
            const auto begin = source->begin();
            std::merge(begin + static_cast<std::ptrdiff_t>(first), begin + static_cast<std::ptrdiff_t>(middle),
                       begin + static_cast<std::ptrdiff_t>(middle), begin + static_cast<std::ptrdiff_t>(last),
                       target->begin() + static_cast<std::ptrdiff_t>(first), voxel_draw_item_less);
        });
        std::swap(source, target);
    }
    if (source != &items) items.swap(*source);
}

} // namespace

std::vector<EditorVoxelDrawItem> build_voxel_draw_list(
    const EditorDocument& document,
    const EditorMaterialLibrary& materials,
    const EditorCamera& camera,
    UiRect viewport,
    const EditorViewportSettings& settings,
    const std::set<EditorObjectId>& selectedObjects) {
    return build_voxel_draw_list(document, materials, camera, viewport, settings, selectedObjects, nullptr);
}

std::vector<EditorVoxelDrawItem> build_voxel_draw_list(
    const EditorDocument& document,
    const EditorMaterialLibrary& materials,
    const EditorCamera& camera,
    UiRect viewport,
    const EditorViewportSettings& settings,
    const std::set<EditorObjectId>& selectedObjects,
    JobSystem* jobs,
    const std::map<EditorObjectId, RigidTransform>* presentationPoses) {
    std::vector<EditorVoxelDrawItem> result;
    result.reserve(std::min<std::size_t>(settings.maximumDrawVoxels, 16384));
    const float viewportHeight = static_cast<float>(std::max(1, viewport.height));
    const float tangent = std::tan(camera.verticalFovRadians * 0.5F);
    const ScreenProjector projector = make_screen_projector(camera, viewport);
    std::map<BrickKey, Bitset512> anchorMasks;
    for (const auto& [id, object] : document.objects()) {
        if (!object.flags.visible) continue;
        if (!settings.isolatedObjects.empty() && !settings.isolatedObjects.contains(id)) continue;
        const bool selected = selectedObjects.contains(id);
        const RigidTransform* pose = nullptr;
        if (presentationPoses) {
            const auto found = presentationPoses->find(id);
            if (found != presentationPoses->end()) pose = &found->second;
        }
        const bool cullEnclosed = settings.cullEnclosedVoxels &&
                                  surface_splats_cover_object(object, camera, viewport, pose);
        anchorMasks.clear();
        if (!object.anchors.empty()) {
            for (const Int3& anchor : object.anchors)
                anchorMasks[brick_key_from_voxel(anchor)].set(voxel_index_unchecked(local_voxel_from_global(anchor)));
        }
        for (const auto& entry : object.voxels->bricks()) {
            const BrickKey key = entry.first;
            const Brick& brick = entry.second;
            const Bitset512* anchorMask = nullptr;
            if (!anchorMasks.empty()) {
                if (const auto anchors = anchorMasks.find(key); anchors != anchorMasks.end())
                    anchorMask = &anchors->second;
            }
            Bitset512 emitted = cullEnclosed ? exposed_voxel_mask(*object.voxels, key, brick) : brick.occupancy();
            if (cullEnclosed && anchorMask) {
                const Bitset512 occupied = brick.occupancy();
                for (std::size_t word = 0; word < emitted.words.size(); ++word)
                    emitted.words[word] |= anchorMask->words[word] & occupied.words[word];
            }
            emitted.for_each_set([&](std::uint16_t index) {
                if (result.size() >= settings.maximumDrawVoxels) return;
                const Int3 voxel = global_from_local(key, local_from_index_unchecked(index));
                const Float3 world = voxel_center_world(object, voxel, pose);
                const ScreenPoint screen = project_with(projector, world);
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
                                  selected, anchorMask && anchorMask->test(index)});
            });
            if (result.size() >= settings.maximumDrawVoxels) break;
        }
        if (result.size() >= settings.maximumDrawVoxels) break;
    }
    sort_voxel_draw_items(result, jobs);
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

namespace {
// Word-at-a-time mixer (one multiply per 64-bit input instead of eight for byte-wise
// FNV-1a). Fingerprints are compared only within a process, never persisted.
struct FingerprintHasher {
    std::uint64_t h{0x9E3779B97F4A7C15ULL};
    void u64(std::uint64_t v) noexcept {
        h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
        h *= 0xBF58476D1CE4E5B9ULL;
        h ^= h >> 31U;
    }
    void f32(float v) noexcept { u64(std::bit_cast<std::uint32_t>(v)); }
    void f3(Float3 v) noexcept { f32(v.x); f32(v.y); f32(v.z); }
    void i32(std::int32_t v) noexcept { u64(static_cast<std::uint32_t>(v)); }
};
} // namespace

std::uint64_t editor_scene_render_fingerprint(const EditorDocument& document) noexcept {
    FingerprintHasher hash;
    hash.u64(document.objects().size());
    for (const auto& [id, object] : document.objects()) {
        hash.u64(id);
        hash.u64(object.parent ? *object.parent + 1U : 0U);
        const auto& f = object.flags;
        hash.u64((f.visible ? 1U : 0U) | (f.locked ? 2U : 0U) | (f.anchored ? 4U : 0U) |
                 (f.structural ? 8U : 0U) | (f.collisionEnabled ? 16U : 0U) | (f.decorative ? 32U : 0U));
        hash.f3(object.transform.position);
        hash.f32(object.transform.rotation.x); hash.f32(object.transform.rotation.y);
        hash.f32(object.transform.rotation.z); hash.f32(object.transform.rotation.w);
        hash.f32(object.voxelSizeMeters);
        hash.u64(reinterpret_cast<std::uintptr_t>(object.voxels.get()));
        if (object.voxels) {
            // The voxel revision changes on every content mutation and is unique per
            // process, so it stands in for walking every brick (O(objects), not O(bricks)).
            hash.u64(object.voxels->revision());
            hash.u64(object.voxels->brick_count());
        }
        hash.u64(object.anchors.size());
        for (const Int3& anchor : object.anchors) { hash.i32(anchor.x); hash.i32(anchor.y); hash.i32(anchor.z); }
    }
    return hash.h;
}

std::uint64_t editor_camera_fingerprint(const EditorCamera& camera) noexcept {
    FingerprintHasher hash;
    hash.f3(camera.position); hash.f3(camera.target); hash.f3(camera.worldUp);
    hash.f32(camera.verticalFovRadians); hash.f32(camera.orthographicHeight);
    hash.f32(camera.nearPlane); hash.f32(camera.farPlane);
    hash.u64(static_cast<std::uint64_t>(camera.projection));
    const auto& lens = camera.physicalLens;
    hash.u64(lens.enabled ? 1U : 0U);
    hash.f32(lens.focalLengthMillimeters); hash.f32(lens.sensorWidthMillimeters);
    hash.f32(lens.sensorHeightMillimeters); hash.f32(lens.lensShiftX); hash.f32(lens.lensShiftY);
    hash.f32(lens.apertureFStop); hash.f32(lens.focusDistanceMeters);
    hash.u64(static_cast<std::uint64_t>(lens.gateFit));
    return hash.h;
}

std::uint64_t editor_selection_fingerprint(const std::set<EditorObjectId>& selection) noexcept {
    FingerprintHasher hash;
    hash.u64(selection.size());
    for (EditorObjectId id : selection) hash.u64(id);
    return hash.h;
}

namespace {
struct SharedDrawJobs {
    std::mutex mutex;
    std::unique_ptr<JobSystem> jobs;  // created on first use, joined at exit
};
SharedDrawJobs& shared_draw_jobs() {
    static SharedDrawJobs shared;
    return shared;
}
} // namespace

const std::vector<EditorVoxelDrawItem>& EditorVoxelDrawListCache::get(
    const EditorDocument& document,
    const EditorMaterialLibrary& materials,
    const EditorCamera& camera,
    UiRect viewport,
    const EditorViewportSettings& settings,
    const std::set<EditorObjectId>& selectedObjects,
    std::uint64_t sceneFingerprint,
    const std::map<EditorObjectId, RigidTransform>* presentationPoses) {
    FingerprintHasher hash;
    hash.u64(sceneFingerprint);
    if (presentationPoses) for (const auto& [id, pose] : *presentationPoses) {
        hash.u64(id);
        for (const float v : {pose.position.x, pose.position.y, pose.position.z,
                            pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w})
            hash.u64(std::bit_cast<std::uint32_t>(v));
    }
    hash.u64(editor_camera_fingerprint(camera));
    hash.i32(viewport.x); hash.i32(viewport.y); hash.i32(viewport.width); hash.i32(viewport.height);
    hash.u64(settings.maximumDrawVoxels);
    hash.u64(settings.cullEnclosedVoxels ? 1U : 0U);
    hash.u64(editor_selection_fingerprint(selectedObjects));
    hash.u64(editor_selection_fingerprint(settings.isolatedObjects) ^ 0x150A7EDULL);
    if (!valid_ || hash.h != key_) {
        // Large lists are sorted on a shared worker pool. Only one thread uses the pool at a
        // time; anyone else (another controller on another thread) sorts alone.
        SharedDrawJobs& shared = shared_draw_jobs();
        std::unique_lock poolLock(shared.mutex, std::defer_lock);
        JobSystem* jobs = nullptr;
        if (JobSystem::default_worker_count() > 0 && poolLock.try_lock()) {
            if (!shared.jobs) shared.jobs = std::make_unique<JobSystem>(JobSystem::default_worker_count());
            jobs = shared.jobs.get();
        }
        items_ = build_voxel_draw_list(document, materials, camera, viewport, settings, selectedObjects, jobs, presentationPoses);
        key_ = hash.h;
        valid_ = true;
        ++rebuilds_;
    }
    return items_;
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
