#pragma once
#include "dve/simulation_clock.hpp"

#include <array>
#include <chrono>
#include <deque>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "dve/audio/midi_input_session.hpp"
#include "dve/audio/midi_output_session.hpp"
#include "dve/audio/mixer.hpp"
#include "dve/editor_asset_browser.hpp"
#include "dve/ai/live_editor_mcp.hpp"
#include "dve/camera_system.hpp"
#include "dve/editor_diagnostics.hpp"
#include "dve/editor_component_inspector.hpp"
#include "dve/editor_control_rig.hpp"
#include "dve/editor_chiptune.hpp"
#include "dve/editor_cinematic_camera.hpp"
#include "dve/editor_synth.hpp"
#include "dve/editor_sprite_authoring.hpp"
#include "dve/editor_workspace_state.hpp"
#include "dve/sprite_animation_graph_renderer.hpp"
#include "dve/sprite_diagnostics.hpp"
#include "dve/render3d_diagnostics.hpp"
#include "dve/sprite_pixel_art.hpp"
#include "dve/sprite_rig2d.hpp"
#include "dve/tilemap_authoring.hpp"
#include "dve/sprite_vertical_slice_presentation.hpp"
#include "dve/editor_tasks.hpp"
#include "dve/editor_audio.hpp"
#include "dve/editor_audio_event.hpp"
#include "dve/editor_materials.hpp"
#include "dve/editor_play_session.hpp"
#include "dve/editor_runtime_settings.hpp"
#include "dve/collision_proxy.hpp"
#include "dve/editor_scatter.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/editor_mirror.hpp"
#include "dve/editor_subdivision.hpp"
#include "dve/editor_model_import.hpp"
#include "dve/editor_voxel_join.hpp"
#include "dve/editor_voxel_selection.hpp"
#include "dve/editor_number_field.hpp"
#include "dve/editor_duplicate_special.hpp"
#include "dve/editor_text3d.hpp"
#include "dve/editor_tools.hpp"
#include "dve/editor_viewport.hpp"
#include "dve/editor_voxel_boolean.hpp"
#include "dve/editor_voxel_slice.hpp"
#include "dve/editor_navigation_filter.hpp"
#include "dve/editor_workspace.hpp"

namespace dve::editor {

// Top-level menu bar categories, in display order. This is the single source of truth for
// both NativeEditorController::menu_name_at()'s hit-testing and every platform host's menu
// bar rendering (apps/dve_native_editor_x11.cpp today); previously each kept its own copy of
// this list and the pixel-width formula that goes with it, which is exactly the kind of
// duplication that silently drifts (add a menu in one place, forget the other, and clicks hit
// the wrong menu). "Window" mirrors Unreal Editor's placement (right after Edit) and is what
// toggles the Scene Hierarchy / Inspector panels and the bottom panel's Assets tab.
inline constexpr std::array<std::string_view, 9> kMenuBarNames{
    "File", "Edit", "Create", "View", "Subdivision", "Tools", "Build", "Window", "Help"};
// Short titles, index-aligned with kMenuBarNames, drawn when a full title does not fit its menu
// bar slot (narrow windows; "Subdivision" is wider than the 71 px slot at 640 px).
inline constexpr std::array<std::string_view, kMenuBarNames.size()> kCompactMenuBarNames{
    "File", "Edit", "New", "View", "Subdiv", "Tools", "Build", "Win", "Help"};

struct EditorPrefabAsset;

enum class EditorToolId : std::uint8_t {
    Select,
    Translate,
    AddVoxel,
    RemoveVoxel,
    PaintMaterial,
    Box,
    Beam,
    Anchor,
    Rotate,
    Scale,
    ScatterBrush,
};

// Tools with a toolbar button, in button order. Scale has no button; it is selected by its
// shortcut or the Edit menu, but still has an editor_tool_info() entry.
inline constexpr std::array<EditorToolId, 10> kToolbarTools{
    EditorToolId::Select, EditorToolId::Translate, EditorToolId::AddVoxel, EditorToolId::RemoveVoxel,
    EditorToolId::PaintMaterial, EditorToolId::Box, EditorToolId::Beam, EditorToolId::Anchor,
    EditorToolId::Rotate, EditorToolId::ScatterBrush};
inline constexpr std::size_t kEditorToolCount = kToolbarTools.size();
// Voxel sizes Scale Voxel Size keeps objects within (1 mm .. 10 m per voxel).
inline constexpr float kMinVoxelSizeMeters = 0.001F;
inline constexpr float kMaxVoxelSizeMeters = 10.0F;
// Height of the inspector's fixed "INSPECTOR" header; scrolled details never draw above it.
inline constexpr int kInspectorHeaderHeight = 28;

// What the toolbar, its tooltips and the viewport hint say about each tool. One table, so the
// button label, the tooltip and the hint cannot drift apart.
struct EditorToolInfo {
    std::string_view name;         // full button label
    std::string_view shortName;    // used when the full name does not fit the button
    std::string_view actionId;     // command whose shortcut selects the tool; empty = none
    std::string_view description;  // what a click or drag does
    std::string_view accepts;      // what the tool acts on
};
[[nodiscard]] const EditorToolInfo& editor_tool_info(EditorToolId tool) noexcept;

enum class EditorTransformSpace : std::uint8_t { World, Local };
enum class PlacementTarget : std::uint8_t { NoTarget, Surface, VoxelCorner, VoxelEdge, VoxelFace, BoundsVertex, CollisionBoxVertex };
enum class PivotMode : std::uint8_t { BoundsCenter, ActiveObject, WorldOrigin, Custom };

enum class PointerButton : std::uint8_t { NoButton, Primary, Auxiliary, Secondary, Extra1 };

enum class EditorFocusRegion : std::uint8_t {
    MenuBar,
    Toolbar,
    SceneHierarchy,
    Viewport,
    Inspector,
    BottomPanel,
};

// The bottom panel is a single screen region with tab-selectable content, matching the
// Problems / Tasks / Console / Profiler labels that were already painted there before any of
// them actually showed anything. Assets is a fifth tab rather than a separate toggleable
// panel: it reuses this already-tabbed region instead of inventing a new layout slot.
enum class BottomPanelTab : std::uint8_t { Problems, Tasks, Console, Profiler, Assets, Assistant };
enum class PendingDestructiveAction : std::uint8_t { Inactive, Quit, NewProject, NewScene, OpenScene, OpenProject };

// What a click-to-edit text field is currently bound to, if any. Position and Rotation each
// edit all three components as one space-separated line (matching how they are already
// displayed as one line each) rather than one field per axis; that halves the interaction
// surface for a modest loss of precision (can't retype just Y), which is the right trade for
// a first pass at "these are editable at all."
enum class TextEditKind : std::uint8_t {
    Inactive, ObjectName, Position, Rotation, Text3DContent, Text3DSize, Text3DDepth,
    Text3DLetterSpacing, Text3DFaceMaterial, Text3DSideMaterial, Text3DFontPath,
    Text3DAlignment, Text3DFillRule, GaborDensity, GaborTint, GaborEmission,
    GaborAnisotropy, GaborShadowStrength, GaborLodBias, HierarchyFilter, AssetSearch,
    AssetRename, AiPrompt, ScatterSetting, SlicePoint, SliceNormal,
    VoxelSize, BrushRadius, SurfaceOffset,  // numeric fields (see NumberFieldId)
    DuplicateSetting                        // Duplicate Special panel; objectId holds the NumberFieldId
};

struct TextEditState {
    TextEditKind kind{TextEditKind::Inactive};
    EditorObjectId objectId{}; // meaningful for ObjectName
    std::string buffer;
    std::string error; // set when a commit attempt fails validation/parsing; cleared on edit
    // True immediately after begin_text_edit(): the buffer is pre-filled with the current
    // value (so Enter with no changes is a no-op commit), but there is no cursor-positioning
    // support, so the standard "F2 selects the whole name, first keystroke replaces it"
    // convention is the only usable behavior; without this, typing would append after the
    // pre-filled text instead of replacing it.
    bool replaceOnNextInput{true};
    int axis{};                     // Position / Rotation: 0 = X, 1 = Y, 2 = Z
    std::set<EditorObjectId> transformSelection;
    std::vector<ObjectTransformChange> transformSnapshot;
    Float3 transformPivot{};
};

// Numeric fields (ART-084/085): inspector Position / Rotation (per axis) and Voxel size; the
// viewport tool strip's Brush radius and Surface offset. Each can be typed (units, arithmetic,
// += -= *= /=), scrubbed by dragging its label, and reset/copied/pasted with a right-click.
enum class NumberFieldId : std::uint8_t {
    Position, Rotation, VoxelSize, BrushRadius, SurfaceOffset,
    // Duplicate Special panel (Edit > Duplicate Special)
    DuplicateCopies, DuplicateOffset, DuplicateRotation, DuplicateScale, DuplicatePoint
};
struct NumberFieldRect {
    NumberFieldId field{};
    int axis{};          // 0..2 for Position / Rotation, else 0
    UiRect label{};      // drag to scrub ("X", "Y", "Z" or the row name)
    UiRect box{};        // click to type
};
// Duplicate Special panel: clickable choices and buttons (the numbers are NumberFieldRects).
struct DuplicatePanelLayout {
    UiRect panel{};
    int titleY{};
    UiRect pivotEach{}, pivotPoint{};       // Pivot: Each copy | Around a point
    UiRect spaceLocal{}, spaceWorld{};      // Offset in: Local | World (Each copy)
    UiRect pointOrigin{}, pointSelection{}; // Set the point: world origin | selection centre
    UiRect group{};                         // Put copies in a group
    UiRect apply{}, cancel{};
    int noteY{};
    std::vector<std::pair<NumberFieldId, UiRect>> rowNames;  // row label positions
    std::vector<NumberFieldRect> fields;                     // the number boxes
};
// One selection rule everywhere (objects, hierarchy, voxels, mesh components): Shift adds,
// Ctrl removes, Ctrl+Shift intersects; Alt also removes where navigation does not use it.
// A Shift+click on something already selected removes it (see click handling).
[[nodiscard]] inline SelectionOperation selection_operation_for_modifiers(std::uint32_t modifiers) noexcept {
    if ((modifiers & 3U) == 3U) return SelectionOperation::Intersect;
    if ((modifiers & 2U) != 0U || (modifiers & 4U) != 0U) return SelectionOperation::Subtract;
    if ((modifiers & 1U) != 0U) return SelectionOperation::Add;
    return SelectionOperation::Replace;
}
enum class InspectorFlagState : std::uint8_t { Off, On, Mixed };
struct InspectorSelectionSummary {
    std::size_t count{};
    bool positionMixed{};
    bool rotationMixed{};
    std::array<InspectorFlagState, 5> flags{};
};

struct ContextMenuItem {
    std::string actionId;
    std::string label;
};

struct ContextMenuState {
    bool open{};
    int x{};
    int y{};
    int width{190};
    std::vector<ContextMenuItem> items;
    std::vector<UiRect> itemRects; // parallel to items; computed once when opened
    std::optional<std::size_t> hoveredItem;
};

// What Select and Move act on (M toggles): whole objects or voxel cells of one object.
enum class SelectionDomain : std::uint8_t { Objects, Voxels, Polygons };
enum class VoxelClickSelection : std::uint8_t { Cell, ConnectedPiece };
// What viewport clicks, boxes, Select All, Invert and Select Similar may pick up (ART-015).
// Masked-out objects are transparent to clicks: the click reaches whatever is behind them.
// The hierarchy panel still selects anything, so masked objects stay reachable.
struct SelectionMask {
    bool locked{true};   // locked objects
    bool helpers{true};  // objects without geometry or children: cameras, lights, spawns, emitters
    bool prefabs{true};  // prefab instances
    [[nodiscard]] bool everything() const noexcept { return locked && helpers && prefabs; }
};

struct MarqueeState {
    bool active{};
    std::optional<EditorObjectId> clickObject;
    std::optional<EditorPickResult> clickVoxel;  // Voxel domain: the voxel under the press
    SelectionOperation operation{SelectionOperation::Replace};
    int startX{};
    int startY{};
    int currentX{};
    int currentY{};
};

struct HierarchyDragState {
    bool active{};
    EditorObjectId sourceId{};
    std::optional<EditorObjectId> hoverTarget;
};

struct EditorCameraBookmark {
    bool occupied{};
    std::string name;
    EditorCamera camera{};
};


struct EditorShortcutPanelState {
    bool open{};
    std::string searchQuery;
    std::optional<ShortcutContext> contextFilter;
    std::size_t selectedRow{};
    bool capturing{};
    ShortcutSlot captureSlot{ShortcutSlot::Primary};
    std::string captureActionId;
    ShortcutContext captureContext{ShortcutContext::Global};
    std::string status;
};

struct NativeShortcutModalLayout {
    UiRect panel{};
    UiRect searchBox{};
    UiRect profilePrevious{};
    UiRect profileNext{};
    UiRect contextPrevious{};
    UiRect contextNext{};
    std::vector<UiRect> rows;
    std::size_t firstVisibleRow{};
    UiRect resetButton{};
    UiRect closeButton{};
};

// Shared geometry for the native top-level menu popup. Keeping this in the controller avoids
// the input and renderer paths independently recreating popup placement, clipping, and scrolling
// rules. `rows` contains only the currently visible actions and is parallel to the action range
// beginning at `firstVisibleAction`.


enum class CommandPaletteResultKind : std::uint8_t {
    Command, Setting, Panel, Asset, SceneObject, Documentation
};

struct CommandPaletteResult {
    CommandPaletteResultKind kind{CommandPaletteResultKind::Command};
    std::string id;
    std::string label;
    std::string breadcrumb;
    std::string description;
    std::string shortcut;
    std::string disabledReason;
    bool enabled{true};
    bool favorite{};
};

// File > Open Scene / Open Project / Save As: an in-editor picker (the native editor has no OS
// file dialog). Type to filter (or type a name for Save As), Up/Down, Enter, Esc.
enum class FilePickerKind : std::uint8_t { OpenScene, OpenProject, SaveSceneAs };
struct FilePickerEntry {
    std::string label;   // what the row shows (relative path or project name)
    std::string detail;  // "recent", "current", the folder, ...
    std::filesystem::path path;
};
struct FilePickerState {
    bool open{};
    FilePickerKind kind{FilePickerKind::OpenScene};
    std::string query;
    std::vector<FilePickerEntry> entries;
    std::size_t selected{};       // index into the filtered rows
    std::string message;          // validation or confirmation text under the search box
    bool messageIsError{};
    std::optional<std::filesystem::path> confirmReplace;  // Save As: Enter again replaces this file
};
struct NativeFilePickerLayout {
    UiRect panel{};
    UiRect searchBox{};
    std::vector<UiRect> rows;
    std::size_t firstVisibleRow{};
    UiRect hintBar{};
};

// Long jobs (Import Model, Reimport, large Flood Fills, Validate Project, Rebuild Collision):
// the work runs on a worker thread on a snapshot of its inputs while the editor shows progress;
// Esc (or the Cancel button) cancels. While one runs, edits are refused, so the scene cannot
// change under it; the result is applied on the UI thread as one undo step (or not at all when
// cancelled, or when the scene changed anyway).
enum class LongJobKind : std::uint8_t { Idle, ReadModel, ImportModel, ReimportModel, FloodFill, ValidateProject, RebuildCollision };
struct LongJobView {
    bool active{};
    LongJobKind kind{LongJobKind::Idle};
    std::string label;      // "Import crate.obj"
    std::string phase;      // "Voxelizing 2 of 5: Wheel"
    float progress{};       // 0..1
    bool cancelling{};
};
struct NativeLongJobLayout {
    UiRect panel{};
    UiRect bar{};
    UiRect cancelButton{};
};

// File > Import Model / Reimport Model options panel (in-editor, like the file pickers).
enum class ImportOptionsRow : std::uint8_t { VoxelSize, Scale, Mode, Node };
struct ImportOptionsState {
    bool open{};
    bool reimport{};
    EditorObjectId reimportTarget{};
    std::filesystem::path relativeSource;      // as stored on the group (project-relative)
    std::shared_ptr<const ImportedModelResult> model;
    std::vector<EditorImportNode> nodes;
    EditorImportSettings settings;
    EditorImportEstimate estimate;
    std::size_t selected{};                     // 0 voxel size, 1 scale, 2 mode, 3.. nodes
    std::string typed;                          // a number being typed into the selected row
    std::string message;
    bool messageIsError{};
    std::optional<Float3> dropPoint;            // dragged from Assets: land the model here
};
struct EditorPrefabAsset;
// Dragging an asset from the Assets panel into the viewport (ART-076).
struct AssetDragState {
    bool armed{};                               // pressed on an asset row
    bool active{};                              // moved far enough to be a drag
    std::string assetId;
    std::string assetName;
    std::filesystem::path relativePath;
    EditorAssetKind kind{EditorAssetKind::Other};
    int startX{}, startY{};
    bool overViewport{};
    std::optional<Float3> point;                // where it would land (surface hit or ground)
    std::optional<EditorObjectId> target;       // the object under the cursor (material drops)
    EditorObjectBounds ghost;                   // world box of what would be placed
    std::string message;                        // what a release here would do, or why not
    bool refused{};                             // a release here does nothing
    std::shared_ptr<EditorPrefabAsset> prefab;  // loaded once when the drag starts
    std::shared_ptr<EditorObject> voxelObject;  // a .dvox asset, built at the origin
    EditorObjectBounds localBounds;             // bounds when placed with no offset
    std::optional<MaterialId> material;
};
struct NativeImportOptionsLayout {
    UiRect panel{};
    std::vector<UiRect> rows;                   // one per visible row (settings, then nodes)
    std::vector<std::size_t> rowIndex;          // the option index each visible row shows
    UiRect estimateBox{};
    UiRect importButton{};
    UiRect cancelButton{};
    UiRect hintBar{};
};
// What a click of the Add, Remove or Paint brush would change under the cursor (ART-044).
struct VoxelBrushPreview {
    EditorObjectId objectId{};
    Int3 center{};
    std::int32_t radius{};
    BrushShape shape{BrushShape::Cube};
    VoxelToolOperation operation{VoxelToolOperation::Add};
    std::vector<Int3> cells;                    // cells that would change (at most kMaxBrushPreviewCells)
    std::size_t count{};                        // every cell that would change
    bool limitedToSelection{};                  // Remove/Paint inside a voxel selection
    bool locked{};                              // the object is locked: nothing would change
};
inline constexpr std::size_t kMaxBrushPreviewCells = 20000;

struct NativeCommandPaletteLayout {
    UiRect panel{};
    UiRect searchBox{};
    std::vector<UiRect> rows;
    std::size_t firstVisibleRow{};
    UiRect hintBar{};
};

struct NativeMenuPopupLayout {
    UiRect popup{};
    std::vector<UiRect> rows;
    std::size_t firstVisibleAction{};
    bool canScrollUp{};
    bool canScrollDown{};
};

struct NativeSettingsModalLayout {
    UiRect panel{};
    UiRect title{};  // "Settings and Preferences", left of the scope tabs
    UiRect searchBox{};
    std::array<UiRect, 3> scopeTabs{};
    UiRect advancedToggle{};
    UiRect changedToggle{};
    std::vector<UiRect> categoryRows;
    std::vector<UiRect> settingRows;
    std::size_t firstVisibleSetting{};
    UiRect detailPanel{};
    UiRect resetSettingButton{};
    UiRect resetCategoryButton{};
    UiRect discardButton{};
    UiRect applyButton{};
};

struct NativeEditorLayout {
    UiRect menuBar{};
    UiRect toolbar{};
    UiRect hierarchy{};
    UiRect viewport{};
    UiRect inspector{};
    UiRect bottomPanel{};
    UiRect statusBar{};
    std::vector<UiRect> toolbarButtons;
    // "? Shortcuts" control in the viewport's bottom-right corner; opens the shortcut guide.
    UiRect viewportHelpButton{};
    std::vector<UiRect> hierarchyRows;  // one per hierarchy_order() entry; empty when scrolled out
    std::vector<UiRect> hierarchyDisclosureButtons;  // same indices; empty for leaves
    int hierarchyScroll{};               // first visible row
    int hierarchyScrollMax{};
    int hierarchyVisibleRows{};
    UiRect hierarchyFilterBox{};
    std::vector<UiRect> inspectorToggles;
    std::vector<UiRect> inspectorFields; // [0]=Position line, [1]=Rotation line
    // Scatter settings rows (one per NativeEditorController::ScatterSettingField) while the
    // scatter settings panel replaces the object inspector; empty otherwise.
    std::vector<UiRect> scatterSettingRows;
    // Inspector detail text at or below this y is not drawn (the flag toggles sit
    // there when the inspector is too short for both); always <= inspector bottom.
    int inspectorContentClipY{};
    // Inspector detail scroll (px). Details that do not fit above the flag toggles scroll with
    // the mouse wheel instead of being cut off; 0 when everything fits.
    int inspectorScroll{};
    int inspectorScrollMax{};
    bool inspectorStackedFields{};
    int inspectorFieldCount{};
    std::vector<UiRect> bottomTabs;      // parallel to BottomPanelTab enumerators, in order
    UiRect assetSearchBox{};
    UiRect assetRefreshButton{};
    UiRect assetFilterButton{};
    std::vector<UiRect> assetRows;
    UiRect aiPromptBox{};
    UiRect aiSendButton{};
    UiRect aiApproveButton{};
    UiRect aiDenyButton{};
};

// One highlighted primary-grid cell of an open voxel Boolean preview, projected to the screen.
struct VoxelBooleanPreviewMarker {
    enum class Kind : std::uint8_t { Added, Removed, Repainted, Overlap };
    Kind kind{Kind::Added};
    float screenX{};
    float screenY{};
    float depth{};
    float pixelRadius{2.0F};
};

struct EditorStatusMessage {
    std::string text;
    bool error{};
    float secondsRemaining{};
};

struct GizmoScreenAxis {
    int axis{}; // 0 none, 1 x, 2 y, 3 z
    ScreenPoint start{};
    ScreenPoint end{};
};

// One rotate ring: the projected circle around an axis, as screen points in
// order (the renderer closes the loop back to the first point).
struct GizmoScreenRing {
    int axis{}; // 1 x, 2 y, 3 z
    std::vector<ScreenPoint> points{};
};

struct NativeSlicePanelLayout {
    UiRect panel{}, point{}, normal{}, snap{}, apply{}, cancel{};
    std::array<UiRect, 3> axes{}, outputs{};
};

// Background recovery saves driven by the editor.autosave_minutes setting.
struct EditorAutosaveStatus {
    bool inFlight{};
    std::uint64_t completed{};
    std::uint64_t failed{};
    std::filesystem::path lastManifest;
    std::string lastError;
};

struct NativeMeshAuthoringState;
enum class MeshBoolean : std::uint8_t;
class IEditorCanvas;
class NativeEditorController {
public:
    explicit NativeEditorController(EditorWorkspace workspace = EditorWorkspace{},
                                    std::filesystem::path userSettingsPath = {},
                                    std::filesystem::path projectSettingsPath = {});

