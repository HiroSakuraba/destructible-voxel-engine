#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dve/camera_system.hpp"
#include "dve/editor_document.hpp"
#include "dve/editor_materials.hpp"
#include "dve/query.hpp"

namespace dve {
class JobSystem;
} // namespace dve

namespace dve::editor {

struct UiRect {
    int x{};
    int y{};
    int width{};
    int height{};
    [[nodiscard]] bool contains(int px, int py) const noexcept {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

enum class EditorProjection : std::uint8_t { Perspective, Orthographic };

struct EditorCamera {
    Float3 position{8.0F, 7.0F, 10.0F};
    Float3 target{0.0F, 1.5F, 0.0F};
    Float3 worldUp{0.0F, 1.0F, 0.0F};
    float verticalFovRadians{0.87266463F}; // 50 degrees
    float orthographicHeight{12.0F};
    float nearPlane{0.01F};
    float farPlane{10000.0F};
    EditorProjection projection{EditorProjection::Perspective};
    camera::CameraPhysicalLens physicalLens{};
};

struct CameraBasis {
    Float3 forward{};
    Float3 right{};
    Float3 up{};
};

struct ViewportRay {
    Float3 origin{};
    Float3 direction{};
};

struct ScreenPoint {
    float x{};
    float y{};
    float depth{};
    bool visible{};
};

struct EditorPickResult {
    EditorObjectId objectId{};
    Int3 voxel{};
    Int3 normal{};
    MaterialId material{kAirMaterial};
    Float3 worldPosition{};
    Float3 worldNormal{};
    float worldDistance{};
};

struct EditorVoxelDrawItem {
    EditorObjectId objectId{};
    Int3 voxel{};
    MaterialId material{kAirMaterial};
    float screenX{};
    float screenY{};
    float depth{};
    float pixelRadius{1.0F};
    bool selected{};
    bool anchored{};
};

struct EditorText3DDrawItem {
    EditorObjectId objectId{};
    std::string text;
    UiRect screenBounds{};
    float depth{};
    std::uint32_t faceMaterialId{};
    std::uint32_t sideMaterialId{};
    bool selected{};
};

struct EditorGaborVolumeDrawItem {
    EditorObjectId objectId{};
    UiRect screenBounds{};
    float depth{};
    std::size_t primitiveCount{};
    std::uint16_t maximumLodLevel{};
    Float3 albedoTint{1.0F, 1.0F, 1.0F};
    float densityMultiplier{1.0F};
    bool selected{};
};

struct EditorViewportSettings {
    bool showGrid{true};
    bool showAnchors{true};
    bool showCollision{false};
    bool showObjectBounds{true};
    bool xraySelection{};
    // Skip voxels buried at least two layers deep (whole 5x5x5 neighbourhood solid)
    // when the splats in front of them cover them; frames were pixel-identical with
    // and without culling in testing (see build_voxel_draw_list). Anchored voxels
    // are always emitted.
    // Buried voxels otherwise consume the maximumDrawVoxels cap and projection/sort
    // time without being visible.
    bool cullEnclosedVoxels{true};
    std::size_t maximumDrawVoxels{120000};
    float gridSpacingMeters{1.0F};
};

struct EditorObjectBounds {
    Float3 minimum{};
    Float3 maximum{};
    bool valid{};
};

[[nodiscard]] CameraBasis camera_basis(const EditorCamera& camera) noexcept;
[[nodiscard]] float editor_camera_vertical_fov(const EditorCamera& camera, float aspectRatio) noexcept;
[[nodiscard]] camera::CameraPose to_camera_pose(const EditorCamera& camera, float aspectRatio) noexcept;
void apply_camera_pose(EditorCamera& camera, const camera::CameraPose& pose) noexcept;
[[nodiscard]] ViewportRay make_viewport_ray(
    const EditorCamera& camera,
    UiRect viewport,
    float screenX,
    float screenY) noexcept;
[[nodiscard]] ScreenPoint project_world_to_screen(
    const EditorCamera& camera,
    UiRect viewport,
    Float3 worldPoint) noexcept;

void orbit_camera(EditorCamera& camera, float deltaX, float deltaY, float sensitivity = 0.006F) noexcept;
void look_camera(EditorCamera& camera, float deltaX, float deltaY, float sensitivity = 0.006F) noexcept;
void pan_camera(EditorCamera& camera, float deltaX, float deltaY, UiRect viewport) noexcept;
void zoom_camera(EditorCamera& camera, float wheelSteps) noexcept;
void fly_camera(EditorCamera& camera, Float3 localMotion, float elapsedSeconds, float speed) noexcept;
void frame_camera_on_bounds(EditorCamera& camera, const EditorObjectBounds& bounds) noexcept;

[[nodiscard]] EditorObjectBounds object_world_bounds(const EditorObject& object) noexcept;
[[nodiscard]] EditorObjectBounds object_world_bounds(const EditorObject& object, const RigidTransform& pose) noexcept;
[[nodiscard]] std::optional<EditorPickResult> pick_editor_document(
    const EditorDocument& document,
    ViewportRay ray,
    float maximumWorldDistance = 10000.0F);
[[nodiscard]] std::vector<EditorVoxelDrawItem> build_voxel_draw_list(
    const EditorDocument& document,
    const EditorMaterialLibrary& materials,
    const EditorCamera& camera,
    UiRect viewport,
    const EditorViewportSettings& settings,
    const std::set<EditorObjectId>& selectedObjects);
[[nodiscard]] std::vector<EditorVoxelDrawItem> build_voxel_draw_list(
    const EditorDocument& document,
    const EditorMaterialLibrary& materials,
    const EditorCamera& camera,
    UiRect viewport,
    const EditorViewportSettings& settings,
    std::optional<EditorObjectId> selectedObject);
// Same list, with the depth sort of large lists split across `jobs` (item-for-item
// identical to the single-threaded build). `jobs` must not be in use by another thread;
// nullptr builds on the calling thread.
[[nodiscard]] std::vector<EditorVoxelDrawItem> build_voxel_draw_list(
    const EditorDocument& document,
    const EditorMaterialLibrary& materials,
    const EditorCamera& camera,
    UiRect viewport,
    const EditorViewportSettings& settings,
    const std::set<EditorObjectId>& selectedObjects,
    JobSystem* jobs,
    const std::map<EditorObjectId, RigidTransform>* presentationPoses = nullptr);

// Cheap O(objects + anchors) fingerprint of everything that affects the voxel draw
// list and selection diagnostics: object ids/parents/flags/transforms/voxel sizes,
// each VoxelObject's content revision (which changes on every voxel edit) and brick
// count, and anchors. Used to rebuild cached per-frame data only when the scene
// actually changed.
[[nodiscard]] std::uint64_t editor_scene_render_fingerprint(const EditorDocument& document) noexcept;
[[nodiscard]] std::uint64_t editor_camera_fingerprint(const EditorCamera& camera) noexcept;
[[nodiscard]] std::uint64_t editor_selection_fingerprint(const std::set<EditorObjectId>& selection) noexcept;

// Memoizes build_voxel_draw_list (projection + depth sort). get() rebuilds only
// when the scene fingerprint, camera, viewport, draw cap, culling setting, or
// selection change.
class EditorVoxelDrawListCache {
public:
    const std::vector<EditorVoxelDrawItem>& get(
        const EditorDocument& document,
        const EditorMaterialLibrary& materials,
        const EditorCamera& camera,
        UiRect viewport,
        const EditorViewportSettings& settings,
        const std::set<EditorObjectId>& selectedObjects,
        std::uint64_t sceneFingerprint,
        const std::map<EditorObjectId, RigidTransform>* presentationPoses = nullptr);
    void invalidate() noexcept { valid_ = false; }
    [[nodiscard]] std::uint64_t rebuild_count() const noexcept { return rebuilds_; }

private:
    std::vector<EditorVoxelDrawItem> items_{};
    std::uint64_t key_{};
    bool valid_{};
    std::uint64_t rebuilds_{};
};

[[nodiscard]] std::vector<EditorText3DDrawItem> build_text3d_draw_list(
    const EditorDocument& document,
    const EditorCamera& camera,
    UiRect viewport,
    const std::set<EditorObjectId>& selectedObjects);
[[nodiscard]] std::vector<EditorText3DDrawItem> build_text3d_draw_list(
    const EditorDocument& document,
    const EditorCamera& camera,
    UiRect viewport,
    std::optional<EditorObjectId> selectedObject);

[[nodiscard]] std::vector<EditorGaborVolumeDrawItem> build_gabor_volume_draw_list(
    const EditorDocument& document,
    const EditorCamera& camera,
    UiRect viewport,
    const std::set<EditorObjectId>& selectedObjects);
[[nodiscard]] std::vector<EditorGaborVolumeDrawItem> build_gabor_volume_draw_list(
    const EditorDocument& document,
    const EditorCamera& camera,
    UiRect viewport,
    std::optional<EditorObjectId> selectedObject);

} // namespace dve::editor
