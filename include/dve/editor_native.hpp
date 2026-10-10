#pragma once
#include "dve/simulation_clock.hpp"

#include <array>
#include <chrono>
#include <deque>
#include <cstdint>
#include <filesystem>
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
#include "dve/editor_scatter.hpp"
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
inline constexpr std::array<std::string_view, 8> kMenuBarNames{
    "File", "Edit", "Create", "View", "Tools", "Build", "Window", "Help"};

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
};

// Tools with a toolbar button (EditorToolId::Select .. Rotate). Scale has no button; it is
// selected by its shortcut or the Edit menu, but still has an editor_tool_info() entry.
inline constexpr std::size_t kEditorToolCount = 9;
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
enum class PendingDestructiveAction : std::uint8_t { Inactive, Quit, NewProject, NewScene };

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
    AssetRename, AiPrompt
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

struct MarqueeState {
    bool active{};
    std::optional<EditorObjectId> clickObject;
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
    int hierarchyScroll{};               // first visible row
    int hierarchyScrollMax{};
    int hierarchyVisibleRows{};
    UiRect hierarchyFilterBox{};
    std::vector<UiRect> inspectorToggles;
    std::vector<UiRect> inspectorFields; // [0]=Position line, [1]=Rotation line
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

// Background recovery saves driven by the editor.autosave_minutes setting.
struct EditorAutosaveStatus {
    bool inFlight{};
    std::uint64_t completed{};
    std::uint64_t failed{};
    std::filesystem::path lastManifest;
    std::string lastError;
};

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
    void pointer_down(PointerButton button, int x, int y, std::uint32_t modifiers = 0);
    void pointer_up(PointerButton button, int x, int y, std::uint32_t modifiers = 0);
    void pointer_wheel(float steps, int x, int y, std::uint32_t modifiers = 0);
    void key_down(std::string_view key, bool control, bool shift, bool alt);
    void key_up(std::string_view key, bool control, bool shift, bool alt);
    void text_input(std::string_view text);
    void text_input(char character) { text_input(std::string_view(&character, 1U)); }

    [[nodiscard]] bool dispatch_action(std::string_view actionId);
    void set_active_tool(EditorToolId tool) noexcept;
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
    [[nodiscard]] CommandResult set_primary_position(Float3 worldPosition);
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
    [[nodiscard]] EditorObjectId scatter_target() const noexcept { return scatterTarget_; }
    [[nodiscard]] std::vector<std::string> scatter_preview_lines() const;
    // Why Scatter Objects cannot start with the current selection; empty when it can.
    [[nodiscard]] std::string scatter_disabled_reason() const;
    [[nodiscard]] VoxelBooleanOperandPolicy voxel_boolean_operand_policy() const noexcept { return voxelBooleanPolicy_; }
    void set_voxel_boolean_operand_policy(VoxelBooleanOperandPolicy policy);
    [[nodiscard]] std::vector<std::string> voxel_boolean_preview_lines() const;
    [[nodiscard]] std::vector<VoxelBooleanPreviewMarker> voxel_boolean_preview_markers(std::size_t limit = 20000) const;
    [[nodiscard]] bool begin_voxel_slice();
    [[nodiscard]] std::string voxel_slice_disabled_reason() const;
    [[nodiscard]] CommandResult commit_voxel_slice();
    void cancel_voxel_slice();
    [[nodiscard]] const EditorVoxelSliceSession& voxel_slice() const noexcept { return voxelSlice_; }
    [[nodiscard]] bool configure_voxel_slice(Float3 point, Float3 normal, VoxelSliceOutput output);
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
    void begin_gizmo_drag(int x, int y);
    void update_gizmo_drag(int x, int y, std::uint32_t modifiers);
    void navigate_pointer(float deltaX, float deltaY);
    void finish_gizmo_drag(bool cancel);
    void set_status(std::string text, bool error = false, float seconds = 3.0F);
    void create_new_project_now();
    void create_new_scene_now();
    [[nodiscard]] Float3 gizmo_axis_world(int axis) const noexcept;
    [[nodiscard]] Float3 selection_pivot() const noexcept;
    [[nodiscard]] EditorObjectBounds selection_bounds() const noexcept;
    [[nodiscard]] std::vector<ObjectTransformChange> selection_transform_snapshot() const;
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
    void open_context_menu(int x, int y, std::optional<EditorObjectId> target, bool fromHierarchy);
    void open_pick_list(int x, int y, const std::vector<EditorPickResult>& hits);
    void walk_selection(std::string_view direction);
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
    bool handle_voxel_boolean_key(std::string_view normalized, bool control, bool shift, bool alt);
    void voxel_boolean_tick();

    EditorWorkspace workspace_;
    EditorMaterialLibrary materials_;
    EditorAssetDatabase assetDatabase_{};
    bool thumbnailBacklog_{};
    EditorAssetBrowserState assetBrowserState_{};
    EditorPlaySession playSession_{};
    EditorText3DAuthoringSession text3dAuthoring_{};
    EditorVoxelSliceSession voxelSlice_{};
    EditorVoxelBooleanSession voxelBoolean_{};
    bool scatterActive_{};
    EditorObjectId scatterTarget_{};
    std::vector<EditorObjectId> scatterSourceRoots_;
    std::shared_ptr<const EditorPrefabAsset> scatterPrefab_;
    ScatterSettings scatterSettings_{};
    ScatterPlan scatterPlan_{};
    [[nodiscard]] std::vector<ScatterSource> scatter_sources() const;
    void replan_scatter();
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
    std::chrono::steady_clock::time_point lastPickTime_{};
    PlacementTarget placementMode_{PlacementTarget::NoTarget};
    std::optional<Float3> placementPoint_;
    std::vector<ObjectTransformChange> placementChanges_;
    Float3 placementPivot_{};
    bool alignToSurfaceNormal_{};
    float placementOffsetMeters_{};
    bool voxelStrokeActive_{};
    std::int32_t voxelBrushRadius_{};
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
};

[[nodiscard]] EditorDocument make_new_project_document();
[[nodiscard]] EditorDocument make_new_scene_document();
[[nodiscard]] EditorDocument make_native_editor_demo_document();

} // namespace dve::editor