    [[nodiscard]] EditorWorkspace& workspace() noexcept { return workspace_; }
    [[nodiscard]] const EditorWorkspace& workspace() const noexcept { return workspace_; }
    [[nodiscard]] EditorMaterialLibrary& materials() noexcept { return materials_; }
    [[nodiscard]] const EditorMaterialLibrary& materials() const noexcept { return materials_; }
    [[nodiscard]] EditorPlaySession& play_session() noexcept { return playSession_; }
    [[nodiscard]] const EditorPlaySession& play_session() const noexcept { return playSession_; }
    void set_gamepad_axes(std::array<float, 2> move, std::array<float, 2> look);
    void clear_navigation_input();
    [[nodiscard]] bool play_camera_possessed() const noexcept { return playCameraPossessed_; }
    [[nodiscard]] GaborVolumeRenderSettings gabor_render_settings() const noexcept;
    [[nodiscard]] VoxelMaterialPolicyConfig voxel_material_policy_config() const noexcept;
    [[nodiscard]] std::vector<GaborVolumeFramePlan> gabor_frame_plans(float projectedDiameterPixels = 256.0F) const;
    [[nodiscard]] EditorText3DAuthoringSession& text3d_authoring() noexcept { return text3dAuthoring_; }
    [[nodiscard]] const EditorText3DAuthoringSession& text3d_authoring() const noexcept { return text3dAuthoring_; }
    [[nodiscard]] const std::filesystem::path& project_root() const noexcept { return projectRoot_; }
    [[nodiscard]] EditorCamera& camera() noexcept { return camera_; }
    [[nodiscard]] const EditorCamera& camera() const noexcept { return camera_; }
    [[nodiscard]] EditorViewportSettings& viewport_settings() noexcept { return viewportSettings_; }
    [[nodiscard]] const EditorViewportSettings& viewport_settings() const noexcept { return viewportSettings_; }
    [[nodiscard]] camera::CameraRigMode camera_mode() const noexcept { return cameraMode_; }
    [[nodiscard]] const std::array<EditorCameraBookmark, 10>& camera_bookmarks() const noexcept { return cameraBookmarks_; }
    [[nodiscard]] const camera::CameraDirector& camera_director() const noexcept { return cameraDirector_; }
    void set_system_theme_light(bool light) noexcept { systemThemeLight_ = light; }
    [[nodiscard]] bool system_theme_light() const noexcept { return systemThemeLight_; }
    [[nodiscard]] camera::CameraDirector& camera_director() noexcept { return cameraDirector_; }
    [[nodiscard]] std::optional<camera::CameraRigId> selected_camera_rig() const noexcept { return selectedCameraRig_; }
    [[nodiscard]] const EditorSettingsPanelState& settings_panel() const noexcept { return settingsPanel_; }
    [[nodiscard]] const EditorShortcutPanelState& shortcut_panel() const noexcept { return shortcutPanel_; }
    [[nodiscard]] EditorShortcutPanelState& shortcut_panel() noexcept { return shortcutPanel_; }
    [[nodiscard]] EditorSettingsPanelState& settings_panel() noexcept { return settingsPanel_; }
    [[nodiscard]] EditorCinematicCameraPanel& cinematic_camera_panel() noexcept { return cinematicCameraPanel_; }
    [[nodiscard]] const EditorCinematicCameraPanel& cinematic_camera_panel() const noexcept { return cinematicCameraPanel_; }
    [[nodiscard]] std::uint32_t settings_capabilities() const noexcept;
    [[nodiscard]] std::vector<std::string> settings_categories() const;
    [[nodiscard]] std::vector<const SettingDefinition*> settings_rows() const;
    [[nodiscard]] NativeSettingsModalLayout settings_modal_layout() const;
    [[nodiscard]] NativeShortcutModalLayout shortcut_modal_layout() const;
    [[nodiscard]] std::vector<ShortcutSearchResult> shortcut_rows() const;
    [[nodiscard]] std::vector<ShortcutContext> active_shortcut_contexts() const;
    [[nodiscard]] EditorToolId active_tool() const noexcept { return activeTool_; }
    // Current binding that selects `tool` (for example "Q"), or empty when it has none.
    [[nodiscard]] std::string tool_shortcut_text(EditorToolId tool) const;
    // The two or three gestures that matter for the active tool, with their current bindings,
    // most important first. The viewport shows as many as fit; the full list is behind the
    // "? Shortcuts" control.
    [[nodiscard]] std::vector<std::string> viewport_tool_gestures() const;
    [[nodiscard]] MaterialId active_material() const noexcept { return activeMaterial_; }
    [[nodiscard]] std::optional<VoxelBrushPreview> voxel_brush_preview() const;
    [[nodiscard]] BrushShape voxel_brush_shape() const noexcept { return voxelBrushShape_; }
    [[nodiscard]] std::int32_t voxel_brush_radius() const noexcept { return voxelBrushRadius_; }
    [[nodiscard]] EditorTransformSpace transform_space() const noexcept { return transformSpace_; }
    [[nodiscard]] EditorFocusRegion focus_region() const noexcept { return focusRegion_; }
    [[nodiscard]] const NativeEditorLayout& layout() const noexcept { return layout_; }
    [[nodiscard]] int window_width() const noexcept { return width_; }
    [[nodiscard]] int window_height() const noexcept { return height_; }
    [[nodiscard]] const std::optional<EditorPickResult>& hover_pick() const noexcept { return hoverPick_; }
    [[nodiscard]] const EditorStatusMessage& status() const noexcept { return status_; }
    // Recovery copy of the current scene: <project>/.dve/recovery/<scene>/. Written in
    // the background after editor.autosave_minutes of unsaved changes; removed by a
    // successful File > Save.
    [[nodiscard]] std::filesystem::path autosave_directory() const;
    [[nodiscard]] std::filesystem::path autosave_manifest_path() const;
    [[nodiscard]] const EditorAutosaveStatus& autosave_status() const noexcept { return autosaveStatus_; }
    [[nodiscard]] bool command_palette_open() const noexcept { return commandPaletteOpen_; }
    [[nodiscard]] std::string_view command_palette_query() const noexcept { return commandPaletteQuery_; }
    [[nodiscard]] std::vector<MenuAction> menu_actions(std::string_view menuName) const;
    [[nodiscard]] bool show_advanced_menus() const noexcept { return showAdvancedMenus_; }
    [[nodiscard]] bool command_is_favorite(std::string_view actionId) const noexcept {
        return favoriteCommandIds_.contains(actionId);
    }
    [[nodiscard]] std::vector<CommandPaletteResult> command_palette_results(std::size_t limit = 10) const;
    [[nodiscard]] std::uint64_t command_palette_rebuild_count() const noexcept { return paletteCacheRebuilds_; }
    [[nodiscard]] NativeCommandPaletteLayout command_palette_layout() const;
    bool open_file_picker(FilePickerKind kind);
    void close_file_picker();
    [[nodiscard]] const FilePickerState& file_picker() const noexcept { return filePicker_; }
    [[nodiscard]] std::vector<std::size_t> file_picker_matches() const;  // entry indexes, filtered by the query
    [[nodiscard]] NativeFilePickerLayout file_picker_layout() const;
    bool accept_file_picker();
    // Open a scene file (asks first when there are unsaved changes, like New Scene).
    bool open_scene_file(const std::filesystem::path& path);
    // Save the scene under the project's scenes/ folder as <name>.dvescene (or a given path).
    bool save_scene_as(std::string_view nameOrPath, bool replaceExisting = false);
    bool open_project_folder(const std::filesystem::path& root);
    // File > Import Model: opens the import options panel for the model selected in the Assets
    // panel (OBJ/glTF/GLB): voxel size, scale, fill mode, which meshes to include, and an
    // estimate checked against the import cap. Enter imports (a long job); the parts go under
    // one group as one undo step.
    bool import_selected_model();
    // Same, for a project file (tools and tests).
    bool open_import_options(const std::filesystem::path& relativeSource);
    // File > Reimport Model: rebuilds the selected import group from its source file with its
    // stored options (shown first); keeps the group's id, name, transform and parent. One undo.
    bool reimport_selected_model();
    [[nodiscard]] std::string reimport_disabled_reason() const;
    [[nodiscard]] const ImportOptionsState& import_options() const noexcept { return importOptions_; }
    [[nodiscard]] NativeImportOptionsLayout import_options_layout() const;
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> import_option_rows() const;  // label, value
    bool set_import_option(ImportOptionsRow row, std::string_view text, std::size_t node = 0);
    void nudge_import_option(std::size_t row, int steps);
    bool accept_import_options();
    void close_import_options(std::string reason = {});
    // Long jobs: see LongJobKind.
    [[nodiscard]] bool long_job_active() const noexcept { return longJob_ != nullptr; }
    [[nodiscard]] LongJobView long_job_view() const;
    [[nodiscard]] NativeLongJobLayout long_job_layout() const;
    bool cancel_long_job();
    // Blocks until the running job's worker is done, then applies (or discards) it as update()
    // would. For tests and headless tools; returns false on timeout or when nothing ran.
    bool wait_for_long_job(double timeoutSeconds = 120.0);
    // Work bigger than this (voxels, or objects for Validate) runs as a long job; smaller work
    // finishes at once. 0 sends everything through the long-job path.
    void set_long_job_threshold(std::uint64_t workUnits) noexcept { longJobThreshold_ = workUnits; }
    [[nodiscard]] std::uint64_t long_job_threshold() const noexcept { return longJobThreshold_; }
    [[nodiscard]] std::string long_job_busy_reason() const;
    [[nodiscard]] std::size_t command_palette_selection() const noexcept { return commandPaletteSelection_; }
    [[nodiscard]] std::optional<std::string_view> open_menu() const noexcept;
    [[nodiscard]] NativeMenuPopupLayout menu_popup_layout() const;
    [[nodiscard]] std::optional<std::size_t> menu_hovered_action() const noexcept {
        return menuHoveredAction_;
    }
    // Set by dispatch_action("file.exit") or the host's window-close/Ctrl+Q handling (both of
    // which should call request_quit(), not set this directly); the platform host is
    // responsible for actually tearing down the window and event loop once this is true.
    [[nodiscard]] bool quit_requested() const noexcept { return quitRequested_; }
    // True while a "you have unsaved changes" confirmation is pending; the host should render
    // a modal and route Enter/Escape (already handled by key_down) instead of normal input.
    [[nodiscard]] bool pending_quit_confirmation() const noexcept {
        return pendingDestructiveAction_ == PendingDestructiveAction::Quit;
    }
    [[nodiscard]] bool pending_destructive_confirmation() const noexcept {
        return pendingDestructiveAction_ != PendingDestructiveAction::Inactive;
    }
    [[nodiscard]] PendingDestructiveAction pending_destructive_action() const noexcept {
        return pendingDestructiveAction_;
    }
    [[nodiscard]] std::string_view pending_confirmation_message() const noexcept;
    void request_quit();
    void confirm_pending_destructive_action();
    void cancel_pending_destructive_action();
    // Compatibility wrappers retained for tests and platform hosts that only initiate quit.
    void confirm_quit() {
        if (pendingDestructiveAction_ == PendingDestructiveAction::Quit)
            confirm_pending_destructive_action();
    }
    void cancel_quit() {
        if (pendingDestructiveAction_ == PendingDestructiveAction::Quit)
            cancel_pending_destructive_action();
    }

