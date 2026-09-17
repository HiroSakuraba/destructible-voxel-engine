#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dve/camera_system.hpp"
#include "dve/editor_document.hpp"
#include "dve/editor_materials.hpp"
#include "dve/query.hpp"

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