    [[nodiscard]] const TextEditState& text_edit() const noexcept { return textEdit_; }
    [[nodiscard]] const ContextMenuState& context_menu() const noexcept { return contextMenu_; }
    [[nodiscard]] const SelectionMask& selection_mask() const noexcept { return selectionMask_; }
    // Whether the selection mask lets viewport selection take this object (or group).
    [[nodiscard]] bool object_selectable(EditorObjectId id) const;
    // Short text drawn under a selection box: window/crossing, visible-only/X-ray, active masks.
    [[nodiscard]] std::string marquee_label() const;
    [[nodiscard]] const MarqueeState& marquee() const noexcept { return marquee_; }
    [[nodiscard]] std::optional<Float3> placement_target() const noexcept { return placementPoint_; }
    [[nodiscard]] std::optional<EditorObjectBounds> scale_preview_bounds() const noexcept { return scalePreviewBounds_; }
    [[nodiscard]] Float3 current_pivot() const noexcept { return selection_pivot(); }
    [[nodiscard]] const HierarchyDragState& hierarchy_drag() const noexcept { return hierarchyDrag_; }
    [[nodiscard]] BottomPanelTab bottom_tab() const noexcept { return bottomTab_; }
    [[nodiscard]] std::string_view hierarchy_filter() const noexcept { return hierarchyFilter_; }
    // Mutable access first finishes a background asset scan (see start_asset_scan), so callers
    // that edit or inspect the database after a refresh see its result. Const access returns
    // the current records without waiting; they are replaced when a scan completes.
    [[nodiscard]] EditorAssetDatabase& asset_database();
    [[nodiscard]] const EditorAssetDatabase& asset_database() const noexcept { return assetDatabase_; }
    // Re-indexes the project's assets on a worker thread; update() applies the result. Opening
    // the Assets panel, its Refresh button, Ctrl+R and asset.refresh use this. A scan asked
    // for while one runs is queued and starts once it finishes.
    [[nodiscard]] bool start_asset_scan(bool announce = true);
    [[nodiscard]] bool asset_scan_in_flight() const noexcept { return static_cast<bool>(assetScanInFlight_); }
    // Waits for a running scan and applies it. Returns false if none was running.
    bool finish_asset_scan();
    [[nodiscard]] EditorAssetBrowserState& asset_browser_state() noexcept { return assetBrowserState_; }
    [[nodiscard]] const EditorAssetBrowserState& asset_browser_state() const noexcept { return assetBrowserState_; }
    [[nodiscard]] std::vector<const EditorAssetRecord*> asset_browser_rows() const;
    [[nodiscard]] audio::AudioMixer& audio_mixer() noexcept { return audioMixer_; }
    [[nodiscard]] const audio::AudioMixer& audio_mixer() const noexcept { return audioMixer_; }
    [[nodiscard]] bool audio_settings_pending() const noexcept { return audioSettingsPending_; }
    [[nodiscard]] audio::Synthesizer& synthesizer() noexcept { return audioMixer_.synthesizer(); }
    [[nodiscard]] const audio::Synthesizer& synthesizer() const noexcept { return audioMixer_.synthesizer(); }
    [[nodiscard]] EditorSynthPanel& synth_panel() noexcept { return synthPanel_; }
    [[nodiscard]] const EditorSynthPanel& synth_panel() const noexcept { return synthPanel_; }
    [[nodiscard]] EditorChiptunePanel& chiptune_panel() noexcept { return chiptunePanel_; }
    [[nodiscard]] const EditorChiptunePanel& chiptune_panel() const noexcept { return chiptunePanel_; }
    [[nodiscard]] EditorAudioPanel& audio_panel() noexcept { return audioPanel_; }
    [[nodiscard]] const EditorAudioPanel& audio_panel() const noexcept { return audioPanel_; }
    [[nodiscard]] EditorAudioEventPanel& audio_event_panel() noexcept { return audioEventPanel_; }
    [[nodiscard]] const EditorAudioEventPanel& audio_event_panel() const noexcept { return audioEventPanel_; }
    [[nodiscard]] EditorControlRigPanel& control_rig_panel() noexcept { return controlRigPanel_; }
    [[nodiscard]] const EditorControlRigPanel& control_rig_panel() const noexcept { return controlRigPanel_; }
    [[nodiscard]] EditorSpriteAuthoringPanel& sprite_authoring_panel() noexcept { return spriteAuthoringPanel_; }
    [[nodiscard]] const EditorSpriteAuthoringPanel& sprite_authoring_panel() const noexcept { return spriteAuthoringPanel_; }
    [[nodiscard]] bool sprite_animation_graph_open() const noexcept { return spriteAnimationGraphOpen_; }
    [[nodiscard]] SpriteAnimationMachineGraphWorkspace& sprite_animation_graph() noexcept {
        return spriteAnimationGraph_;
    }
    [[nodiscard]] const SpriteAnimationMachineGraphWorkspace& sprite_animation_graph() const noexcept {
        return spriteAnimationGraph_;
    }
    [[nodiscard]] SpriteAnimationGraphRect sprite_animation_graph_viewport() const noexcept {
        return {60.0F, 76.0F, static_cast<float>(std::max(320, width_ - 120)),
                static_cast<float>(std::max(240, height_ - 152))};
    }
    [[nodiscard]] bool sprite_level_playing() const noexcept { return spriteLevelPlaying_; }
    [[nodiscard]] const gameplay::ChiptuneVerticalSlice* sprite_level() const noexcept {
        return spriteLevel_.get();
    }
    [[nodiscard]] const gameplay::SpriteVerticalSlicePresentation* sprite_level_presentation() const noexcept {
        return spriteLevelPresentation_.get();
    }
    [[nodiscard]] bool sprite_diagnostics_open() const noexcept { return spriteDiagnosticsOpen_; }
    [[nodiscard]] bool render3d_diagnostics_open() const noexcept { return render3DDiagnosticsOpen_; }
    [[nodiscard]] const Render3DDiagnosticsReport& render3d_diagnostics_report() const noexcept { return render3DDiagnosticsReport_; }
    void set_render3d_diagnostics_report(Render3DDiagnosticsReport report) { render3DDiagnosticsReport_ = std::move(report); }
    [[nodiscard]] bool sprite_pixel_art_open() const noexcept { return spritePixelArtOpen_; }
    [[nodiscard]] SpritePixelArtSession& sprite_pixel_art() noexcept { return spritePixelArt_; }
    [[nodiscard]] const SpritePixelArtSession& sprite_pixel_art() const noexcept { return spritePixelArt_; }
    [[nodiscard]] UiRect sprite_pixel_art_canvas() const noexcept {
        return {72, 112, std::max(256, width_ - 360), std::max(256, height_ - 200)};
    }
    [[nodiscard]] bool sprite_rig2d_open() const noexcept { return spriteRig2DOpen_; }
    [[nodiscard]] SpriteRig2DAuthoringSession& sprite_rig2d() noexcept { return spriteRig2D_; }
    [[nodiscard]] const SpriteRig2DAuthoringSession& sprite_rig2d() const noexcept { return spriteRig2D_; }
    [[nodiscard]] float sprite_rig2d_preview_time() const noexcept { return spriteRig2DPreviewTime_; }
    [[nodiscard]] bool sprite_rig2d_preview_playing() const noexcept { return spriteRig2DPreviewPlaying_; }
    [[nodiscard]] UiRect sprite_rig2d_canvas() const noexcept {
        return {72, 112, std::max(320, width_ - 360), std::max(256, height_ - 200)};
    }
    [[nodiscard]] bool tile_world_editor_open() const noexcept { return tileWorldEditorOpen_; }
    [[nodiscard]] TileWorldDesktopWorkspace& tile_world_editor() noexcept { return tileWorldEditor_; }
    [[nodiscard]] const TileWorldDesktopWorkspace& tile_world_editor() const noexcept { return tileWorldEditor_; }
    [[nodiscard]] TileCanvasRect tile_world_palette_viewport() const noexcept {
        return {24.0F, 92.0F, 176.0F, static_cast<float>(std::max(240, height_ - 160))};
    }
    [[nodiscard]] TileCanvasRect tile_world_canvas_viewport() const noexcept {
        return {216.0F, 92.0F, static_cast<float>(std::max(320, width_ - 592)),
                static_cast<float>(std::max(240, height_ - 160))};
    }

    void resize(int width, int height);
    void update(float elapsedSeconds);
    // True while the editor changes on screen without input: a Play/Simulate session, sprite
    // level or 2D rig playback, the sprite authoring panel (timeline and palette previews),
    // camera blends, held fly-navigation keys, sounding synth voices and the thumbnail
    // backlog. Hosts redraw at full rate while it is true or input is arriving, and may
    // lower the redraw rate otherwise (anything not listed still updates, just less often).
    [[nodiscard]] bool animating() const noexcept;
    void set_wake_callback(std::function<void()> callback) { wakeCallback_=std::move(callback); }
    [[nodiscard]] double idle_update_seconds() const noexcept;
    void pointer_move(int x, int y, std::uint32_t modifiers = 0);
    // Raw deltas keep fractional precision and do not change UI hover/picking coordinates.
    void pointer_relative(float deltaX, float deltaY);
    [[nodiscard]] bool navigation_pointer_active() const noexcept;
    [[nodiscard]] bool raw_mouse_requested() const noexcept { return rawMouseRequested_; }
    // Pick walking (arrow keys, ART-014): objects go to parent / first child / previous / next
    // sibling; mesh components step to the neighbour in that screen direction (Left/Right) or
    // grow / shrink by one ring (Up/Down); voxel selections grow / shrink. The camera never moves.
    enum class WalkDirection : std::uint8_t { Up, Down, Left, Right };
    bool pick_walk(WalkDirection direction);
    void pointer_down(PointerButton button, int x, int y, std::uint32_t modifiers = 0);
    void pointer_up(PointerButton button, int x, int y, std::uint32_t modifiers = 0);
    void pointer_wheel(float steps, int x, int y, std::uint32_t modifiers = 0);
    void key_down(std::string_view key, bool control, bool shift, bool alt);
    void key_up(std::string_view key, bool control, bool shift, bool alt);
    void text_input(std::string_view text);
    void text_input(char character) { text_input(std::string_view(&character, 1U)); }

    [[nodiscard]] bool dispatch_action(std::string_view actionId);
    void set_active_tool(EditorToolId tool);
    void set_active_material(MaterialId material) noexcept;
    void set_transform_space(EditorTransformSpace space) noexcept { transformSpace_ = space; }
    void focus_next(bool reverse = false) noexcept;
    void frame_selection() noexcept;
    void set_camera_mode(camera::CameraRigMode mode) noexcept;
    [[nodiscard]] camera::CameraRigId create_camera_rig_from_view(std::string name = {});
    void save_camera_bookmark(std::size_t index);
    [[nodiscard]] bool load_camera_bookmark(std::size_t index) noexcept;
    void open_settings(SettingScope scope, std::string category = {});
    void open_shortcut_editor();
    void configure_ai_assistant(std::filesystem::path projectRoot = {});
    // Explicit project roots activate per-project UI persistence. Accepted quit
    // saves once; hosts may also checkpoint explicitly outside their frame loop.
    [[nodiscard]] bool save_workspace_state(std::string* error = nullptr);
    [[nodiscard]] std::filesystem::path workspace_state_path() const;
    void configure_menu_state(std::filesystem::path path);
    // User-scope settings persistence (e.g. <project>/.dve/user/editor_settings.txt). Loads the
    // file if present; afterwards Settings > Apply and UI-zoom hotkeys save the User layer.
    void configure_user_settings(std::filesystem::path path);
    [[nodiscard]] const std::filesystem::path& user_settings_path() const noexcept { return userSettingsPath_; }
    bool save_user_settings(std::string* error = nullptr) const;
    // UI zoom (see dve/editor_ui_zoom.hpp). ui_zoom() is the requested, step-snapped value from
    // the `editor.ui_scale` setting; effective_ui_zoom() additionally honours the window cap the
    // host reported via set_ui_zoom_window_limit(). Hosts render with effective_ui_zoom().
    [[nodiscard]] float ui_zoom() const noexcept;
    [[nodiscard]] float effective_ui_zoom() const noexcept;
    void set_ui_zoom_window_limit(float maximumZoom) noexcept;
    [[nodiscard]] float ui_zoom_window_limit() const noexcept { return uiZoomWindowLimit_; }
    bool set_ui_zoom(float requested);
    bool step_ui_zoom(int direction);
    // On-screen piano size for the synth and chiptune editors (User-scope setting
    // `editor.keyboard_keys`: 25, 37, 49, 61, 76 or 88 keys). Saved like UI zoom.
    [[nodiscard]] int keyboard_key_count() const noexcept;
    bool set_keyboard_key_count(int keys);
    // MIDI input (see dve/editor_midi.hpp; hosts call start_editor_midi()). The session polls
    // ports on its own thread; update() picks up status changes without blocking.
    void attach_midi_input(std::unique_ptr<audio::IMidiBackend> backend,
                           audio::MidiInputSession::Options options = {});
    // Synth MIDI out (poll_midi_output) is queued on this output session from update(); the
    // session picks the port from `midi.output_port` and follows hotplug on its own thread.
    void attach_midi_output(std::unique_ptr<audio::IMidiBackend> backend,
                            audio::MidiOutputSession::Options options = {});
    [[nodiscard]] audio::MidiInputSession* midi_input_session() noexcept { return midiInput_.get(); }
    [[nodiscard]] const audio::MidiInputStatus& midi_input_status() const noexcept { return midiStatus_; }
    [[nodiscard]] std::string midi_input_summary() const { return midiStatus_.summary(); }
    // Requested port ("" = Auto, "none" = off, otherwise a port name) saved in User settings.
    [[nodiscard]] std::string midi_input_port() const;
    bool set_midi_input_port(std::string name);
    // Steps through Auto, None and the present ports (synth header button).
    bool cycle_midi_input_port(int direction);
    // MIDI output, mirroring the input: "" = Auto (first port), "none" = off, or a port name.
    [[nodiscard]] audio::MidiOutputSession* midi_output_session() noexcept { return midiOutput_.get(); }
    [[nodiscard]] const audio::MidiOutputStatus& midi_output_status() const noexcept { return midiOutputStatus_; }
    [[nodiscard]] std::string midi_output_summary() const { return midiOutputStatus_.summary(); }
    [[nodiscard]] std::string midi_output_port() const;
    bool set_midi_output_port(std::string name);
    bool cycle_midi_output_port(int direction);
    // Pulls the latest session status into the settings choices / synth header (update() does it).
    void refresh_midi_status();
    // Last pointer position seen by pointer_move (logical px), for hover tooltips.
    [[nodiscard]] int hover_x() const noexcept { return hoverX_; }
    [[nodiscard]] int hover_y() const noexcept { return hoverY_; }
    [[nodiscard]] bool create_text3d(std::filesystem::path fontPath, std::string text = "3D Text",
                                     Text3DCookOptions options = {});
    [[nodiscard]] bool create_gabor_volume(std::filesystem::path sourcePath = {});
    [[nodiscard]] bool edit_selected_text3d();
    [[nodiscard]] bool commit_text3d_authoring();
    void cancel_text3d_authoring();
    [[nodiscard]] bool submit_ai_prompt(std::string prompt);
    [[nodiscard]] bool start_live_mcp_host(ai::LiveEditorMcpHostOptions options = {},
                                           std::string* error = nullptr);
    void stop_live_mcp_host() noexcept;
    [[nodiscard]] ai::LiveEditorMcpHostStatus live_mcp_status() const;
    [[nodiscard]] ai::DveAiBridge* ai_bridge() noexcept { return aiBridge_.get(); }
    [[nodiscard]] const ai::DveAiBridge* ai_bridge() const noexcept { return aiBridge_.get(); }
    void close_shortcut_editor() noexcept;
    void set_shortcut_context_override(std::optional<ShortcutContext> context) noexcept { shortcutContextOverride_ = context; }
    void close_settings(bool applyChanges);
    void refresh_menu_state() noexcept;
    void apply_settings_to_runtime() noexcept;
    void reset_camera_navigation() noexcept;
    [[nodiscard]] bool apply_audio_settings_to_runtime() noexcept;
    [[nodiscard]] CommandResult nudge_selection(Float3 worldDelta);
    // Mirror the selected objects (with their children) across the world plane through the
    // current pivot whose normal is axis 0/1/2 (X/Y/Z); as copies or in place. One undo step.
    bool mirror_selection(int axis, bool copy);
    // Half-model mirroring of the selected polygon mesh / subdivision cage across its own plane
    // (local axis through the local origin): bake the whole mesh with a welded seam, or copy just
    // the selected faces. Live Mirror shows the other half until Apply bakes it.
    bool mirror_mesh(int axis, bool selectedFaces);
    bool toggle_live_mirror(int axis);
    bool apply_live_mirror();
    [[nodiscard]] std::optional<int> live_mirror_axis(EditorObjectId id) const;
    [[nodiscard]] CommandResult set_primary_position(Float3 worldPosition);
    [[nodiscard]] InspectorSelectionSummary inspector_selection_summary() const;
    // Numeric fields: the shown value ("" when the field does not apply, "—" when the selected
    // objects differ), typing a value (units, arithmetic, += -= *= /=), reset and scrub state.
    [[nodiscard]] bool number_field_available(NumberFieldId field) const;
    [[nodiscard]] std::string number_field_text(NumberFieldId field, int axis) const;
    bool set_number_field(NumberFieldId field, int axis, std::string_view text, std::string_view verb = "Set");
    bool reset_number_field(NumberFieldId field, int axis);
    [[nodiscard]] bool number_scrub_active() const noexcept { return numberScrub_.armed && numberScrub_.moved; }
    // Where the fields are now: per-axis boxes on the inspector's Position / Rotation rows, Voxel
    // size on the next row, and the viewport tool strip (Brush radius / Surface offset).
    [[nodiscard]] std::vector<NumberFieldRect> number_field_rects() const;
    // Duplicate Special (Edit > Duplicate Special, Ctrl+Shift+D): N copies, each offset, turned and
    // scaled from the last; a ghost preview until Apply (one undo), Esc cancels.
    struct DuplicatePreviewBox { RigidTransform pose; Float3 minimum; Float3 maximum; float scale{1.0F}; };
    bool open_duplicate_special();
    bool apply_duplicate_special();
    void cancel_duplicate_special(std::string reason = {});
    [[nodiscard]] bool duplicate_special_active() const noexcept { return duplicateSpecial_.active; }
    [[nodiscard]] DuplicateSpecialSettings& duplicate_special_settings() noexcept { return duplicateSpecial_.settings; }
    [[nodiscard]] const DuplicateSpecialSettings& duplicate_special_settings() const noexcept { return duplicateSpecial_.settings; }
    [[nodiscard]] std::vector<DuplicatePreviewBox> duplicate_special_preview(std::size_t limit = 2000) const;
    [[nodiscard]] DuplicatePanelLayout duplicate_panel_layout() const;
    [[nodiscard]] std::string duplicate_special_summary() const;
    [[nodiscard]] CommandResult toggle_inspector_flag(std::size_t index);
    [[nodiscard]] CommandResult set_primary_rotation_euler_degrees(Float3 degrees);
    [[nodiscard]] CommandResult rescale_primary_voxel_object(Float3 scale);
    // Scale Voxel Size (Edit menu, command palette): multiplies the voxel size of every selected
    // voxel object by `factor` and scales their positions about current_pivot(), as one undo step.
    // The voxel count is unchanged (contrast rescale_primary_voxel_object and the Scale tool, which
    // resample). Non-voxel objects in the selection are skipped. The factor is limited so every
    // resulting voxel size stays within [kMinVoxelSizeMeters, kMaxVoxelSizeMeters].
    [[nodiscard]] CommandResult scale_selection_voxel_size(float factor);
    // Why Scale Voxel Size is unavailable for the current selection; empty when it can run.
    [[nodiscard]] std::string voxel_size_scale_disabled_reason() const;
    [[nodiscard]] std::vector<ComponentInspectorSection> primary_component_sections() const;
    [[nodiscard]] CommandResult add_component_to_primary(std::string_view type);
    [[nodiscard]] CommandResult remove_component_from_primary(ComponentId componentId);
    [[nodiscard]] CommandResult set_component_enabled_on_primary(ComponentId componentId, bool enabled);
    [[nodiscard]] CommandResult reorder_component_on_primary(ComponentId componentId, std::size_t newIndex);
    [[nodiscard]] CommandResult set_component_property_text_on_primary(
        ComponentId componentId, std::string property, std::string_view text);
    // Cached: recomputed only when the selection or the scene fingerprint changes.
    [[nodiscard]] const EditorSelectionDiagnostics& selection_diagnostics() const;

    // Cached voxel draw list (projection + depth sort); rebuilt only when the scene,
    // camera, viewport, draw cap, or selection change.
    [[nodiscard]] const std::vector<EditorVoxelDrawItem>& draw_items() const;
    [[nodiscard]] std::vector<EditorPolygonDrawTriangle> polygon_draw_items() const;
    bool import_polygon_model(const std::filesystem::path&);
    bool import_exact_cad_model(const std::filesystem::path&);
    [[nodiscard]] std::size_t draw_item_count() const { return draw_items().size(); }
    // Cached draw list for the selected camera rig's picture-in-picture preview.
    [[nodiscard]] const std::vector<EditorVoxelDrawItem>& camera_preview_draw_items(
        const EditorCamera& previewCamera, UiRect previewRect,
        const EditorViewportSettings& previewSettings) const;
    struct RenderCacheStats {
        std::uint64_t drawListRebuilds{};
        std::uint64_t previewDrawListRebuilds{};
        std::uint64_t selectionDiagnosticsRebuilds{};
    };
    [[nodiscard]] RenderCacheStats render_cache_stats() const noexcept {
        return {drawListCache_.rebuild_count(), previewDrawListCache_.rebuild_count(), selectionDiagnosticsRebuilds_};
    }
    [[nodiscard]] std::vector<EditorText3DDrawItem> text3d_draw_items() const;
    [[nodiscard]] std::vector<EditorGaborVolumeDrawItem> gabor_volume_draw_items() const;
    [[nodiscard]] std::vector<EditorObjectId> hierarchy_order() const;
    [[nodiscard]] bool hierarchy_collapsed(EditorObjectId id) const noexcept { return collapsedHierarchy_.contains(id); }
    bool set_hierarchy_expanded(EditorObjectId id, bool expanded);
    bool reveal_selection_in_hierarchy();

    // Authored voxel Booleans (ART-060). begin_voxel_boolean opens a non-destructive preview
    // for the current selection (active object = target A, other selected objects = operands);
    // while it is open Enter commits, Esc cancels, 1/2/3 switch Union/Difference/Intersection,
    // S swaps the target, O cycles the operand policy and M toggles the overlap material.
    [[nodiscard]] VoxelBooleanSelection voxel_boolean_selection() const;
    // Empty when a Boolean can start now; otherwise what to select or change (ART-114).
    [[nodiscard]] std::string voxel_boolean_disabled_reason() const;
    [[nodiscard]] bool begin_voxel_boolean(VoxelBooleanOperation operation);
    [[nodiscard]] CommandResult commit_voxel_boolean();
    void cancel_voxel_boolean(std::string_view reason = {});
    // Makes the first operand the new target (A) and the old target an operand (B).
    [[nodiscard]] bool swap_voxel_boolean_target();
    [[nodiscard]] const EditorVoxelBooleanSession& voxel_boolean() const noexcept { return voxelBoolean_; }
    // Scatter Objects (Create > Scatter Objects): copies of the selected objects and/or the prefab
    // selected in the Assets panel, placed pseudo-randomly on the active (last clicked) voxel
    // object. Preview first; Enter commits one undo step, Esc cancels. See editor_scatter.hpp.
    bool begin_scatter();
    bool commit_scatter();
    void cancel_scatter(std::string reason = {});
    void set_scatter_settings(const ScatterSettings& settings);
    [[nodiscard]] bool scatter_active() const noexcept { return scatterActive_; }
    [[nodiscard]] const ScatterSettings& scatter_settings() const noexcept { return scatterSettings_; }
    [[nodiscard]] const ScatterPlan& scatter_plan() const noexcept { return scatterPlan_; }
    [[nodiscard]] EditorObjectId scatter_target() const noexcept { return scatterTargets_.empty() ? 0 : scatterTargets_.front(); }
    [[nodiscard]] const std::vector<EditorObjectId>& scatter_targets() const noexcept { return scatterTargets_; }
    // Two-step selection for several surfaces: Create > Set Scatter Sources remembers the
    // selected objects (and the Assets-panel prefab) as what to copy; afterwards every selected
    // voxel object is a surface for Scatter Objects, and the brush paints those sources. Stays
    // set until Clear Scatter Sources (or until the sources are deleted).
    bool set_scatter_sources();
    void clear_scatter_sources();
    [[nodiscard]] bool scatter_sources_pinned() const noexcept { return !pinnedScatterRoots_.empty() || pinnedScatterPrefab_ != nullptr; }
    [[nodiscard]] const std::vector<EditorObjectId>& pinned_scatter_sources() const noexcept { return pinnedScatterRoots_; }
    // Scatter settings panel: replaces the object inspector while the fill preview is open or the
    // Scatter brush is the active tool. Click a row to type a value (Align toggles); the wheel
    // over a row nudges it. Settings are stored on the scatter group they made.
    enum class ScatterSettingField : std::uint8_t {
        Count, Spacing, Seed, Align, YawJitter, MinScale, MaxScale, BrushRadius, BrushDensity
    };
    static constexpr std::size_t kScatterSettingFieldCount = 9;
    struct ScatterSettingRow {
        ScatterSettingField field{};
        std::string label;
        std::string value;
        bool used{true};  // false: shown dimmed (e.g. brush radius while filling)
    };
    [[nodiscard]] bool scatter_settings_panel_active() const noexcept;
    [[nodiscard]] std::vector<ScatterSettingRow> scatter_setting_rows() const;
    bool set_scatter_setting(ScatterSettingField field, std::string_view text, std::string* error = nullptr);
    void nudge_scatter_setting(ScatterSettingField field, int steps);
    [[nodiscard]] std::vector<std::string> scatter_preview_lines() const;
    // Why Scatter Objects cannot start with the current selection; empty when it can.
    [[nodiscard]] std::string scatter_disabled_reason() const;
    // Scatter Brush tool: drag paints copies of the objects (and/or Assets-panel prefab) that
    // were selected when the tool was chosen; Shift-drag erases scatter copies; Ctrl+wheel or
    // Ctrl-drag sets the radius. Each stroke is one undo step; strokes of one tool session share
    // one "Scatter (N)" group.
    struct ScatterBrushView {
        std::optional<Float3> cursor;          // brush centre on the ground under the pointer
        float radius{2.0F};
        bool stroking{};
        bool erasing{};
        std::vector<ScatterSample> pending;    // spots this stroke will add
        std::vector<EditorObjectId> erasing_ids;
        std::size_t sourceCount{};
    };
    [[nodiscard]] ScatterBrushView scatter_brush_view() const;
    void set_scatter_brush_radius(float meters);
    [[nodiscard]] VoxelBooleanOperandPolicy voxel_boolean_operand_policy() const noexcept { return voxelBooleanPolicy_; }
    void set_voxel_boolean_operand_policy(VoxelBooleanOperandPolicy policy);
    [[nodiscard]] std::vector<std::string> voxel_boolean_preview_lines() const;
    [[nodiscard]] std::vector<VoxelBooleanPreviewMarker> voxel_boolean_preview_markers(std::size_t limit = 20000) const;
    // Join (Tools > Voxel > Join Objects): merge the other selected voxel objects into the active
    // one, previewed; off-grid objects need R (resample) or G (group instead) every time.
    // Separate Islands: split each selected voxel object into its connected pieces; the largest
    // stays. See editor_voxel_join.hpp.
    // Groups (ART-035/036). A group is an object with children and no geometry of its own
    // (Edit > Group, Scatter and Import make them). Groups are closed by default: a viewport click
    // or box on any piece selects the outermost closed group, which moves as a unit. Open Group
    // (or double-click) lets clicks reach the pieces inside until Close Group. Open/closed is
    // editor view state, not saved. A group's pivot is its own transform: new groups put it at
    // the bottom centre; Edit Pivot moves it with the Move tool without moving the pieces.
    [[nodiscard]] bool is_group(EditorObjectId id) const;
    [[nodiscard]] bool is_group_open(EditorObjectId id) const { return openGroups_.contains(id); }
    [[nodiscard]] const std::set<EditorObjectId>& open_groups() const noexcept { return openGroups_; }
    // What a viewport click on `id` selects: the outermost closed group around it, or itself.
    [[nodiscard]] EditorObjectId viewport_selection_target(EditorObjectId id) const;
    [[nodiscard]] EditorObjectBounds group_bounds(EditorObjectId id) const;
    bool open_selected_groups();
    bool close_groups_around_selection();
    void close_all_groups();
    [[nodiscard]] bool pivot_editing() const noexcept { return pivotEditing_; }
    bool toggle_pivot_editing();
    bool set_group_pivot(bool bottomCentre);
    // Voxel selection (Edit > Selection Domain, M). See editor_voxel_selection.hpp.
    [[nodiscard]] SelectionDomain selection_domain() const noexcept { return selectionDomain_; }
    void set_selection_domain(SelectionDomain domain);
    [[nodiscard]] const PolygonSelection& polygon_selection() const noexcept { return polygonSelection_; }
    bool set_polygon_mode(PolygonSelectionMode mode);
    void select_polygon_components(const std::set<std::uint64_t>&, SelectionOperation);
    bool apply_polygon_edit(PolygonEdit, const PolygonEditSettings& = {});
    bool transform_polygon_components(Float3 translation, Quaternion rotation, Float3 scale, Float3 worldPivot);
    bool join_selected_polygons();
    bool separate_selected_polygon();
    bool extract_selected_polygon();
    [[nodiscard]] std::vector<std::string> polygon_preview_lines() const;
    void cancel_polygon_preview();
    bool dispatch_mesh_authoring_action(std::string_view);
    bool start_exact_boolean(EditorObjectId a, EditorObjectId b, MeshBoolean operation);
    bool open_mesh_boolean_preview(EditorObjectId a, EditorObjectId b, MeshBoolean operation);
    bool mesh_authoring_key(std::string_view,bool control,bool shift,bool alt);
    bool mesh_authoring_pointer_down(PointerButton,int,int,std::uint32_t);
    bool mesh_authoring_pointer_move(int,int,std::uint32_t);
    bool mesh_authoring_pointer_up(PointerButton,int,int,std::uint32_t);
    bool mesh_authoring_wheel(float,int,int);
    void cancel_mesh_authoring_drag();
    void draw_mesh_authoring(const IEditorCanvas&,UiRect) const;
    [[nodiscard]] bool mesh_authoring_dragging() const noexcept;
    [[nodiscard]] std::set<EditorObjectId> mesh_authoring_hidden_objects() const;
    void refresh_mesh_authoring_menu_state();
    bool dispatch_subdivision_action(std::string_view action);
    void refresh_subdivision_menu_state();
    void draw_subdivision_cage(const IEditorCanvas&,UiRect viewport) const;
    [[nodiscard]] EditorObjectId voxel_selection_object() const noexcept { return voxelSelectionObject_; }
    [[nodiscard]] const std::set<Int3>& voxel_selection() const noexcept { return voxelSelection_; }
    void select_voxels(EditorObjectId object, const std::set<Int3>& cells, SelectionOperation operation);
    void clear_voxel_selection();
    bool grow_voxel_selection_cmd();
    bool shrink_voxel_selection_cmd();
    bool invert_voxel_selection_cmd();
    bool select_all_voxels();
    bool delete_selected_voxels();
    bool extract_selected_voxels();
    bool copy_selected_voxels(bool cut);
    bool paste_voxels();
    bool move_selected_voxels(Int3 offset);
    bool edit_selected_voxel_region(VoxelRegionEdit edit, VoxelRegionSettings settings);
    bool begin_voxel_region_preview(VoxelRegionEdit edit, VoxelRegionSettings settings);
    bool begin_geometry_conversion(bool toVoxels, bool subdivisionOutput = false);
    bool update_voxel_authoring_preview();
    bool apply_voxel_authoring_preview();
    void cancel_voxel_authoring_preview();
    bool voxel_authoring_key(std::string_view key, bool control, bool shift, bool alt);
    bool voxel_authoring_pointer_down(PointerButton button, int x, int y, std::uint32_t modifiers);
    void draw_voxel_authoring(const IEditorCanvas&, UiRect viewport) const;
    [[nodiscard]] bool voxel_authoring_active() const noexcept;
    [[nodiscard]] bool voxel_authoring_preview_ready() const noexcept;
    [[nodiscard]] bool voxel_preview_replaces_cell(EditorObjectId object, Int3 cell) const noexcept;
    [[nodiscard]] std::string voxel_region_disabled_reason() const;
    // During a Move-gizmo drag in the Voxel domain: the whole-voxel offset the release applies.
    [[nodiscard]] std::optional<Int3> voxel_move_preview() const noexcept;
    [[nodiscard]] bool voxel_brush_selecting() const noexcept { return voxelBrushSelecting_; }
    // Flood Fill (Voxel > Flood Fill): arms a one-shot fill; the next viewport click repaints the
    // clicked voxel's face-connected same-material region with the active material (one undo).
    bool arm_flood_fill();
    [[nodiscard]] bool flood_fill_armed() const noexcept { return floodFillArmed_; }
    [[nodiscard]] const AssetDragState& asset_drag() const noexcept { return assetDrag_; }
    void cancel_asset_drag(std::string reason = {});
    [[nodiscard]] bool follow_selection() const noexcept { return followSelection_; }
    bool flood_fill_at(EditorObjectId object, Int3 voxel);
    // View > Drop Selection to Surface: drops each selected object straight down onto what is
    // below it (any visible object that is not being dropped, else the y = 0 grid); one undo.
    bool drop_selection_to_surface();
    // Voxel > Rebuild Collision: rebuilds the merged box proxies Play uses for the selected (or all)
    // voxel objects and reports the box count. View > Collision Shapes draws these boxes.
    bool rebuild_collision_proxies();
    [[nodiscard]] const std::vector<VoxelBox>& collision_proxy(EditorObjectId id) const;
    // Build > Validate Project: lists problems (missing files, broken references, empty or
    // invalid objects, stale assets) in the Problems panel; returns how many were found.
    std::size_t validate_project();
    bool begin_voxel_join();
    CommandResult commit_voxel_join();
    void cancel_voxel_join(std::string reason = {});
    [[nodiscard]] std::string voxel_join_disabled_reason() const;
    [[nodiscard]] const EditorVoxelJoinSession& voxel_join() const noexcept { return voxelJoin_; }
    bool separate_voxel_islands();
    [[nodiscard]] std::string separate_islands_disabled_reason() const;
    [[nodiscard]] bool begin_voxel_slice();
    [[nodiscard]] std::string voxel_slice_disabled_reason() const;
    [[nodiscard]] CommandResult commit_voxel_slice();
    void cancel_voxel_slice();
    [[nodiscard]] const EditorVoxelSliceSession& voxel_slice() const noexcept { return voxelSlice_; }
    [[nodiscard]] bool configure_voxel_slice(Float3 point, Float3 normal, VoxelSliceOutput output);
    [[nodiscard]] NativeSlicePanelLayout voxel_slice_panel_layout() const;
    [[nodiscard]] GizmoScreenAxis voxel_slice_move_handle() const;
    [[nodiscard]] std::vector<GizmoScreenRing> voxel_slice_rotate_handles() const;
    [[nodiscard]] bool voxel_slice_snap() const noexcept { return sliceSnap_; }
    [[nodiscard]] int voxel_slice_drag_handle() const noexcept { return sliceDrag_; }
    [[nodiscard]] std::vector<GizmoScreenAxis> gizmo_axes() const;
    // View-only isolation (View > Isolate Selection): toggles between showing only the
    // selection (with its children) and showing everything. Authored flags are untouched.
    bool toggle_isolation();
    [[nodiscard]] const std::set<EditorObjectId>& isolated_objects() const noexcept {
        return viewportSettings_.isolatedObjects;
    }
    [[nodiscard]] bool is_isolated_out(EditorObjectId id) const noexcept;
    [[nodiscard]] std::vector<GizmoScreenRing> gizmo_rings() const;
    [[nodiscard]] int hit_test_gizmo_axis(int x, int y) const;
    [[nodiscard]] int gizmo_axis() const noexcept { return gizmoAxis_; }
    [[nodiscard]] bool gizmo_dragging() const noexcept { return gizmoDragging_; }

private:
    [[nodiscard]] std::vector<CommandPaletteResult> build_command_palette_results(std::size_t limit) const;
    void recompute_layout();
    void update_hover(int x, int y);
    void begin_voxel_stroke(int x, int y);
    void continue_voxel_stroke(int x, int y);
    void finish_voxel_stroke() noexcept;
    void apply_voxel_tool(const EditorPickResult& pick);
    void begin_gizmo_drag(int x, int y, std::uint32_t modifiers = 0);
    void update_gizmo_drag(int x, int y, std::uint32_t modifiers);
    void navigate_pointer(float deltaX, float deltaY);
    void finish_gizmo_drag(bool cancel);
    void set_status(std::string text, bool error = false, float seconds = 3.0F);
    void create_new_project_now();
    void create_new_scene_now();
    [[nodiscard]] Float3 gizmo_axis_world(int axis) const noexcept;
    [[nodiscard]] Float3 selection_pivot() const noexcept;
    [[nodiscard]] EditorObjectBounds selection_bounds() const noexcept;
    // With includeGroupMembers, selected groups bring their (unattached) descendants, so a group
    // moves and turns as one unit; pivot editing and the Scale tool move the group alone.
    [[nodiscard]] std::vector<ObjectTransformChange> selection_transform_snapshot(bool includeGroupMembers = true) const;
    void apply_transform_preview(const std::vector<ObjectTransformChange>& changes, bool after);
    [[nodiscard]] std::optional<std::string> menu_name_at(int x) const;
    void open_top_level_menu(std::string menuName);
    void close_top_level_menu() noexcept;
    void move_menu_selection(int direction);
    void switch_top_level_menu(int direction);
    [[nodiscard]] bool activate_menu_selection();
    [[nodiscard]] bool activate_command_palette_selection();
    void record_command_use(std::string_view actionId);
    void persist_menu_state() noexcept;

    [[nodiscard]] CommandResult duplicate_objects(const std::vector<EditorObjectId>& ids, Float3 offset);
    void begin_text_edit(TextEditKind kind, EditorObjectId objectId, std::string initialBuffer);
    void commit_text_edit();
    void cancel_text_edit() noexcept;
    struct NumberScrubState {
        bool armed{};                 // pressed on a field label
        bool moved{};                 // dragged far enough to be a scrub (else it was a click)
        NumberFieldId field{};
        int axis{};
        int startX{};
        std::vector<ObjectTransformChange> transforms;
        std::vector<ObjectVoxelSizeChange> sizes;
        float startScalar{};          // brush radius / surface offset at the press
    };
    NumberScrubState numberScrub_;
    std::optional<std::pair<NumberFieldId, int>> contextNumberField_;
    std::string numberFieldClipboard_;
    std::optional<std::uint64_t> polygonWalkCurrent_;  // the component the last Left/Right step reached
    bool polygon_walk(WalkDirection direction);
    void begin_number_field_edit(NumberFieldId field, int axis);
    void update_number_scrub(int x, std::uint32_t modifiers);
    void finish_number_scrub(bool cancel);
    [[nodiscard]] std::vector<EditorObjectId> number_field_objects(NumberFieldId field) const;
    // Why the selected objects cannot take an edit to `field` ("" when they can).
    [[nodiscard]] std::string number_field_refusal(NumberFieldId field) const;
    void open_context_menu(int x, int y, std::optional<EditorObjectId> target, bool fromHierarchy);
    void open_pick_list(int x, int y, const std::vector<EditorPickResult>& hits);
    void update_placement_preview(int x, int y);
    void finish_placement(bool cancel);
    void close_context_menu() noexcept;
    [[nodiscard]] std::optional<EditorObjectId> hierarchy_row_object_at(int x, int y) const;
    void handle_settings_key(std::string_view normalized, bool control, bool shift, bool alt);
    void handle_shortcut_editor_key(std::string_view normalized, bool control, bool shift, bool alt);
    [[nodiscard]] bool dispatch_shortcut_gesture(const ShortcutGesture& gesture);
    void synchronize_menu_shortcuts() noexcept;
    void remember_camera_position();
    [[nodiscard]] bool start_play_session(EditorMode mode);
    [[nodiscard]] bool stop_play_session(bool acceptChanges = false);
    void toggle_play_camera_possession();
    void route_play_key(std::string_view normalized, bool pressed);
    void refresh_play_input_axes();
    [[nodiscard]] bool refresh_asset_database(bool announce = true);
    [[nodiscard]] std::filesystem::path find_default_text3d_font() const;
    [[nodiscard]] std::filesystem::path find_default_gabor_asset() const;
    bool handle_voxel_slice_key(std::string_view normalized, bool control, bool shift, bool alt);
    bool begin_slice_pointer(int x, int y, std::uint32_t modifiers);
    void update_slice_pointer(int x, int y, std::uint32_t modifiers);
    void finish_slice_pointer(bool cancel);
    bool handle_voxel_boolean_key(std::string_view normalized, bool control, bool shift, bool alt);
    void voxel_boolean_tick();

    EditorWorkspace workspace_;
    EditorMaterialLibrary materials_;
    EditorAssetDatabase assetDatabase_{};
    bool thumbnailBacklog_{};
    EditorAssetBrowserState assetBrowserState_{};
    AssetDragState assetDrag_;
    std::optional<Float3> pendingImportDrop_;   // a model drop waiting for its options to open
    void begin_asset_drag(const EditorAssetRecord& asset, int x, int y);
    void update_asset_drag(int x, int y);
    void finish_asset_drag(int x, int y);
    bool load_asset_drag_payload();
    bool place_dragged_asset();
    [[nodiscard]] std::string material_drop_refusal(const EditorObject& target) const;
    [[nodiscard]] std::string asset_drop_blocked_reason() const;
    EditorPlaySession playSession_{};
    EditorText3DAuthoringSession text3dAuthoring_{};
    EditorVoxelSliceSession voxelSlice_{};
    bool sliceSnap_{true};
    int sliceDrag_{}; // -1 normal translation, 1/2/3 world-axis rotation
    int sliceStartX_{}, sliceStartY_{};
    Float3 sliceStartPoint_{}, sliceStartNormal_{};
    float sliceStartParameter_{}, sliceLastAngle_{}, sliceAngle_{};
    bool sliceAxisFallback_{};
    EditorVoxelBooleanSession voxelBoolean_{};
    EditorVoxelJoinSession voxelJoin_{};
    bool floodFillArmed_{};
    std::set<EditorObjectId> openGroups_;
    bool pivotEditing_{};
    [[nodiscard]] bool has_own_geometry(const EditorObject& object) const;
    void prune_groups();
    SelectionDomain selectionDomain_{SelectionDomain::Objects};
    PolygonSelection polygonSelection_{};
    std::optional<CookedPolygonAsset> polygonDragBefore_;
    std::optional<CookedPolygonAsset> polygonPreviewBefore_;
    std::optional<SubdivisionSettings> polygonPreviewSubdivision_;
    std::optional<CookedPolygonAsset> polygonClipboard_;
    std::optional<SubdivisionSettings> polygonClipboardSubdivision_;
    bool subdivisionCageVisible_{true};
    bool subdivisionCageXray_{};
    bool subdivisionCageOnly_{};
    mutable SubdivisionSettings subdivisionCageDrawSettings_{};
    mutable std::uint64_t subdivisionCageDrawHash_{};
    mutable EditorObjectId subdivisionCageDrawObject_{};
    mutable std::vector<PolygonEdge> subdivisionCageDrawEdges_;
    RigidTransform polygonClipboardPose_{};
    Float3 polygonClipboardScale_{1,1,1};
    bool polygonClipboardNewest_{};
    PolygonSelection polygonPreviewSelection_{};
    PolygonEdit polygonPreviewOperation_{PolygonEdit::Extrude};
    PolygonEditSettings polygonPreviewSettings_{};
    std::string polygonPreviewTyped_;
    bool polygonPreviewValid_{};
    bool polygonLoopCutPreview_{},polygonLoopCutDragging_{};
    bool polygonDissolvePreview_{};
    bool polygonFaceAuthoringPreview_{};
    bool polygonBoundaryPreview_{},polygonBoundaryBridge_{};
    bool begin_polygon_boundary(bool bridge);
    bool polygonVertexDissolvePreview_{},polygonMergePreview_{};
    bool begin_polygon_cleanup(bool merge);
    int polygonLoopCutStartX_{};
    float polygonLoopCutStartFraction_{.5F};
    std::optional<std::string> polygonPreviewUvRecord_;
    std::optional<ModelingTopology> polygonPreviewModelingBefore_,polygonPreviewModelingAfter_;
    ModelingId polygonPreviewModelingFloor_{1};
    bool polygonPreviewModelingMigrated_{};
    // Allocation floor before the preview; Cancel/refusal restore it so no unapplied ID is reserved.
    std::optional<ModelingId> polygonPreviewModelingNextIdBefore_;
    bool begin_modeling_identity_preview(EditorObject&);
    bool update_modeling_identity_preview(const CookedPolygonAsset&,const std::optional<SubdivisionSettings>&,PolygonTopologyLineage);
    bool begin_polygon_loop_cut();
    bool begin_polygon_dissolve();
    bool clean_unused_polygon_vertices();
    bool polygonSlicePreview_{};
    Float3 polygonSlicePoint_{}, polygonSliceOrigin_{}, polygonSliceNormal_{1,0,0};
    int polygonSliceOutput_{}; // 0 front, 1 back, 2 separate
    bool polygonSliceCap_{true};
    std::optional<CookedPolygonAsset> polygonSliceBack_;
    bool begin_polygon_slice();
    void prune_polygon_selection();
    void finish_polygon_marquee(int, int);
    bool polygon_component_visible(std::uint64_t) const;
    bool handle_polygon_action(std::string_view);
    bool begin_polygon_preview(PolygonEdit);
    void update_polygon_preview();
    bool commit_polygon_preview();
    bool handle_polygon_preview_key(std::string_view);
    Float3 polygon_selection_centre() const;
    struct VoxelAuthoringState {
        enum class Kind { Inactive, Region, PolygonToVoxel, VoxelToPolygon };
        Kind kind{Kind::Inactive};
        EditorObjectId object{},output{};
        VoxelRegionEdit edit{VoxelRegionEdit::Extrude};
        VoxelRegionSettings settings{};
        GeometryConversionSettings conversion{};
        bool subdivisionOutput{};
        std::uint32_t subdivisionLevel{2};
        int pivotMode{}; // bounds centre, cell (0,0,0) centre, custom cell centre
        Int3 customPivot{};
        std::vector<MaterialId> paletteSlots;
        std::optional<EditorDocument> preview;
        std::set<Int3> previewCells;
        std::uint64_t documentRevision{},selectionSerial{};
        bool ready{},fieldEditing{},replaceTyped{true};
        int field{}; // amount/size, pivot X/Y/Z
        std::string typed,error,note;
    } voxelAuthoring_;
    std::uint64_t voxelAuthoringGeneration_{};
    VoxelClickSelection voxelClickSelection_{VoxelClickSelection::ConnectedPiece};
    bool commit_voxel_authoring_field();
    struct VoxelAuthoringLayout {
        UiRect panel{},amount{},refresh{},apply{},cancel{};
        std::array<UiRect,3> axes{},pivots{},coordinates{};
    };
    [[nodiscard]] VoxelAuthoringLayout voxel_authoring_layout(UiRect viewport) const;
    [[nodiscard]] std::string voxel_authoring_field_value(int field) const;
    EditorObjectId voxelSelectionObject_{};
    std::set<Int3> voxelSelection_;
    std::uint64_t voxelSelectionRevision_{};
    bool voxelBrushSelecting_{};
    bool voxelSelectionBrushEnabled_{};
    std::optional<Int3> voxelRegionRotationPivot_;
    EditorObjectId voxelRegionRotationObject_{};
    std::uint64_t voxelRegionRotationSerial_{};
    SelectionOperation voxelBrushOperation_{SelectionOperation::Add};
    int voxelMoveAxis_{};
    int voxelMoveStartX_{};
    int voxelMoveStartY_{};
    int voxelMoveSteps_{};
    std::optional<VoxelClipboard> voxelClipboard_;
    bool voxelClipboardNewest_{};  // Paste uses whichever clipboard was filled last
    std::uint64_t voxelSelectionSerial_{};  // bumped whenever voxelSelection_ changes
    mutable std::uint64_t voxelCentreCacheSerial_{};
    mutable bool voxelCentreCacheValid_{};
    mutable Float3 voxelCentreCacheLocal_{};
    void prune_voxel_selection();
    void reset_voxel_selection_state();
    [[nodiscard]] EditorSelectionSnapshot capture_selection_snapshot() const;
    void restore_selection_snapshot(const EditorSelectionSnapshot& snapshot);
    void close_previews_for_voxel_edit(std::string_view what);
    [[nodiscard]] Float3 voxel_selection_centre() const;
    [[nodiscard]] Float3 voxel_move_axis_world(int axis) const;
    void voxel_brush_select_at(int x, int y);
    void finish_voxel_marquee(int x, int y);
    // View > Follow Selection (Shift+F): the camera keeps the selection framed as it moves.
    bool followSelection_{};
    std::optional<Float3> followAnchor_;
    struct CachedCollisionProxy { std::uint64_t revision{}; std::uint64_t objectStamp{}; std::vector<VoxelBox> boxes; };
    mutable std::map<EditorObjectId, CachedCollisionProxy> collisionProxyCache_;
    bool handle_voxel_join_key(std::string_view normalized, bool control, bool shift, bool alt);
    bool scatterActive_{};
    struct DuplicateSpecialState {
        bool active{};
        std::vector<EditorObjectId> roots;  // the selected roots when the panel opened
        DuplicateSpecialSettings settings;  // kept between uses
    };
    DuplicateSpecialState duplicateSpecial_;
    [[nodiscard]] std::size_t duplicate_objects_per_copy() const;
    std::vector<EditorObjectId> scatterTargets_;
    std::vector<EditorObjectId> scatterSourceRoots_;
    std::vector<EditorObjectId> pinnedScatterRoots_;
    std::shared_ptr<const EditorPrefabAsset> pinnedScatterPrefab_;
    // Scatter settings come from the newest scatter group the first time a scene scatters (see
    // adopt_scatter_settings); this remembers which scene that was.
    std::optional<std::filesystem::path> scatterSettingsSceneKey_;
    void adopt_scatter_settings();
    [[nodiscard]] std::shared_ptr<const EditorPrefabAsset> load_selected_scatter_prefab(std::string* error) const;
    std::shared_ptr<const EditorPrefabAsset> scatterPrefab_;
    ScatterSettings scatterSettings_{};
    ScatterPlan scatterPlan_{};
    [[nodiscard]] std::vector<ScatterSource> scatter_sources() const;
    void replan_scatter();
    // Scatter Brush session.
    std::vector<EditorObjectId> brushSourceRoots_;
    std::shared_ptr<const EditorPrefabAsset> brushPrefab_;
    std::optional<EditorObjectId> brushGroup_;
    std::optional<Float3> brushCursor_;
    bool brushStroking_{};
    bool brushErasing_{};
    bool brushResizing_{};
    int brushResizeLastX_{};
    std::vector<ScatterSample> brushPending_;
    std::set<EditorObjectId> brushEraseIds_;
    std::optional<Float3> brushLastDab_;
    std::uint64_t brushDabCounter_{};
    void begin_scatter_brush_session();
    void keep_isolated_with_subtree(EditorObjectId root);
    [[nodiscard]] std::vector<ScatterSource> brush_sources() const;
    [[nodiscard]] bool brush_ground_accepts(EditorObjectId id) const;
    void update_brush_cursor(int x, int y);
    void brush_dab();
    void finish_brush_stroke(bool cancel);
    bool handle_scatter_key(std::string_view normalized, bool control, bool shift, bool alt);
    VoxelBooleanOperandPolicy voxelBooleanPolicy_{VoxelBooleanOperandPolicy::Hide};
    std::uint64_t voxelBooleanMenuKey_{};
    std::filesystem::path projectRoot_;
    bool workspaceProjectConfigured_{};
    EditorWorkspaceState startupWorkspaceState_;
    std::filesystem::path pendingWorkspaceAsset_;
    [[nodiscard]] EditorWorkspaceState capture_workspace_state() const;
    void restore_workspace_state();
    void restore_workspace_asset_selection();
    void save_workspace_state_on_quit();
    EditorCamera camera_;
    CameraNavigationFilter cameraNavigationFilter_{};
    float cameraInputAcceleration_{};
    float cameraInputSmoothing_{};
    std::array<float, 2> pendingCameraLook_{}; // angular delta, consumed on update
    std::optional<camera::CameraRigId> navigationCameraRig_{};
    camera::CameraRigMode cameraMode_{camera::CameraRigMode::Orbit};
    camera::CameraDirector cameraDirector_{};
    struct CameraTargetHistory {
        Float3 position{};
        Float3 velocity{};
        std::uint64_t generation{};
    };
    struct CameraRigSettingsBaseline {
        camera::CameraFramingSettings framing;
        camera::CameraCollisionSettings collision;
        camera::CameraFramingSettings appliedFraming;
        camera::CameraCollisionSettings appliedCollision;
    };
    std::map<camera::CameraRigId, CameraRigSettingsBaseline> cameraRigSettingsBaselines_;
    std::map<camera::CameraTargetId, CameraTargetHistory> cameraTargetHistory_;
    std::uint64_t cameraTargetGeneration_{};
    std::optional<camera::CameraRigId> selectedCameraRig_{};
    camera::CameraRigId nextCameraRigId_{1};
    std::array<EditorCameraBookmark, 10> cameraBookmarks_{};
    EditorSettingsPanelState settingsPanel_{};
    EditorShortcutPanelState shortcutPanel_{};
    EditorCinematicCameraPanel cinematicCameraPanel_{};
    std::optional<ShortcutContext> shortcutContextOverride_{};
    EditorViewportSettings viewportSettings_;
    // Per-frame render caches (logically const: pure memoization of derived data).
    mutable EditorVoxelDrawListCache drawListCache_{};
    mutable EditorVoxelDrawListCache previewDrawListCache_{};
    mutable EditorSelectionDiagnostics selectionDiagnostics_{};
    mutable std::uint64_t selectionDiagnosticsKey_{};
    mutable bool selectionDiagnosticsValid_{};
    mutable std::uint64_t selectionDiagnosticsRebuilds_{};
    [[nodiscard]] std::uint64_t material_density_fingerprint() const noexcept;
    NativeEditorLayout layout_;
    int width_{1280};
    int height_{800};
    EditorToolId activeTool_{EditorToolId::Select};
    MaterialId activeMaterial_{kDefaultSurfaceMaterial};
    EditorTransformSpace transformSpace_{EditorTransformSpace::World};
    EditorFocusRegion focusRegion_{EditorFocusRegion::Viewport};
    std::optional<EditorPickResult> hoverPick_;
    std::optional<std::string> openMenu_;
    std::optional<std::size_t> menuHoveredAction_;
    std::size_t menuScrollOffset_{};
    bool commandPaletteOpen_{};
    std::string commandPaletteQuery_;
    std::size_t commandPaletteSelection_{};
    std::deque<std::string> recentCommandIds_;
    std::set<std::string, std::less<>> favoriteCommandIds_;
    mutable std::vector<CommandPaletteResult> paletteCache_;
    mutable std::uint64_t paletteCacheKey_{};
    mutable bool paletteCacheValid_{};
    mutable std::uint64_t paletteCacheRebuilds_{};
    bool showAdvancedMenus_{};
    std::filesystem::path menuStatePath_;
    std::filesystem::path userSettingsPath_;
    std::filesystem::path projectSettingsPath_;
    float uiZoomWindowLimit_{2.0F};
    bool quitRequested_{};
    PendingDestructiveAction pendingDestructiveAction_{PendingDestructiveAction::Inactive};
    FilePickerState filePicker_;
    ImportOptionsState importOptions_;
    bool handle_import_options_key(std::string_view normalized);
    void refresh_import_estimate();
    bool start_import_job(bool reimport);
    struct LongJob;
    std::shared_ptr<LongJob> longJob_;
    std::uint64_t longJobThreshold_{250'000};
    // Runs work (on the worker thread, or at once when workUnits <= the threshold) and then
    // commit on the UI thread unless it was cancelled. Refuses when another job runs.
    bool start_long_job(LongJobKind kind, std::string label, std::uint64_t workUnits,
                        std::function<void(LongJob&)> work, std::function<bool(LongJob&)> commit);
    bool open_import_options_for(const std::filesystem::path& relativeSource, bool reimport, EditorObjectId target,
                                 EditorImportSettings settings);
    std::size_t lastValidateFound_{};
    void poll_long_job();
    void finish_long_job();
    std::filesystem::path pendingOpenPath_;
    bool open_scene_now(const std::filesystem::path& path);
    bool open_project_now(const std::filesystem::path& root);
    void remember_recent_scene(const std::filesystem::path& path);
    bool handle_file_picker_key(std::string_view normalized);
    std::function<void()> wakeCallback_;
    EditorStatusMessage status_{"Ready", false, 0.0F};

    PointerButton dragButton_{PointerButton::NoButton};
    bool rawMouseRequested_{true};
    std::string navigationShortcutProfile_{"DVE Default"};
    std::string configuredShortcutProfile_;
    int hoverX_{-100000};
    int hoverY_{-100000};
    int inspectorScroll_{};
    int hierarchyScroll_{};
    std::set<EditorObjectId> collapsedHierarchy_;  // view state; never changes authored groups
    void reset_hierarchy_view();
    void scroll_hierarchy_to(EditorObjectId id);
    bool navigate_hierarchy(std::string_view key);
    int lastPointerX_{};
    int lastPointerY_{};
    int pointerDownX_{};
    int pointerDownY_{};
    PointerButton lastClickButton_{PointerButton::NoButton};
    int lastClickX_{};
    int lastClickY_{};
    std::chrono::steady_clock::time_point lastClickTime_{};
    bool doubleClickConsumed_{};
    int lastPickX_{};
    int lastPickY_{};
    std::size_t pickCycleIndex_{};
    // Where an Alt+click orbit started: released without moving, it opens the pick list (ART-013).
    std::optional<std::pair<int, int>> orbitPress_;
    SelectionMask selectionMask_;
    // Selection targets under a viewport point, nearest first, closed groups folded, masks applied.
    [[nodiscard]] std::vector<EditorPickResult> selectable_targets_at(int x, int y) const;
    // Of `candidates`, the selection targets with a visible (unoccluded) part inside `rect`.
    [[nodiscard]] std::set<EditorObjectId> targets_seen_in_rect(UiRect rect, const std::set<EditorObjectId>& candidates) const;
    bool dispatch_selection_filter_action(std::string_view action);
    std::chrono::steady_clock::time_point lastPickTime_{};
    PlacementTarget placementMode_{PlacementTarget::NoTarget};
    std::optional<Float3> placementPoint_;
    std::vector<ObjectTransformChange> placementChanges_;
    Float3 placementPivot_{};
    bool alignToSurfaceNormal_{};
    float placementOffsetMeters_{};
    bool voxelStrokeActive_{};
    std::int32_t voxelBrushRadius_{};
    BrushShape voxelBrushShape_{BrushShape::Cube};
    // The brush preview is recomputed only when the cursor cell, brush, tool or object changes.
    struct BrushPreviewKey {
        EditorObjectId object{}; Int3 voxel{}; Int3 normal{}; std::int32_t radius{}; BrushShape shape{};
        EditorToolId tool{}; MaterialId material{}; std::uint64_t revision{}; std::size_t selection{};
        EditorObjectId selectionObject{}; bool locked{};
        auto operator<=>(const BrushPreviewKey&) const = default;
    };
    mutable std::optional<BrushPreviewKey> brushPreviewKey_;
    mutable std::optional<VoxelBrushPreview> brushPreview_;
    [[nodiscard]] bool in_voxel_brush(Int3 cell, Int3 center) const noexcept;
    [[nodiscard]] std::size_t selection_fingerprint() const noexcept;
    std::string activePointerCommand_;
    std::vector<ShortcutGesture> heldShortcutGestures_;
    std::optional<EditorCamera> previousCamera_;
    std::optional<EditorCamera> nextCamera_;
    std::optional<EditorCamera> prePlayEditorCamera_;
    std::optional<EditorCamera> possessedPlayCamera_;
    bool playCameraPossessed_{};
    std::uint64_t strokeId_{1};

    bool gizmoDragging_{};
    int gizmoAxis_{};
    int gizmoStartX_{};
    int gizmoStartY_{};
    // Plane-measured rotate dragging: the pointer ray is intersected with
    // the grabbed ring's plane and the world angle around the axis is
    // accumulated (unwrapped), so rotation tracks the pointer exactly
    // regardless of how the ring projects on screen. Falls back to the
    // linear projection method when the grab ray is nearly parallel to
    // the ring plane.
    Float3 gizmoDragBasisU_{};
    Float3 gizmoDragBasisV_{};
    float gizmoDragWorldAngle_{};
    float gizmoDragWorldAccum_{};
    bool gizmoDragPlane_{};
    Float3 gizmoPivot_{};
    std::vector<ObjectTransformChange> gizmoChanges_;
    std::vector<ObjectVoxelSizeChange> gizmoSizeChanges_;
    bool gizmoResampleScale_{};
    std::optional<EditorObjectBounds> scalePreviewBounds_;
    Float3 scaleFactors_{1.0F, 1.0F, 1.0F};
    PivotMode pivotMode_{PivotMode::BoundsCenter};
    Float3 customPivot_{};
    std::string transformNumeric_;
    int gizmoLastX_{};
    int gizmoLastY_{};
    std::uint32_t gizmoLastModifiers_{};
    // In-process clipboard for Edit > Cut/Copy/Paste. Deep-cloned objects, not references, so
    // pasting after the source object was itself deleted (undoably or not) still works, and
    // pasting more than once duplicates the same captured content each time.
    std::vector<EditorObject> clipboard_;

    TextEditState textEdit_;
    ContextMenuState contextMenu_;
    MarqueeState marquee_;
    HierarchyDragState hierarchyDrag_;
    BottomPanelTab bottomTab_{BottomPanelTab::Console};
    std::string hierarchyFilter_;
    audio::AudioMixer audioMixer_{};
    std::uint64_t appliedSettingsRevision_{};
    bool systemThemeLight_{};
    bool audioSettingsPending_{};
    editor::AudioSpatializerMode appliedSpatializerMode_{editor::AudioSpatializerMode::Native};
    std::array<float, 2> gamepadMove_{};
    std::array<float, 2> gamepadLook_{};
    EditorSynthPanel synthPanel_{};
    EditorChiptunePanel chiptunePanel_{};
    EditorAudioPanel audioPanel_{};
    EditorAudioEventPanel audioEventPanel_{};
    EditorControlRigPanel controlRigPanel_{};
    EditorSpriteAuthoringPanel spriteAuthoringPanel_{};
    SpriteAnimationMachineGraphWorkspace spriteAnimationGraph_{};
    bool spriteAnimationGraphOpen_{};
    std::unique_ptr<gameplay::ChiptuneVerticalSlice> spriteLevel_{};
    std::unique_ptr<gameplay::SpriteVerticalSlicePresentation> spriteLevelPresentation_{};
    gameplay::ChiptuneSliceInput spriteLevelInput_{};
    bool spriteLevelPlaying_{};
    SimulationClock spriteLevelClock_{};
    bool spriteDiagnosticsOpen_{};
    bool render3DDiagnosticsOpen_{};
    Render3DDiagnosticsReport render3DDiagnosticsReport_{make_render3d_diagnostics_demo_report()};
    SpritePixelArtSession spritePixelArt_{};
    bool spritePixelArtOpen_{};
    std::filesystem::path spritePixelArtPath_{};
    bool spritePixelPainting_{};
    std::int32_t spritePixelLastX_{};
    std::int32_t spritePixelLastY_{};
    SpriteRig2DAuthoringSession spriteRig2D_{};
    bool spriteRig2DOpen_{};
    std::filesystem::path spriteRig2DPath_{};
    bool spriteRig2DPreviewPlaying_{true};
    float spriteRig2DPreviewTime_{};
    TileWorldDesktopWorkspace tileWorldEditor_{};
    bool tileWorldEditorOpen_{};
    std::uint64_t tileWorldEditorTicks_{};
    std::unique_ptr<ai::DveAiBridge> aiBridge_;
    std::unique_ptr<ai::LiveEditorMcpHost> liveMcpHost_;
    EditorTaskManager aiTasks_{1, 8};
    struct AutosaveJob;
    std::shared_ptr<AutosaveJob> autosaveInFlight_;
    double autosaveElapsedSeconds_{};
    std::uint64_t manualSaveCount_{};
    EditorAutosaveStatus autosaveStatus_;
    bool recoveryChecked_{};
    struct AssetScanJob;
    std::shared_ptr<AssetScanJob> assetScanInFlight_;
    bool assetRescanQueued_{};
    bool assetRescanAnnounce_{};
    void asset_scan_tick();
    void apply_asset_scan(AssetScanJob& job);
    // Declared after the state above so their workers are joined before that state goes.
    EditorTaskManager autosaveTasks_{1, 2};
    EditorTaskManager assetScanTasks_{1, 2};
    EditorTaskManager longJobTasks_{1, 16}; // one worker; cancelled jobs still queued skip their work
    // Destroyed before longJobTasks_ joins its worker: asks a running long job to stop.
    struct LongJobCancelOnExit {
        std::shared_ptr<LongJob>* job{};
        ~LongJobCancelOnExit();
    } longJobCancelOnExit_{&longJob_};
    void autosave_tick(float elapsedSeconds);
    void report_recovered_autosave();
    // Declared last so they are destroyed first: the session's callbacks post into the synth.
    std::unique_ptr<audio::MidiOutputSession> midiOutput_;
    std::unique_ptr<audio::MidiInputSession> midiInput_;
    audio::MidiInputStatus midiStatus_{};
    std::uint64_t midiStatusGeneration_{~std::uint64_t{0}};
    std::string midiChoicesPort_{"\x01"};
    audio::MidiOutputStatus midiOutputStatus_{};
    std::uint64_t midiOutputStatusGeneration_{~std::uint64_t{0}};
    std::string midiOutputChoicesPort_{"\x01"};
    std::shared_ptr<NativeMeshAuthoringState> meshAuthoring_;
};

[[nodiscard]] EditorDocument make_new_project_document();
[[nodiscard]] EditorDocument make_new_scene_document();
[[nodiscard]] EditorDocument make_native_editor_demo_document();

} // namespace dve::editor
